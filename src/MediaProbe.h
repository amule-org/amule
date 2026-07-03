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
};

namespace MediaProbe
{

// Locate an ffprobe binary. Tries in order:
//   1. `ffprobe` on $PATH (or %PATH% on Windows) via a quick
//      `ffprobe -version` invocation — this is the fast path when
//      the user has ffmpeg installed in the shell PATH.
//   2. A per-platform list of well-known install locations. Homebrew
//      / MacPorts on macOS, scoop / chocolatey / winget on Windows,
//      distro-standard bin paths on Linux + OpenBSD.
// Returns an empty wxString if nothing was found — the caller should
// then treat the "media metadata extraction" feature as disabled by
// default and only enable it once the user points at a binary via
// the Preferences panel.
//
// Runs synchronously. Cheap (a few dozen ms max for the PATH probe,
// plus a handful of stat()s for the well-known-path scan). Suitable
// for calling once during CamuleApp::OnInit's slow-path.
wxString AutoDetectPath();

// Probe a single file. Returns true on success and populates `out`;
// returns false on any failure — binary missing / unreadable file /
// non-media file / ffprobe hard error / malformed output. Failures
// emit a debug-level log line but never surface anything user-facing
// (a file the probe can't read still gets shared, just without
// media tags).
//
// The probe forks a subprocess and waits synchronously; expect
// 30-100 ms per file on typical hardware. Callers MUST run this off
// the main thread — CSharedFileList threading extends the existing
// batch-scan path.
bool Probe(const wxString &ffprobePath, const CPath &file, MediaInfo &out);

} // namespace MediaProbe

#endif // MEDIAPROBE_H
