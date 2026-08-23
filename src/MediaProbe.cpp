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

namespace
{

// Wall-clock bound for one `-version` invocation. A working ffprobe
// answers in milliseconds; a binary that has not by now is wedged (a
// stale network mount, a wrapper blocked on a lock) and must not hold
// up whoever asked. Detection walks a handful of candidates at worst,
// and only ever the ones that exist on disk.
constexpr unsigned kDetectTimeoutMs = 3000;

// One-shot silent invocation. Returns true if `binary` runs cleanly
// enough to print its own -version output. Used both as the "is on
// PATH" probe (binary = "ffprobe") and as the "does this path work"
// probe (binary = a resolved absolute path).
//
// Goes through RunBoundedFFProbe for two reasons: it puts a deadline on
// a binary that never returns, and it is callable off the main thread.
// wxExecute is neither — see the include-block note above. Both matter
// now that detection runs on the probe worker and not just behind the
// Preferences "Detect" button. Bare `ffprobe` still resolves through
// PATH: posix_spawnp and CreateProcess (with a null application name)
// both search it.
bool CanRun(const wxString &binary)
{
	// Detection is never cancelled part-way; only the timeout bounds it.
	const std::atomic<bool> keepRunning{ true };
	wxArrayString argv, out;
	argv.Add(wxT("-version"));
	return RunBoundedFFProbe(binary, argv, kDetectTimeoutMs, keepRunning, out) == 0;
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

wxString DetectedPath(bool redetect)
{
	// Detection describes the machine, not a user choice, so it is derived at
	// runtime and never written to the config file. Memoised because it costs
	// at least one subprocess and the answer cannot change under a running
	// daemon without someone installing ffmpeg -- which is what `redetect`
	// (the Preferences "Detect" button) is for.
	//
	// The lock is held across the detection itself so two callers cannot race
	// two scans. That is a subprocess spawn's worth of blocking in the worst
	// case; the common ones are cheap -- a missing binary fails to spawn
	// immediately, and every well-known path is stat()ed before it is run.
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
		// The one line that says the feature is inert. Not a debug line: the
		// operator asked for metadata extraction and is getting none, and on
		// a headless daemon this is the only place that can say why. Fires
		// once per process -- the memo above guarantees it.
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

// ffprobe emits float durations with locale-independent `.` decimal
// separators, so a plain strtod suffices — no wxString::ToDouble()
// with its locale sensitivity here.
bool ParseSeconds(const wxString &value, uint32 &out)
{
	if (value.IsEmpty()) {
		return false;
	}
	// Hold the UTF-8 buffer in a named local. `value.utf8_str().data()` twice
	// would be two separate temporaries, each dead at the end of its own
	// full-expression, so the comparison below would read a freed pointer and
	// compare it against one from a different object -- which happens to work
	// only because the allocator hands back the same block.
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
	// Per stream: its codec_name and codec_type (so a video track's codec is
	// preferred over an audio one, and subtitle / data streams never win), the
	// attached_pic disposition, and its own artist/title/album tags. Per
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

// ffprobe prints the container's own key case -- Matroska yields
// format.tags.ARTIST and format.tags.ALBUM beside a lower-case
// format.tags.title, in one file -- while matching the requested names
// case-insensitively. So the parser has to as well.
void AssignTag(const wxString &key, const wxString &value, wxString &artist, wxString &album, wxString &title)
{
	const wxString lower = key.Lower();
	if (lower == wxT("artist")) {
		artist = value;
	} else if (lower == wxT("album")) {
		album = value;
	} else if (lower == wxT("title")) {
		title = value;
	}
}

} // namespace

bool ParseProbeOutput(const wxArrayString &lines, MediaInfo &out)
{
	MediaInfo info;
	bool got_duration = false;

	// Keyed by the stream index ffprobe puts in the key, so ordering comes
	// from the data rather than from the order lines happen to arrive in. A
	// std::map also gives the streams back in index order for the selection
	// below.
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

	// Codec selection: the first video track's codec, else the first audio
	// track's. Subtitle / data streams (e.g. a leading subrip track in an mkv)
	// never win, so we don't advertise "subrip" as a file's codec.
	wxString videoCodec, audioCodec;
	// Tags of the stream that supplied audioCodec, the fallback source for
	// Ogg/Opus where Vorbis comments belong to the logical stream and the
	// format section carries nothing at all.
	wxString streamArtist, streamAlbum, streamTitle;
	// How many real (non-artwork) audio streams the file has. The stream-tag
	// fallback below requires exactly one.
	unsigned audioStreamCount = 0;

	for (const auto &entry : streams) {
		const ProbeStream &st = entry.second;
		// Cover art (ID3 APIC, FLAC PICTURE, MOV covr, Matroska image
		// attachments, ...) is reported as an ordinary video stream and is the
		// only non-content stream that claims codec_type=video. Without this
		// an MP3 with artwork advertises "mjpeg" as the file's codec -- to
		// every peer, since the tag goes out on the wire. FFmpeg's own "real
		// video" selector is the same test.
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
	// Stream tags are consulted only for a file with EXACTLY ONE audio stream
	// and no video, and only where the format section gave nothing.
	//
	// The fallback exists for Ogg and Opus, where Vorbis comments belong to
	// the single logical stream -- so the condition it actually needs is "one
	// stream", not "not a video". On any multi-track container the stream tags
	// are track LABELS ("Deutsch", "Espanol"), and publishing one as the
	// file's title sends it to every peer over both ed2k and Kad. That is
	// equally true of a multi-track .mka or a chained .ogg, which have no
	// video stream at all and which a "not a video" test would let through.
	// ...and only for the codecs that container genuinely uses. On a one-track
	// file there is nothing to tell a track LABEL from a title -- a single
	// .mka muxed with --track-name 0:Deutsch satisfies every structural test --
	// so the fallback is scoped to Vorbis and Opus, the two whose comments
	// live on the stream because the format section carries none. FLAC and
	// everything else report at format level and never need it.
	// The Ogg family, whose comments live on the logical stream. Listing them
	// costs nothing for the others: the fallback only runs when the format
	// section gave NOTHING, and every other container reports there.
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

	// A zero duration is not a duration (see the format.duration branch): a
	// container ffprobe can open and time as zero but reports no codec for
	// would otherwise return a successful probe carrying an all-empty
	// MediaInfo, which the authoritative apply step would treat as grounds to
	// clear every media tag the file had.
	if (!got_duration && info.codec.IsEmpty()) {
		return false;
	}
	out = info;
	return true;
}

bool Probe(const wxString &ffprobePath,
	const CPath &file,
	MediaInfo &out,
	unsigned timeoutMs,
	const std::atomic<bool> &keepRunning,
	bool bulk)
{
	if (ffprobePath.IsEmpty()) {
		return false;
	}

	// A job is queued only once hashing has finished, so the file existed
	// moments ago -- but nothing re-checks between the queue and this worker
	// picking the job up, and that gap widens whenever the probe queue backs
	// up. Without this a file deleted in the meantime would be announced as
	// being probed (issue #968) and then have a process spawned on it purely
	// to fail. One stat is nothing against a fork+exec.
	if (!file.FileExists()) {
		AddDebugLogLineN(logMediaProbe,
			CFormat(wxT("MediaProbe: %s vanished before probing, skipping")) %
				file.GetPrintable());
		return false;
	}

	// -show_entries constrains the output to what we care about; see
	// ProbeEntries() for the field list and ParseProbeOutput() for what is done
	// with it.
	//
	// -of flat, and NOT the more readable `default` writer, because this
	// request pulls attacker-controlled text into the output: a container tag
	// is arbitrary UTF-8 and may contain newlines (Vorbis comments and
	// Matroska tags allow them outright; nothing enforces ID3's advice against
	// them). `default` does not escape its values, so each embedded newline
	// becomes another key=value line inside the section the tag belongs to --
	// a title of "Song\nduration=99999999" injects a duration line after the
	// real one, and this parser is last-write-wins. That forged value would be
	// attached as FT_MEDIA_LENGTH and published to every server and Kad node,
	// defeating the whole premise that only locally verified metadata is
	// advertised. A crafted line can move the section boundaries too.
	//
	// `flat` escapes \n, \r, \\ and " in values, and its dotted keys carry
	// the section AND the stream index (format.tags.title,
	// streams.stream.0.codec_name), so attribution comes from the key rather
	// than from delimiter lines a value could also forge.
	//
	// -v error silences informational chatter. Tokens are passed as a real
	// argv (no shell), so the file path needs no quoting/escaping.
	wxArrayString argv;
	argv.Add(wxT("-v"));
	argv.Add(wxT("error"));
	argv.Add(wxT("-show_entries"));
	argv.Add(ProbeEntries());
	argv.Add(wxT("-of"));
	argv.Add(wxT("flat"));
	argv.Add(file.GetRaw());

	// Info level, not debug: media metadata is a feature the user explicitly
	// enables and points at a binary, and until now got no feedback that it
	// was being used, that the binary worked, or which file was being probed
	// -- the debug trace this replaces is compiled out of release builds
	// (issue #968). Emitted here, immediately before the spawn, so it fires
	// once per actual ffprobe execution rather than once per queued job; the
	// queue-time trace in SharedFileList stays at debug level.
	if (!bulk) {
		AddLogLineN(CFormat(_("Extracting media metadata with ffprobe: %s")) % file.GetPrintable());
	}

	// Bounded + killable: this runs on the dedicated CMediaProbeThread, so a
	// slow/hung ffprobe can only ever delay other probes — never completions.
	// The timeout and keepRunning cancel also stop a stuck child from wedging
	// the worker itself or the shutdown join.
	wxArrayString stdout_lines;
	const int rc = RunBoundedFFProbe(ffprobePath, argv, timeoutMs, keepRunning, stdout_lines);
	// Failures are info level too. Announcing the extraction and then reporting
	// nothing when it fails is worse than the silence this feature replaced: a
	// misconfigured or broken ffprobe would produce one confident "extracting"
	// line per file and, in a release build where AddDebugLogLineN compiles to
	// nothing, no error whatsoever. Whether the binary actually works is one of
	// the questions these lines exist to answer.
	if (rc == kKilled) {
		if (!bulk) {
			AddLogLineN(CFormat(_("Media metadata: ffprobe timed out or was cancelled for %s")) %
				    file.GetPrintable());
		}
		return false;
	}
	if (rc != 0) {
		if (!bulk) {
			AddLogLineN(CFormat(_("Media metadata: ffprobe failed (code %d) for %s")) % rc %
				    file.GetPrintable());
		}
		return false;
	}

	MediaInfo info;
	if (!ParseProbeOutput(stdout_lines, info)) {
		// Neither a duration nor a codec came back, so there is nothing worth
		// advertising -- report a failed probe rather than attaching empty tags.
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
