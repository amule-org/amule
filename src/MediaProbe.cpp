//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
//

#include "MediaProbe.h"

#include "config.h" // Needed for AMULE_MEDIA_PROBE, AMULE_MEDIA_PROBE_RELPATH

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <wx/arrstr.h>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/stopwatch.h>
#include <wx/tokenzr.h>
#include <wx/utils.h> // Needed for wxMilliSleep / wxGetenv

#include <common/Format.h>

#include "Logger.h"
#include "libs/common/Path.h"

// Native child-process primitives for the bounded, killable probe runner. wxExecute/wxProcess async
// bookkeeping is bound to the main-thread event loop (its SIGCHLD reaper fires
// wxProcess::OnTerminate there), so polling it from the probe worker races that loop -- a use-
// after-free. Managing the helper with native primitives keeps the whole lifecycle on this thread.
#ifdef __WXMSW__
#include <windows.h>
#else
#include <csignal>
#include <cerrno>
#include <sys/stat.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#else
extern char **environ;
#endif
#endif

namespace MediaProbe
{

namespace
{

// Return values for RunBoundedHelper: a child exit code (>= 0), or one of:
constexpr int kSpawnFailed = -1; // unavailable helper, abnormal exit, malformed or oversized reply
constexpr int kKilled = -2;      // overran timeoutMs, or keepRunning went false
constexpr int kCrashed = -3;     // died abnormally after printing its header

// Runs `exe` with `input` as stdin, capturing stdout to a temp file, bounded by `timeoutMs`
// wall-clock. The wait loop also polls `keepRunning`, so worker shutdown kills the child at once.
int RunBoundedHelper(const wxString &exe,
	unsigned timeoutMs,
	const std::atomic<bool> &keepRunning,
	wxArrayString &stdoutLines,
	const CPath &input)
{
	if (!keepRunning || !timeoutMs)
		return kKilled;
	const wxString tmpPath = wxFileName::CreateTempFileName(wxT("amule-mediaprobe"));
	if (tmpPath.IsEmpty()) {
		return kSpawnFailed;
	}

	int exitCode = kSpawnFailed;
	const wxFileOffset kMaxProbeOutputBytes = 64 * 1024;
	const auto outputTooLarge = [&] {
		wxFFile output(tmpPath, wxT("rb"));
		return output.IsOpened() && output.Length() > kMaxProbeOutputBytes;
	};

#ifdef __WXMSW__
	SECURITY_ATTRIBUTES sa;
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	sa.lpSecurityDescriptor = nullptr;
	HANDLE hOut = ::CreateFileW(tmpPath.wc_str(),
		GENERIC_WRITE,
		FILE_SHARE_READ,
		&sa,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_TEMPORARY,
		nullptr);
	if (hOut == INVALID_HANDLE_VALUE) {
		wxRemoveFile(tmpPath);
		return kSpawnFailed;
	}

	// Kill-on-close job: one process, 1 GiB of memory.
	HANDLE hJob = ::CreateJobObjectW(nullptr, nullptr);
	if (hJob != nullptr) {
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
		::ZeroMemory(&jeli, sizeof(jeli));
		jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
							JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
							JOB_OBJECT_LIMIT_PROCESS_MEMORY;
		jeli.BasicLimitInformation.ActiveProcessLimit = 1;
		jeli.ProcessMemoryLimit = size_t(1024) * 1024 * 1024;
		if (!::SetInformationJobObject(
			    hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
			::CloseHandle(hJob);
			hJob = nullptr;
		}
	}

	const wxString cmdLine = wxT("\"") + exe + wxT("\"");
	std::vector<wchar_t> cmdBuf(cmdLine.wc_str(), cmdLine.wc_str() + cmdLine.length() + 1);

	HANDLE hIn = ::CreateFileW(input.GetRaw().wc_str(),
		GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		&sa,
		OPEN_EXISTING,
		0,
		nullptr);
	HANDLE hErr = ::CreateFileW(
		L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
	if (hIn == INVALID_HANDLE_VALUE || hErr == INVALID_HANDLE_VALUE) {
		if (hIn != INVALID_HANDLE_VALUE)
			::CloseHandle(hIn);
		if (hErr != INVALID_HANDLE_VALUE)
			::CloseHandle(hErr);
		::CloseHandle(hOut);
		if (hJob)
			::CloseHandle(hJob);
		wxRemoveFile(tmpPath);
		return kSpawnFailed;
	}
	STARTUPINFOEXW extended;
	::ZeroMemory(&extended, sizeof(extended));
	STARTUPINFOW &si = extended.StartupInfo;
	::ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(extended);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = hIn;
	si.hStdOutput = hOut;
	si.hStdError = hErr;
	SIZE_T bytes = 0;
	::InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
	std::vector<unsigned char> attributes(bytes);
	extended.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
	HANDLE inherited[] = { hIn, hOut, hErr };
	if (!::InitializeProcThreadAttributeList(extended.lpAttributeList, 1, 0, &bytes)) {
		::CloseHandle(hIn);
		::CloseHandle(hErr);
		::CloseHandle(hOut);
		if (hJob)
			::CloseHandle(hJob);
		wxRemoveFile(tmpPath);
		return kSpawnFailed;
	}
	if (!::UpdateProcThreadAttribute(extended.lpAttributeList,
		    0,
		    PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
		    inherited,
		    sizeof(inherited),
		    nullptr,
		    nullptr)) {
		::DeleteProcThreadAttributeList(extended.lpAttributeList);
		::CloseHandle(hIn);
		::CloseHandle(hErr);
		::CloseHandle(hOut);
		if (hJob)
			::CloseHandle(hJob);
		wxRemoveFile(tmpPath);
		return kSpawnFailed;
	}

	PROCESS_INFORMATION pi;
	::ZeroMemory(&pi, sizeof(pi));

	// Start suspended so the child is in the job before it runs (and thus
	// before it can spawn a grandchild that escapes the job), then resume.
	const BOOL ok = ::CreateProcessW(exe.wc_str(),
		cmdBuf.data(),
		nullptr,
		nullptr,
		TRUE,
		CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
		nullptr,
		nullptr,
		&si,
		&pi);
	::DeleteProcThreadAttributeList(extended.lpAttributeList);
	::CloseHandle(hIn);
	::CloseHandle(hErr);
	::CloseHandle(hOut);
	if (!ok) {
		if (hJob != nullptr) {
			::CloseHandle(hJob);
		}
		wxRemoveFile(tmpPath);
		return kSpawnFailed;
	}
	if (hJob != nullptr && !::AssignProcessToJobObject(hJob, pi.hProcess)) {
		// An outer job may reject assignment. Timeout must then terminate the
		// process directly rather than waiting forever after killing an empty job.
		::CloseHandle(hJob);
		hJob = nullptr;
	}
	::ResumeThread(pi.hThread);

	wxStopWatch sw;
	for (;;) {
		if (::WaitForSingleObject(pi.hProcess, 25) == WAIT_OBJECT_0) {
			DWORD code = 0;
			if (::GetExitCodeProcess(pi.hProcess, &code)) {
				// Unhandled Windows exceptions use NTSTATUS codes with the high bit
				// set. Ordinary, undocumented error exits must remain retryable, just
				// as on POSIX, even if the helper already printed its header.
				exitCode = code >= 0x80000000UL ? kCrashed : static_cast<int>(code);
			}
			break;
		}
		const bool oversized = outputTooLarge();
		if (!keepRunning || static_cast<unsigned>(sw.Time()) >= timeoutMs || oversized) {
			if (hJob != nullptr) {
				::TerminateJobObject(hJob, 1);
			} else {
				::TerminateProcess(pi.hProcess, 1);
			}
			::WaitForSingleObject(pi.hProcess, INFINITE);
			exitCode = oversized ? kSpawnFailed : kKilled;
			break;
		}
	}
	::CloseHandle(pi.hProcess);
	::CloseHandle(pi.hThread);
	if (hJob != nullptr) {
		::CloseHandle(hJob);
	}
#else
	const std::string exeNative(exe.fn_str());
	char *const cargv[] = { const_cast<char *>(exeNative.c_str()), nullptr };

	// Bundled helper must inherit the bundle library environment.
	char **envp = environ;

	const std::string tmpNative(tmpPath.fn_str());

	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	const int inputFd = open(input.GetRaw().fn_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	struct stat inputStat;
	if (inputFd < 0 || fstat(inputFd, &inputStat) != 0 || !S_ISREG(inputStat.st_mode)) {
		if (inputFd >= 0)
			close(inputFd);
		posix_spawn_file_actions_destroy(&fa);
		wxRemoveFile(tmpPath);
		return kSpawnFailed;
	}
	posix_spawn_file_actions_adddup2(&fa, inputFd, STDIN_FILENO);
	if (inputFd != STDIN_FILENO)
		posix_spawn_file_actions_addclose(&fa, inputFd);
	else
		fcntl(inputFd, F_SETFD, 0);
	// stdout -> temp file, stderr -> /dev/null (we parse stdout only).
	posix_spawn_file_actions_addopen(
		&fa, STDOUT_FILENO, tmpNative.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
	posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

	// Own process group, so the kill below (-pid) also takes any child it started.
	posix_spawnattr_t attr;
	posix_spawnattr_init(&attr);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
	posix_spawnattr_setpgroup(&attr, 0);

	pid_t pid = 0;
	const int rc = posix_spawn(&pid, cargv[0], &fa, &attr, cargv, envp);
	close(inputFd);
	posix_spawnattr_destroy(&attr);
	posix_spawn_file_actions_destroy(&fa);
	if (rc != 0) {
		wxRemoveFile(tmpPath);
		return kSpawnFailed;
	}

	wxStopWatch sw;
	for (;;) {
		int status = 0;
		const pid_t r = waitpid(pid, &status, WNOHANG);
		if (r == pid) {
			exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : kCrashed;
			break;
		}
		if (r == -1 && errno == EINTR) {
			continue;
		}
		if (r == -1) { // vanished / already reaped
			exitCode = kSpawnFailed;
			break;
		}
		const bool oversized = outputTooLarge();
		if (!keepRunning || static_cast<unsigned>(sw.Time()) >= timeoutMs || oversized) {
			kill(-pid, SIGKILL);
			while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
			} // reap the leader; init reaps the rest
			exitCode = oversized ? kSpawnFailed : kKilled;
			break;
		}
		wxMilliSleep(25);
	}
#endif

	// A legitimate reply is a few hundred bytes; past 64 KiB it is a failed probe. A fast exit
	// can beat the polling check, so check again before reading any exit as a verdict.
	if (outputTooLarge()) {
		AddDebugLogLineN(logMediaProbe, wxT("MediaProbe: helper output over 64 KiB, ignoring"));
		exitCode = kSpawnFailed;
	}
	// A crash before the header is a broken install; after it, the file caused it.
	if (exitCode == kCrashed) {
		wxFFile f(tmpPath, wxT("rb"));
		char header[7] = {};
		if (!f.IsOpened() || f.Read(header, sizeof(header)) != sizeof(header) ||
			std::memcmp(header, "AMMP 1\n", sizeof(header)) != 0) {
			exitCode = kSpawnFailed;
		}
	}
	if (exitCode == 0) {
		wxFFile f(tmpPath, wxT("rb"));
		wxString content;
		if (f.IsOpened() && f.ReadAll(&content, wxConvUTF8)) {
			// Exactly LF-delimited records, including the terminal newline. Do not
			// silently discard blank lines or CR bytes from malformed responses.
			if (content.empty() || content.Last() != wxT('\n') ||
				content.find(wxT('\r')) != wxString::npos ||
				content.find(wxT('\0')) != wxString::npos) {
				exitCode = kSpawnFailed;
			} else {
				content.RemoveLast();
				wxStringTokenizer tok(content, wxT("\n"), wxTOKEN_RET_EMPTY_ALL);
				while (tok.HasMoreTokens()) {
					stdoutLines.Add(tok.GetNextToken());
				}
			}
		}
	}
	wxRemoveFile(tmpPath);
	return exitCode;
}

} // anonymous namespace

wxArrayString HelperCandidates(const wxString &executable)
{
	wxFileName sibling(executable);
#ifdef __WXMSW__
	sibling.SetFullName(wxT("amule-media-probe.exe"));
#else
	sibling.SetFullName(wxT("amule-media-probe"));
#endif
	wxArrayString candidates;
	candidates.Add(sibling.GetFullPath());
#ifdef AMULE_MEDIA_PROBE_RELPATH
	wxFileName installed(
		wxFileName(executable).GetPathWithSep() + wxString::FromUTF8(AMULE_MEDIA_PROBE_RELPATH));
	installed.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE);
	candidates.Add(installed.GetFullPath());
#endif
	return candidates;
}

wxString AutoDetectPath()
{
#ifdef AMULE_MEDIA_PROBE
	for (const wxString &candidate : HelperCandidates(wxStandardPaths::Get().GetExecutablePath())) {
		if (wxFileName::FileExists(candidate)) {
			return candidate;
		}
	}
#endif
	return wxString();
}

wxString DetectedPath(bool redetect)
{
	static std::mutex mutex;
	static bool done = false;
	static wxString cached;
	std::lock_guard<std::mutex> lock(mutex);
	if (!done || redetect) {
		cached = AutoDetectPath();
		done = true;
#ifdef AMULE_MEDIA_PROBE
		// A build without the helper disables the toggle instead.
		if (cached.empty())
			AddLogLineN(_("Media metadata: bundled amule-media-probe helper is unavailable."));
#endif
	}
	return cached;
}

namespace
{

// Longest tag value we keep. Tags are attacker-chosen and go into known.met, the log and every
// offered-file packet.
const size_t kMaxTagChars = 256;

// Caps the length and drops control characters, which would let one log line look like several.
wxString SanitiseTagValue(const wxString &value)
{
	wxString out;
	for (const wxUniChar c : value) {
		if (out.length() >= kMaxTagChars)
			break;
		const wxUint32 v = c.GetValue();
		if (v == 0x09 || (v > 0x1F && v != 0x7F && !(v >= 0x80 && v <= 0x9F)))
			out += c;
	}
	out.Trim(true).Trim(false);
	return out;
}

// A quoted helper string: only \n, \r, \\ and \" escapes; no raw quotes or control characters
// other than tab.
bool Unquote(const wxString &raw, wxString &out)
{
	if (raw.length() < 2 || raw[0] != wxT('"') || raw.Last() != wxT('"'))
		return false;
	out.clear();
	for (size_t i = 1; i + 1 < raw.length(); ++i) {
		wxUniChar c = raw[i];
		if (c == wxT('"') || (c < 32 && c != wxT('\t')) || c == 127)
			return false;
		if (c == wxT('\\')) {
			if (++i + 1 >= raw.length())
				return false;
			c = raw[i];
			if (c == wxT('n'))
				c = wxT('\n');
			else if (c == wxT('r'))
				c = wxT('\r');
			else if (c != wxT('\\') && c != wxT('"'))
				return false;
		}
		out += c;
	}
	return true;
}

// Digits only; 13 of them cover UINT32_MAX kbps in bits per second.
bool ParseNumber(const wxString &value, wxULongLong_t &out)
{
	if (value.empty() || value.length() > 13)
		return false;
	for (const wxUniChar c : value)
		if (c < wxT('0') || c > wxT('9'))
			return false;
	return value.ToULongLong(&out);
}

} // namespace

bool ParseHelperOutput(const wxArrayString &lines, MediaMetadata &out)
{
	if (lines.size() != 9 || lines[0] != wxT("AMMP 1") || lines[8] != wxT("END"))
		return false;
	static const wxChar *const keys[] = { wxT("format.duration="),
		wxT("format.bit_rate="),
		wxT("streams.stream.0.codec_type="),
		wxT("streams.stream.0.codec_name="),
		wxT("format.tags.artist="),
		wxT("format.tags.album="),
		wxT("format.tags.title=") };
	wxString values[7];
	for (size_t i = 0; i < 7; ++i)
		if (!lines[i + 1].StartsWith(keys[i], &values[i]))
			return false;
	wxULongLong_t seconds = 0, bitsPerSecond = 0;
	wxString type, codec, tags[3];
	if (!ParseNumber(values[0], seconds) || seconds > UINT32_MAX ||
		!ParseNumber(values[1], bitsPerSecond) || bitsPerSecond % 1000 ||
		bitsPerSecond / 1000 > UINT32_MAX || !Unquote(values[2], type) || type != wxT("audio") ||
		!Unquote(values[3], codec) || codec.length() > 32)
		return false;
	for (const wxUniChar c : codec)
		if (!((c >= wxT('a') && c <= wxT('z')) || (c >= wxT('0') && c <= wxT('9')) || c == wxT('_')))
			return false;
	for (size_t i = 0; i < 3; ++i)
		if (!Unquote(values[i + 4], tags[i]))
			return false;
	// A result with neither duration nor codec would clear the file's existing tags.
	if (!seconds && codec.empty())
		return false;
	out.length_seconds = static_cast<uint32>(seconds);
	out.bitrate_kbps = static_cast<uint32>(bitsPerSecond / 1000);
	out.codec = codec;
	out.artist = SanitiseTagValue(tags[0]);
	out.album = SanitiseTagValue(tags[1]);
	out.title = SanitiseTagValue(tags[2]);
	return true;
}

ProbeOutcome Probe(const wxString &helperPath,
	const CPath &file,
	MediaMetadata &out,
	unsigned timeoutMs,
	const std::atomic<bool> &keepRunning,
	bool bulk,
	bool logFailure)
{
	if (!keepRunning || !timeoutMs)
		return ProbeOutcome::Cancelled;
	if (helperPath.empty())
		return ProbeOutcome::Unavailable;
	if (!file.FileExists()) {
		AddDebugLogLineN(logMediaProbe,
			CFormat(wxT("MediaProbe: %s vanished before probing, skipping")) %
				file.GetPrintable());
		if (logFailure) {
			AddLogLineN(CFormat(_("Media metadata: %s is gone, nothing to extract")) %
				    file.GetPrintable());
		}
		return ProbeOutcome::Vanished;
	}
	if (!bulk)
		AddLogLineN(CFormat(_("Extracting media metadata: %s")) % file.GetPrintable());
	wxArrayString lines;
	MediaMetadata result;
	const int rc = RunBoundedHelper(helperPath, timeoutMs, keepRunning, lines, file);
	if (!keepRunning || rc == kKilled)
		return ProbeOutcome::Cancelled;
	if (rc == 0 && ParseHelperOutput(lines, result)) {
		out = result;
		return ProbeOutcome::Extracted;
	}
	if (logFailure)
		AddLogLineN(CFormat(_("Media metadata: parser failed for %s")) % file.GetPrintable());
	// Only documented parser rejections and a crash while parsing describe the file.
	// Loader failures, protocol errors and other exits must not poison known.met entries.
	if (rc == 10)
		return ProbeOutcome::NoUsableMetadata;
	if (rc == 11 || rc == kCrashed)
		return ProbeOutcome::UnreadableFile;
	return ProbeOutcome::Unavailable;
}

} // namespace MediaProbe
