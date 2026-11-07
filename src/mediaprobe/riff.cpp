// SPDX-License-Identifier: GPL-2.0-or-later
// RIFF (WAV, AVI including OpenDML) and IFF (AIFF, AIFF-C).
#include "probe.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace probe
{

namespace
{
struct Chunk
{
	char id[4];
	uint64_t size;
	int64_t data;
};

// Walks the chunks in [p, end). A chunk running past the end is clamped when allowed
// (streamed WAV writers leave the data size at 0xFFFFFFFF), otherwise the walk stops.
template <typename F> void chunks(Reader &r, int64_t p, int64_t end, bool bigEndian, F visit)
{
	for (unsigned guard = 0; p + 8 <= end && guard < 100000; ++guard) {
		Chunk c;
		r.read(p, c.id, 4);
		c.size = bigEndian ? r.be32(p + 4) : r.le32(p + 4);
		c.data = p + 8;
		if (c.size > static_cast<uint64_t>(end - c.data)) {
			if (std::memcmp(c.id, "data", 4) && std::memcmp(c.id, "SSND", 4) &&
				std::memcmp(c.id, "LIST", 4))
				return;
			c.size = static_cast<uint64_t>(end - c.data);
		}
		if (!visit(c))
			return;
		p = c.data + static_cast<int64_t>(c.size) + static_cast<int64_t>(c.size & 1);
	}
}

void infoList(Reader &r, int64_t p, int64_t end, Result &res)
{
	chunks(r, p, end, false, [&](const Chunk &c) {
		std::string *dst = !std::memcmp(c.id, "INAM", 4)   ? &res.title
				   : !std::memcmp(c.id, "IART", 4) ? &res.artist
				   : !std::memcmp(c.id, "IPRD", 4) ? &res.album
								   : nullptr;
		if (dst)
			setTag(*dst,
				r.bytes(c.data,
					static_cast<size_t>(std::min<uint64_t>(c.size, kMaxTagRead))));
		return true;
	});
}

struct Wave
{
	unsigned tag = 0, bits = 0, block = 0;
	uint32_t rate = 0, avgBytes = 0;
	uint64_t data = 0, ds64Data = 0, factSamples = 0;
	bool haveFmt = false, haveData = false;
};

void waveFormat(Reader &r, const Chunk &c, bool be, Wave &w)
{
	if (c.size < 16)
		throw Malformed{};
	auto u16 = [&](int64_t o) { return be ? r.be16(o) : r.le16(o); };
	auto u32 = [&](int64_t o) { return be ? r.be32(o) : r.le32(o); };
	w.tag = u16(c.data);
	w.rate = u32(c.data + 4);
	w.avgBytes = u32(c.data + 8);
	w.block = u16(c.data + 12);
	w.bits = u16(c.data + 14);
	if (w.tag == 0xFFFE && c.size >= 40) // WAVE_FORMAT_EXTENSIBLE: the GUID starts with the tag.
		w.tag = u16(c.data + 24);
	w.haveFmt = true;
}

struct AviStream
{
	char type[4] = {}, handler[4] = {};
	uint32_t scale = 0, rate = 0, length = 0;
	std::string codec;
};

struct Avi
{
	uint32_t usPerFrame = 0, totalFrames = 0;
	std::vector<AviStream> streams;
};

void aviHeaders(Reader &r, int64_t p, int64_t end, Avi &avi)
{
	chunks(r, p, end, false, [&](const Chunk &c) {
		if (!std::memcmp(c.id, "avih", 4) && c.size >= 20) {
			avi.usPerFrame = r.le32(c.data);
			avi.totalFrames = r.le32(c.data + 16);
		} else if (!std::memcmp(c.id, "LIST", 4) && c.size >= 4) {
			const int64_t listEnd = c.data + static_cast<int64_t>(c.size);
			if (r.match(c.data, "strl", 4) && avi.streams.size() < 64) {
				AviStream s;
				chunks(r, c.data + 4, listEnd, false, [&](const Chunk &k) {
					if (!std::memcmp(k.id, "strh", 4) && k.size >= 36) {
						r.read(k.data, s.type, 4);
						r.read(k.data + 4, s.handler, 4);
						s.scale = r.le32(k.data + 20);
						s.rate = r.le32(k.data + 24);
						s.length = r.le32(k.data + 32);
					} else if (!std::memcmp(k.id, "strf", 4)) {
						if (!std::memcmp(s.type, "vids", 4) && k.size >= 20) {
							char compression[4];
							r.read(k.data + 16, compression, 4);
							static const char zero[4] = {};
							s.codec = !std::memcmp(compression, zero, 4)
									  ? "rawvideo"
									  : fourccCodec(compression);
							if (s.codec.empty())
								s.codec = fourccCodec(s.handler);
						} else if (!std::memcmp(s.type, "auds", 4) && k.size >= 16) {
							s.codec = waveCodec(
								r.le16(k.data), r.le16(k.data + 14), false);
						}
					}
					return true;
				});
				avi.streams.push_back(s);
			}
		}
		return true;
	});
}

void aviResult(const Avi &avi, Result &res)
{
	const AviStream *video = nullptr, *audio = nullptr;
	for (const auto &s : avi.streams) {
		if (!video && !std::memcmp(s.type, "vids", 4))
			video = &s;
		if (!audio && !std::memcmp(s.type, "auds", 4))
			audio = &s;
	}
	if (video) {
		res.codec = video->codec;
		// strh counts the whole stream, including OpenDML extension RIFFs. avih counts only
		// the first RIFF, and some muxers fill OpenDML dmlh with every chunk of every
		// stream, so dmlh is ignored, as FFmpeg does.
		if (video->rate && video->scale && video->length)
			res.seconds = double(video->length) * video->scale / video->rate;
		else if (avi.usPerFrame)
			res.seconds = double(avi.totalFrames) * avi.usPerFrame / 1e6;
	} else if (audio) {
		res.codec = audio->codec;
		if (audio->rate && audio->scale)
			res.seconds = double(audio->length) * audio->scale / audio->rate;
	}
	// FFmpeg writing to a pipe leaves no frame count in avih and a placeholder length in strh.
	const AviStream *timed = video ? video : audio;
	if (timed && !avi.totalFrames && timed->length == 0x40000000)
		res.seconds = 0;
}
} // namespace

void parseRiff(Reader &r, Result &res)
{
	const bool be = r.match(0, "RIFX", 4);
	const bool rf64 = r.match(0, "RF64", 4) || r.match(0, "BW64", 4);
	const bool avi = r.match(8, "AVI ", 4);
	if (!avi && !r.match(8, "WAVE", 4))
		throw Malformed{};
	Wave w;
	Avi a;
	Result info;
	chunks(r, 12, r.size(), be, [&](const Chunk &c) {
		if (!std::memcmp(c.id, "fmt ", 4) && !avi) {
			waveFormat(r, c, be, w);
		} else if (!std::memcmp(c.id, "data", 4) && !avi) {
			w.data = rf64 && c.size == static_cast<uint64_t>(r.size() - c.data) && w.ds64Data
					 ? w.ds64Data
					 : c.size;
			w.data = std::min<uint64_t>(w.data, static_cast<uint64_t>(r.size() - c.data));
			w.haveData = true;
		} else if (!std::memcmp(c.id, "ds64", 4) && c.size >= 16) {
			w.ds64Data = r.le64(c.data + 8);
		} else if (!std::memcmp(c.id, "fact", 4) && c.size >= 4) {
			w.factSamples = be ? r.be32(c.data) : r.le32(c.data);
		} else if (!std::memcmp(c.id, "LIST", 4) && c.size >= 4) {
			const int64_t end = c.data + static_cast<int64_t>(c.size);
			if (r.match(c.data, "INFO", 4))
				infoList(r, c.data + 4, end, info);
			else if (avi && r.match(c.data, "hdrl", 4))
				aviHeaders(r, c.data + 4, end, a);
		} else if (!std::memcmp(c.id, "id3 ", 4) || !std::memcmp(c.id, "ID3 ", 4)) {
			parseId3v2(r, c.data, res);
		}
		return true;
	});
	// ID3v2 has explicit text encodings, so it takes precedence over RIFF INFO.
	setTag(res.title, info.title);
	setTag(res.artist, info.artist);
	setTag(res.album, info.album);
	if (avi)
		return aviResult(a, res);
	if (!w.haveFmt)
		throw Malformed{};
	res.codec = waveCodec(w.tag, w.bits, be);
	const bool pcm = w.tag == 1 || w.tag == 3;
	if (!w.haveData)
		return;
	if (!pcm && w.factSamples && w.rate)
		res.seconds = double(w.factSamples) / w.rate;
	else if (w.avgBytes)
		res.seconds = double(w.data) / w.avgBytes;
}

void parseAiff(Reader &r, Result &res)
{
	const bool aifc = r.match(8, "AIFC", 4);
	if (!aifc && !r.match(8, "AIFF", 4))
		throw Malformed{};
	Result names;
	bool comm = false;
	chunks(r, 12, r.size(), true, [&](const Chunk &c) {
		if (!std::memcmp(c.id, "COMM", 4) && c.size >= 18) {
			const uint32_t frames = r.be32(c.data + 2);
			const unsigned bits = r.be16(c.data + 6);
			// 80-bit IEEE 754 extended sample rate.
			const int exponent = (r.be16(c.data + 8) & 0x7FFF) - 16383 - 63;
			const double rate = std::ldexp(double(r.be64(c.data + 10)), exponent);
			if (rate > 0 && std::isfinite(rate))
				res.seconds = frames / rate;
			char type[4] = { 'N', 'O', 'N', 'E' };
			if (aifc && c.size >= 22)
				r.read(c.data + 18, type, 4);
			if (!std::memcmp(type, "NONE", 4) || !std::memcmp(type, "twos", 4))
				res.codec = bits == 8 ? "pcm_s8" : waveCodec(1, bits, true);
			else if (!std::memcmp(type, "sowt", 4))
				res.codec = bits == 8 ? "pcm_s8" : waveCodec(1, bits, false);
			else if (!std::memcmp(type, "fl32", 4) || !std::memcmp(type, "FL32", 4))
				res.codec = "pcm_f32be";
			else if (!std::memcmp(type, "fl64", 4) || !std::memcmp(type, "FL64", 4))
				res.codec = "pcm_f64be";
			else if (!std::memcmp(type, "ulaw", 4) || !std::memcmp(type, "ULAW", 4))
				res.codec = "pcm_mulaw";
			else if (!std::memcmp(type, "alaw", 4) || !std::memcmp(type, "ALAW", 4))
				res.codec = "pcm_alaw";
			else if (!std::memcmp(type, "ima4", 4))
				res.codec = "adpcm_ima_qt";
			comm = true;
		} else if (!std::memcmp(c.id, "ID3 ", 4) || !std::memcmp(c.id, "id3 ", 4)) {
			parseId3v2(r, c.data, res);
		} else if (!std::memcmp(c.id, "NAME", 4) || !std::memcmp(c.id, "AUTH", 4)) {
			std::string &dst = c.id[0] == 'N' ? names.title : names.artist;
			setTag(dst,
				r.bytes(c.data,
					static_cast<size_t>(std::min<uint64_t>(c.size, kMaxTagRead))));
		}
		return true;
	});
	if (!comm)
		throw Malformed{};
	setTag(res.title, names.title);
	setTag(res.artist, names.artist);
}

} // namespace probe
