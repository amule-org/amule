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
#include <cstring>
#include <unordered_map>

namespace
{

struct CodecEntry
{
	const char *id;
	const char *display;
};

// Keys are what reaches us: ffprobe codec_name for local files, and for remote ones whatever the
// peer put in FT_MEDIA_CODEC -- eMule's video FOURCC or WAVE_FORMAT define (MediaInfo.cpp), plus
// VORB, WMA1 and WMA2, kept from aMule's earlier table in case a peer still sends them.
// Labels name the format, not the encoder: x264 is H.264, Xvid and DivX 4/5 are MPEG-4 Part 2.
const CodecEntry kCodecMap[] = {
	// Video
	{ "H264", "H.264" },
	{ "X264", "H.264" },
	{ "AVC1", "H.264" },
	{ "HEVC", "H.265" },
	{ "HVC1", "H.265" },
	{ "HEV1", "H.265" },
	{ "X265", "H.265" },
	{ "AV1", "AV1" },
	{ "AV01", "AV1" },
	{ "VP8", "VP8" },
	{ "VP80", "VP8" },
	{ "VP9", "VP9" },
	{ "VP90", "VP9" },
	{ "MPEG4", "MPEG-4" },
	{ "XVID", "MPEG-4" },
	{ "DIVX", "MPEG-4" },
	{ "DX50", "MPEG-4" },
	{ "FMP4", "MPEG-4" },
	{ "MP4V", "MPEG-4" },
	// DivX 3 is a hacked MS MPEG-4 v3, not MPEG-4 Part 2.
	{ "DIV3", "MS MPEG-4 v3" },
	{ "DIV4", "MS MPEG-4 v3" },
	{ "MP43", "MS MPEG-4 v3" },
	{ "MSMPEG4V3", "MS MPEG-4 v3" },
	{ "MP42", "MS MPEG-4 v2" },
	{ "MSMPEG4V2", "MS MPEG-4 v2" },
	{ "MPG4", "MS MPEG-4 v1" },
	{ "MSMPEG4V1", "MS MPEG-4 v1" },
	{ "MPEG1VIDEO", "MPEG-1" },
	{ "MPEG2VIDEO", "MPEG-2" },
	{ "H263", "H.263" },
	{ "WMV1", "WMV 7" },
	{ "WMV2", "WMV 8" },
	{ "WMV3", "WMV 9" },
	{ "WMVA", "VC-1" },
	{ "WVC1", "VC-1" },
	{ "VC1", "VC-1" },
	{ "RV10", "RealVideo 1" },
	{ "RV13", "RealVideo 1" },
	{ "RV20", "RealVideo 2" },
	{ "RV30", "RealVideo 3" },
	{ "RV40", "RealVideo 4" },
	{ "THEORA", "Theora" },
	{ "FLV1", "Sorenson Spark" },
	{ "MJPG", "Motion JPEG" },
	{ "MJPEG", "Motion JPEG" },
	{ "IV32", "Indeo 3" },
	{ "INDEO3", "Indeo 3" },
	{ "IV40", "Indeo 4" },
	{ "INDEO4", "Indeo 4" },
	{ "IV50", "Indeo 5" },
	{ "INDEO5", "Indeo 5" },
	{ "CVID", "Cinepak" },
	{ "CINEPAK", "Cinepak" },
	{ "CRAM", "MS Video 1" },
	{ "MSVIDEO1", "MS Video 1" },
	{ "VP6", "VP6" },
	{ "VP6F", "VP6" },
	{ "VP6A", "VP6" },
	{ "SVQ1", "Sorenson Video 1" },
	{ "SVQ3", "Sorenson Video 3" },
	{ "TSCC", "TechSmith Screen Capture" },
	{ "TSCC2", "TechSmith Screen Capture" },
	// Audio
	{ "MP1", "MP1" },
	{ "MP2", "MP2" },
	{ "MP3", "MP3" },
	{ "AAC", "AAC" },
	{ "MPEG_ADTS_AAC", "AAC" },
	{ "MPEG_RAW_AAC", "AAC" },
	{ "RAAC", "AAC" },
	{ "RACP", "AAC" },
	{ "AAC_LATM", "AAC" },
	{ "AC3", "AC-3" },
	{ "DOLBY_AC3_SPDIF", "AC-3" },
	{ "ESST_AC3", "AC-3" },
	{ "DNET", "AC-3" },
	{ "EAC3", "E-AC-3" },
	{ "DTS", "DTS" },
	{ "TRUEHD", "TrueHD" },
	{ "FLAC", "FLAC" },
	{ "ALAC", "ALAC" },
	{ "OPUS", "Opus" },
	{ "VORBIS", "Vorbis" },
	{ "VORB", "Vorbis" },
	{ "SPEEX", "Speex" },
	{ "APE", "Monkey's Audio" },
	{ "WAVPACK", "WavPack" },
	{ "MUSEPACK7", "Musepack" },
	{ "MUSEPACK8", "Musepack" },
	{ "TTA", "TTA" },
	{ "WMAV1", "WMA 1" },
	{ "WMA1", "WMA 1" },
	{ "MSAUDIO1", "WMA 1" },
	{ "WMAV2", "WMA 2" },
	{ "WMA2", "WMA 2" },
	{ "WMAUDIO2", "WMA 2" },
	{ "WMAPRO", "WMA Pro" },
	{ "WMAUDIO3", "WMA Pro" },
	{ "WMALOSSLESS", "WMA Lossless" },
	{ "WMAUDIO_LOSSLESS", "WMA Lossless" },
	{ "WMAVOICE", "WMA Voice" },
	{ "WMAVOICE9", "WMA Voice" },
	{ "WMAVOICE10", "WMA Voice" },
	{ "COOK", "Cook" },
	{ "SIPR", "Sipro" },
	{ "RA14", "RealAudio 14.4" },
	{ "RA_144", "RealAudio 14.4" },
	{ "RA28", "RealAudio 28.8" },
	{ "RA_288", "RealAudio 28.8" },
	{ "AMR_NB", "AMR-NB" },
	{ "AMR_WB", "AMR-WB" },
	{ "GSM610", "GSM 6.10" },
	{ "GSM_MS", "GSM 6.10" },
	{ "GSM", "GSM 6.10" },
	{ "PCM", "PCM" },
	{ "IEEE_FLOAT", "PCM" },
	// Companded, not linear PCM; listed here so the PCM_ prefix rule does not catch them.
	{ "ALAW", "G.711" },
	{ "MULAW", "G.711" },
	{ "PCM_ALAW", "G.711" },
	{ "PCM_MULAW", "G.711" },
	{ "ADPCM", "ADPCM" },
	{ "DVI_ADPCM", "ADPCM" },
};

// ffprobe splits PCM and ADPCM per variant (pcm_s16le, adpcm_ima_wav...).
const CodecEntry kCodecPrefixMap[] = {
	{ "PCM_", "PCM" },
	{ "ADPCM_", "ADPCM" },
};

} // namespace

std::string MediaCodecLabel(const std::string &raw)
{
	if (raw.empty()) {
		return raw;
	}

	std::string upper = raw;
	// ASCII only: std::toupper follows the locale, and under tr_TR 'i' does not become 'I'.
	std::transform(upper.begin(), upper.end(), upper.begin(), [](char c) {
		return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
	});

	// Sorting a list by codec calls this twice per comparison.
	static const std::unordered_map<std::string, const char *> exact = [] {
		std::unordered_map<std::string, const char *> m;
		for (const CodecEntry &entry : kCodecMap) {
			m.emplace(entry.id, entry.display);
		}
		return m;
	}();
	const auto it = exact.find(upper);
	if (it != exact.end()) {
		return it->second;
	}
	for (const CodecEntry &entry : kCodecPrefixMap) {
		if (upper.compare(0, std::strlen(entry.id), entry.id) == 0) {
			return entry.display;
		}
	}
	return raw;
}

// File_checked_for_headers
