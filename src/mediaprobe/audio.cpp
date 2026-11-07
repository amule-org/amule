// SPDX-License-Identifier: GPL-2.0-or-later
// Audio-only formats: MPEG audio (MP1/MP2/MP3), ADTS AAC, FLAC, TTA, WavPack and AMR.
#include "probe.h"

#include <algorithm>
#include <cstring>

namespace probe
{

namespace
{
// Frame scans read only headers, but touch the whole file; larger files use an estimate.
constexpr int64_t kMaxScan = 512LL * 1024 * 1024;
// After damage, how far a frame count looks for the next frame. Damage it cannot cross leaves
// the duration unknown, since the count would be short, unless no frame follows in the last
// kResync bytes either: that is trailing junk, and the count stands.
constexpr int64_t kResync = 64 * 1024;

struct MpegFrame
{
	unsigned version = 0; // 1, 2 or 25 (MPEG 2.5)
	unsigned layer = 0, bitrate = 0, rate = 0, samples = 0, length = 0;
	bool mono = false;
};

bool mpegFrame(uint32_t h, MpegFrame &f)
{
	static const unsigned short kbps[2][3][15] = {
		{ { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448 },
			{ 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 },
			{ 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 } },
		{ { 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256 },
			{ 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 },
			{ 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 } }
	};
	static const unsigned rates[3] = { 44100, 48000, 32000 };
	if ((h & 0xFFE00000) != 0xFFE00000)
		return false;
	const unsigned v = (h >> 19) & 3, l = (h >> 17) & 3, bi = (h >> 12) & 15, si = (h >> 10) & 3;
	if (v == 1 || l == 0 || bi == 0 || bi == 15 || si == 3)
		return false;
	f.version = v == 3 ? 1 : v == 2 ? 2 : 25;
	f.layer = 4 - l;
	f.bitrate = kbps[f.version == 1 ? 0 : 1][f.layer - 1][bi];
	f.rate = rates[si] >> (f.version == 1 ? 0 : f.version == 2 ? 1 : 2);
	f.samples = f.layer == 1 ? 384 : (f.layer == 3 && f.version != 1) ? 576 : 1152;
	const unsigned padding = (h >> 9) & 1;
	// Layer I frames are counted in 4-byte slots.
	f.length = f.layer == 1 ? (12 * f.bitrate * 1000 / f.rate + padding) * 4
				: f.samples / 8 * f.bitrate * 1000 / f.rate + padding;
	f.mono = ((h >> 6) & 3) == 3;
	return f.length >= 4;
}

bool same(const MpegFrame &a, const MpegFrame &b)
{
	return a.version == b.version && a.layer == b.layer && a.rate == b.rate;
}

// First frame in [p, limit) whose successor is also a compatible frame.
int64_t findMpegFrame(Reader &r, int64_t p, int64_t limit, int64_t end, MpegFrame &f)
{
	for (; p + 4 <= limit; ++p) {
		if (r.u8(p) != 0xFF || !mpegFrame(r.be32(p), f))
			continue;
		const int64_t next = p + f.length;
		MpegFrame g;
		if (next + 4 > end || (mpegFrame(r.be32(next), g) && same(f, g)))
			return p;
	}
	return -1;
}

struct AdtsFrame
{
	unsigned rate = 0, length = 0, samples = 0;
};

bool adtsFrame(Reader &r, int64_t p, AdtsFrame &f)
{
	static const unsigned rates[13] = {
		96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350
	};
	if (!r.has(p, 7))
		return false;
	uint8_t b[7];
	r.read(p, b, 7);
	if (b[0] != 0xFF || (b[1] & 0xF6) != 0xF0)
		return false;
	const unsigned si = (b[2] >> 2) & 15;
	f.length = (b[3] & 3) << 11 | b[4] << 3 | b[5] >> 5;
	if (si >= 13 || f.length < 7)
		return false;
	f.rate = rates[si];
	f.samples = 1024 * ((b[6] & 3) + 1);
	return true;
}

// First frame in [p, limit) at this rate whose successor is also one, or which ends the file.
int64_t findAdtsFrame(Reader &r, int64_t p, int64_t limit, int64_t end, unsigned rate, AdtsFrame &f)
{
	for (; p + 7 <= limit; ++p) {
		AdtsFrame g;
		if (adtsFrame(r, p, f) && f.rate == rate && p + f.length <= end &&
			(p + f.length + 7 > end || (adtsFrame(r, p + f.length, g) && g.rate == rate)))
			return p;
	}
	return -1;
}
} // namespace

void parseMpegAudio(Reader &r, int64_t start, Result &res)
{
	const int64_t end = trailingTags(r, res);
	MpegFrame f;
	const int64_t first = findMpegFrame(r, start, std::min(end, start + 65536), end, f);
	if (first < 0)
		throw Malformed{};
	res.codec = f.layer == 3 ? "mp3" : f.layer == 2 ? "mp2" : "mp1";
	// Xing/Info and VBRI headers carry the frame count of VBR files, and the bytes it covers.
	const int64_t xing = first + 4 + (f.version == 1 ? (f.mono ? 17 : 32) : (f.mono ? 9 : 17));
	uint32_t frames = 0, bytes = 0;
	if ((r.match(xing, "Xing", 4) || r.match(xing, "Info", 4)) && r.has(xing, 12) &&
		(r.be32(xing + 4) & 1)) {
		frames = r.be32(xing + 8);
		if ((r.be32(xing + 4) & 2) && r.has(xing + 12, 4))
			bytes = r.be32(xing + 12);
	} else if (r.match(first + 36, "VBRI", 4) && r.has(first + 36, 18)) {
		bytes = r.be32(first + 36 + 10);
		frames = r.be32(first + 36 + 14);
	}
	// Joined files keep the first part's header, so a much larger stream is counted, as in FFmpeg.
	if (frames && !(bytes && end - first > int64_t(bytes) + bytes / 16)) {
		res.seconds = double(frames) * f.samples / f.rate;
		return;
	}
	if (end - first > kMaxScan)
		return; // No reliable duration without a full scan.
	// No header: count frames. Mid-file damage shows as a frame pair in the last kResync bytes.
	auto pairNearEnd = [&](int64_t from) {
		MpegFrame h;
		for (int64_t q = from; (q = findMpegFrame(r, q, end, end, h)) >= 0; ++q)
			if (q + h.length + 4 <= end && same(f, h))
				return true;
		return false;
	};
	uint64_t count = 0;
	for (int64_t p = first; p + 4 <= end;) {
		MpegFrame g;
		if (!mpegFrame(r.be32(p), g) || !same(f, g)) {
			const int64_t limit = std::min(end, p + kResync);
			p = findMpegFrame(r, p + 1, limit, end, g);
			if (p < 0 || !same(f, g)) {
				if (limit < end && pairNearEnd(std::max(limit, end - kResync)))
					return;
				break;
			}
		}
		++count;
		p += g.length;
	}
	res.seconds = double(count) * f.samples / f.rate;
}

void parseAdts(Reader &r, int64_t start, Result &res)
{
	const int64_t end = trailingTags(r, res);
	AdtsFrame f;
	if (!adtsFrame(r, start, f))
		throw Malformed{};
	res.codec = "aac";
	if (end - start > kMaxScan)
		return; // No reliable estimate without a full scan.
	uint64_t samples = 0;
	for (int64_t p = start; p + 7 <= end;) {
		AdtsFrame g;
		if (!adtsFrame(r, p, g) || g.rate != f.rate || p + g.length > end) {
			const int64_t limit = std::min(end, p + kResync);
			p = findAdtsFrame(r, p + 1, limit, end, f.rate, g);
			if (p < 0) {
				if (limit < end &&
					findAdtsFrame(
						r, std::max(limit, end - kResync), end, end, f.rate, g) >= 0)
					return;
				break;
			}
		}
		samples += g.samples;
		p += g.length;
	}
	res.seconds = double(samples) / f.rate;
}

void parseFlac(Reader &r, int64_t start, Result &res)
{
	if (!r.match(start, "fLaC", 4))
		throw Malformed{};
	res.codec = "flac";
	int64_t p = start + 4;
	for (unsigned guard = 0; guard < 1024; ++guard) {
		const unsigned type = r.u8(p);
		const uint32_t length = r.be24(p + 1);
		const int64_t data = p + 4;
		if (!r.has(data, length))
			throw Malformed{};
		if ((type & 0x7F) == 0 && length >= 18) {
			uint8_t b[18];
			r.read(data, b, 18);
			const uint32_t rate = uint32_t(b[10]) << 12 | uint32_t(b[11]) << 4 | b[12] >> 4;
			const uint64_t total = uint64_t(b[13] & 0x0F) << 32 | uint32_t(b[14]) << 24 |
					       uint32_t(b[15]) << 16 | uint32_t(b[16]) << 8 | b[17];
			if (rate)
				res.seconds = double(total) / rate;
		} else if ((type & 0x7F) == 4) {
			const std::string comments = r.bytes(data, std::min<uint32_t>(length, 1 << 20));
			parseVorbisComments(
				reinterpret_cast<const uint8_t *>(comments.data()), comments.size(), res);
		}
		p = data + length;
		if (type & 0x80)
			break;
	}
	trailingTags(r, res);
}

void parseTta(Reader &r, int64_t start, Result &res)
{
	if (!r.match(start, "TTA1", 4) || !r.has(start, 18))
		throw Malformed{};
	res.codec = "tta";
	const uint32_t rate = r.le32(start + 10), samples = r.le32(start + 14);
	if (rate)
		res.seconds = double(samples) / rate;
	trailingTags(r, res);
}

void parseWavPack(Reader &r, int64_t start, Result &res)
{
	static const unsigned rates[15] = { 6000,
		8000,
		9600,
		11025,
		12000,
		16000,
		22050,
		24000,
		32000,
		44100,
		48000,
		64000,
		88200,
		96000,
		192000 };
	if (!r.match(start, "wvpk", 4) || !r.has(start, 32))
		throw Malformed{};
	res.codec = "wavpack";
	const uint32_t low = r.le32(start + 12);
	const unsigned rateIndex = (r.le32(start + 24) >> 23) & 15;
	if (low != 0xFFFFFFFF && rateIndex < 15) {
		const uint64_t total = uint64_t(r.u8(start + 11)) << 32 | low; // WavPack 5: 40-bit count.
		res.seconds = double(total) / rates[rateIndex];
	}
	trailingTags(r, res);
}

void parseAmr(Reader &r, Result &res)
{
	// Payload bytes per frame type, plus one header byte; each frame is 20 ms.
	static const unsigned char narrow[16] = { 12, 13, 15, 17, 19, 20, 26, 31, 5, 0, 0, 0, 0, 0, 0, 0 };
	static const unsigned char wide[16] = { 17, 23, 32, 36, 40, 46, 50, 58, 60, 5, 0, 0, 0, 0, 0, 0 };
	const bool wb = r.match(0, "#!AMR-WB\n", 9);
	if (!wb && !r.match(0, "#!AMR\n", 6))
		throw Malformed{};
	res.codec = wb ? "amr_wb" : "amr_nb";
	const unsigned char *sizes = wb ? wide : narrow;
	uint64_t frames = 0;
	int64_t p = wb ? 9 : 6;
	if (r.size() > kMaxScan)
		return;
	while (p < r.size()) {
		const unsigned type = (r.u8(p) >> 3) & 15;
		if (!sizes[type] && type != 15 && !(wb && type == 14))
			return; // Reserved frame type: damaged, so the count would be short.
		p += 1 + sizes[type];
		++frames;
	}
	res.seconds = static_cast<double>(frames) * 0.02;
}

} // namespace probe
