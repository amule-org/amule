// SPDX-License-Identifier: GPL-2.0-or-later
// Ogg: Vorbis, Opus, FLAC, Speex and Theora. Reads the header packets at the start and
// the last granule position at the end; audio and video packets are never read.
#include "probe.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>

namespace probe
{

namespace
{
constexpr size_t kMaxPacket = 1 << 20;

struct Page
{
	unsigned type = 0;
	uint64_t granule = 0;
	uint32_t serial = 0;
	int64_t body = 0, end = 0;
	std::vector<uint8_t> lacing;
};

bool page(Reader &r, int64_t p, Page &g)
{
	if (!r.has(p, 27) || !r.match(p, "OggS", 4) || r.u8(p + 4) != 0)
		return false;
	g.type = r.u8(p + 5);
	g.granule = r.le64(p + 6);
	g.serial = r.le32(p + 14);
	const unsigned segments = r.u8(p + 26);
	g.lacing.resize(segments);
	if (segments)
		r.read(p + 27, g.lacing.data(), segments);
	int64_t length = 0;
	for (auto l : g.lacing)
		length += l;
	g.body = p + 27 + segments;
	g.end = g.body + length;
	return r.has(g.body, static_cast<uint64_t>(length));
}

struct Stream
{
	std::string codec;
	unsigned rate = 0, preSkip = 0, shift = 0;
	double fps = 0, unit = 0; // unit: OGM seconds per granule.
	bool video = false;
	std::vector<std::string> packets; // Header packets, reassembled across pages.
	std::string partial;
	bool done = false;
};

void identify(Stream &s, const std::string &p)
{
	auto le32 = [&](size_t o) {
		return uint32_t(uint8_t(p[o])) | uint32_t(uint8_t(p[o + 1])) << 8 |
		       uint32_t(uint8_t(p[o + 2])) << 16 | uint32_t(uint8_t(p[o + 3])) << 24;
	};
	auto le64 = [&](size_t o) { return uint64_t(le32(o)) | uint64_t(le32(o + 4)) << 32; };
	auto be32 = [&](size_t o) {
		return uint32_t(uint8_t(p[o])) << 24 | uint32_t(uint8_t(p[o + 1])) << 16 |
		       uint32_t(uint8_t(p[o + 2])) << 8 | uint32_t(uint8_t(p[o + 3]));
	};
	if (p.size() >= 16 && !p.compare(0, 7, "\x01vorbis", 7)) {
		s.codec = "vorbis";
		s.rate = le32(12);
	} else if (p.size() >= 12 && !p.compare(0, 8, "OpusHead")) {
		s.codec = "opus";
		s.rate = 48000; // Opus granules always count 48 kHz samples.
		s.preSkip = uint8_t(p[10]) | uint8_t(p[11]) << 8;
	} else if (p.size() >= 30 &&
		   !p.compare(0,
			   5,
			   "\x7F"
			   "FLAC") &&
		   !p.compare(9, 4, "fLaC")) {
		s.codec = "flac";
		s.rate = uint32_t(uint8_t(p[27])) << 12 | uint32_t(uint8_t(p[28])) << 4 | uint8_t(p[29]) >> 4;
	} else if (p.size() >= 40 && !p.compare(0, 8, "Speex   ")) {
		s.codec = "speex";
		s.rate = le32(36);
	} else if (p.size() >= 42 && !p.compare(0, 7, "\x80theora", 7)) {
		s.codec = "theora";
		s.video = true;
		const uint32_t num = be32(22), den = be32(26);
		s.fps = den ? double(num) / den : 0;
		s.shift = (uint8_t(p[40]) & 3) << 3 | uint8_t(p[41]) >> 5;
	} else if (p.size() >= 43 && (!p.compare(0, 9, "\x01video\0\0\0", 9) || !p.compare(0,
											9,
											"\x01"
											"audio\0\0\0",
											9))) {
		// OGM (Ogg Media) stream header: video subtype is a FourCC, audio subtype the
		// WAVE format tag in hex. Granules count time units of 100 ns per sample.
		char subtype[5] = {};
		p.copy(subtype, 4, 9);
		s.video = p[1] == 'v';
		s.codec = s.video ? fourccCodec(subtype)
				  : waveCodec(unsigned(std::strtoul(subtype, nullptr, 16)),
					    uint8_t(p[41]) | uint8_t(p[42]) << 8,
					    false);
		const uint64_t timeUnit = le64(17), samplesPerUnit = le64(25);
		if (samplesPerUnit)
			s.unit = double(timeUnit) / 1e7 / double(samplesPerUnit);
	}
}

// The comment header is the second packet, except for Ogg FLAC where any header packet
// can be a VORBIS_COMMENT metadata block.
void comments(const Stream &s, Result &res)
{
	for (size_t i = 1; i < s.packets.size(); ++i) {
		const std::string &p = s.packets[i];
		const auto *d = reinterpret_cast<const uint8_t *>(p.data());
		if (s.codec == "vorbis" && !p.compare(0, 7, "\x03vorbis", 7))
			parseVorbisComments(d + 7, p.size() - 7, res);
		else if (s.codec == "opus" && !p.compare(0, 8, "OpusTags"))
			parseVorbisComments(d + 8, p.size() - 8, res);
		else if (s.codec == "theora" && !p.compare(0, 7, "\x81theora", 7))
			parseVorbisComments(d + 7, p.size() - 7, res);
		else if (s.codec == "speex" && i == 1)
			parseVorbisComments(d, p.size(), res);
		else if (s.codec == "flac" && p.size() >= 4 && (d[0] & 0x7F) == 4)
			parseVorbisComments(d + 4, p.size() - 4, res);
	}
}

double seconds(const Stream &s, uint64_t granule)
{
	if (s.unit > 0)
		return static_cast<double>(granule) * s.unit;
	if (s.codec == "theora") {
		if (s.fps <= 0 || s.shift >= 32)
			return 0;
		const uint64_t frames = (granule >> s.shift) + (granule & ((1ULL << s.shift) - 1));
		return static_cast<double>(frames) / s.fps;
	}
	if (!s.rate)
		return 0;
	return granule > s.preSkip ? double(granule - s.preSkip) / s.rate : 0;
}
} // namespace

void parseOgg(Reader &r, Result &res)
{
	std::map<uint32_t, Stream> streams;
	std::vector<uint32_t> order;
	// Header packets: walk the first pages until each stream has three packets.
	int64_t p = 0;
	for (unsigned n = 0; n < 512; ++n) {
		Page g;
		if (!page(r, p, g))
			break;
		p = g.end;
		if (g.type & 2) { // Beginning of stream.
			if (streams.size() >= 16 || streams.count(g.serial))
				throw Malformed{};
			streams[g.serial];
			order.push_back(g.serial);
		}
		auto it = streams.find(g.serial);
		if (it == streams.end() || it->second.done)
			continue;
		Stream &s = it->second;
		int64_t q = g.body;
		for (auto l : g.lacing) {
			if (s.partial.size() + l <= kMaxPacket)
				s.partial += r.bytes(q, l);
			q += l;
			if (l < 255) { // Packet ends.
				if (s.packets.empty())
					identify(s, s.partial);
				s.packets.push_back(std::move(s.partial));
				s.partial.clear();
				if (s.packets.size() >= 3) {
					s.done = true;
					break;
				}
			}
		}
		if (std::all_of(streams.begin(), streams.end(), [](const auto &e) { return e.second.done; }))
			break;
	}
	if (order.empty())
		throw Malformed{};
	// Video first, then audio; tags from the chosen stream, then from the others.
	const Stream *chosen = nullptr;
	for (uint32_t serial : order)
		if (!chosen && streams[serial].video && !streams[serial].codec.empty())
			chosen = &streams[serial];
	for (uint32_t serial : order)
		if (!chosen && !streams[serial].codec.empty())
			chosen = &streams[serial];
	if (!chosen)
		return;
	res.codec = chosen->codec;
	comments(*chosen, res);
	for (uint32_t serial : order)
		if (&streams[serial] != chosen)
			comments(streams[serial], res);
	// Duration: the last page of the chosen stream, searched backwards from the end.
	uint32_t serial = 0;
	for (const auto &e : streams)
		if (&e.second == chosen)
			serial = e.first;
	for (int64_t window = 64 * 1024; window <= 1024 * 1024; window *= 4) {
		const int64_t from = std::max<int64_t>(0, r.size() - window);
		const std::string tail = r.bytes(from, static_cast<size_t>(r.size() - from));
		for (size_t i = tail.size(); i-- > 0;) {
			if (tail.compare(i, 4, "OggS"))
				continue;
			Page g;
			if (!page(r, from + static_cast<int64_t>(i), g))
				continue;
			if (!streams.count(g.serial))
				return; // A chained file: the first link's granule would be short.
			if (g.serial == serial && g.granule != ~0ULL) {
				res.seconds = seconds(*chosen, g.granule);
				return;
			}
		}
		if (!from)
			return;
	}
}

} // namespace probe
