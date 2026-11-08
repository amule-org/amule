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
#include "mediaprobe/sandbox.h"

#include "config.h" // Needed for AMULE_MEDIA_PROBE, AMULE_MEDIA_PROBE_RELPATH

#include <cstdint>
#include <cstdlib>
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
#endif

namespace MediaProbe
{

namespace
{

// Return values for RunBoundedHelper: a child exit code (>= 0), or one of:
constexpr int kSpawnFailed = -1; // unavailable helper, abnormal exit, malformed or oversized reply
constexpr int kKilled = -2;      // overran timeoutMs, or keepRunning went false
constexpr int kCrashed = -3;     // died abnormally after printing its header
// A policy unavailable on this machine cannot be fixed by trying another file.
// Keep the user's saved preference, but disable extraction until the next start.
std::atomic<bool> restrictionsAvailable{ true };

// Runs `exe` with `input` as stdin, capturing stdout to a temp file, bounded by `timeoutMs`
// wall-clock. The wait loop also polls `keepRunning`, so worker shutdown kills the child at once.
int RunBoundedHelper(const wxString &exe,
	unsigned timeoutMs,
	const std::atomic<bool> &keepRunning,
	wxArrayString &stdoutLines,
	const CPath &input,
	wxString &failureReason)
{
	failureReason = wxT("helper setup failed");
	if (!keepRunning || !timeoutMs) {
		failureReason = wxT("cancelled before launch");
		return kKilled;
	}
#ifndef __WXMSW__
	// A kernel I/O stall can delay even SIGKILL. Never block shutdown indefinitely
	// in waitpid, or accumulate more helpers while a previous one cannot be reaped.
	static std::mutex runnerMutex;
	static pid_t pendingChild = 0;
	const std::lock_guard<std::mutex> runnerLock(runnerMutex);
	if (pendingChild != 0) {
		const pid_t result = waitpid(pendingChild, nullptr, WNOHANG);
		if (result == 0 || (result < 0 && errno == EINTR)) {
			failureReason = wxT("previous helper is still terminating; refusing another probe");
			return kSpawnFailed;
		}
		pendingChild = 0;
	}
#endif
	const wxString tmpPath = wxFileName::CreateTempFileName(wxT("amule-mediaprobe"));
	if (tmpPath.IsEmpty()) {
		failureReason = wxT("cannot create metadata capture file");
		return kSpawnFailed;
	}

	// Diagnostics are separate from the strict metadata protocol. The file is
	// bounded while the child runs, and only sanitized text reaches the logger.
	struct Diagnostics
	{
		wxString path = wxFileName::CreateTempFileName(wxT("amule-mediaprobe-errors"));
		~Diagnostics()
		{
			if (!path.empty())
				wxRemoveFile(path);
		}
	} diagnostics;
	if (diagnostics.path.empty()) {
		wxRemoveFile(tmpPath);
		failureReason = wxT("cannot create diagnostic capture");
		return kSpawnFailed;
	}
	int exitCode = kSpawnFailed;
	const auto outputTooLarge = [&] {
		wxFFile output(tmpPath, wxT("rb"));
		wxFFile errors(diagnostics.path, wxT("rb"));
		return (output.IsOpened() && output.Length() > static_cast<wxFileOffset>(
								       probe::sandbox::kReplyLimitBytes)) ||
		       (errors.IsOpened() &&
			       errors.Length() >
				       static_cast<wxFileOffset>(probe::sandbox::kDiagnosticLimitBytes));
	};

#ifdef __WXMSW__
	const auto windowsFailure = [&](const wxString &stage, DWORD error) {
		failureReason = CFormat(wxT("%s failed (Windows error %u)")) % stage % error;
	};
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
		windowsFailure(wxT("open metadata capture"), ::GetLastError());
		wxRemoveFile(tmpPath);
		return kSpawnFailed;
	}

	// Outer job owns the trusted launcher and its single restricted parser.
	// The launcher puts the parser in a nested job with a one-process limit.
	HANDLE hJob = ::CreateJobObjectW(nullptr, nullptr);
	DWORD jobError = hJob == nullptr ? ::GetLastError() : ERROR_SUCCESS;
	if (hJob != nullptr) {
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
		::ZeroMemory(&jeli, sizeof(jeli));
		jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
							JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
							JOB_OBJECT_LIMIT_PROCESS_MEMORY;
		jeli.BasicLimitInformation.ActiveProcessLimit = 2;
		jeli.ProcessMemoryLimit = probe::sandbox::kMemoryLimitBytes;
		if (!::SetInformationJobObject(
			    hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
			jobError = ::GetLastError();
			::CloseHandle(hJob);
			hJob = nullptr;
		}
	}
	if (hJob == nullptr) {
		windowsFailure(wxT("configure helper Job limits"), jobError);
		::CloseHandle(hOut);
		wxRemoveFile(tmpPath);
		return probe::sandbox::kSetupFailed;
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
	const DWORD inputError = hIn == INVALID_HANDLE_VALUE ? ::GetLastError() : ERROR_SUCCESS;
	HANDLE hErr = ::CreateFileW(diagnostics.path.wc_str(),
		GENERIC_WRITE,
		FILE_SHARE_READ,
		&sa,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_TEMPORARY,
		nullptr);
	if (hIn == INVALID_HANDLE_VALUE || hErr == INVALID_HANDLE_VALUE) {
		windowsFailure(
			hIn == INVALID_HANDLE_VALUE ? wxT("open input file") : wxT("open diagnostic capture"),
			hIn == INVALID_HANDLE_VALUE ? inputError : ::GetLastError());
		if (hIn != INVALID_HANDLE_VALUE)
			::CloseHandle(hIn);
		if (hErr != INVALID_HANDLE_VALUE)
			::CloseHandle(hErr);
		::CloseHandle(hOut);
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
		windowsFailure(wxT("initialize inherited handle list"), ::GetLastError());
		::CloseHandle(hIn);
		::CloseHandle(hErr);
		::CloseHandle(hOut);
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
		windowsFailure(wxT("restrict inherited handles"), ::GetLastError());
		::DeleteProcThreadAttributeList(extended.lpAttributeList);
		::CloseHandle(hIn);
		::CloseHandle(hErr);
		::CloseHandle(hOut);
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
		DETACHED_PROCESS | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
		nullptr,
		nullptr,
		&si,
		&pi);
	const DWORD spawnError = ok ? ERROR_SUCCESS : ::GetLastError();
	::DeleteProcThreadAttributeList(extended.lpAttributeList);
	::CloseHandle(hIn);
	::CloseHandle(hErr);
	::CloseHandle(hOut);
	if (!ok) {
		failureReason = CFormat(wxT("CreateProcess failed (Windows error %u)")) % spawnError;
		::CloseHandle(hJob);
		wxRemoveFile(tmpPath);
		return kSpawnFailed;
	}
	// Reject the suspended launcher if either boundary cannot be established.
	// Running without the outer Job would weaken cancellation and resource limits.
	const bool confined = ::AssignProcessToJobObject(hJob, pi.hProcess) != FALSE;
	if (!confined || ::ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
		windowsFailure(confined ? wxT("resume confined helper") : wxT("confine suspended helper"),
			::GetLastError());
		::TerminateProcess(pi.hProcess, 1);
		::WaitForSingleObject(pi.hProcess, 5000);
		::CloseHandle(pi.hProcess);
		::CloseHandle(pi.hThread);
		::CloseHandle(hJob);
		wxRemoveFile(tmpPath);
		return confined ? kSpawnFailed : probe::sandbox::kSetupFailed;
	}

	wxStopWatch sw;
	for (;;) {
		const DWORD waitResult = ::WaitForSingleObject(pi.hProcess, 25);
		if (waitResult == WAIT_FAILED) {
			windowsFailure(wxT("wait for helper"), ::GetLastError());
			::TerminateJobObject(hJob, 1);
			::WaitForSingleObject(pi.hProcess, 5000);
			break;
		}
		if (waitResult == WAIT_OBJECT_0) {
			DWORD code = 0;
			if (::GetExitCodeProcess(pi.hProcess, &code)) {
				// Unhandled Windows exceptions use NTSTATUS codes with the high bit
				// set. Ordinary, undocumented error exits must remain retryable, just
				// as on POSIX, even if the helper already printed its header.
				exitCode = code >= 0x80000000UL ? kCrashed : static_cast<int>(code);
				failureReason = CFormat(wxT("helper exited with Windows status %u")) % code;
			} else {
				windowsFailure(wxT("read helper exit status"), ::GetLastError());
			}
			break;
		}
		const bool oversized = outputTooLarge();
		if (!keepRunning || static_cast<unsigned>(sw.Time()) >= timeoutMs || oversized) {
			::TerminateJobObject(hJob, 1);
			::WaitForSingleObject(pi.hProcess, 5000);
			failureReason = oversized ? wxT("metadata or diagnostics exceeded output limit")
						  : (!keepRunning ? wxT("cancelled while helper was running")
								  : wxT("helper deadline exceeded"));
			exitCode = oversized ? kSpawnFailed : kKilled;
			break;
		}
	}
	::CloseHandle(pi.hProcess);
	::CloseHandle(pi.hThread);
	::CloseHandle(hJob);
#else
	const std::string exeNative(exe.fn_str());
	char *const cargv[] = { const_cast<char *>(exeNative.c_str()), nullptr };

	// Preserve bundle loader paths without copying unrelated secrets from the
	// core's environment into a process that will parse attacker-controlled data.
	std::vector<std::string> environment{ "LC_ALL=C", "PATH=/usr/bin:/bin" };
	for (const char *name : { "LD_LIBRARY_PATH",
		     "DYLD_LIBRARY_PATH",
		     "DYLD_FALLBACK_LIBRARY_PATH",
		     "APPDIR",
		     "APPIMAGE" }) {
		if (const char *value = std::getenv(name))
			environment.emplace_back(std::string(name) + '=' + value);
	}
	std::vector<char *> envp;
	for (auto &value : environment)
		envp.push_back(value.data());
	envp.push_back(nullptr);

	const std::string tmpNative(tmpPath.fn_str());

	posix_spawn_file_actions_t fa;
	const int actionsError = posix_spawn_file_actions_init(&fa);
	if (actionsError != 0) {
		failureReason = CFormat(wxT("cannot initialize file actions (error %d)")) % actionsError;
		wxRemoveFile(tmpPath);
		return kSpawnFailed;
	}
	const int inputFd = open(input.GetRaw().fn_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	struct stat inputStat;
	if (inputFd < 0 || fstat(inputFd, &inputStat) != 0 || !S_ISREG(inputStat.st_mode)) {
		failureReason = CFormat(wxT("cannot open a regular input file (errno %d)")) % errno;
		if (inputFd >= 0)
			close(inputFd);
		posix_spawn_file_actions_destroy(&fa);
		wxRemoveFile(tmpPath);
		return kSpawnFailed;
	}
	int setupError = posix_spawn_file_actions_adddup2(&fa, inputFd, STDIN_FILENO);
	const auto rememberError = [&](int error) {
		if (setupError == 0)
			setupError = error;
	};
	if (inputFd != STDIN_FILENO)
		rememberError(posix_spawn_file_actions_addclose(&fa, inputFd));
	else if (fcntl(inputFd, F_SETFD, 0) != 0)
		rememberError(errno);
	// Capture stdout and diagnostics separately; only stdout is parsed as metadata.
	rememberError(posix_spawn_file_actions_addopen(
		&fa, STDOUT_FILENO, tmpNative.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600));
	rememberError(posix_spawn_file_actions_addopen(
		&fa, STDERR_FILENO, diagnostics.path.fn_str(), O_WRONLY | O_TRUNC, 0600));

	// Own process group, so the kill below (-pid) also takes any child it started.
	posix_spawnattr_t attr;
	const int attrError = posix_spawnattr_init(&attr);
	rememberError(attrError);
	if (attrError == 0) {
		rememberError(posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP));
		rememberError(posix_spawnattr_setpgroup(&attr, 0));
	}
	if (setupError != 0) {
		failureReason =
			CFormat(wxT("cannot configure confined helper launch (error %d)")) % setupError;
		close(inputFd);
		if (attrError == 0)
			posix_spawnattr_destroy(&attr);
		posix_spawn_file_actions_destroy(&fa);
		wxRemoveFile(tmpPath);
		return kSpawnFailed;
	}

	pid_t pid = 0;
	const int rc = posix_spawn(&pid, cargv[0], &fa, &attr, cargv, envp.data());
	close(inputFd);
	posix_spawnattr_destroy(&attr);
	posix_spawn_file_actions_destroy(&fa);
	if (rc != 0) {
		failureReason = CFormat(wxT("posix_spawn failed (error %d)")) % rc;
		wxRemoveFile(tmpPath);
		return kSpawnFailed;
	}

	wxStopWatch sw;
	for (;;) {
		int status = 0;
		const pid_t r = waitpid(pid, &status, WNOHANG);
		if (r == pid) {
			exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : kCrashed;
			failureReason =
				WIFSIGNALED(status)
					? CFormat(wxT("helper terminated by signal %d")) % WTERMSIG(status)
					: CFormat(wxT("helper exited with status %d")) % exitCode;
			break;
		}
		if (r == -1 && errno == EINTR) {
			continue;
		}
		if (r == -1) { // vanished / already reaped
			failureReason = CFormat(wxT("waitpid failed (errno %d)")) % errno;
			exitCode = kSpawnFailed;
			break;
		}
		const bool oversized = outputTooLarge();
		if (!keepRunning || static_cast<unsigned>(sw.Time()) >= timeoutMs || oversized) {
			kill(-pid, SIGKILL);
			bool reaped = false;
			for (unsigned attempt = 0; attempt < 100; ++attempt) {
				const pid_t result = waitpid(pid, &status, WNOHANG);
				if (result == pid || (result < 0 && errno == ECHILD)) {
					reaped = true;
					break;
				}
				wxMilliSleep(10);
			}
			if (!reaped)
				pendingChild = pid;
			failureReason = oversized ? wxT("metadata or diagnostics exceeded output limit")
						  : (!keepRunning ? wxT("cancelled while helper was running")
								  : wxT("helper deadline exceeded"));
			exitCode = oversized ? kSpawnFailed : kKilled;
			if (!reaped)
				failureReason += wxT("; termination pending; further probes suspended");
			break;
		}
		wxMilliSleep(25);
	}
#endif

	// A legitimate reply is a few hundred bytes; past 64 KiB it is a failed probe. A fast exit
	// can beat the polling check, so check again before reading any exit as a verdict.
	if (outputTooLarge()) {
		AddDebugLogLineN(logMediaProbe,
			wxT("MediaProbe: metadata or diagnostics output limit exceeded, ignoring"));
		exitCode = kSpawnFailed;
		failureReason = wxT("metadata or diagnostics exceeded output limit");
	}
	// A crash before the header is a broken install; after it, the file caused it.
	if (exitCode == kCrashed) {
		wxFFile f(tmpPath, wxT("rb"));
		char header[7] = {};
		if (!f.IsOpened() || f.Read(header, sizeof(header)) != sizeof(header) ||
			std::memcmp(header, "AMMP 1\n", sizeof(header)) != 0) {
			exitCode = kSpawnFailed;
			failureReason += wxT("; failed before parser header");
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
				failureReason = wxT("invalid metadata protocol framing");
			} else {
				content.RemoveLast();
				wxStringTokenizer tok(content, wxT("\n"), wxTOKEN_RET_EMPTY_ALL);
				while (tok.HasMoreTokens()) {
					stdoutLines.Add(tok.GetNextToken());
				}
			}
		} else {
			exitCode = kSpawnFailed;
			failureReason = wxT("cannot read helper reply as UTF-8");
		}
	}
	if (exitCode != 0) {
		wxFFile errors(diagnostics.path, wxT("rb"));
		if (errors.IsOpened()) {
			char raw[probe::sandbox::kLoggedDiagnosticLimitBytes];
			const size_t count = errors.Read(raw, sizeof(raw));
			wxString safe;
			for (size_t i = 0; i < count; ++i) {
				const unsigned char c = static_cast<unsigned char>(raw[i]);
				safe += c >= 32 && c < 127 ? wxChar(c) : wxChar(' ');
			}
			if (!safe.empty())
				failureReason += wxT("; diagnostic: ") + safe;
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
	if (!restrictionsAvailable)
		return wxString();
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
	if (!restrictionsAvailable)
		return ProbeOutcome::Unavailable;
	const auto logFailureReason = [&](const wxString &reason) {
		const wxString message =
			CFormat(_("Media metadata: no result for %s: %s")) % file.GetPrintable() % reason;
		if (logFailure)
			AddLogLineN(message);
		else
			AddDebugLogLineN(logMediaProbe, message);
	};
	if (!keepRunning || !timeoutMs) {
		logFailureReason(wxT("cancelled before launch"));
		return ProbeOutcome::Cancelled;
	}
	if (helperPath.empty()) {
		logFailureReason(wxT("helper unavailable"));
		return ProbeOutcome::Unavailable;
	}
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
	wxString reason;
	const int rc = RunBoundedHelper(helperPath, timeoutMs, keepRunning, lines, file, reason);
	if (!keepRunning || rc == kKilled) {
		logFailureReason(reason);
		return ProbeOutcome::Cancelled;
	}
	if (rc == 0 && ParseHelperOutput(lines, result)) {
		out = result;
		return ProbeOutcome::Extracted;
	}
	if (rc == 0)
		reason = wxT("invalid or incomplete metadata reply; discarding all fields");
	if (rc == probe::sandbox::kSetupFailed) {
		if (restrictionsAvailable.exchange(false))
			AddLogLineN(
				CFormat(_("Media metadata extraction disabled until restart: %s")) % reason);
	} else {
		logFailureReason(reason);
	}
	// Only documented parser rejections and a crash while parsing describe the file.
	// Loader failures, protocol errors and other exits must not poison known.met entries.
	if (rc == 10)
		return ProbeOutcome::NoUsableMetadata;
	if (rc == 11 || rc == kCrashed)
		return ProbeOutcome::UnreadableFile;
	return ProbeOutcome::Unavailable;
}

} // namespace MediaProbe
