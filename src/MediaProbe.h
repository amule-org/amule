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

#ifndef MEDIAPROBE_H
#define MEDIAPROBE_H

#include <wx/arrstr.h> // Needed for wxArrayString
#include <atomic>

#include <wx/string.h>

#include "Types.h"

class CPath;

// Media metadata extracted from a shared file so we can advertise it
// alongside search results via FT_MEDIA_LENGTH / _BITRATE / _CODEC
// on the CKnownFile. Populated by ffprobe when the user has it
// installed; fields default to 0 / empty so downstream code can gate
// on nonzero-ness cheaply.
struct MediaInfo
{
	// FT_MEDIA_LENGTH — seconds, uint32 on the wire.
	uint32 length_seconds = 0;
	// FT_MEDIA_BITRATE — kilobits per second, uint32 on the wire.
	uint32 bitrate_kbps = 0;
	// FT_MEDIA_CODEC — free-form codec name string as ffprobe
	// reports it (e.g. "h264", "aac", "vorbis"). Displayed as-is;
	// FormatMediaCodec() in OtherFunctions.h maps a few common
	// FOURCCs / format strings to friendlier UI labels.
	wxString codec;
	// FT_MEDIA_ARTIST / _ALBUM / _TITLE — container tags, empty when the
	// file carries none. Read from the format section, falling back to the
	// stream's own tags only for an audio-only file; see ParseProbeOutput.
	wxString artist;
	wxString album;
	wxString title;
};

namespace MediaProbe
{

// Parse ffprobe's `-show_entries ... -of default=nk=0` output into a
// MediaInfo. Split out of Probe() so the parsing rules -- which carry all the
// container-specific subtleties -- are testable without spawning a process or
// shipping media fixtures; Probe() is then just "run the binary and call this".
//
// Returns false when nothing usable was parsed (neither a duration nor a
// codec), which the caller reports as a failed probe so no empty tags are
// attached.
bool ParseProbeOutput(const wxArrayString &lines, MediaInfo &out);

// The `-show_entries` argument Probe() passes, exposed so a test feeds the
// parser output produced by this exact request rather than a hand-written
// approximation of it.
const wxChar *ProbeEntries();

// Locate an ffprobe binary. Tries in order:
//   1. `ffprobe` on $PATH (or %PATH% on Windows) via a quick
//      `ffprobe -version` invocation — this is the fast path when
//      the user has ffmpeg installed in the shell PATH.
//   2. A per-platform list of well-known install locations. Homebrew
//      / MacPorts on macOS, scoop / chocolatey / winget on Windows,
//      distro-standard bin paths on Linux + OpenBSD.
// Returns an empty wxString if nothing was found.
//
// Runs synchronously and spawns at least one child process, each bounded
// by its own timeout. Cheap in the ordinary cases — the PATH probe is a
// few dozen ms, a missing binary fails to spawn at once, and every
// well-known path is stat()ed before it is run — but it is still a
// subprocess walk, so prefer DetectedPath() below, which pays for it once.
//
// Safe to call from any thread.
wxString AutoDetectPath();

// AutoDetectPath() memoised for the life of the process, and the entry
// point everything but the detection itself should use.
//
// The result describes the machine rather than a user choice, so it is
// derived at runtime and never persisted: an empty `ffprobe_path`
// preference means "ask this", not "feature off". `redetect` forces a
// fresh scan and replaces the cache — for the Preferences "Detect"
// button, whose whole purpose is to notice an ffmpeg installed since the
// process started.
//
// Logs the outcome exactly once: a visible line when nothing was found
// (the only notice a headless operator gets that extraction is inert), a
// logMediaProbe debug line naming the binary when something was.
//
// Safe to call from any thread; concurrent callers serialise on the
// first scan.
wxString DetectedPath(bool redetect = false);

// Probe a single file. Returns true on success and populates `out`;
// returns false on any failure — binary missing / unreadable file /
// non-media file / ffprobe hard error / malformed output. Failures
// emit a debug-level log line but never surface anything user-facing
// (a file the probe can't read still gets shared, just without
// media tags).
//
// The probe spawns ffprobe as a child process and waits for it with a
// `timeoutMs` wall-clock bound: if the child outlives the deadline it is
// killed and the probe reports failure. `keepRunning` is polled during the
// wait — when it flips false (worker shutdown) the child is killed
// immediately so the caller's join can complete. Both bounds mean a
// slow/hung ffprobe can never wedge the worker or the shutdown path.
//
// Callers MUST run this off the main thread — it blocks for the duration
// of the child (typically 30-100 ms, at most `timeoutMs`).
//! Why a probe produced no metadata. The distinction matters because one of
//! these is a statement about the FILE and the rest are statements about the
//! environment: only the first justifies recording "this file cannot be
//! probed" and skipping it on later scans. Marking on the others would let a
//! mistyped ffprobe path, a spun-down disk, or a shutdown mid-scan brand every
//! file it touched as permanently unprobeable (issue #1116).
enum class ProbeOutcome
{
	Extracted,        //!< usable metadata came back
	NoUsableMetadata, //!< ffprobe ran and exited 0, and there was nothing in it
	UnreadableFile,   //!< ffprobe ran and exited non-zero: it could not read this file
	OutputTooLarge,   //!< ffprobe ran, but the file's tags are implausibly large
	Unavailable,      //!< the binary could not be launched at all
	Cancelled,        //!< timed out, or shutdown cancelled the drain
	Vanished,         //!< the file was gone by the time the worker got to it
};

//! True for the outcomes that are a verdict on the file rather than on the
//! machine it runs on, and so may be recorded against the file.
inline bool IsFileVerdict(ProbeOutcome outcome)
{
	// UnreadableFile counts: a non-zero exit is how a WORKING ffprobe rejects
	// a file it cannot parse, which is the ordinary broken-download case and
	// the one users report as retried forever. A binary that cannot be
	// launched is a different code path entirely (kSpawnFailed), so a missing
	// or mistyped path never arrives here.
	//
	// The gap: a non-zero exit cannot distinguish "ffprobe read this file and
	// it is broken" from "ffprobe could not open it". FileExists() is a stat,
	// so a file present but unreadable -- no read permission, or on a network
	// mount that is away or hiccupping -- reaches a perfectly good ffprobe,
	// exits non-zero, and is recorded as unprobeable. A share on SMB or NFS
	// can lose a whole library's metadata to one bad moment that way, which is
	// likelier than the other case here: a configured path pointing at some
	// other executable that exits non-zero.
	//
	// Separating the two means reading ffprobe's stderr, which currently goes
	// to /dev/null. Until then a media refresh is the way back: it ignores the
	// marks and clears them on success.
	return outcome == ProbeOutcome::NoUsableMetadata || outcome == ProbeOutcome::UnreadableFile ||
	       outcome == ProbeOutcome::OutputTooLarge;
}

//! \param bulk true when the caller is draining a batch of queued probes (a
//! share scan), in which case the per-file "extracting" announcement is
//! suppressed and the caller reports one summary for the whole operation --
//! otherwise a large media library produces one line per file, which is
//! exactly the scale-with-the-share-size property the discovery lines were
//! summarised to avoid. A genuine single-file event still speaks.
//! \param logFailure whether a failure may name its file. Failures are worth
//! naming even in bulk (a count alone is not actionable), but a broken ffprobe
//! fails EVERY file, so the caller caps how many it names per operation and
//! folds the rest into the summary.
ProbeOutcome Probe(const wxString &ffprobePath,
	const CPath &file,
	MediaInfo &out,
	unsigned timeoutMs,
	const std::atomic<bool> &keepRunning,
	bool bulk = false,
	bool logFailure = true);

} // namespace MediaProbe

#endif // MEDIAPROBE_H
