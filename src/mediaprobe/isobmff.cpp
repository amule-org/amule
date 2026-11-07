// SPDX-License-Identifier: GPL-2.0-or-later
// ISO base media file format: MP4, M4A, M4V, MOV and 3GP. Reads moov, and the moof boxes of
// a fragmented file that has no total duration. mdat is skipped by size.
#include "probe.h"

#include <algorithm>
#include <cstring>

namespace probe
{

namespace
{
struct Box
{
	char type[4];
	int64_t data, end;
};

// Returns false when the walk is incomplete. At the top level, where a fragmented file has
// two boxes per fragment, the limit is higher, and a box that does not fit ends the walk: an
// mdat as a cut file, anything else as damage.
template <typename F> bool boxes(Reader &r, int64_t p, int64_t end, F visit, bool top = false)
{
	const unsigned limit = top ? 200000 : 10000;
	for (unsigned guard = 0; p + 8 <= end; ++guard) {
		if (guard == limit)
			return false;
		Box b;
		uint64_t size = r.be32(p);
		r.read(p + 4, b.type, 4);
		const bool mdat = !std::memcmp(b.type, "mdat", 4);
		b.data = p + 8;
		if (size == 1) {
			if (top && !r.has(p + 8, 8))
				return mdat;
			size = r.be64(p + 8);
			b.data = p + 16;
		} else if (size == 0) {
			if (top && !mdat)
				return false; // Zeroed damage rather than a box running to the end.
			size = static_cast<uint64_t>(end - p);
		}
		if (size < static_cast<uint64_t>(b.data - p))
			throw Malformed{};
		if (size > static_cast<uint64_t>(end - p)) {
			if (!top)
				throw Malformed{};
			return mdat;
		}
		b.end = p + static_cast<int64_t>(size);
		if (!visit(b))
			return true;
		p = b.end;
	}
	return true;
}

bool is(const Box &b, const char *type)
{
	return !std::memcmp(b.type, type, 4);
}

// Duration and timescale fields of mvhd and mdhd share one layout.
void timing(Reader &r, const Box &b, uint64_t &duration, uint32_t &scale)
{
	if (r.u8(b.data) == 1) {
		if (b.end - b.data < 32)
			throw Malformed{};
		scale = r.be32(b.data + 20);
		duration = r.be64(b.data + 24);
		if (duration == ~0ULL) // Unknown, as in Smooth Streaming files.
			duration = 0;
	} else {
		scale = r.be32(b.data + 12);
		duration = r.be32(b.data + 16);
		if (duration == 0xFFFFFFFF)
			duration = 0;
	}
}

// objectTypeIndication from an esds DecoderConfigDescriptor.
unsigned objectType(Reader &r, const Box &esds)
{
	int64_t p = esds.data + 4; // FullBox version and flags.
	for (int guard = 0; guard < 8 && p + 2 <= esds.end; ++guard) {
		const unsigned tag = r.u8(p++);
		uint32_t length = 0;
		for (int i = 0; i < 4 && p < esds.end; ++i) {
			const unsigned b = r.u8(p++);
			length = length << 7 | (b & 0x7F);
			if (!(b & 0x80))
				break;
		}
		if (tag == 0x03) { // ES_Descriptor: skip its fixed fields and descend.
			if (p + 3 > esds.end)
				break;
			const unsigned flags = r.u8(p + 2);
			p += 3;
			if (flags & 0x80)
				p += 2;
			if (flags & 0x40)
				p += 1 + r.u8(p);
			if (flags & 0x20)
				p += 2;
		} else if (tag == 0x04) {
			return p < esds.end ? r.u8(p) : 0;
		} else {
			p += length;
		}
	}
	return 0;
}

std::string esdsCodec(unsigned oti, bool video)
{
	if (video) {
		if (oti == 0x20)
			return "mpeg4";
		if (oti >= 0x60 && oti <= 0x65)
			return "mpeg2video";
		if (oti == 0x6A)
			return "mpeg1video";
		if (oti == 0x6C)
			return "mjpeg";
		return {};
	}
	if (oti == 0x40 || (oti >= 0x66 && oti <= 0x68))
		return "aac";
	if (oti == 0x69 || oti == 0x6B)
		return "mp3";
	if (oti == 0xA5)
		return "ac3";
	if (oti == 0xA6)
		return "eac3";
	if (oti == 0xA9)
		return "dts";
	if (oti == 0xDD)
		return "vorbis";
	return {};
}

// Finds an esds box among the sample entry children, looking inside QuickTime 'wave'.
// Depth is bounded so nested 'wave' boxes cannot exhaust the stack.
unsigned entryObjectType(Reader &r, int64_t p, int64_t end, int depth = 0)
{
	unsigned oti = 0;
	boxes(r, p, end, [&](const Box &b) {
		if (is(b, "esds"))
			oti = objectType(r, b);
		else if (is(b, "wave") && depth < 2)
			oti = entryObjectType(r, b.data, b.end, depth + 1);
		return !oti;
	});
	return oti;
}

std::string audioCodec(Reader &r, const Box &entry)
{
	const char *t = entry.type;
	static const struct
	{
		const char *type, *codec;
	} names[] = { { "ac-3", "ac3" },
		{ "ec-3", "eac3" },
		{ "Opus", "opus" },
		{ "fLaC", "flac" },
		{ "alac", "alac" },
		{ "samr", "amr_nb" },
		{ "sawb", "amr_wb" },
		{ "ima4", "adpcm_ima_qt" },
		{ "sowt", "pcm_s16le" },
		{ "in24", "pcm_s24be" },
		{ "in32", "pcm_s32be" },
		{ "fl32", "pcm_f32be" },
		{ "fl64", "pcm_f64be" },
		{ "raw ", "pcm_u8" },
		{ "ulaw", "pcm_mulaw" },
		{ "alaw", "pcm_alaw" },
		{ ".mp3", "mp3" },
		{ "dtsc", "dts" },
		{ "dtsh", "dts" },
		{ "dtsl", "dts" },
		{ "mlpa", "truehd" } };
	for (const auto &n : names)
		if (!std::memcmp(t, n.type, 4))
			return n.codec;
	if (!std::memcmp(t, "twos", 4))
		return entry.end - entry.data >= 20 && r.be16(entry.data + 18) == 8 ? "pcm_s8" : "pcm_s16be";
	if (!std::memcmp(t, "mp4a", 4)) {
		// Sound sample entry: 28 bytes, plus 16 (QuickTime v1) or 36 (v2).
		const unsigned version = entry.end - entry.data >= 10 ? r.be16(entry.data + 8) : 0;
		const int64_t children = entry.data + 28 + (version == 1 ? 16 : version == 2 ? 36 : 0);
		return children <= entry.end ? esdsCodec(entryObjectType(r, children, entry.end), false)
					     : std::string();
	}
	return {};
}

std::string videoCodec(Reader &r, const Box &entry)
{
	if (!std::memcmp(entry.type, "mp4v", 4)) {
		const int64_t children = entry.data + 78; // Visual sample entry size.
		return children <= entry.end ? esdsCodec(entryObjectType(r, children, entry.end), true)
					     : std::string();
	}
	return fourccCodec(entry.type);
}

struct Track
{
	char handler[4] = {};
	bool enabled = true;
	uint32_t id = 0;
	uint64_t duration = 0;
	uint32_t scale = 0;
	std::string codec;
};

Track track(Reader &r, const Box &trak)
{
	Track t;
	boxes(r, trak.data, trak.end, [&](const Box &b) {
		if (is(b, "tkhd") && b.end - b.data >= 4) {
			t.enabled = r.be24(b.data + 1) & 1;
			const int64_t id = r.u8(b.data) == 1 ? 20 : 12;
			if (b.end - b.data >= id + 4)
				t.id = r.be32(b.data + id);
		}
		if (!is(b, "mdia"))
			return true;
		Box stsd{};
		boxes(r, b.data, b.end, [&](const Box &m) {
			if (is(m, "hdlr") && m.end - m.data >= 12)
				r.read(m.data + 8, t.handler, 4);
			else if (is(m, "mdhd") && m.end - m.data >= 24)
				timing(r, m, t.duration, t.scale);
			else if (is(m, "minf"))
				boxes(r, m.data, m.end, [&](const Box &n) {
					if (is(n, "stbl"))
						boxes(r, n.data, n.end, [&](const Box &s) {
							if (is(s, "stsd"))
								stsd = s;
							return true;
						});
					return true;
				});
			return true;
		});
		// First sample entry after FullBox header and entry count.
		if (stsd.end - stsd.data >= 16)
			boxes(r, stsd.data + 8, stsd.end, [&](const Box &entry) {
				t.codec = !std::memcmp(t.handler, "vide", 4)   ? videoCodec(r, entry)
					  : !std::memcmp(t.handler, "soun", 4) ? audioCodec(r, entry)
									       : std::string();
				return false;
			});
		return true;
	});
	return t;
}

// Sample durations of one track across the fragments, in its timescale.
struct Fragmented
{
	uint32_t id, defaultDuration;
	uint64_t sum = 0, end = 0; // Sum of durations; latest decode time plus duration.
};

void moof(Reader &r, const Box &moof, std::vector<Fragmented> &tracks)
{
	boxes(r, moof.data, moof.end, [&](const Box &traf) {
		if (!is(traf, "traf"))
			return true;
		Fragmented *t = nullptr;
		uint32_t duration = 0;
		uint64_t base = 0, total = 0;
		boxes(r, traf.data, traf.end, [&](const Box &b) {
			if (is(b, "tfhd") && b.end - b.data >= 8) {
				const uint32_t flags = r.be24(b.data + 1), id = r.be32(b.data + 4);
				for (auto &f : tracks)
					if (f.id == id)
						t = &f;
				if (!t)
					return false; // Not declared in moov.
				duration = t->defaultDuration;
				const int64_t o = b.data + 8 + (flags & 1 ? 8 : 0) + (flags & 2 ? 4 : 0);
				if (flags & 8 && o + 4 <= b.end)
					duration = r.be32(o);
			} else if (is(b, "tfdt") && b.end - b.data >= 8) {
				base = r.u8(b.data) == 1 && b.end - b.data >= 12 ? r.be64(b.data + 4)
										 : r.be32(b.data + 4);
			} else if (is(b, "trun") && t && b.end - b.data >= 8) {
				const uint32_t flags = r.be24(b.data + 1), count = r.be32(b.data + 4);
				if (!(flags & 0x100)) { // Every sample has the default duration.
					total += uint64_t(count) * duration;
					return true;
				}
				int64_t entry = 0;
				for (uint32_t bit = 0x100; bit <= 0x800; bit <<= 1)
					entry += flags & bit ? 4 : 0;
				const int64_t p = b.data + 8 + (flags & 1 ? 4 : 0) + (flags & 4 ? 4 : 0);
				if (p > b.end || count > (b.end - p) / entry)
					throw Malformed{};
				for (uint32_t i = 0; i < count; ++i)
					total += r.be32(p + i * entry);
			}
			return true;
		});
		if (t) {
			t->sum += total;
			t->end = std::max(t->end, base + total);
		}
		return true;
	});
}

std::string *tagField(const char type[4], Result &res)
{
	if (!std::memcmp(type, "\xA9nam", 4) || !std::memcmp(type, "titl", 4))
		return &res.title;
	if (!std::memcmp(type,
		    "\xA9"
		    "ART",
		    4) ||
		!std::memcmp(type, "perf", 4))
		return &res.artist;
	if (!std::memcmp(type,
		    "\xA9"
		    "alb",
		    4) ||
		!std::memcmp(type, "albm", 4))
		return &res.album;
	return nullptr;
}

std::string capped(Reader &r, int64_t p, int64_t end)
{
	return p < end ? r.bytes(p, static_cast<size_t>(std::min<int64_t>(end - p, kMaxTagRead)))
		       : std::string();
}

// iTunes metadata: ilst items holding 'data' boxes (type 1 is UTF-8, 2 is UTF-16BE).
void ilst(Reader &r, const Box &list, Result &res)
{
	boxes(r, list.data, list.end, [&](const Box &item) {
		std::string *dst = tagField(item.type, res);
		if (dst && dst->empty())
			boxes(r, item.data, item.end, [&](const Box &d) {
				if (!is(d, "data") || d.end - d.data < 8)
					return true;
				const uint32_t type = r.be32(d.data) & 0xFFFFFF;
				const std::string raw = capped(r, d.data + 8, d.end);
				if (type == 1)
					setTag(*dst, raw);
				else if (type == 2)
					setTag(*dst,
						utf16(reinterpret_cast<const uint8_t *>(raw.data()),
							raw.size(),
							true));
				return false;
			});
		return true;
	});
}

void meta(Reader &r, const Box &m, Result &res)
{
	// ISO 'meta' is a FullBox; QuickTime's is not. Detect by the first child type.
	const int64_t first = r.match(m.data + 4, "hdlr", 4) ? m.data : m.data + 4;
	boxes(r, first, m.end, [&](const Box &b) {
		if (is(b, "ilst"))
			ilst(r, b, res);
		return true;
	});
}

void udta(Reader &r, const Box &u, Result &res)
{
	boxes(r, u.data, u.end, [&](const Box &b) {
		if (is(b, "meta")) {
			meta(r, b, res);
			return true;
		}
		std::string *dst = tagField(b.type, res);
		if (!dst || !dst->empty())
			return true;
		if (static_cast<unsigned char>(b.type[0]) == 0xA9) {
			// QuickTime user data text: 16-bit length, 16-bit language, text.
			if (b.end - b.data >= 4) {
				const int64_t length = r.be16(b.data);
				setTag(*dst, capped(r, b.data + 4, std::min(b.end, b.data + 4 + length)));
			}
		} else if (b.end - b.data >= 6) {
			// 3GPP asset: FullBox, 16-bit language, UTF-8 or BOM-prefixed UTF-16 string.
			std::string raw = capped(r, b.data + 6, b.end);
			if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFE &&
				static_cast<unsigned char>(raw[1]) == 0xFF)
				raw = utf16(reinterpret_cast<const uint8_t *>(raw.data()) + 2,
					raw.size() - 2,
					true);
			setTag(*dst, raw);
		}
		return true;
	});
}
} // namespace

void parseMp4(Reader &r, Result &res)
{
	bool moov = false, mvex = false;
	uint64_t duration = 0, fragments = 0;
	uint32_t scale = 0;
	std::vector<Track> tracks;
	std::vector<Fragmented> fragmented;
	auto visitTop = [&](const Box &top) {
		if (is(top, "moof")) {
			moof(r, top, fragmented);
			return true;
		}
		if (!is(top, "moov"))
			return true;
		moov = true;
		boxes(r, top.data, top.end, [&](const Box &b) {
			if (is(b, "mvhd") && b.end - b.data >= 24)
				timing(r, b, duration, scale);
			else if (is(b, "trak") && tracks.size() < 64)
				tracks.push_back(track(r, b));
			else if (is(b, "udta"))
				udta(r, b, res);
			else if (is(b, "meta"))
				meta(r, b, res);
			else if (is(b, "mvex"))
				boxes(r, b.data, b.end, [&](const Box &m) {
					mvex = true;
					if (is(m, "mehd") && m.end - m.data >= 8)
						fragments = r.u8(m.data) == 1 ? r.be64(m.data + 4)
									      : r.be32(m.data + 4);
					else if (is(m, "trex") && m.end - m.data >= 16 &&
						 fragmented.size() < 64)
						fragmented.push_back(
							{ r.be32(m.data + 4), r.be32(m.data + 12) });
					return true;
				});
			return true;
		});
		// Without mehd, mvhd covers only the samples in moov: add up the fragments.
		return mvex && !fragments;
	};
	// An incomplete walk would make the fragment sum short.
	const bool complete = boxes(r, 0, r.size(), visitTop, true);
	if (!moov)
		throw Malformed{};
	// In a fragmented file mvhd may cover only the samples in moov.
	duration = std::max(duration, fragments);
	if (duration && scale)
		res.seconds = double(duration) / scale;
	else
		for (const auto &t : tracks)
			if (t.scale)
				res.seconds = std::max(res.seconds, double(t.duration) / t.scale);
	if (mvex && !fragments) {
		if (!complete)
			res.seconds = 0;
		for (const auto &f : fragmented)
			for (const auto &t : tracks)
				if (complete && t.id == f.id && t.scale)
					res.seconds = std::max(res.seconds,
						double(std::max(t.duration + f.sum, f.end)) / t.scale);
	}
	// First enabled video track, else first enabled audio track. Disabled tracks
	// (chapter images in audiobooks) count only when nothing else exists.
	const Track *chosen = nullptr;
	for (bool enabledOnly : { true, false })
		for (const char *handler : { "vide", "soun" })
			for (const auto &t : tracks)
				if (!chosen && (t.enabled || !enabledOnly) &&
					!std::memcmp(t.handler, handler, 4))
					chosen = &t;
	if (chosen)
		res.codec = chosen->codec;
}

} // namespace probe
