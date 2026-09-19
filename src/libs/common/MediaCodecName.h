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

#ifndef MEDIA_CODEC_NAME_H
#define MEDIA_CODEC_NAME_H

#include <string>

/**
 * Codec FOURCC to the label the interface shows, e.g. "h264" to "H.264".
 *
 * One table for every surface: the desktop reaches it through FormatMediaCodec() (OtherFunctions.h)
 * and amuleapi calls it directly when serialising `media.codec`, so a file reads the same in the
 * GUI, the REST rows and the SSE payload. Lives in mulecommon because EventDiff.cpp is one of those
 * callers and its test target is kept to a handful of sources.
 *
 * Matching is case-insensitive. An unmapped codec is returned unchanged, so something useful still
 * shows rather than nothing. Untranslated: these are labels, not messages.
 */
std::string MediaCodecLabel(const std::string &raw);

#endif // MEDIA_CODEC_NAME_H
// File_checked_for_headers
