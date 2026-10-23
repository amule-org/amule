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

#include <map>
#include <cmath>
#include <cstdlib>
#include <cstdint>
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
constexpr int kSpawnFailed = -1;    // unavailable helper, abnormal exit or malformed response
constexpr int kKilled = -2;         // overran timeoutMs, or keepRunning went false
constexpr int kOutputTooLarge = -3; // reply exceeded the read budget; see the slurp

// Spawn `exe argv...`, capturing stdout to a temp file, bounded by `timeoutMs` wall-clock. The wait
// loop also polls `keepRunning`, so worker shutdown kills the child at once. Real argv, no shell,
// so paths need no escaping. See the include-block note for why this bypasses wxExecute/wxProcess
// entirely.
int RunBoundedHelper(const wxString &exe,
	const wxArrayString &argv,
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
	const wxString program = exe;
	if (program.IsEmpty()) {
		wxRemoveFile(tmpPath);
		return kSpawnFailed;
	}

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

	// Kill-on-close job so terminating it takes down the whole process tree, the Windows
	// equivalent of the POSIX process-group kill. ffprobe.exe forks nothing, but a wrapper-
	// style path would, and TerminateProcess would orphan it.
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

	// Quote each token for the single command-line string CreateProcess wants.
	wxString cmdLine = wxT("\"") + program + wxT("\"");
	for (const wxString &a : argv) {
		cmdLine += wxT(" \"") + a + wxT("\"");
	}
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
			::GetExitCodeProcess(pi.hProcess, &code);
			exitCode = static_cast<int>(code);
			break;
		}
		const bool oversized = outputTooLarge();
		if (!keepRunning || static_cast<unsigned>(sw.Time()) >= timeoutMs || oversized) {
			// Kill the whole job (wrapper + real ffprobe); fall back to the bare
			// process if the job could not be created.
			if (hJob != nullptr) {
				::TerminateJobObject(hJob, 1);
			} else {
				::TerminateProcess(pi.hProcess, 1);
			}
			::WaitForSingleObject(pi.hProcess, INFINITE);
			exitCode = oversized ? kOutputTooLarge : kKilled;
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

	// Own process group so a kill takes out the whole tree: ffprobe forks nothing, but a
	// wrapper-style path (snap's /snap/bin/ffprobe, a flatpak-run shim) is a shell that execs
	// the real binary as a child. setpgroup(0) makes the child a group leader, and the kill
	// path below signals -pid.
	posix_spawnattr_t attr;
	posix_spawnattr_init(&attr);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
	posix_spawnattr_setpgroup(&attr, 0);

	pid_t pid = 0;
	const int rc = posix_spawn(&pid, cargv[0], &fa, &attr, cargv.data(), envp);
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
			exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : kSpawnFailed;
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
			kill(-pid, SIGKILL); // whole group: wrapper + real ffprobe
			while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
			} // reap the leader; init reaps the rest
			exitCode = oversized ? kOutputTooLarge : kKilled;
			break;
		}
		wxMilliSleep(25);
	}
#endif

	// Slurp captured stdout. On a kill the file is partial or empty, but the caller treats
	// kKilled as failure and never reads stdoutLines then.
	//
	// Bounded, because container tags are attacker-chosen text of arbitrary length: uncapped, a
	// crafted 100 MB title is slurped whole into a wxString and copied again by the tokenizer
	// and the unescaper before kMaxTagChars ever applies. A legitimate five-field reply is a
	// few hundred bytes, and anything past the cap is treated as a failed probe rather than
	// truncated.
	if (exitCode == 0) {
		wxFFile f(tmpPath, wxT("rb"));
		wxString content;
		if (f.IsOpened() && f.Length() > kMaxProbeOutputBytes) {
			AddDebugLogLineN(logMediaProbe,
				CFormat(wxT("MediaProbe: helper output too large (%lld bytes), ignoring")) %
					static_cast<long long>(f.Length()));
			// Its own sentinel, not kSpawnFailed: ffprobe ran fine, so
			// reporting "failed (code -1)" would name the wrong thing.
			exitCode = kOutputTooLarge;
		} else if (f.IsOpened() && f.ReadAll(&content, wxConvUTF8)) {
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

wxString AutoDetectPath()
{
	wxFileName binary(wxStandardPaths::Get().GetExecutablePath());
#ifdef __WXMSW__
	binary.SetFullName(wxT("amule-media-probe.exe"));
#else
	binary.SetFullName(wxT("amule-media-probe"));
#endif
	return binary.FileExists() ? binary.GetFullPath() : wxString();
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
		if (cached.empty())
			AddLogLineN(_("Media metadata: bundled amule-media-probe helper is unavailable."));
	}
	return cached;
}

namespace
{

// ffprobe emits float durations with a locale-independent `.` separator, so a
// plain strtod suffices -- no locale-sensitive wxString::ToDouble() here.
bool ParseSeconds(const wxString &value, uint32 &out)
{
	if (value.IsEmpty()) {
		return false;
	}
	// Named local: `value.utf8_str().data()` twice would be two separate temporaries, each dead
	// at the end of its own full-expression, so the comparison below would read a freed pointer
	// and compare it against one from a different object -- which happens to work only because
	// the allocator hands back the same block.
	const wxScopedCharBuffer buf = value.utf8_str();
	const char *const str = buf.data();
	char *end = nullptr;
	const double d = std::strtod(str, &end);
	if (end == str || d < 0.0) {
		return false;
	}
	// Cap at uint32 range (~136 years -- plenty).
	if (d > static_cast<double>(0xFFFFFFFFu)) {
		out = 0xFFFFFFFFu;
	} else {
		// Sub-second precision has no consumer in the FT_MEDIA_LENGTH tag.
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
	// Named local, for the reason given in ParseSeconds above.
	const wxScopedCharBuffer buf = value.utf8_str();
	const char *const str = buf.data();
	char *end = nullptr;
	const unsigned long long bps = std::strtoull(str, &end, 10);
	if (end == str) {
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

const wxChar *ProbeEntries()
{
	// Per stream: codec_name and codec_type (so a video track's codec beats an audio one and
	// subtitle/data streams never win), attached_pic, and its own artist/title/album. Per
	// format: duration, bit_rate and the same three tags.
	return wxT("format=duration,bit_rate"
		   ":format_tags=artist,title,album"
		   ":stream=codec_name,codec_type"
		   ":stream_disposition=attached_pic"
		   ":stream_tags=artist,title,album");
}

namespace
{

// One stream, accumulated from the `streams.stream.<n>.*` keys.
struct ProbeStream
{
	wxString codec;
	wxString type;
	bool attached_pic = false;
	wxString artist, album, title;
};

// Unwrap a `flat` value: quoted, with \n \r \\ and \" escaped. Anything
// unquoted (ffprobe emits bare integers for dispositions) is returned as-is.
wxString UnflattenValue(const wxString &raw)
{
	if (raw.length() < 2 || raw[0] != wxT('"') || raw.Last() != wxT('"')) {
		return raw;
	}
	const wxString body = raw.Mid(1, raw.length() - 2);
	wxString out;
	out.reserve(body.length());
	for (size_t i = 0; i < body.length(); ++i) {
		if (body[i] != wxT('\\') || i + 1 >= body.length()) {
			out += body[i];
			continue;
		}
		switch (body[++i].GetValue()) {
		case wxT('n'):
			out += wxT('\n');
			break;
		case wxT('r'):
			out += wxT('\r');
			break;
		case wxT('\\'):
			out += wxT('\\');
			break;
		case wxT('"'):
			out += wxT('"');
			break;
		default:
			// Not an escape ffprobe produces; keep both characters rather
			// than silently eating the backslash.
			out += wxT('\\');
			out += body[i];
			break;
		}
	}
	return out;
}

// Longest tag value we keep. Generous for a real artist/album/title; the point is to bound what a
// crafted file can make this node store and publish, not to fit any real metadata. These values go
// into known.met, into the log line, and into every offered-file packet sent to every server and
// client.
const size_t kMaxTagChars = 256;

// Clean one tag value before it is allowed any further: bound the length and drop control
// characters.
//
// The cap is what keeps oversized text out of known.met, out of the log line and off the wire;
// without it the only bound anywhere was the wire format's 0xFFFF truncation, a packet-integrity
// guard rather than a policy. How much is READ is bounded separately, at the slurp in
// RunBoundedHelper.
//
// Control characters are dropped because the value reaches a log line (and through it GET
// /api/v1/logs/amule) and several list controls, where a raw newline lets one field impersonate
// several.
wxString SanitiseTagValue(const wxString &value)
{
	wxString out;
	out.reserve(value.length() < kMaxTagChars ? value.length() : kMaxTagChars);
	for (const wxUniChar c : value) {
		if (out.length() >= kMaxTagChars) {
			break;
		}
		// Keep printable characters and ordinary spaces; drop C0/C1 controls
		// and DEL. Non-ASCII is kept as-is -- these are UTF-8 tags.
		const wxUint32 v = c.GetValue();
		if (v == 0x09 || v == 0x20 || (v > 0x1F && v != 0x7F && !(v >= 0x80 && v <= 0x9F))) {
			out += c;
		}
	}
	out.Trim(true).Trim(false);
	return out;
}

// ffprobe prints the container's own key case -- Matroska yields format.tags.ARTIST and
// format.tags.ALBUM beside a lower-case format.tags.title, in one file -- while matching the
// requested names case-insensitively. So the parser has to as well.
void AssignTag(const wxString &key, const wxString &value, wxString &artist, wxString &album, wxString &title)
{
	const wxString lower = key.Lower();
	// Sanitised here rather than at each call site: this is the one door every
	// container tag comes through.
	if (lower == wxT("artist")) {
		artist = SanitiseTagValue(value);
	} else if (lower == wxT("album")) {
		album = SanitiseTagValue(value);
	} else if (lower == wxT("title")) {
		title = SanitiseTagValue(value);
	}
}

} // namespace

bool ParseProbeOutput(const wxArrayString &lines, MediaInfo &out)
{
	MediaInfo info;
	bool got_duration = false;

	// Keyed by the stream index ffprobe puts in the key, so ordering comes from the data rather
	// than from the order lines happen to arrive in; the map also hands the streams back in
	// index order for the selection below.
	std::map<unsigned long, ProbeStream> streams;
	wxString formatArtist, formatAlbum, formatTitle;

	for (const wxString &line : lines) {
		// Split on the first '=' only. Values are quoted and escaped by the
		// `flat` writer, so a '=' inside one cannot end the key.
		const int eq = line.Find(wxT('='));
		if (eq == wxNOT_FOUND) {
			continue;
		}
		const wxString key = line.Mid(0, eq);
		const wxString value = UnflattenValue(line.Mid(eq + 1));

		if (key.StartsWith(wxT("format."))) {
			const wxString field = key.Mid(7);
			if (field == wxT("duration")) {
				// A parsed ZERO is not a duration; see the note below.
				got_duration =
					ParseSeconds(value, info.length_seconds) && info.length_seconds > 0;
			} else if (field == wxT("bit_rate")) {
				(void)ParseBitrateKbps(value, info.bitrate_kbps);
			} else if (field.StartsWith(wxT("tags."))) {
				AssignTag(field.Mid(5), value, formatArtist, formatAlbum, formatTitle);
			}
			continue;
		}

		if (!key.StartsWith(wxT("streams.stream."))) {
			continue;
		}
		wxString rest = key.Mid(15);
		const wxString indexText = rest.BeforeFirst(wxT('.'));
		unsigned long index = 0;
		if (indexText.IsEmpty() || !indexText.ToULong(&index)) {
			continue;
		}
		const wxString field = rest.AfterFirst(wxT('.'));
		ProbeStream &cur = streams[index];
		if (field == wxT("codec_name")) {
			cur.codec = value;
		} else if (field == wxT("codec_type")) {
			cur.type = value;
		} else if (field == wxT("disposition.attached_pic")) {
			cur.attached_pic = (value == wxT("1"));
		} else if (field.StartsWith(wxT("tags."))) {
			AssignTag(field.Mid(5), value, cur.artist, cur.album, cur.title);
		}
	}

	// First video track's codec, else the first audio track's. Subtitle and data
	// streams never win, so we don't advertise "subrip" as a file's codec.
	wxString videoCodec, audioCodec;
	// Tags of the stream that supplied audioCodec: the fallback source for Ogg/Opus, where
	// Vorbis comments belong to the logical stream and the format section carries nothing at
	// all.
	wxString streamArtist, streamAlbum, streamTitle;
	// Real (non-artwork) audio streams; the fallback below requires exactly one.
	unsigned audioStreamCount = 0;

	for (const auto &entry : streams) {
		const ProbeStream &st = entry.second;
		// Cover art (ID3 APIC, FLAC PICTURE, MOV covr, Matroska image attachments) is
		// reported as an ordinary video stream, and is the only non-content stream claiming
		// codec_type=video. Without this an MP3 with artwork advertises "mjpeg" as the
		// file's codec -- to every peer, since the tag goes out on the wire. FFmpeg's own
		// "real video" selector is the same test.
		if (st.attached_pic || st.codec.IsEmpty()) {
			continue;
		}
		if (st.type == wxT("video")) {
			if (videoCodec.IsEmpty()) {
				videoCodec = st.codec;
			}
		} else if (st.type == wxT("audio")) {
			++audioStreamCount;
			if (audioCodec.IsEmpty()) {
				audioCodec = st.codec;
				streamArtist = st.artist;
				streamAlbum = st.album;
				streamTitle = st.title;
			}
		}
	}

	if (!videoCodec.IsEmpty()) {
		info.codec = videoCodec;
	} else if (!audioCodec.IsEmpty()) {
		info.codec = audioCodec;
	}

	info.artist = formatArtist;
	info.album = formatAlbum;
	info.title = formatTitle;
	// Stream tags are consulted only for a file with exactly one audio stream, no video and an
	// Ogg-family codec, and only where the format section gave nothing. Vorbis comments belong
	// to the single logical stream there, which is why the fallback exists at all. Anywhere
	// else stream tags are track LABELS ("Deutsch", "Espanol"), and publishing one as the
	// file's title sends it to every peer over ed2k and Kad; on a one-track file nothing
	// structural tells a label from a title (a single .mka muxed with --track-name 0:Deutsch
	// passes every other test here), so the scope is by codec rather than by "not a video". The
	// other containers lose nothing: they report in the format section.
	const bool streamTagCodec = (audioCodec == wxT("vorbis") || audioCodec == wxT("opus") ||
				     audioCodec == wxT("flac") || audioCodec == wxT("speex"));
	if (audioStreamCount == 1 && videoCodec.IsEmpty() && streamTagCodec) {
		if (info.artist.IsEmpty()) {
			info.artist = streamArtist;
		}
		if (info.album.IsEmpty()) {
			info.album = streamAlbum;
		}
		if (info.title.IsEmpty()) {
			info.title = streamTitle;
		}
	}

	// A zero duration is not a duration (see the format.duration branch), so a container
	// ffprobe can open and time as zero while reporting no codec would otherwise be a
	// successful probe carrying an all-empty MediaInfo -- which the authoritative apply step
	// treats as grounds to clear every media tag the file had.
	if (!got_duration && info.codec.IsEmpty()) {
		return false;
	}
	out = info;
	return true;
}

namespace
{
bool IsHelperString(const wxString &value)
{
	if (value.length() < 2 || value[0] != wxT('"') || value.Last() != wxT('"'))
		return false;
	for (size_t i = 1; i + 1 < value.length(); ++i) {
		const wxUniChar c = value[i];
		if (c == wxT('"') || (c < 32 && c != wxT('\t')) || c == 127)
			return false;
		if (c == wxT('\\')) {
			if (++i + 1 >= value.length())
				return false;
			const wxUniChar escaped = value[i];
			if (escaped != wxT('n') && escaped != wxT('r') && escaped != wxT('\\') &&
				escaped != wxT('"'))
				return false;
		}
	}
	return true;
}
bool ParseHelperOutput(const wxArrayString &lines, MediaInfo &info)
{
	if (lines.size() != 9 || lines[0] != wxT("AMMP 1") || lines[8] != wxT("END"))
		return false;
	static const wxChar *keys[] = { wxT("format.duration="),
		wxT("format.bit_rate="),
		wxT("streams.stream.0.codec_type="),
		wxT("streams.stream.0.codec_name="),
		wxT("format.tags.artist="),
		wxT("format.tags.album="),
		wxT("format.tags.title=") };
	for (size_t i = 0; i < 7; ++i)
		if (!lines[i + 1].StartsWith(keys[i]))
			return false;
	if (lines[3].AfterFirst(wxT('=')) != wxT("\"audio\""))
		return false;
	for (size_t i = 4; i <= 7; ++i)
		if (!IsHelperString(lines[i].AfterFirst(wxT('='))))
			return false;
	for (size_t i : { size_t(1), size_t(2) }) {
		const wxString value = lines[i].AfterFirst(wxT('='));
		if (value.empty() || value.length() > 13)
			return false;
		for (const wxUniChar c : value)
			if (c < wxT('0') || c > wxT('9'))
				return false;
		wxULongLong_t number = 0;
		if (!value.ToULongLong(&number) || (i == 1 && number > UINT32_MAX) ||
			(i == 2 && (number > uint64_t(UINT32_MAX) * 1000 || number % 1000 != 0)))
			return false;
	}
	const wxString codec = UnflattenValue(lines[4].AfterFirst(wxT('=')));
	if (codec.length() > 32)
		return false;
	for (const wxUniChar c : codec) {
		if (!((c >= wxT('a') && c <= wxT('z')) || (c >= wxT('0') && c <= wxT('9')) || c == wxT('_')))
			return false;
	}
	return ParseProbeOutput(lines, info);
}
} // namespace

ProbeOutcome Probe(const wxString &helperPath,
	const CPath &file,
	MediaInfo &out,
	unsigned timeoutMs,
	const std::atomic<bool> &keepRunning,
	bool bulk,
	bool logFailure)
{
	if (!keepRunning || !timeoutMs)
		return ProbeOutcome::Cancelled;
	if (helperPath.empty())
		return ProbeOutcome::Unavailable;
	if (!file.FileExists())
		return ProbeOutcome::Vanished;
	if (!bulk)
		AddLogLineN(CFormat(_("Extracting media metadata: %s")) % file.GetPrintable());
	wxArrayString args, lines;
	MediaInfo result;
	const int rc = RunBoundedHelper(helperPath, args, timeoutMs, keepRunning, lines, file);
	if (!keepRunning || rc == kKilled)
		return ProbeOutcome::Cancelled;
	if (rc == 0 && ParseHelperOutput(lines, result)) {
		out = result;
		return ProbeOutcome::Extracted;
	}
	if (logFailure)
		AddLogLineN(CFormat(_("Media metadata: parser failed for %s")) % file.GetPrintable());
	// Only documented parser rejections describe the file. Loader failures,
	// protocol errors and unexpected exits must not poison known.met entries.
	if (rc == 10)
		return ProbeOutcome::NoUsableMetadata;
	if (rc == 11)
		return ProbeOutcome::UnreadableFile;
	return ProbeOutcome::Unavailable;
}

} // namespace MediaProbe
