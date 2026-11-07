// SPDX-License-Identifier: GPL-2.0-or-later
// Container codec identifiers to the FFmpeg codec names aMule publishes.
// Unknown identifiers return an empty name: the codec is missing data, never a guess.
#include "probe.h"

#include <cstring>
#include <map>

namespace probe
{

std::string fourccCodec(const char fourcc[4])
{
	static const std::map<std::string, std::string> names = {
		{ "XVID", "mpeg4" },
		{ "DIVX", "mpeg4" },
		{ "DX50", "mpeg4" },
		{ "FMP4", "mpeg4" },
		{ "MP4V", "mpeg4" },
		{ "3IV2", "mpeg4" },
		{ "M4S2", "mpeg4" },
		{ "MP4S", "mpeg4" },
		{ "RMP4", "mpeg4" },
		{ "SMP4", "mpeg4" },
		{ "DIV3", "msmpeg4v3" },
		{ "DIV4", "msmpeg4v3" },
		{ "DIV5", "msmpeg4v3" },
		{ "DIV6", "msmpeg4v3" },
		{ "MP43", "msmpeg4v3" },
		{ "AP41", "msmpeg4v3" },
		{ "COL1", "msmpeg4v3" },
		{ "MPG3", "msmpeg4v3" },
		{ "MP42", "msmpeg4v2" },
		{ "DIV2", "msmpeg4v2" },
		{ "MPG4", "msmpeg4v1" },
		{ "MP41", "msmpeg4v1" },
		{ "H264", "h264" },
		{ "X264", "h264" },
		{ "AVC1", "h264" },
		{ "AVC3", "h264" },
		{ "DAVC", "h264" },
		{ "VSSH", "h264" },
		{ "HEVC", "hevc" },
		{ "HVC1", "hevc" },
		{ "HEV1", "hevc" },
		{ "H265", "hevc" },
		{ "X265", "hevc" },
		{ "AV01", "av1" },
		{ "VP80", "vp8" },
		{ "VP08", "vp8" },
		{ "VP90", "vp9" },
		{ "VP09", "vp9" },
		{ "MJPG", "mjpeg" },
		{ "JPEG", "mjpeg" },
		{ "AVRN", "mjpeg" },
		{ "DMB1", "mjpeg" },
		{ "AVDJ", "mjpeg" },
		{ "WMV1", "wmv1" },
		{ "WMV2", "wmv2" },
		{ "WMV3", "wmv3" },
		{ "WVC1", "vc1" },
		{ "WMVA", "vc1" },
		{ "HFYU", "huffyuv" },
		{ "FFVH", "ffvhuff" },
		{ "FFV1", "ffv1" },
		{ "APCH", "prores" },
		{ "APCN", "prores" },
		{ "APCS", "prores" },
		{ "APCO", "prores" },
		{ "AP4H", "prores" },
		{ "AP4X", "prores" },
		{ "CFHD", "cfhd" },
		{ "MJP2", "jpeg2000" },
		{ "AVDN", "dnxhd" },
		{ "AVDH", "dnxhd" },
		{ "S263", "h263" },
		{ "H263", "h263" },
		{ "U263", "h263" },
		{ "FLV1", "flv1" },
		{ "MPG1", "mpeg1video" },
		{ "MP1V", "mpeg1video" },
		{ "MPG2", "mpeg2video" },
		{ "MP2V", "mpeg2video" },
		{ "I420", "rawvideo" },
		{ "IYUV", "rawvideo" },
		{ "YV12", "rawvideo" },
		{ "UYVY", "rawvideo" },
		{ "YUY2", "rawvideo" },
		{ "THEO", "theora" },
	};
	std::string key(fourcc, 4);
	for (auto &c : key)
		if (c >= 'a' && c <= 'z')
			c = char(c - 'a' + 'A');
	const auto it = names.find(key);
	return it == names.end() ? std::string() : it->second;
}

std::string waveCodec(unsigned formatTag, unsigned bits, bool bigEndian)
{
	const char *endian = bigEndian ? "be" : "le";
	switch (formatTag) {
	case 0x0001:
		if (bits == 8)
			return "pcm_u8";
		if (bits == 16 || bits == 24 || bits == 32 || bits == 64)
			return "pcm_s" + std::to_string(bits) + endian;
		return {};
	case 0x0003:
		if (bits == 32 || bits == 64)
			return "pcm_f" + std::to_string(bits) + endian;
		return {};
	case 0x0002:
		return "adpcm_ms";
	case 0x0006:
		return "pcm_alaw";
	case 0x0007:
		return "pcm_mulaw";
	case 0x000A:
		return "wmavoice";
	case 0x0011:
		return "adpcm_ima_wav";
	case 0x0020:
		return "adpcm_yamaha";
	case 0x0031:
		return "gsm_ms";
	case 0x0050:
		return "mp2";
	case 0x0055:
		return "mp3";
	case 0x00FF:
	case 0x1610:
	case 0x4143:
	case 0x706D:
	case 0xA106:
		return "aac";
	case 0x0160:
		return "wmav1";
	case 0x0161:
		return "wmav2";
	case 0x0162:
		return "wmapro";
	case 0x0163:
		return "wmalossless";
	case 0x2000:
		return "ac3";
	case 0x2001:
		return "dts";
	case 0xF1AC:
		return "flac";
	default:
		return {};
	}
}

std::string matroskaCodec(const std::string &id, unsigned bits, const std::string &codecPrivate)
{
	static const std::map<std::string, std::string> names = {
		{ "V_MPEG4/ISO/AVC", "h264" },
		{ "V_MPEGH/ISO/HEVC", "hevc" },
		{ "V_AV1", "av1" },
		{ "V_VP8", "vp8" },
		{ "V_VP9", "vp9" },
		{ "V_MPEG4/ISO/SP", "mpeg4" },
		{ "V_MPEG4/ISO/ASP", "mpeg4" },
		{ "V_MPEG4/ISO/AP", "mpeg4" },
		{ "V_MPEG4/MS/V3", "msmpeg4v3" },
		{ "V_MPEG1", "mpeg1video" },
		{ "V_MPEG2", "mpeg2video" },
		{ "V_THEORA", "theora" },
		{ "V_PRORES", "prores" },
		{ "V_MJPEG", "mjpeg" },
		{ "V_FFV1", "ffv1" },
		{ "V_UNCOMPRESSED", "rawvideo" },
		{ "A_MPEG/L3", "mp3" },
		{ "A_MPEG/L2", "mp2" },
		{ "A_MPEG/L1", "mp1" },
		{ "A_AC3", "ac3" },
		{ "A_EAC3", "eac3" },
		{ "A_TRUEHD", "truehd" },
		{ "A_MLP", "mlp" },
		{ "A_FLAC", "flac" },
		{ "A_OPUS", "opus" },
		{ "A_VORBIS", "vorbis" },
		{ "A_ALAC", "alac" },
		{ "A_WAVPACK4", "wavpack" },
		{ "A_TTA1", "tta" },
	};
	const auto it = names.find(id);
	if (it != names.end())
		return it->second;
	if (id.compare(0, 5, "A_AAC") == 0)
		return "aac";
	if (id.compare(0, 5, "A_DTS") == 0)
		return "dts";
	if (id == "A_PCM/INT/LIT")
		return bits == 8 ? "pcm_u8" : waveCodec(1, bits, false);
	if (id == "A_PCM/INT/BIG")
		return bits == 8 ? "pcm_s8" : waveCodec(1, bits, true);
	if (id == "A_PCM/FLOAT/IEEE")
		return waveCodec(3, bits, false);
	// VfW and ACM compatibility modes carry a BITMAPINFOHEADER or a WAVEFORMATEX.
	if (id == "V_MS/VFW/FOURCC" && codecPrivate.size() >= 20)
		return fourccCodec(codecPrivate.data() + 16);
	if (id == "A_MS/ACM" && codecPrivate.size() >= 16) {
		const auto *p = reinterpret_cast<const uint8_t *>(codecPrivate.data());
		return waveCodec(p[0] | p[1] << 8, p[14] | p[15] << 8, false);
	}
	return {};
}

} // namespace probe
