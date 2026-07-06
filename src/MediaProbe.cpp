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

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include <wx/arrstr.h>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/stopwatch.h>
#include <wx/tokenzr.h>
#include <wx/utils.h> // Needed for wxExecute (AutoDetectPath) / wxMilliSleep

#include <common/Format.h>

#include "Logger.h"
#include "libs/common/Path.h"

// Native child-process primitives for the bounded, killable probe runner.
// wxExecute/wxProcess async bookkeeping is bound to the main-thread event
// loop (its SIGCHLD reaper fires wxProcess::OnTerminate there), so polling
// it from the probe worker races that loop — a use-after-free. Managing
// ffprobe with native primitives keeps the whole lifecycle on this thread.
#ifdef __WXMSW__
#include <windows.h>
#else
#include <csignal>
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

// One-shot silent invocation. Returns true if `binary` runs cleanly
// enough to print its own -version output. Used both as the "is on
// PATH" probe (binary = "ffprobe") and as the "does this path work"
// probe (binary = a resolved absolute path).
bool CanRun(const wxString &binary)
{
	wxArrayString out, err;
	// wxEXEC_NODISABLE / wxEXEC_NOEVENTS mirror the pattern in
	// AppImageIntegration.cpp — we never want the wait to spin the
	// event loop or grey out top-level windows.
	const long rc = wxExecute(
		binary + wxT(" -version"), out, err, wxEXEC_SYNC | wxEXEC_NODISABLE | wxEXEC_NOEVENTS);
	// wxExecute returns the child's exit code on success or -1 if it
	// couldn't spawn (typical: file not found on Windows CreateProcess,
	// or ENOENT after fork+exec on POSIX).
	return rc == 0;
}

// Platform-specific well-known install locations, tried in order.
// Only one entry per install-manager: we're looking for the first
// existing binary, not enumerating every possible location. Order
// matters — ARM64 Homebrew (`/opt/homebrew`) comes before Intel
// (`/usr/local`) because a bare `ffprobe` PATH lookup on Apple
// Silicon usually finds the ARM64 one first anyway.
wxArrayString WellKnownPaths()
{
	wxArrayString paths;
#if defined(__WXMAC__)
	paths.Add(wxT("/opt/homebrew/bin/ffprobe"));
	paths.Add(wxT("/usr/local/bin/ffprobe"));
	paths.Add(wxT("/opt/local/bin/ffprobe")); // MacPorts
#elif defined(__WXMSW__)
	// Common Windows package-manager install roots. WinGet's per-app
	// dir includes the package version so we can't hardcode a leaf;
	// probing via `where.exe` (which CanRun("ffprobe") uses under the
	// hood) is the reliable path for WinGet users. Chocolatey +
	// scoop have stable predictable roots.
	paths.Add(wxT("C:\\ffmpeg\\bin\\ffprobe.exe"));
	paths.Add(wxT("C:\\ProgramData\\chocolatey\\bin\\ffprobe.exe"));
	if (const wxChar *home = wxGetenv(wxT("USERPROFILE"))) {
		paths.Add(wxString(home) + wxT("\\scoop\\apps\\ffmpeg\\current\\bin\\ffprobe.exe"));
	}
#else
	// Linux + OpenBSD share the same handful of standard prefixes.
	// Snap and Flatpak users typically launch ffprobe out of their
	// sandbox root (`/snap/bin/ffprobe`, or a flatpak-run wrapper);
	// we cover the snap case explicitly and let flatpak users point
	// the preference at their wrapper manually if they hit it.
	paths.Add(wxT("/usr/bin/ffprobe"));
	paths.Add(wxT("/usr/local/bin/ffprobe"));
	paths.Add(wxT("/snap/bin/ffprobe"));
#endif
	return paths;
}

} // anonymous namespace

wxString AutoDetectPath()
{
	// Fast path: unadorned `ffprobe` on the shell PATH. This is what
	// most Linux + BSD installs give us for free (package-installed
	// binaries land in a PATH dir). On macOS + Windows this often
	// fails even when ffprobe IS installed, because GUI-launched
	// processes get a minimal PATH (launchd default on macOS lacks
	// /opt/homebrew; Windows GUI apps sometimes miss chocolatey /
	// scoop until reboot).
	if (CanRun(wxT("ffprobe"))) {
		return wxT("ffprobe");
	}

	// Fallback: probe the per-platform well-known list.
	for (const wxString &candidate : WellKnownPaths()) {
		if (wxFileName::FileExists(candidate) && CanRun(candidate)) {
			return candidate;
		}
	}

	return wxEmptyString;
}

namespace
{

// ffprobe emits float durations with locale-independent `.` decimal
// separators, so a plain strtod suffices — no wxString::ToDouble()
// with its locale sensitivity here.
bool ParseSeconds(const wxString &value, uint32 &out)
{
	if (value.IsEmpty()) {
		return false;
	}
	char *end = nullptr;
	const double d = std::strtod(value.utf8_str().data(), &end);
	if (end == value.utf8_str().data() || d < 0.0) {
		return false;
	}
	// Cap at uint32 range (~136 years — plenty).
	if (d > static_cast<double>(0xFFFFFFFFu)) {
		out = 0xFFFFFFFFu;
	} else {
		// Round to nearest whole second; sub-second precision has no
		// consumer in the FT_MEDIA_LENGTH tag.
		out = static_cast<uint32>(std::llround(d));
	}
	return true;
}

// ffprobe emits format.bit_rate as bits/second; the tag wire format
// is kbps.
bool ParseBitrateKbps(const wxString &value, uint32 &out)
{
	if (value.IsEmpty() || value == wxT("N/A")) {
		return false;
	}
	char *end = nullptr;
	const unsigned long long bps = std::strtoull(value.utf8_str().data(), &end, 10);
	if (end == value.utf8_str().data()) {
		return false;
	}
	const unsigned long long kbps = bps / 1000ULL;
	if (kbps > 0xFFFFFFFFULL) {
		out = 0xFFFFFFFFu;
	} else {
		out = static_cast<uint32>(kbps);
	}
	return true;
}

} // anonymous namespace

namespace
{

// Return values for RunBoundedFFProbe: a child exit code (>= 0), or one of:
constexpr int kSpawnFailed = -1; // couldn't launch the binary at all
constexpr int kKilled = -2;      // overran timeoutMs, or keepRunning went false

// Spawn `exe argv...`, capturing stdout to a temp file, and wait for it with
// a `timeoutMs` wall-clock bound. The wait loop also polls `keepRunning`; when
// it flips false (worker shutdown) the child is killed at once so the caller's
// thread-join can return. Args are passed as a real argv vector — no shell —
// so paths with spaces / quotes need no escaping. See the include-block note
// for why this bypasses wxExecute/wxProcess entirely.
int RunBoundedFFProbe(const wxString &exe,
	const wxArrayString &argv,
	unsigned timeoutMs,
	const std::atomic<bool> &keepRunning,
	wxArrayString &stdoutLines)
{
	const wxString tmpPath = wxFileName::CreateTempFileName(wxT("amule-mediaprobe"));
	if (tmpPath.IsEmpty()) {
		return kSpawnFailed;
	}

	int exitCode = kSpawnFailed;

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

	// Job object with kill-on-close so terminating (or closing) it takes down
	// the whole process tree — the Windows equivalent of the POSIX
	// process-group kill. ffprobe.exe forks nothing, but a wrapper-style path
	// (rare on Windows) would; a plain TerminateProcess would orphan it.
	HANDLE hJob = ::CreateJobObjectW(nullptr, nullptr);
	if (hJob != nullptr) {
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
		::ZeroMemory(&jeli, sizeof(jeli));
		jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		::SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
	}

	// Quote each token for the single command-line string CreateProcess wants.
	wxString cmdLine = wxT("\"") + exe + wxT("\"");
	for (const wxString &a : argv) {
		cmdLine += wxT(" \"") + a + wxT("\"");
	}
	std::vector<wchar_t> cmdBuf(cmdLine.wc_str(), cmdLine.wc_str() + cmdLine.length() + 1);

	STARTUPINFOW si;
	::ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
	si.hStdOutput = hOut;
	si.hStdError = hOut;

	PROCESS_INFORMATION pi;
	::ZeroMemory(&pi, sizeof(pi));

	// Start suspended so the child is in the job before it runs (and thus
	// before it can spawn a grandchild that escapes the job), then resume.
	const BOOL ok = ::CreateProcessW(nullptr,
		cmdBuf.data(),
		nullptr,
		nullptr,
		TRUE,
		CREATE_NO_WINDOW | CREATE_SUSPENDED,
		nullptr,
		nullptr,
		&si,
		&pi);
	::CloseHandle(hOut);
	if (!ok) {
		if (hJob != nullptr) {
			::CloseHandle(hJob);
		}
		wxRemoveFile(tmpPath);
		return kSpawnFailed;
	}
	if (hJob != nullptr) {
		::AssignProcessToJobObject(hJob, pi.hProcess);
	}
	::ResumeThread(pi.hThread);

	wxStopWatch sw;
	for (;;) {
		if (::WaitForSingleObject(pi.hProcess, 25) == WAIT_OBJECT_0) {
			DWORD code = 0;
			::GetExitCodeProcess(pi.hProcess, &code);
			exitCode = static_cast<int>(code);
			break;
		}
		if (!keepRunning || static_cast<unsigned>(sw.Time()) >= timeoutMs) {
			// Kill the whole job (wrapper + real ffprobe), mirroring the
			// POSIX process-group kill; fall back to the bare process if the
			// job could not be created.
			if (hJob != nullptr) {
				::TerminateJobObject(hJob, 1);
			} else {
				::TerminateProcess(pi.hProcess, 1);
			}
			::WaitForSingleObject(pi.hProcess, INFINITE);
			exitCode = kKilled;
			break;
		}
	}
	::CloseHandle(pi.hProcess);
	::CloseHandle(pi.hThread);
	if (hJob != nullptr) {
		::CloseHandle(hJob);
	}
#else
	std::vector<std::string> storage;
	storage.reserve(argv.GetCount() + 1);
	storage.emplace_back(exe.fn_str());
	for (const wxString &a : argv) {
		storage.emplace_back(a.fn_str());
	}
	std::vector<char *> cargv;
	cargv.reserve(storage.size() + 1);
	for (std::string &s : storage) {
		cargv.push_back(const_cast<char *>(s.c_str()));
	}
	cargv.push_back(nullptr);

	const std::string tmpNative(tmpPath.fn_str());

	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	// stdout -> temp file, stderr -> /dev/null (we parse stdout only).
	posix_spawn_file_actions_addopen(
		&fa, STDOUT_FILENO, tmpNative.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
	posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

	// Put the child in its own process group so a kill can take out the
	// whole tree. ffprobe itself forks nothing, but a wrapper-style path
	// (snap's /snap/bin/ffprobe, a flatpak-run shim) is a shell that execs
	// the real binary as a child — killing only the wrapper would orphan
	// it. setpgroup(0) makes the child a group leader (pgid == pid); the
	// kill path below signals -pid to hit the group.
	posix_spawnattr_t attr;
	posix_spawnattr_init(&attr);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
	posix_spawnattr_setpgroup(&attr, 0);

	pid_t pid = 0;
	const int rc = posix_spawnp(&pid, cargv[0], &fa, &attr, cargv.data(), environ);
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
			exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : kKilled;
			break;
		}
		if (r == -1) { // vanished / already reaped
			exitCode = kKilled;
			break;
		}
		if (!keepRunning || static_cast<unsigned>(sw.Time()) >= timeoutMs) {
			kill(-pid, SIGKILL);      // whole group: wrapper + real ffprobe
			waitpid(pid, &status, 0); // reap the leader; init reaps the rest
			exitCode = kKilled;
			break;
		}
		wxMilliSleep(25);
	}
#endif

	// Slurp captured stdout. On a kill the file is partial/empty, but the
	// caller treats kKilled as failure and never reads stdoutLines then.
	if (exitCode >= 0) {
		wxFFile f(tmpPath, wxT("rb"));
		wxString content;
		if (f.IsOpened() && f.ReadAll(&content, wxConvUTF8)) {
			wxStringTokenizer tok(content, wxT("\r\n"), wxTOKEN_STRTOK);
			while (tok.HasMoreTokens()) {
				stdoutLines.Add(tok.GetNextToken());
			}
		}
	}
	wxRemoveFile(tmpPath);
	return exitCode;
}

} // anonymous namespace

bool Probe(const wxString &ffprobePath,
	const CPath &file,
	MediaInfo &out,
	unsigned timeoutMs,
	const std::atomic<bool> &keepRunning)
{
	if (ffprobePath.IsEmpty()) {
		return false;
	}

	// -show_entries constrains the output to what we care about: each stream's
	// codec_name plus its codec_type (so we can prefer the video track's codec,
	// then the audio track's, over subtitle / data streams), and the format-
	// level duration + bit_rate. -of default=nk=0:nw=1 emits one bare
	// "key=value" per line, no INI section headers, no line wrapping; ffprobe
	// prints the stream fields in the requested order, so each stream is a
	// codec_name line immediately followed by its codec_type line. -v error
	// silences informational chatter. Tokens are passed as a real argv (no
	// shell), so the file path needs no quoting/escaping.
	wxArrayString argv;
	argv.Add(wxT("-v"));
	argv.Add(wxT("error"));
	argv.Add(wxT("-show_entries"));
	argv.Add(wxT("format=duration,bit_rate:stream=codec_name,codec_type"));
	argv.Add(wxT("-of"));
	argv.Add(wxT("default=nk=0:nw=1"));
	argv.Add(file.GetRaw());

	// Traced under the dedicated logMediaProbe category so a subsystem that
	// silently shells out to ffprobe is diagnosable when the category is on.
	AddDebugLogLineN(logMediaProbe, CFormat(wxT("MediaProbe: probing %s")) % file.GetPrintable());

	// Bounded + killable: this runs on the dedicated CMediaProbeThread, so a
	// slow/hung ffprobe can only ever delay other probes — never completions.
	// The timeout and keepRunning cancel also stop a stuck child from wedging
	// the worker itself or the shutdown join.
	wxArrayString stdout_lines;
	const int rc = RunBoundedFFProbe(ffprobePath, argv, timeoutMs, keepRunning, stdout_lines);
	if (rc == kKilled) {
		AddDebugLogLineN(logMediaProbe,
			CFormat(wxT("MediaProbe: ffprobe timed out / cancelled for %s")) %
				file.GetPrintable());
		return false;
	}
	if (rc != 0) {
		AddDebugLogLineN(logMediaProbe,
			CFormat(wxT("MediaProbe: ffprobe failed (rc=%d) for %s")) % rc % file.GetPrintable());
		return false;
	}

	MediaInfo info;
	bool got_duration = false;
	// Codec selection: the first video track's codec, else the first audio
	// track's. Subtitle / data streams (e.g. a leading subrip track in an
	// mkv) never win, so we don't advertise "subrip" as a file's codec.
	wxString videoCodec, audioCodec, pendingCodec;
	for (const wxString &line : stdout_lines) {
		// Split on the first '=' only — codec_name values themselves
		// don't contain '=' but the parser mustn't assume that.
		const int eq = line.Find(wxT('='));
		if (eq == wxNOT_FOUND) {
			continue;
		}
		const wxString key = line.Mid(0, eq);
		const wxString value = line.Mid(eq + 1);
		if (key == wxT("duration")) {
			got_duration = ParseSeconds(value, info.length_seconds);
		} else if (key == wxT("bit_rate")) {
			(void)ParseBitrateKbps(value, info.bitrate_kbps);
		} else if (key == wxT("codec_name")) {
			// Held until this stream's codec_type line arrives next.
			pendingCodec = value;
		} else if (key == wxT("codec_type")) {
			if (value == wxT("video") && videoCodec.IsEmpty()) {
				videoCodec = pendingCodec;
			} else if (value == wxT("audio") && audioCodec.IsEmpty()) {
				audioCodec = pendingCodec;
			}
			pendingCodec.clear();
		}
	}
	if (!videoCodec.IsEmpty()) {
		info.codec = videoCodec;
	} else if (!audioCodec.IsEmpty()) {
		info.codec = audioCodec;
	}
	const bool got_codec = !info.codec.IsEmpty();

	// A file with zero streams / zero duration produces an all-blank
	// MediaInfo which is meaningless to advertise — treat it as a
	// probe failure so the caller doesn't attach empty tags.
	if (!got_duration && !got_codec) {
		AddDebugLogLineN(logMediaProbe,
			CFormat(wxT("MediaProbe: no metadata parsed for %s")) % file.GetPrintable());
		return false;
	}

	AddDebugLogLineN(logMediaProbe,
		CFormat(wxT("MediaProbe: extracted %s -> length=%us bitrate=%ukbps codec=%s")) %
			file.GetPrintable() % info.length_seconds % info.bitrate_kbps % info.codec);
	out = info;
	return true;
}

} // namespace MediaProbe
