// SPDX-License-Identifier: GPL-2.0-or-later
// Less common containers: ASF (WMA, WMV), FLV, MPEG program streams (MPG, VOB),
// MPEG transport streams (TS, M2TS) and RealMedia (RM, RMVB).
#include "probe.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace probe
{

namespace
{
const uint8_t kFileProperties[16] = {
	0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11, 0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65
};
const uint8_t kStreamProperties[16] = {
	0x91, 0x07, 0xDC, 0xB7, 0xB7, 0xA9, 0xCF, 0x11, 0x8E, 0xE6, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65
};
const uint8_t kContentDescription[16] = {
	0x33, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C
};
const uint8_t kExtendedContent[16] = {
	0x40, 0xA4, 0xD0, 0xD2, 0x07, 0xE3, 0xD2, 0x11, 0x97, 0xF0, 0x00, 0xA0, 0xC9, 0x5E, 0xA8, 0x50
};
const uint8_t kAudioMedia[16] = {
	0x40, 0x9E, 0x69, 0xF8, 0x4D, 0x5B, 0xCF, 0x11, 0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B
};
const uint8_t kVideoMedia[16] = {
	0xC0, 0xEF, 0x19, 0xBC, 0x4D, 0x5B, 0xCF, 0x11, 0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B
};

std::string utf16le(Reader &r, int64_t p, uint64_t n)
{
	const std::string raw = r.bytes(p, static_cast<size_t>(std::min<uint64_t>(n, kMaxTagRead)));
	return utf16(reinterpret_cast<const uint8_t *>(raw.data()), raw.size(), false);
}
} // namespace

void parseAsf(Reader &r, Result &res)
{
	const uint64_t headerSize = r.le64(16);
	if (headerSize < 30 || headerSize > static_cast<uint64_t>(r.size()))
		throw Malformed{};
	const int64_t end = static_cast<int64_t>(headerSize);
	std::string video, audio;
	bool hasVideo = false;
	for (int64_t p = 30; p + 24 <= end;) {
		uint8_t guid[16];
		r.read(p, guid, 16);
		const uint64_t size = r.le64(p + 16);
		if (size < 24 || size > static_cast<uint64_t>(end - p))
			throw Malformed{};
		const int64_t data = p + 24, objectEnd = p + static_cast<int64_t>(size);
		if (!std::memcmp(guid, kFileProperties, 16) && size >= 104) {
			const double play = static_cast<double>(r.le64(data + 40)) / 1e7,
				     preroll = static_cast<double>(r.le64(data + 56)) / 1e3;
			if (play > preroll)
				res.seconds = play - preroll;
		} else if (!std::memcmp(guid, kStreamProperties, 16) && size >= 78) {
			uint8_t type[16];
			r.read(data, type, 16);
			const int64_t specific = data + 54;
			if (!std::memcmp(type, kAudioMedia, 16) && specific + 16 <= objectEnd &&
				audio.empty())
				audio = waveCodec(r.le16(specific), r.le16(specific + 14), false);
			else if (!std::memcmp(type, kVideoMedia, 16) && specific + 31 <= objectEnd &&
				 !hasVideo) {
				char compression[4];
				r.read(specific + 11 + 16, compression, 4);
				video = fourccCodec(compression);
				hasVideo = true;
			}
		} else if (!std::memcmp(guid, kContentDescription, 16) && size >= 34) {
			const uint16_t title = r.le16(data), author = r.le16(data + 2);
			if (data + 10 + title + author <= objectEnd) {
				setTag(res.title, utf16le(r, data + 10, title));
				setTag(res.artist, utf16le(r, data + 10 + title, author));
			}
		} else if (!std::memcmp(guid, kExtendedContent, 16) && size >= 26) {
			const unsigned count = r.le16(data);
			int64_t q = data + 2;
			for (unsigned i = 0; i < count && i < 1024 && q + 2 <= objectEnd; ++i) {
				const uint16_t nameLength = r.le16(q);
				if (q + 2 + nameLength + 4 > objectEnd)
					break;
				const std::string name = utf16le(r, q + 2, nameLength);
				const uint16_t valueType = r.le16(q + 2 + nameLength),
					       valueLength = r.le16(q + 4 + nameLength);
				const int64_t value = q + 6 + nameLength;
				if (value + valueLength > objectEnd)
					break;
				if (valueType == 0 && name == "WM/AlbumTitle")
					setTag(res.album, utf16le(r, value, valueLength));
				q = value + valueLength;
			}
		}
		p = objectEnd;
	}
	res.codec = hasVideo ? video : audio; // An unknown video codec stays empty.
}

namespace
{
struct Amf
{
	explicit Amf(Reader &reader)
	: r(reader)
	{
	}
	Reader &r;
	int64_t end = 0;
	double duration = -1;
	int video = -1, audio = -1;
	Result tags;
	unsigned values = 0;

	std::string string(int64_t &p, bool longString = false)
	{
		const uint32_t n = longString ? r.be32(p) : r.be16(p);
		p += longString ? 4 : 2;
		if (n > static_cast<uint64_t>(end - p))
			throw Malformed{};
		const std::string s = r.bytes(p, std::min<uint32_t>(n, kMaxTagRead));
		p += n;
		return s;
	}

	// Parses one AMF0 value at p; `key` names it inside an object or array.
	void value(int64_t &p, const std::string &key, int depth)
	{
		if (p >= end || ++values > 4096 || depth > 4)
			throw Malformed{};
		const unsigned type = r.u8(p++);
		switch (type) {
		case 0: { // Number.
			const uint64_t bits = r.be64(p);
			double d;
			std::memcpy(&d, &bits, 8);
			p += 8;
			// Codec ids are small integers; anything else must not reach an int cast.
			const bool id = d >= 0 && d < 256;
			if (key == "duration" && std::isfinite(d))
				duration = d;
			else if (key == "videocodecid" && id)
				video = static_cast<int>(d);
			else if (key == "audiocodecid" && id)
				audio = static_cast<int>(d);
			break;
		}
		case 1:
			++p;
			break; // Boolean.
		case 2:
		case 12: { // String, long string.
			const std::string s = string(p, type == 12);
			if (key == "title")
				setTag(tags.title, s);
			else if (key == "artist")
				setTag(tags.artist, s);
			else if (key == "album")
				setTag(tags.album, s);
			break;
		}
		case 3: // Object.
		case 8: // ECMA array.
			if (type == 8)
				p += 4;
			for (;;) {
				if (r.has(p, 3) && r.be24(p) == 9) {
					p += 3;
					break;
				}
				const std::string name = string(p);
				value(p, name, depth + 1);
			}
			break;
		case 5:
		case 6:
			break; // Null, undefined.
		case 10: { // Strict array. Numbers, such as a keyframe index, are skipped without counting.
			const uint32_t n = r.be32(p);
			p += 4;
			for (uint32_t i = 0; i < n; ++i) {
				if (p < end && r.u8(p) == 0)
					p += 9;
				else
					value(p, std::string(), depth + 1);
			}
			break;
		}
		case 11:
			p += 10;
			break; // Date.
		default:
			throw Malformed{};
		}
	}
};

std::string flvVideo(int id)
{
	switch (id) {
	case 2:
		return "flv1";
	case 4:
		return "vp6f";
	case 5:
		return "vp6a";
	case 7:
		return "h264";
	case 12:
		return "hevc";
	default:
		return {};
	}
}

std::string flvAudio(int id)
{
	switch (id) {
	case 2:
	case 14:
		return "mp3";
	case 3:
		return "pcm_s16le";
	case 7:
		return "pcm_alaw";
	case 8:
		return "pcm_mulaw";
	case 10:
		return "aac";
	case 11:
		return "speex";
	default:
		return {};
	}
}
} // namespace

void parseFlv(Reader &r, Result &res)
{
	const uint32_t offset = r.be32(5);
	if (offset < 9 || offset > r.size())
		throw Malformed{};
	Amf amf(r);
	std::string video, audio;
	bool sawVideo = false;
	int64_t p = int64_t(offset) + 4; // PreviousTagSize0.
	for (unsigned n = 0; n < 64 && p + 11 <= r.size(); ++n) {
		const unsigned type = r.u8(p) & 0x1F;
		const uint32_t size = r.be24(p + 1);
		const int64_t data = p + 11;
		if (!r.has(data, size))
			break;
		if (type == 18 && size > 13) {
			int64_t q = data;
			amf.end = data + size;
			if (r.u8(q++) == 2 && amf.string(q) == "onMetaData")
				amf.value(q, std::string(), 0);
		} else if (type == 9 && size >= 1 && !sawVideo) {
			sawVideo = true;
			const unsigned b = r.u8(data);
			if ((b & 0x80) && size >= 5) { // Enhanced RTMP: a FourCC follows.
				char fourcc[4];
				r.read(data + 1, fourcc, 4);
				video = fourccCodec(fourcc);
			} else {
				video = flvVideo(static_cast<int>(b & 15));
			}
		} else if (type == 8 && size >= 1 && audio.empty()) {
			audio = flvAudio(r.u8(data) >> 4);
		}
		p = data + size + 4;
		if (amf.duration >= 0 && sawVideo && !audio.empty())
			break;
	}
	if (amf.video >= 0 && !sawVideo) {
		sawVideo = true;
		video = flvVideo(amf.video);
	}
	if (audio.empty() && amf.audio >= 0)
		audio = flvAudio(amf.audio);
	res.codec = sawVideo ? video : audio;
	setTag(res.title, amf.tags.title);
	setTag(res.artist, amf.tags.artist);
	setTag(res.album, amf.tags.album);
	if (amf.duration > 0) {
		res.seconds = amf.duration;
	} else if (r.size() >= 15) {
		// No metadata: the last tag's timestamp, found through the final PreviousTagSize.
		const uint32_t last = r.be32(r.size() - 4);
		const int64_t tag = r.size() - 4 - last;
		if (last >= 11 && tag > offset && r.has(tag, 8))
			res.seconds = (r.be24(tag + 4) | uint32_t(r.u8(tag + 7)) << 24) / 1000.0;
	}
}

namespace
{
std::string streamTypeCodec(unsigned type)
{
	switch (type) {
	case 0x01:
		return "mpeg1video";
	case 0x02:
		return "mpeg2video";
	case 0x03:
	case 0x04:
		return "mp2"; // MPEG audio stream types do not distinguish layers.
	case 0x0F:
	case 0x11:
		return "aac";
	case 0x10:
		return "mpeg4";
	case 0x1B:
		return "h264";
	case 0x24:
		return "hevc";
	case 0xEA:
		return "vc1";
	case 0x80:
		return "pcm_bluray";
	case 0x81:
		return "ac3";
	case 0x84: // Blu-ray
	case 0x87: // ATSC
		return "eac3";
	case 0x82:
	case 0x85:
	case 0x86:
		return "dts";
	case 0x83:
		return "truehd";
	default:
		return {};
	}
}

bool isVideoType(unsigned type)
{
	return type == 0x01 || type == 0x02 || type == 0x10 || type == 0x1B || type == 0x24 || type == 0xEA;
}

// 33-bit MPEG timestamp in 90 kHz units from a PES optional header field.
uint64_t pts(Reader &r, int64_t p)
{
	uint8_t b[5];
	r.read(p, b, 5);
	return uint64_t(b[0] >> 1 & 7) << 30 | uint64_t(b[1]) << 22 | uint64_t(b[2] >> 1) << 15 |
	       uint64_t(b[3]) << 7 | b[4] >> 1;
}

double span(uint64_t first, uint64_t last)
{
	const uint64_t wrap = 1ULL << 33;
	return double((last + wrap - first) % wrap) / 90000.0;
}
} // namespace

void parseMpegTs(Reader &r, int64_t packet, Result &res)
{
	const int64_t lead = packet == 192 ? 4 : 0;
	int pmtPid = -1, videoPid = -1, audioPid = -1;
	unsigned videoType = 0, audioType = 0;
	// Program tables from the first packets.
	for (int64_t p = lead; p + 188 <= r.size() && p < 4 * 1024 * 1024 && (videoPid < 0 && audioPid < 0);
		p += packet) {
		if (r.u8(p) != 0x47)
			throw Malformed{};
		const unsigned flags = r.u8(p + 1);
		const unsigned pid = (flags & 0x1F) << 8 | r.u8(p + 2);
		if (!(flags & 0x40))
			continue; // Tables start only at payload unit starts.
		int64_t q = p + 4;
		const unsigned adaptation = (r.u8(p + 3) >> 4) & 3;
		if (adaptation == 2)
			continue;
		if (adaptation == 3)
			q += 1 + r.u8(q);
		if (q >= p + 188)
			continue;
		q += 1 + r.u8(q); // Pointer field.
		if (q + 8 > p + 188)
			continue;
		const unsigned table = r.u8(q);
		const unsigned length = (r.be16(q + 1) & 0x0FFF);
		const int64_t sectionEnd = std::min<int64_t>(q + 3 + length - 4, p + 188); // Without CRC.
		if (pid == 0 && table == 0x00) {
			for (int64_t e = q + 8; e + 4 <= sectionEnd; e += 4)
				if (r.be16(e)) { // Program 0 is the network PID.
					pmtPid = r.be16(e + 2) & 0x1FFF;
					break;
				}
		} else if (static_cast<int>(pid) == pmtPid && table == 0x02 && q + 12 <= sectionEnd) {
			int64_t e = q + 12 + (r.be16(q + 10) & 0x0FFF);
			// Blu-ray stream types count only with an HDMV registration, as in FFmpeg.
			bool hdmv = false;
			for (int64_t d = q + 12; d + 6 <= e && d + 6 <= sectionEnd; d += 2 + r.u8(d + 1))
				if (r.u8(d) == 0x05 &&
					(r.match(d + 2, "HDMV", 4) || r.match(d + 2, "HDPR", 4)))
					hdmv = true;
			while (e + 5 <= sectionEnd) {
				const unsigned type = r.u8(e), esPid = r.be16(e + 1) & 0x1FFF,
					       info = r.be16(e + 3) & 0x0FFF;
				const bool bluray = type == 0x80 || (type >= 0x82 && type <= 0x86);
				unsigned effective = bluray && !hdmv ? 0 : type;
				if (type == 0x06) { // Private data: AC-3/E-AC-3/DTS descriptors.
					for (int64_t d = e + 5; d + 2 <= e + 5 + info && d + 2 <= sectionEnd;
						d += 2 + r.u8(d + 1)) {
						const unsigned tag = r.u8(d);
						effective = tag == 0x6A   ? 0x81
							    : tag == 0x7A ? 0x87
							    : tag == 0x7B ? 0x82
									  : effective;
					}
				}
				if (isVideoType(effective) && videoPid < 0) {
					videoPid = static_cast<int>(esPid);
					videoType = effective;
				} else if (!streamTypeCodec(effective).empty() && audioPid < 0 &&
					   !isVideoType(effective)) {
					audioPid = static_cast<int>(esPid);
					audioType = effective;
				}
				e += 5 + info;
			}
		}
	}
	if (videoPid < 0 && audioPid < 0)
		throw Malformed{};
	res.codec = streamTypeCodec(videoPid >= 0 ? videoType : audioType);
	const int timingPid = videoPid >= 0 ? videoPid : audioPid;
	// Duration: first and last PTS of the main stream, near the start and the end.
	auto ptsAt = [&](int64_t p) -> int64_t {
		if (r.u8(p) != 0x47)
			return -1;
		const unsigned flags = r.u8(p + 1);
		if (!(flags & 0x40) || static_cast<int>((flags & 0x1F) << 8 | r.u8(p + 2)) != timingPid)
			return -1;
		int64_t q = p + 4;
		const unsigned adaptation = (r.u8(p + 3) >> 4) & 3;
		if (adaptation == 2)
			return -1;
		if (adaptation == 3)
			q += 1 + r.u8(q);
		if (q + 14 > p + 188 || !r.match(q, "\0\0\1", 3) || !(r.u8(q + 7) & 0x80))
			return -1;
		return static_cast<int64_t>(pts(r, q + 9));
	};
	int64_t first = -1, last = -1;
	for (int64_t p = lead; first < 0 && p + 188 <= r.size() && p < 8 * 1024 * 1024; p += packet)
		first = ptsAt(p);
	const int64_t count = (r.size() - lead - 188) / packet + 1; // Whole packets.
	for (int64_t i = count - 1; last < 0 && i >= 0 && i > count - 65536; --i)
		last = ptsAt(lead + i * packet);
	if (first >= 0 && last >= 0)
		res.seconds = span(static_cast<uint64_t>(first), static_cast<uint64_t>(last));
}

void parseMpegPs(Reader &r, Result &res)
{
	// Stream codecs and the first PTS from the start, the last PTS from the end.
	bool video = false;
	std::string audio;
	int64_t first = -1, last = -1;
	auto scan = [&](int64_t from, int64_t to, bool start) {
		for (int64_t p = from; p + 14 <= to;) {
			if (!r.match(p, "\0\0\1", 3)) {
				++p;
				continue;
			}
			const unsigned id = r.u8(p + 3);
			if (id == 0xBA) { // Pack header: MPEG-2 has a stuffing length, MPEG-1 is fixed.
				p += (r.u8(p + 4) & 0xC0) == 0x40 ? 14 + (r.u8(p + 13) & 7) : 12;
				continue;
			}
			if (id <= 0xB9) { // Elementary stream start codes and the end code carry no length.
				p += 4;
				continue;
			}
			const unsigned length = r.be16(p + 4);
			const bool isVideo = id >= 0xE0 && id <= 0xEF;
			const bool isAudio = (id >= 0xC0 && id <= 0xDF) || id == 0xBD;
			if ((isVideo || isAudio) && r.has(p + 6, 3)) {
				int64_t ts = -1;
				int64_t payload = p + 6;
				if ((r.u8(p + 6) & 0xC0) == 0x80) { // MPEG-2 PES header.
					if ((r.u8(p + 7) & 0x80) && r.has(p + 9, 5))
						ts = static_cast<int64_t>(pts(r, p + 9));
					payload = p + 9 + r.u8(p + 8);
				} else { // MPEG-1: stuffing, optional STD buffer, then the timestamp.
					int64_t q = p + 6;
					while (q < p + 6 + 16 && r.has(q, 1) && r.u8(q) == 0xFF)
						++q;
					if (r.has(q, 1) && (r.u8(q) & 0xC0) == 0x40)
						q += 2;
					if (r.has(q, 5) && (r.u8(q) & 0xE0) == 0x20)
						ts = static_cast<int64_t>(pts(r, q));
				}
				if (isVideo && !video && start)
					video = true;
				if (start && isAudio && audio.empty() && r.has(payload, 1)) {
					// Private stream 1: the substream ID gives AC-3, DTS or LPCM.
					const unsigned sub = r.u8(payload) & 0xF8;
					audio = id != 0xBD    ? "mp2"
						: sub == 0x80 ? "ac3"
						: sub == 0x88 ? "dts"
						: sub == 0xA0 ? "pcm_dvd"
							      : "";
				}
				if (ts >= 0 && (isVideo || !video)) {
					if (start && first < 0)
						first = ts;
					if (!start)
						last = ts;
				}
			}
			p += 6 + length;
		}
	};
	scan(0, std::min<int64_t>(r.size(), 1024 * 1024), true);
	// MPEG-1 or MPEG-2 video: the sequence extension start code (00 00 01 B5) only exists in MPEG-2.
	// Without a sequence header (00 00 01 B3) it is another codec, such as H.264.
	if (video) {
		const std::string head =
			r.bytes(0, static_cast<size_t>(std::min<int64_t>(r.size(), 1024 * 1024)));
		const bool mpeg = head.find(std::string("\0\0\1\xB3", 4)) != std::string::npos;
		const bool mpeg2 = head.find(std::string("\0\0\1\xB5", 4)) != std::string::npos;
		res.codec = !mpeg ? "" : mpeg2 ? "mpeg2video" : "mpeg1video";
	} else {
		res.codec = audio;
	}
	scan(std::max<int64_t>(0, r.size() - 1024 * 1024), r.size(), false);
	if (first >= 0 && last >= 0)
		res.seconds = span(static_cast<uint64_t>(first), static_cast<uint64_t>(last));
}

namespace
{
std::string realCodec(const std::string &tag)
{
	static const char *const map[][2] = {
		{ "RV10", "rv10" },
		{ "RV20", "rv20" },
		{ "RV30", "rv30" },
		{ "RV40", "rv40" },
		{ "RV60", "rv60" },
		{ "cook", "cook" },
		{ "28_8", "ra_288" },
		{ "atrc", "atrac3" },
		{ "sipr", "sipr" },
		{ "dnet", "ac3" },
		{ "raac", "aac" },
		{ "racp", "aac" },
		{ "LSD:", "ralf" },
		{ "lpcJ", "ra_144" },
	};
	for (const auto &m : map)
		if (tag == m[0])
			return m[1];
	return {};
}
} // namespace

// RealMedia: PROP has the duration, MDPR the streams and CONT the tags. The walk stops at
// the first DATA chunk, so packets are never read.
void parseRealMedia(Reader &r, Result &res)
{
	std::string video, audio;
	int64_t p = 0;
	for (unsigned guard = 0; r.has(p, 10) && guard < 4096; ++guard) {
		char id[4];
		r.read(p, id, 4);
		// FFmpeg's DATA size can run past the end of the file; it is never read.
		if (!std::memcmp(id, "DATA", 4) || !std::memcmp(id, "INDX", 4))
			break;
		const uint32_t size = r.be32(p + 4);
		if (size < 10 || !r.has(p, size))
			throw Malformed{};
		const int64_t d = p + 10, end = p + size; // Data after id, size and object version.
		// Length-prefixed strings; each must stay inside its chunk.
		auto prefixed = [&](int64_t &q, unsigned prefix) {
			const unsigned n = prefix == 1 ? r.u8(q) : r.be16(q);
			q += prefix;
			if (static_cast<int64_t>(n) > end - q)
				throw Malformed{};
			std::string s = r.bytes(q, std::min<size_t>(n, kMaxTagRead));
			q += n;
			return s;
		};
		if (!std::memcmp(id, "PROP", 4) && size >= 50) {
			res.seconds = r.be32(d + 20) / 1000.0;
		} else if (!std::memcmp(id, "MDPR", 4) && size >= 41) {
			int64_t q =
				d + 30; // Stream number, bit rates, packet sizes, start, preroll, duration.
			prefixed(q, 1); // Stream name.
			const std::string mime = prefixed(q, 1);
			if (q + 4 > end)
				throw Malformed{};
			const uint32_t length = r.be32(q);
			q += 4;
			if (length > end - q)
				throw Malformed{};
			if (mime == "video/x-pn-realvideo" && length >= 12 && r.match(q + 4, "VIDO", 4)) {
				if (video.empty())
					video = realCodec(r.bytes(q + 8, 4));
			} else if (mime == "audio/x-pn-realaudio" && length >= 6 &&
				   r.match(q, ".ra\xFD", 4) && audio.empty()) {
				const unsigned version = r.be16(q + 4);
				if (version == 3) {
					audio = "ra_144";
				} else if (version == 4 && length >= 57) {
					int64_t o = q + 56;
					prefixed(o, 1); // Interleaver.
					audio = realCodec(prefixed(o, 1));
				} else if (version == 5 && length >= 70) {
					audio = realCodec(r.bytes(q + 66, 4));
				}
			}
		} else if (!std::memcmp(id, "CONT", 4)) {
			int64_t q = d;
			setTag(res.title, prefixed(q, 2));
			setTag(res.artist, prefixed(q, 2));
		}
		p = end;
	}
	res.codec = !video.empty() ? video : audio;
}

} // namespace probe
