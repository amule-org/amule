// SPDX-License-Identifier: GPL-2.0-or-later
// Isolated media parser: input is an inherited, seekable regular file on stdin.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <clocale>
#include <iostream>
#include <locale>
#include <map>
#include <string>
#include <vector>
#include <type_traits>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define MI_CALL __stdcall
#else
#include <sys/resource.h>
#include <sys/types.h>
static_assert(sizeof(off_t) >= sizeof(int64_t), "Media probe requires 64-bit file offsets");
#define MI_CALL
#endif
extern "C" {
// MediaInfoLib's stable wide-character C ABI; no C++/ZenLib ABI dependency.
void *MI_CALL MediaInfo_New();
void MI_CALL MediaInfo_Delete(void *);
size_t MI_CALL MediaInfo_Open_Buffer_Init(void *, uint64_t, uint64_t);
size_t MI_CALL MediaInfo_Open_Buffer_Continue(void *, const unsigned char *, size_t);
uint64_t MI_CALL MediaInfo_Open_Buffer_Continue_GoTo_Get(void *);
size_t MI_CALL MediaInfo_Open_Buffer_Finalize(void *);
const wchar_t *MI_CALL MediaInfo_Get(void *, int, size_t, const wchar_t *, int, int);
size_t MI_CALL MediaInfo_Count_Get(void *, int, size_t);
const wchar_t *MI_CALL MediaInfo_Option(void *, const wchar_t *, const wchar_t *);
}

struct Result
{
	uint32_t length = 0, bitrate = 0;
	std::string codec, artist, album, title;
	bool usable() const { return length || !codec.empty(); }
};
static int seek(int64_t offset, int whence)
{
#ifdef _WIN32
	return _fseeki64(stdin, offset, whence);
#else
	return fseeko(stdin, offset, whence);
#endif
}
static int64_t tell()
{
#ifdef _WIN32
	return _ftelli64(stdin);
#else
	return ftello(stdin);
#endif
}
static std::string utf8(const wchar_t *value)
{
	if (!value)
		return {};
	std::string out;
	while (*value) {
		uint32_t c = static_cast<uint32_t>(static_cast<std::make_unsigned_t<wchar_t>>(*value++));
#ifdef _WIN32
		if (c >= 0xd800 && c <= 0xdbff && *value >= 0xdc00 && *value <= 0xdfff)
			c = 0x10000 + ((c - 0xd800) << 10) +
			    (static_cast<uint32_t>(static_cast<std::make_unsigned_t<wchar_t>>(*value++)) -
				    0xdc00);
#endif
		if ((c >= 0xd800 && c <= 0xdfff) || c > 0x10ffff)
			c = 0xfffd;
		const size_t width = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
		if (out.size() + width > 4096)
			break;
		if (c < 0x80)
			out += char(c);
		else if (c < 0x800) {
			out += char(0xc0 | (c >> 6));
			out += char(0x80 | (c & 63));
		} else if (c < 0x10000) {
			out += char(0xe0 | (c >> 12));
			out += char(0x80 | ((c >> 6) & 63));
			out += char(0x80 | (c & 63));
		} else {
			out += char(0xf0 | (c >> 18));
			out += char(0x80 | ((c >> 12) & 63));
			out += char(0x80 | ((c >> 6) & 63));
			out += char(0x80 | (c & 63));
		}
		if (out.size() >= 4096)
			break;
	}
	return out;
}
static uint32_t numeric(const std::string &text, double divisor, bool round)
{
	char *end = nullptr;
	double value = std::strtod(text.c_str(), &end) / divisor;
	if (end == text.c_str() || *end || !std::isfinite(value) || value <= 0)
		return 0;
	value = round ? std::floor(value + 0.5) : std::floor(value);
	return static_cast<uint32_t>(std::min(value, double(UINT32_MAX)));
}
static std::string get(void *h, int kind, size_t index, const wchar_t *field)
{
	return utf8(MediaInfo_Get(h, kind, index, field, 1, 0));
}
static std::string miCodec(void *h, int kind, size_t i)
{
	const std::string format = get(h, kind, i, L"Format");
	const std::string id = get(h, kind, i, L"CodecID");
	// A format family can contain incompatible codecs with distinct wire names.
	if (format == "MPEG-4 Visual") {
		if (id == "MPG4" || id == "MP41")
			return "msmpeg4v1";
		if (id == "MP42")
			return "msmpeg4v2";
		if (id == "MP43" || id == "DIV3" || id == "DIV4" || id == "DIV5" || id == "DIV6")
			return "msmpeg4v3";
	}
	if (format == "VC-1" && id == "WMV3")
		return "wmv3";
	if (format == "HuffYUV")
		return id == "FFVH" ? "ffvhuff" : "huffyuv";
	if (format == "AMR") {
		const auto profile = get(h, kind, i, L"Format_Profile");
		if (profile == "Narrow band")
			return "amr_nb";
		if (profile == "Wide band")
			return "amr_wb";
	}
	if (format == "GSM 6.10" && get(h, 0, 0, L"Format") == "Wave" && id == "31")
		return "gsm_ms";
	if (format == "ADPCM") {
		const auto profile = get(h, kind, i, L"Format_Profile");
		if (profile == "A-Law")
			return "pcm_alaw";
		if (profile == "U-Law")
			return "pcm_mulaw";
		if (id == "ima4")
			return "adpcm_ima_qt";
		// Numeric CodecID values are hexadecimal WAVE format identifiers.
		if (get(h, 0, 0, L"Format") == "Wave") {
			if (id == "11")
				return "adpcm_ima_wav";
			if (id == "2")
				return "adpcm_ms";
			if (id == "20")
				return "adpcm_yamaha";
		}
	}
	if (format == "YUV" && (id == "I420" || id == "IYUV" || id == "YV12" || id == "UYVY" || id == "YUY2"))
		return "rawvideo";
	static const std::map<std::string, std::string> names = { { "AVC", "h264" },
		{ "HEVC", "hevc" },
		{ "AV1", "av1" },
		{ "VP8", "vp8" },
		{ "VP9", "vp9" },
		{ "MPEG-4 Visual", "mpeg4" },
		{ "xvid", "mpeg4" },
		{ "XviD", "mpeg4" },
		{ "DivX", "mpeg4" },
		{ "H.263", "h263" },
		{ "ProRes", "prores" },
		{ "MLP FBA", "truehd" },
		{ "TrueHD", "truehd" },
		{ "AAC", "aac" },
		{ "FLAC", "flac" },
		{ "Opus", "opus" },
		{ "Vorbis", "vorbis" },
		{ "Speex", "speex" },
		{ "AC-3", "ac3" },
		{ "E-AC-3", "eac3" },
		{ "DTS", "dts" },
		{ "Theora", "theora" },
		{ "FFV1", "ffv1" },
		{ "ALAC", "alac" },
		{ "WavPack", "wavpack" },
		{ "TTA", "tta" },
		{ "Monkey's Audio", "ape" },
		{ "Musepack SV7", "musepack7" },
		{ "Musepack SV8", "musepack8" },
		{ "MLP", "mlp" },
		{ "CineForm", "cfhd" },
		{ "JPEG 2000", "jpeg2000" },
		{ "VC-3", "dnxhd" },
		{ "Sorenson Spark", "flv1" },
		{ "VC-1", "vc1" },
		{ "WMV2", "wmv2" },
		{ "WMV1", "wmv1" },
		{ "JPEG", "mjpeg" } };
	auto it = names.find(format);
	if (it != names.end())
		return it->second;
	if (format == "WMA") {
		const auto profile = get(h, kind, i, L"Format_Profile");
		const auto version = get(h, kind, i, L"Format_Version");
		if (profile.find("Lossless") != std::string::npos)
			return "wmalossless";
		if (profile.find("Pro") != std::string::npos)
			return "wmapro";
		if (profile.find("Voice") != std::string::npos)
			return "wmavoice";
		return version == "Version 1" ? "wmav1" : "wmav2";
	}
	if (format == "MPEG Audio") {
		auto profile = get(h, kind, i, L"Format_Profile");
		if (profile.find("Layer 3") != std::string::npos)
			return "mp3";
		if (profile.find("Layer 2") != std::string::npos)
			return "mp2";
		if (profile.find("Layer 1") != std::string::npos)
			return "mp1";
	}
	if (format == "MPEG Video") {
		auto version = get(h, kind, i, L"Format_Version");
		if (version.find('2') != std::string::npos)
			return "mpeg2video";
		if (version.find('1') != std::string::npos)
			return "mpeg1video";
	}
	if (format == "PCM") {
		auto bits = get(h, kind, i, L"BitDepth");
		auto sign = get(h, kind, i, L"Format_Settings_Sign");
		auto endian = get(h, kind, i, L"Format_Settings_Endianness");
		if (sign.empty() && (get(h, kind, i, L"Format_Profile") == "Float" || id == "fl32" ||
					    id == "FL32" || id == "fl64" || id == "FL64"))
			sign = "Float";
		if (sign == "Float" && endian.empty() && get(h, 0, 0, L"Format") == "Wave") {
			// MediaInfo omits float WAV endianness. Read the RIFF signature,
			// including the big-endian RIFX variant, rather than guessing it.
			const auto position = tell();
			unsigned char header[12];
			if (position >= 0 && !seek(0, SEEK_SET)) {
				if (std::fread(header, 1, sizeof(header), stdin) == sizeof(header) &&
					!std::memcmp(header + 8, "WAVE", 4)) {
					if (!std::memcmp(header, "RIFX", 4))
						endian = "Big";
					else if (!std::memcmp(header, "RIFF", 4) ||
						 !std::memcmp(header, "RF64", 4) ||
						 !std::memcmp(header, "BW64", 4))
						endian = "Little";
				}
				if (seek(position, SEEK_SET))
					return {};
			}
		}
		if (sign.empty() && get(h, 0, 0, L"Format") == "AIFF")
			sign = "Signed";
		if (bits == "8" && sign == "Unsigned")
			return "pcm_u8";
		if (bits == "8" && sign == "Signed")
			return "pcm_s8";
		if ((bits == "16" || bits == "24" || bits == "32" || bits == "64") &&
			(endian == "Little" || endian == "Big") &&
			(sign == "Signed" || sign == "Unsigned" || sign == "Float"))
			return std::string("pcm_") +
			       (sign == "Float"           ? "f"
				       : sign == "Signed" ? "s"
							  : "u") +
			       bits + (endian == "Little" ? "le" : "be");
	}
	return {}; // Unknown vocabulary is explicit missing data.
}
static bool streamTags(const std::string &codec)
{
	return codec == "vorbis" || codec == "opus" || codec == "flac" || codec == "speex";
}
static void miTags(void *h, int kind, size_t index, Result &r)
{
	auto first = [&](std::initializer_list<const wchar_t *> fields) {
		for (auto field : fields) {
			auto s = get(h, kind, index, field);
			if (!s.empty())
				return s;
		}
		return std::string{};
	};
	r.artist = first({ L"Performer", L"Artist", L"ARTIST", L"artist" });
	r.album = first({ L"Album", L"ALBUM", L"album" });
	r.title = first({ L"Title", L"Track", L"TITLE" });
	if (kind == 0 && (get(h, 0, 0, L"Format") == "Wave" || get(h, 0, 0, L"Format") == "AVI")) {
		if (r.artist.empty())
			r.artist = get(h, 0, 0, L"Director");
		if (r.album.empty())
			r.album = get(h, 0, 0, L"OriginalSourceForm/Name");
	}
}
static int mediaInfo(Result &r, int64_t size, bool full = false, unsigned seekCount = 0)
{
	void *h = MediaInfo_New();
	if (!h)
		return 12;
	struct Owner
	{
		void *h;
		~Owner() { MediaInfo_Delete(h); }
	} owner{ h };
	MediaInfo_Option(h, L"CarriageReturnReplace", L"");
	MediaInfo_Option(h, L"ParseSpeed", full ? L"1" : L"0.5");
	if (seek(0, SEEK_SET))
		return 12;
	MediaInfo_Open_Buffer_Init(h, size, 0);
	unsigned char buffer[64 * 1024];
	for (;;) {
		const size_t n = std::fread(buffer, 1, sizeof(buffer), stdin);
		if (!n)
			break;
		const size_t status = MediaInfo_Open_Buffer_Continue(h, buffer, n);
		uint64_t next = MediaInfo_Open_Buffer_Continue_GoTo_Get(h);
		if (next != UINT64_MAX) {
			// Forward seeks make progress through the bounded file. Count only
			// repeated or backward seeks across both parses to stop seek loops.
			const auto position = tell();
			if (position < 0)
				return 12;
			if (next > static_cast<uint64_t>(size))
				return 11;
			if (next <= static_cast<uint64_t>(position) && ++seekCount > 64)
				return 11;
			if (seek(static_cast<int64_t>(next), SEEK_SET))
				return 11;
			MediaInfo_Open_Buffer_Init(h, size, next);
		} else if (status & 8) {
			// A completed stream can still request a seek to trailing tags.
			// Honour that request before accepting the finished flag (e.g. TTA).
			break;
		}
	}
	if (std::ferror(stdin))
		return 12;
	MediaInfo_Open_Buffer_Finalize(h);
	r.length = numeric(get(h, 0, 0, L"Duration"), 1000, true); // C API milliseconds.
	r.bitrate = numeric(get(h, 0, 0, L"OverallBitRate"), 1000, false);
	// A fast raw AVC parse can expose a duration for only the inspected frames.
	// Neither a nonzero value nor VUI frame rate proves a complete timeline.
	// Keep codec/tags, but do not publish this partial duration or derived bitrate.
	const bool rawAvc = get(h, 0, 0, L"Format") == "AVC";
	if (rawAvc) {
		r.length = 0;
		r.bitrate = 0;
	}
	miTags(h, 0, 0, r);
	const size_t videos = MediaInfo_Count_Get(h, 1, size_t(-1));
	const size_t audios = MediaInfo_Count_Get(h, 2, size_t(-1));
	// Images are Stream_Image, not video. Prefer first actual video, then audio.
	if (videos)
		r.codec = miCodec(h, 1, 0);
	else if (audios)
		r.codec = miCodec(h, 2, 0);
	if (!videos && audios == 1 && streamTags(r.codec)) {
		Result tags;
		miTags(h, 2, 0, tags);
		if (r.artist.empty())
			r.artist = tags.artist;
		if (r.album.empty())
			r.album = tags.album;
		if (r.title.empty())
			r.title = tags.title;
	}
	// Full scans are bounded by size and the parent's process watchdog.
	if (!r.length && !rawAvc && !full && size <= 512LL * 1024 * 1024) {
		Result retry;
		int rc = mediaInfo(retry, size, true, seekCount);
		if (rc == 0 || rc == 10) {
			// The full pass can recover fields independently, and may omit
			// fields found during the fast pass. Merge what it found even when
			// it still has no duration or codec of its own.
			if (retry.length)
				r.length = retry.length;
			if (retry.bitrate)
				r.bitrate = retry.bitrate;
			if (!retry.codec.empty())
				r.codec = retry.codec;
			if (!retry.artist.empty())
				r.artist = retry.artist;
			if (!retry.album.empty())
				r.album = retry.album;
			if (!retry.title.empty())
				r.title = retry.title;
		} else if (rc && !r.usable())
			return rc;
	}
	return r.usable() ? 0 : 10;
}
static void quoted(const std::string &s, bool json = true)
{
	std::cout << '"';
	// utf8() already validates and bounds complete code points.
	for (unsigned char c : s) {
		if (c == '\\' || c == '"')
			std::cout << '\\' << c;
		else if (c == '\n')
			std::cout << "\\n";
		else if (c == '\r')
			std::cout << "\\r";
		else if (c == '\t')
			std::cout << (json ? "\\t" : "\t");
		else if (c >= 32 && c != 127)
			std::cout << c;
	}
	std::cout << '"';
}
int main(int argc, char **argv)
{
	std::setlocale(LC_NUMERIC, "C");
#ifdef _WIN32
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
#else
	rlimit noCore{ 0, 0 };
	setrlimit(RLIMIT_CORE, &noCore);
	rlimit cpu{ 25, 25 };
	setrlimit(RLIMIT_CPU, &cpu);
#if !defined(__SANITIZE_ADDRESS__)
	// ASan reserves a large shadow address space; ordinary builds have a 1 GiB cap.
#if defined(__has_feature)
#if !__has_feature(address_sanitizer)
	rlimit memory{ 1024ULL * 1024 * 1024, 1024ULL * 1024 * 1024 };
	setrlimit(RLIMIT_AS, &memory);
#endif
#else
	rlimit memory{ 1024ULL * 1024 * 1024, 1024ULL * 1024 * 1024 };
	setrlimit(RLIMIT_AS, &memory);
#endif
#endif
#endif
	bool json = false;
	for (int i = 1; i < argc; ++i) {
		std::string arg = argv[i];
		if (arg == "--json")
			json = true;
		else if (arg == "--version") {
			std::cout << "amule-media-probe 1; "
				  << utf8(MediaInfo_Option(nullptr, L"Info_Version", L"")) << '\n';
			return 0;
		} else
			return 12;
	}
	if (seek(0, SEEK_END))
		return 12;
	const auto size = tell();
	if (size < 0 || seek(0, SEEK_SET))
		return 12;
	Result r;
	int rc = 12;
	try {
		rc = mediaInfo(r, size);
	} catch (...) {
		return 12;
	}
	if (rc)
		return rc;
	if (json) {
		std::cout << "{\"backend\":";
		quoted("mediainfo");
		std::cout << ",\"length_seconds\":" << r.length << ",\"bitrate_kbps\":" << r.bitrate;
		for (const auto &item : { std::make_pair("codec", r.codec),
			     { "artist", r.artist },
			     { "album", r.album },
			     { "title", r.title } }) {
			std::cout << ",\"" << item.first << "\":";
			quoted(item.second);
		}
		std::cout << "}\n";
	} else {
		// Versioned, fixed flat vocabulary reuses the core's tested escaping parser.
		std::cout << "AMMP 1\nformat.duration=" << r.length
			  << "\nformat.bit_rate=" << uint64_t(r.bitrate) * 1000 << '\n';
		std::cout << "streams.stream.0.codec_type=\"audio\"\nstreams.stream.0.codec_name=";
		quoted(r.codec);
		std::cout << '\n';
		for (const auto &item :
			{ std::make_pair("artist", r.artist), { "album", r.album }, { "title", r.title } }) {
			std::cout << "format.tags." << item.first << '=';
			quoted(item.second, false);
			std::cout << '\n';
		}
		std::cout << "END\n";
	}
	return 0;
}
