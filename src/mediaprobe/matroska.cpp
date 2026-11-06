// SPDX-License-Identifier: GPL-2.0-or-later
// Matroska and WebM (EBML). Reads Info, Tracks and Tags, following SeekHead when they
// come after the first Cluster. Cluster contents are never read.
#include "probe.h"

#include <algorithm>
#include <cstring>

namespace probe
{

namespace
{
constexpr uint64_t kUnknown = ~0ULL;

enum : uint32_t
{
	kEbml = 0x1A45DFA3,
	kSegment = 0x18538067,
	kSeekHead = 0x114D9B74,
	kSeek = 0x4DBB,
	kSeekId = 0x53AB,
	kSeekPosition = 0x53AC,
	kInfo = 0x1549A966,
	kTimestampScale = 0x2AD7B1,
	kDuration = 0x4489,
	kSegmentTitle = 0x7BA9,
	kTracks = 0x1654AE6B,
	kTrackEntry = 0xAE,
	kTrackType = 0x83,
	kCodecId = 0x86,
	kCodecPrivate = 0x63A2,
	kAudio = 0xE1,
	kBitDepth = 0x6264,
	kTags = 0x1254C367,
	kTag = 0x7373,
	kTargets = 0x63C0,
	kSimpleTag = 0x67C8,
	kTagName = 0x45A3,
	kTagString = 0x4487,
	kCluster = 0x1F43B675,
};

struct Element
{
	uint32_t id;
	uint64_t size; // kUnknown for unknown-size elements.
	int64_t data;
};

// Reads the element header at p. Children must fit their parent.
Element header(Reader &r, int64_t p, int64_t end)
{
	auto vint = [&](int64_t &q, unsigned maxLength, bool keepMarker, bool &allOnes) {
		const unsigned first = r.u8(q);
		unsigned length = 1;
		while (length <= maxLength && !(first & (0x80 >> (length - 1))))
			++length;
		if (length > maxLength)
			throw Malformed{};
		const unsigned mask = 0xFF >> length;
		uint64_t value = keepMarker ? first : (first & mask);
		allOnes = (first & mask) == mask;
		for (unsigned i = 1; i < length; ++i) {
			const unsigned b = r.u8(q + i);
			value = value << 8 | b;
			allOnes = allOnes && b == 0xFF;
		}
		q += length;
		return value;
	};
	Element e;
	int64_t q = p;
	bool ignored, unknown;
	e.id = static_cast<uint32_t>(vint(q, 4, true, ignored));
	e.size = vint(q, 8, false, unknown);
	if (unknown)
		e.size = kUnknown;
	e.data = q;
	if (e.data > end || (e.size != kUnknown && e.size > static_cast<uint64_t>(end - e.data)))
		throw Malformed{};
	return e;
}

uint64_t uinteger(Reader &r, const Element &e)
{
	if (e.size > 8)
		throw Malformed{};
	uint64_t v = 0;
	for (uint64_t i = 0; i < e.size; ++i)
		v = v << 8 | r.u8(e.data + static_cast<int64_t>(i));
	return v;
}

double floating(Reader &r, const Element &e)
{
	if (e.size == 4) {
		const uint32_t bits = r.be32(e.data);
		float f;
		std::memcpy(&f, &bits, 4);
		return f;
	}
	if (e.size == 8) {
		const uint64_t bits = r.be64(e.data);
		double d;
		std::memcpy(&d, &bits, 8);
		return d;
	}
	throw Malformed{};
}

// EBML strings may be padded with NULs.
std::string text(Reader &r, const Element &e, size_t cap = kMaxTagRead)
{
	std::string s = r.bytes(e.data, static_cast<size_t>(std::min<uint64_t>(e.size, cap)));
	s.erase(std::find(s.begin(), s.end(), '\0'), s.end());
	return s;
}

// Visits the children of a known-size master element.
template <typename F> void children(Reader &r, const Element &parent, F visit)
{
	if (parent.size == kUnknown)
		throw Malformed{};
	const int64_t end = parent.data + static_cast<int64_t>(parent.size);
	for (int64_t p = parent.data; p < end;) {
		const Element e = header(r, p, end);
		if (e.size == kUnknown)
			throw Malformed{};
		visit(e);
		p = e.data + static_cast<int64_t>(e.size);
	}
}

struct Track
{
	uint64_t type = 0, bits = 0;
	std::string codecId, codecPrivate;
};

struct State
{
	uint64_t scale = 1000000;
	double duration = 0;
	bool info = false, tracks = false, tags = false;
	std::vector<Track> trackList;
	std::string segmentTitle;
	std::vector<std::pair<uint32_t, uint64_t>> seeks;
};

void top(Reader &r, const Element &e, State &s, Result &res)
{
	switch (e.id) {
	case kSeekHead:
		children(r, e, [&](const Element &seek) {
			if (seek.id != kSeek || s.seeks.size() >= 64)
				return;
			uint32_t id = 0;
			uint64_t position = kUnknown;
			children(r, seek, [&](const Element &c) {
				if (c.id == kSeekId && c.size <= 4)
					id = static_cast<uint32_t>(uinteger(r, c));
				else if (c.id == kSeekPosition)
					position = uinteger(r, c);
			});
			if (position != kUnknown)
				s.seeks.emplace_back(id, position);
		});
		break;
	case kInfo:
		if (s.info)
			break;
		s.info = true;
		children(r, e, [&](const Element &c) {
			if (c.id == kTimestampScale)
				s.scale = uinteger(r, c);
			else if (c.id == kDuration)
				s.duration = floating(r, c);
			else if (c.id == kSegmentTitle)
				s.segmentTitle = text(r, c);
		});
		break;
	case kTracks:
		if (s.tracks)
			break;
		s.tracks = true;
		children(r, e, [&](const Element &entry) {
			if (entry.id != kTrackEntry || s.trackList.size() >= 128)
				return;
			Track t;
			children(r, entry, [&](const Element &c) {
				if (c.id == kTrackType)
					t.type = uinteger(r, c);
				else if (c.id == kCodecId)
					t.codecId = text(r, c, 64);
				else if (c.id == kCodecPrivate)
					t.codecPrivate = r.bytes(
						c.data, static_cast<size_t>(std::min<uint64_t>(c.size, 64)));
				else if (c.id == kAudio)
					children(r, c, [&](const Element &a) {
						if (a.id == kBitDepth)
							t.bits = uinteger(r, a);
					});
			});
			s.trackList.push_back(t);
		});
		break;
	case kTags:
		if (s.tags)
			break;
		s.tags = true;
		children(r, e, [&](const Element &tag) {
			if (tag.id != kTag)
				return;
			// Only global tags: any track, edition, chapter or attachment UID in Targets
			// makes the tag describe a part of the file, not the file.
			bool global = true;
			children(r, tag, [&](const Element &c) {
				if (c.id == kTargets)
					children(r, c, [&](const Element &t) {
						const bool uid = t.id == 0x63C5 || t.id == 0x63C9 ||
								 t.id == 0x63C4 || t.id == 0x63C6;
						if (uid && uinteger(r, t))
							global = false;
					});
			});
			if (!global)
				return;
			children(r, tag, [&](const Element &c) {
				if (c.id != kSimpleTag)
					return;
				std::string name, value;
				children(r, c, [&](const Element &f) {
					if (f.id == kTagName)
						name = text(r, f, 64);
					else if (f.id == kTagString)
						value = text(r, f);
				});
				for (auto &ch : name)
					if (ch >= 'a' && ch <= 'z')
						ch = char(ch - 'a' + 'A');
				if (name == "TITLE")
					setTag(res.title, value);
				else if (name == "ARTIST")
					setTag(res.artist, value);
				else if (name == "ALBUM")
					setTag(res.album, value);
			});
		});
		break;
	default:
		break;
	}
}
} // namespace

void parseMatroska(Reader &r, Result &res)
{
	const Element ebml = header(r, 0, r.size());
	if (ebml.id != kEbml || ebml.size == kUnknown)
		throw Malformed{};
	// A truncated file keeps its original Segment size; read what is there.
	const Element segment = header(r, ebml.data + static_cast<int64_t>(ebml.size), INT64_MAX);
	if (segment.id != kSegment)
		throw Malformed{};
	const int64_t start = segment.data;
	const int64_t end = segment.size == kUnknown
				    ? r.size()
				    : std::min<int64_t>(r.size(), start + static_cast<int64_t>(segment.size));
	State s;
	// Level 1 elements in order, up to the first Cluster or unknown-size element. A file
	// cut inside an element keeps what came before it.
	for (int64_t p = start; p < end;) {
		const Element e = header(r, p, INT64_MAX);
		if (e.id == kCluster || e.size == kUnknown || e.data > end ||
			e.size > static_cast<uint64_t>(end - e.data))
			break;
		top(r, e, s, res);
		p = e.data + static_cast<int64_t>(e.size);
	}
	// Info, Tracks and Tags often sit after the clusters; SeekHead points at them.
	for (const auto &seek : std::vector<std::pair<uint32_t, uint64_t>>(s.seeks)) {
		const bool wanted = (seek.first == kInfo && !s.info) ||
				    (seek.first == kTracks && !s.tracks) || (seek.first == kTags && !s.tags);
		if (!wanted || seek.second >= static_cast<uint64_t>(end - start))
			continue;
		const Element e = header(r, start + static_cast<int64_t>(seek.second), INT64_MAX);
		if (e.id == seek.first && e.size != kUnknown && e.data <= end &&
			e.size <= static_cast<uint64_t>(end - e.data))
			top(r, e, s, res);
	}
	setTag(res.title, s.segmentTitle);
	if (s.duration > 0 && s.scale)
		res.seconds = s.duration * double(s.scale) / 1e9;
	// First video track, else first audio track. An unknown video codec stays empty
	// rather than publishing the audio codec for a video file.
	const Track *chosen = nullptr;
	for (uint64_t type : { 1, 2 })
		for (const auto &t : s.trackList)
			if (!chosen && t.type == type)
				chosen = &t;
	if (chosen)
		res.codec = matroskaCodec(
			chosen->codecId, static_cast<unsigned>(chosen->bits), chosen->codecPrivate);
}

} // namespace probe
