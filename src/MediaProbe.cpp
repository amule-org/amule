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

#include <wx/arrstr.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>

#include <common/Format.h>

#include "Logger.h"
#include "libs/common/Path.h"

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

bool Probe(const wxString &ffprobePath, const CPath &file, MediaInfo &out)
{
	if (ffprobePath.IsEmpty()) {
		return false;
	}

	// -show_entries constrains the output to the three fields we care
	// about (codec of every stream + format-level duration + format-
	// level bit_rate). -of default=nk=0:nw=1 emits one bare "key=value"
	// per line, no INI section headers, no line wrapping — trivial to
	// scan. -v error silences the informational lines that would
	// otherwise fight for stdout.
	//
	// The file path is quoted so paths with spaces work on both
	// POSIX and Windows. wxExecute forwards the string to the shell
	// as-is; embedded `"` in filenames would corrupt the quoting but
	// that's a vanishingly rare case on shared media files.
	const wxString cmd = wxT("\"") + ffprobePath + wxT("\"") + wxT(" -v error") +
			     wxT(" -show_entries format=duration,bit_rate:stream=codec_name") +
			     wxT(" -of default=nk=0:nw=1 \"") + file.GetRaw() + wxT("\"");

	wxArrayString stdout_lines, stderr_lines;
	const long rc =
		wxExecute(cmd, stdout_lines, stderr_lines, wxEXEC_SYNC | wxEXEC_NODISABLE | wxEXEC_NOEVENTS);
	if (rc != 0) {
		AddDebugLogLineN(logGeneral,
			CFormat(wxT("MediaProbe: ffprobe failed (rc=%ld) for %s")) % rc %
				file.GetPrintable());
		return false;
	}

	MediaInfo info;
	bool got_duration = false;
	bool got_codec = false;
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
		} else if (key == wxT("codec_name") && !got_codec) {
			// First stream's codec wins — this is video for
			// video containers, audio for audio-only files.
			info.codec = value;
			got_codec = true;
		}
	}

	// A file with zero streams / zero duration produces an all-blank
	// MediaInfo which is meaningless to advertise — treat it as a
	// probe failure so the caller doesn't attach empty tags.
	if (!got_duration && !got_codec) {
		AddDebugLogLineN(logGeneral,
			CFormat(wxT("MediaProbe: no metadata parsed for %s")) % file.GetPrintable());
		return false;
	}

	out = info;
	return true;
}

} // namespace MediaProbe
