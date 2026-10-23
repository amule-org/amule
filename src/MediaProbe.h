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

// Media metadata extracted from a shared file, advertised alongside search results via
// FT_MEDIA_LENGTH / _BITRATE / _CODEC on the CKnownFile. Populated by the bundled helper; fields default to 0
// or empty so downstream code can gate on nonzero-ness cheaply.
struct MediaInfo
{
	// FT_MEDIA_LENGTH -- seconds, uint32 on the wire.
	uint32 length_seconds = 0;
	// FT_MEDIA_BITRATE -- kilobits per second, uint32 on the wire.
	uint32 bitrate_kbps = 0;
	// FT_MEDIA_CODEC -- free-form codec name as ffprobe reports it ("h264", "aac", "vorbis").
	// Stored and sent raw; MediaCodecLabel() gives the label the UIs show.
	wxString codec;
	// FT_MEDIA_ARTIST / _ALBUM / _TITLE -- container tags, empty when the file carries none.
	// Read from the format section, falling back to the stream's own tags only for an audio-
	// only file; see ParseProbeOutput.
	wxString artist;
	wxString album;
	wxString title;
};

namespace MediaProbe
{

// Parse ffprobe's output into a MediaInfo. Split out of Probe() so the parsing rules -- which carry
// all the container-specific subtleties -- are testable without spawning a process or shipping
// media fixtures. Returns false when nothing usable was parsed (neither a duration nor a codec),
// which the caller reports as a failed probe so no empty tags are attached.
bool ParseProbeOutput(const wxArrayString &lines, MediaInfo &out);

// The legacy ffprobe `-show_entries` reference request, exposed so a test feeds the parser output produced
// by this exact request rather than a hand-written approximation of it.
const wxChar *ProbeEntries();

// Locate the bundled amule-media-probe executable alongside the running core.
wxString AutoDetectPath();

// AutoDetectPath() memoised for the life of the process, and the entry point everything but the
// detection itself should use.
//
// Logs missing helper once. redetect refreshes the cached sibling path.
wxString DetectedPath(bool redetect = false);

// Probe a single file, populating `out` only for Extracted. Other outcomes describe
// file rejection, an unavailable helper or cancellation. The file remains shared.
//
// The probe spawns the helper once per file and waits for it with a `timeoutMs` wall-clock bound, killing
// it on expiry. `keepRunning` is polled during the wait, so worker shutdown kills the child at once
// and the caller's join can complete. Both bounds mean a slow or hung helper can never wedge the
// worker.
//
// Callers MUST run this off the main thread: it blocks for the duration of the child, typically
// 30-100 ms and at most `timeoutMs`.

//! Why a probe produced no metadata. The distinction matters because one of these is a statement
//! about the FILE and the rest about the environment: only the first justifies recording "this file
//! cannot be probed" and skipping it on later scans. Marking on the others would let a missing
//! helper, a spun-down disk or a shutdown mid-scan brand every file it touched as permanently
//! unprobeable (issue #1116).
enum class ProbeOutcome
{
	Extracted,        //!< usable metadata came back
	NoUsableMetadata, //!< MediaInfo found no usable metadata
	UnreadableFile,   //!< MediaInfo explicitly rejected the input
	OutputTooLarge,   //!< legacy outcome retained for compatibility
	Unavailable,      //!< helper unavailable, bad protocol or unexpected exit
	Cancelled,        //!< timed out, or shutdown cancelled the drain
	Vanished,         //!< the file was gone by the time the worker got to it
};

//! True for the outcomes that are a verdict on the file rather than on the
//! machine it runs on, and so may be recorded against the file.
inline bool IsFileVerdict(ProbeOutcome outcome)
{
	// Only file verdicts persist; cancellation and missing helpers are retried.
	return outcome == ProbeOutcome::NoUsableMetadata || outcome == ProbeOutcome::UnreadableFile;
}

//! \param bulk true when the caller is draining a batch of queued probes, in which case the per-
//! file "extracting" announcement is suppressed and the caller reports one summary -- otherwise a
//! large media library produces one line per file. A genuine single-file event still speaks.
//! \param logFailure whether a failure may name its file. Failures are worth naming even in bulk,
//! since a count alone is not actionable, but a broken ffprobe fails EVERY file, so the caller caps
//! how many it names.
ProbeOutcome Probe(const wxString &helperPath,
	const CPath &file,
	MediaInfo &out,
	unsigned timeoutMs,
	const std::atomic<bool> &keepRunning,
	bool bulk = false,
	bool logFailure = true);

} // namespace MediaProbe

#endif // MEDIAPROBE_H
