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

// Return values for RunBoundedFFProbe: a child exit code (>= 0), or one of:
constexpr int kSpawnFailed = -1;    // couldn't launch the binary at all
constexpr int kKilled = -2;         // overran timeoutMs, or keepRunning went false
constexpr int kOutputTooLarge = -3; // reply exceeded the read budget; see the slurp

// Spawn `exe argv...`, capturing stdout to a temp file, bounded by `timeoutMs`
// wall-clock. The wait loop also polls `keepRunning`, so worker shutdown kills
// the child at once. Real argv, no shell, so paths need no escaping. See the
// include-block note for why this bypasses wxExecute/wxProcess entirely.
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

	// Kill-on-close job so terminating it takes down the whole process tree, the
	// Windows equivalent of the POSIX process-group kill. ffprobe.exe forks
	// nothing, but a wrapper-style path would, and TerminateProcess would orphan it.
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
			// Kill the whole job (wrapper + real ffprobe); fall back to the bare
			// process if the job could not be created.
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

	// Own process group so a kill takes out the whole tree: ffprobe forks
	// nothing, but a wrapper-style path (snap's /snap/bin/ffprobe, a flatpak-run
	// shim) is a shell that execs the real binary as a child. setpgroup(0) makes
	// the child a group leader, and the kill path below signals -pid.
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
	//
	// Bounded, because container tags are attacker-chosen text of arbitrary
	// length: uncapped, a crafted 100 MB title is slurped whole into a wxString
	// and copied again by the tokenizer and the unescaper before kMaxTagChars
	// ever applies. A legitimate five-field reply is a few hundred bytes, and
	// anything past the cap is treated as a failed probe rather than truncated.
	const wxFileOffset kMaxProbeOutputBytes = 1024 * 1024;
	if (exitCode >= 0) {
		wxFFile f(tmpPath, wxT("rb"));
		wxString content;
		if (f.IsOpened() && f.Length() > kMaxProbeOutputBytes) {
			AddDebugLogLineN(logMediaProbe,
				CFormat(wxT("MediaProbe: ffprobe output too large (%lld bytes), ignoring")) %
					static_cast<long long>(f.Length()));
			// Its own sentinel, not kSpawnFailed: ffprobe ran fine, so
			// reporting "failed (code -1)" would name the wrong thing.
			exitCode = kOutputTooLarge;
		} else if (f.IsOpened() && f.ReadAll(&content, wxConvUTF8)) {
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

namespace
{

// Wall-clock bound for one `-version` invocation. A working ffprobe answers in
// milliseconds; one that has not by now is wedged (a stale network mount, a
// wrapper blocked on a lock) and must not hold up whoever asked.
constexpr unsigned kDetectTimeoutMs = 3000;

// True if `binary` runs cleanly enough to print its own -version output. Serves
// both as the "is ffprobe on PATH" probe and as the "does this path work" one.
//
// Goes through RunBoundedFFProbe because that puts a deadline on a binary that
// never returns and is callable off the main thread; wxExecute is neither (see
// the include-block note above). Bare `ffprobe` still resolves through PATH:
// posix_spawnp and CreateProcess with a null application name both search it.
bool CanRun(const wxString &binary)
{
	// Detection is never cancelled part-way; only the timeout bounds it.
	const std::atomic<bool> keepRunning{ true };
	wxArrayString argv, out;
	argv.Add(wxT("-version"));
	return RunBoundedFFProbe(binary, argv, kDetectTimeoutMs, keepRunning, out) == 0;
}

// Well-known install locations, tried in order; one entry per install-manager,
// since we want the first existing binary rather than every possible location.
// ARM64 Homebrew before Intel, matching what a bare PATH lookup finds first.
wxArrayString WellKnownPaths()
{
	wxArrayString paths;
#if defined(__WXMAC__)
	paths.Add(wxT("/opt/homebrew/bin/ffprobe"));
	paths.Add(wxT("/usr/local/bin/ffprobe"));
	paths.Add(wxT("/opt/local/bin/ffprobe")); // MacPorts
#elif defined(__WXMSW__)
	// WinGet's per-app dir carries the package version, so there is no leaf to
	// hardcode -- WinGet users are found by the `where.exe` PATH probe instead.
	// Chocolatey and scoop have stable roots.
	paths.Add(wxT("C:\\ffmpeg\\bin\\ffprobe.exe"));
	paths.Add(wxT("C:\\ProgramData\\chocolatey\\bin\\ffprobe.exe"));
	if (const wxChar *home = wxGetenv(wxT("USERPROFILE"))) {
		paths.Add(wxString(home) + wxT("\\scoop\\apps\\ffmpeg\\current\\bin\\ffprobe.exe"));
	}
#else
	// Snap and Flatpak users typically launch ffprobe out of their sandbox root;
	// the snap case is covered explicitly, flatpak users point the preference at
	// their wrapper manually.
	paths.Add(wxT("/usr/bin/ffprobe"));
	paths.Add(wxT("/usr/local/bin/ffprobe"));
	paths.Add(wxT("/snap/bin/ffprobe"));
#endif
	return paths;
}

} // anonymous namespace

wxString AutoDetectPath()
{
	// Fast path: bare `ffprobe` on PATH, which is what most Linux and BSD
	// installs give for free. On macOS and Windows this often fails even when
	// ffprobe IS installed, because GUI-launched processes get a minimal PATH
	// (launchd's lacks /opt/homebrew; Windows GUI apps can miss choco/scoop).
	if (CanRun(wxT("ffprobe"))) {
		return wxT("ffprobe");
	}

	for (const wxString &candidate : WellKnownPaths()) {
		if (wxFileName::FileExists(candidate) && CanRun(candidate)) {
			return candidate;
		}
	}

	return wxEmptyString;
}

wxString DetectedPath(bool redetect)
{
	// Detection describes the machine, not a user choice, so it is derived at
	// runtime and never written to the config file. Memoised because it costs at
	// least one subprocess and cannot change under a running daemon without
	// someone installing ffmpeg -- which `redetect` is for.
	//
	// The lock is held across the detection itself so two callers cannot race two
	// scans; worst case that is one subprocess spawn's worth of blocking.
	static std::mutex mutex;
	static bool done = false;
	static wxString cached;

	std::lock_guard<std::mutex> lock(mutex);
	if (done && !redetect) {
		return cached;
	}
	cached = AutoDetectPath();
	done = true;

	if (cached.IsEmpty()) {
		// Not a debug line: the operator asked for metadata extraction and is
		// getting none, and on a headless daemon this is the only place that can
		// say why. Fires once per process -- the memo above guarantees it.
		AddLogLineN(_("Media metadata: no ffprobe binary found. Install ffmpeg, or set the "
			      "ffprobe path in preferences; length, bitrate and codec will not be "
			      "extracted."));
	} else {
		AddDebugLogLineN(
			logMediaProbe, CFormat(wxT("MediaProbe: auto-detected ffprobe at %s")) % cached);
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
	// Named local: `value.utf8_str().data()` twice would be two separate
	// temporaries, each dead at the end of its own full-expression, so the
	// comparison below would read a freed pointer and compare it against one from
	// a different object -- which happens to work only because the allocator
	// hands back the same block.
	const wxScopedCharBuffer buf = value.utf8_str();
	const char *const str = buf.data();
	char *end = nullptr;
	const double d = std::strtod(str, &end);
	if (end == str || d < 0.0) {
		return false;
	}
	// Cap at uint32 range (~136 years — plenty).
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
	// Per stream: codec_name and codec_type (so a video track's codec beats an
	// audio one and subtitle/data streams never win), attached_pic, and its own
	// artist/title/album. Per format: duration, bit_rate and the same three tags.
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

// Longest tag value we keep. Generous for a real artist/album/title; the point
// is to bound what a crafted file can make this node store and publish, not to
// fit any real metadata. These values go into known.met, into the log line, and
// into every offered-file packet sent to every server and client.
const size_t kMaxTagChars = 256;

// Clean one tag value before it is allowed any further: bound the length and
// drop control characters.
//
// The cap is what keeps oversized text out of known.met, out of the log line
// and off the wire; without it the only bound anywhere was the wire format's
// 0xFFFF truncation, a packet-integrity guard rather than a policy. How much is
// READ is bounded separately, at the slurp in RunBoundedFFProbe.
//
// Control characters are dropped because the value reaches a log line (and
// through it GET /api/v0/logs/amule) and several list controls, where a raw
// newline lets one field impersonate several.
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

// ffprobe prints the container's own key case -- Matroska yields
// format.tags.ARTIST and format.tags.ALBUM beside a lower-case
// format.tags.title, in one file -- while matching the requested names
// case-insensitively. So the parser has to as well.
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

	// Keyed by the stream index ffprobe puts in the key, so ordering comes from
	// the data rather than from the order lines happen to arrive in; the map also
	// hands the streams back in index order for the selection below.
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
	// Tags of the stream that supplied audioCodec: the fallback source for
	// Ogg/Opus, where Vorbis comments belong to the logical stream and the format
	// section carries nothing at all.
	wxString streamArtist, streamAlbum, streamTitle;
	// Real (non-artwork) audio streams; the fallback below requires exactly one.
	unsigned audioStreamCount = 0;

	for (const auto &entry : streams) {
		const ProbeStream &st = entry.second;
		// Cover art (ID3 APIC, FLAC PICTURE, MOV covr, Matroska image attachments)
		// is reported as an ordinary video stream, and is the only non-content
		// stream claiming codec_type=video. Without this an MP3 with artwork
		// advertises "mjpeg" as the file's codec -- to every peer, since the tag
		// goes out on the wire. FFmpeg's own "real video" selector is the same test.
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
	// Stream tags are consulted only for a file with exactly one audio stream, no
	// video and an Ogg-family codec, and only where the format section gave
	// nothing. Vorbis comments belong to the single logical stream there, which is
	// why the fallback exists at all. Anywhere else stream tags are track LABELS
	// ("Deutsch", "Espanol"), and publishing one as the file's title sends it to
	// every peer over ed2k and Kad; on a one-track file nothing structural tells a
	// label from a title (a single .mka muxed with --track-name 0:Deutsch passes
	// every other test here), so the scope is by codec rather than by "not a
	// video". The other containers lose nothing: they report in the format section.
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

	// A zero duration is not a duration (see the format.duration branch), so a
	// container ffprobe can open and time as zero while reporting no codec would
	// otherwise be a successful probe carrying an all-empty MediaInfo -- which the
	// authoritative apply step treats as grounds to clear every media tag the file
	// had.
	if (!got_duration && info.codec.IsEmpty()) {
		return false;
	}
	out = info;
	return true;
}

ProbeOutcome Probe(const wxString &ffprobePath,
	const CPath &file,
	MediaInfo &out,
	unsigned timeoutMs,
	const std::atomic<bool> &keepRunning,
	bool bulk,
	bool logFailure)
{
	if (ffprobePath.IsEmpty()) {
		return ProbeOutcome::Unavailable;
	}

	// A job is queued only once hashing has finished, but nothing re-checks
	// between the queue and the worker picking the job up, and that gap widens
	// whenever the probe queue backs up. One stat beats announcing a probe of a
	// file deleted in the meantime and then forking on it purely to fail.
	if (!file.FileExists()) {
		// A share with stale known.met entries hits this on every refresh, so it
		// has to say what happened: without a line it is indistinguishable from a
		// file ffprobe rejected, while still consuming a naming-budget slot.
		AddDebugLogLineN(logMediaProbe,
			CFormat(wxT("MediaProbe: %s vanished before probing, skipping")) %
				file.GetPrintable());
		if (logFailure) {
			AddLogLineN(CFormat(_("Media metadata: %s is gone, nothing to extract")) %
				    file.GetPrintable());
		}
		return ProbeOutcome::Vanished;
	}

	// -show_entries constrains the output to what we care about; see
	// ProbeEntries() for the field list.
	//
	// -of flat, and NOT the more readable `default` writer, because this request
	// pulls attacker-controlled text into the output: a container tag is arbitrary
	// UTF-8 and may contain newlines (Vorbis comments and Matroska tags allow them
	// outright). `default` does not escape its values, so each embedded newline
	// becomes another key=value line inside the tag's own section -- a title of
	// "Song\nduration=99999999" injects a duration line after the real one, and
	// this parser is last-write-wins. That forged value would be published as
	// FT_MEDIA_LENGTH to every server and Kad node. A crafted line can move the
	// section boundaries too.
	//
	// `flat` escapes \n, \r, \\ and " in values, and its dotted keys carry the
	// section AND the stream index, so attribution comes from the key rather than
	// from delimiter lines a value could also forge.
	//
	// -v error silences informational chatter.
	wxArrayString argv;
	argv.Add(wxT("-v"));
	argv.Add(wxT("error"));
	argv.Add(wxT("-show_entries"));
	argv.Add(ProbeEntries());
	argv.Add(wxT("-of"));
	argv.Add(wxT("flat"));
	argv.Add(file.GetRaw());

	// Info level, not debug: media metadata is a feature the user explicitly
	// enables and points at a binary, so it needs feedback that the binary worked
	// and which file was probed. Emitted immediately before the spawn, so it fires
	// once per ffprobe execution rather than once per queued job.
	if (!bulk) {
		AddLogLineN(CFormat(_("Extracting media metadata with ffprobe: %s")) % file.GetPrintable());
	}

	// Bounded + killable: this runs on the dedicated CMediaProbeThread, so a
	// slow or hung ffprobe can only delay other probes, never completions, and
	// cannot wedge the worker or the shutdown join.
	wxArrayString stdout_lines;
	const int rc = RunBoundedFFProbe(ffprobePath, argv, timeoutMs, keepRunning, stdout_lines);
	// Failures are info level and are named even in bulk. They are rare, and now
	// that a failed file is marked and not retried each one is reported once and
	// then never again; a count with no filenames withholds the only thing the
	// user needs to act on.
	if (rc == kKilled) {
		if (logFailure) {
			AddLogLineN(CFormat(_("Media metadata: ffprobe timed out or was cancelled for %s")) %
				    file.GetPrintable());
		}
		return ProbeOutcome::Cancelled;
	}
	if (rc == kOutputTooLarge) {
		// ffprobe itself succeeded; what it produced was implausible for the five
		// fields asked for, which means the file carries a tag crafted to be
		// enormous. Named separately so this is not reported as a failure of the
		// binary.
		if (logFailure) {
			AddLogLineN(CFormat(_("Media metadata: ignoring implausibly large ffprobe output "
					      "for %s")) %
				    file.GetPrintable());
		}
		return ProbeOutcome::OutputTooLarge;
	}
	// Distinguished from a non-zero exit on purpose: this one is about the binary
	// rather than the file, so it must not be recorded against the file and needs
	// a message that sends the user to their ffprobe setting.
	if (rc == kSpawnFailed) {
		if (logFailure) {
			AddLogLineN(CFormat(_("Media metadata: could not run ffprobe (%s) -- check the "
					      "media metadata settings")) %
				    ffprobePath);
		}
		return ProbeOutcome::Unavailable;
	}
	if (rc != 0) {
		if (logFailure) {
			AddLogLineN(CFormat(_("Media metadata: ffprobe failed (code %d) for %s")) % rc %
				    file.GetPrintable());
		}
		return ProbeOutcome::UnreadableFile;
	}

	MediaInfo info;
	if (!ParseProbeOutput(stdout_lines, info)) {
		// Neither a duration nor a codec came back, so there is nothing worth
		// advertising -- report a failed probe rather than attaching empty tags.
		// Named in the log because it is the most likely way a file fails (ffprobe
		// exits 0), and the file is otherwise silently re-probed on every reload.
		if (logFailure) {
			AddLogLineN(CFormat(_("Media metadata: ffprobe found nothing usable in %s")) %
				    file.GetPrintable());
		}
		return ProbeOutcome::NoUsableMetadata;
	}

	AddDebugLogLineN(logMediaProbe,
		CFormat(wxT("MediaProbe: extracted %s -> length=%us bitrate=%ukbps codec=%s")) %
			file.GetPrintable() % info.length_seconds % info.bitrate_kbps % info.codec);
	out = info;
	return ProbeOutcome::Extracted;
}

} // namespace MediaProbe
