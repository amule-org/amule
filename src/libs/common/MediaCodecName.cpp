//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// Any parts of this program derived from the xMule, lMule or eMule project,
// or contributed by third-party developers are copyrighted by their
// respective authors.
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

#include "MediaCodecName.h"

#include <algorithm>
#include <cctype>

namespace
{

struct CodecEntry
{
	const char *fcc;
	const char *display;
};

// Inspired by eMule AI's MediaInfo.cpp (GPL v2+).
const CodecEntry kCodecMap[] = {
	// Video
	{ "H264", "H.264" },
	{ "X264", "x264" },
	{ "AVC1", "H.264" },
	{ "HEVC", "H.265 / HEVC" },
	{ "HVC1", "H.265 / HEVC" },
	{ "XVID", "Xvid" },
	{ "DIVX", "DivX" },
	{ "DX50", "DivX 5" },
	{ "DIV3", "DivX 3" },
	{ "DIV4", "DivX 4" },
	{ "FMP4", "MPEG-4" },
	{ "MP4V", "MPEG-4" },
	{ "MPG4", "MS MPEG-4 v1" },
	{ "MP42", "MS MPEG-4 v2" },
	{ "MP43", "MS MPEG-4 v3" },
	{ "WMV1", "WMV 7" },
	{ "WMV2", "WMV 8" },
	{ "WMV3", "WMV 9" },
	{ "MJPG", "Motion JPEG" },
	{ "VP90", "VP9" },
	{ "VP80", "VP8" },
	{ "AV01", "AV1" },
	// Audio
	{ "MP3", "MP3" },
	{ "AAC", "AAC" },
	{ "AC3", "AC-3" },
	{ "FLAC", "FLAC" },
	{ "OPUS", "Opus" },
	{ "VORB", "Vorbis" },
	{ "WMA1", "WMA 1" },
	{ "WMA2", "WMA 2" },
};

} // namespace

std::string MediaCodecLabel(const std::string &raw)
{
	if (raw.empty()) {
		return raw;
	}

	std::string upper = raw;
	std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) {
		return static_cast<char>(std::toupper(c));
	});

	for (const CodecEntry &entry : kCodecMap) {
		if (upper == entry.fcc) {
			return entry.display;
		}
	}
	return raw;
}

// File_checked_for_headers
