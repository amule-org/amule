// SPDX-License-Identifier: GPL-2.0-or-later
// Bounded reader and format dispatch.
#include "probe.h"

#include <algorithm>
#include <cstring>

namespace probe
{

namespace
{
// A 512 MiB MP3 frame scan reads the whole file once; nothing legitimate needs more.
constexpr uint64_t kMaxBytes = 640ULL * 1024 * 1024;
constexpr uint64_t kMaxReads = 8ULL * 1024 * 1024;
constexpr size_t kCacheSize = 64 * 1024;
} // namespace

size_t MemSource::readAt(int64_t offset, void *buffer, size_t n)
{
	if (offset < 0 || static_cast<uint64_t>(offset) >= m_size)
		return 0;
	const size_t count = std::min(n, m_size - static_cast<size_t>(offset));
	std::memcpy(buffer, m_data + offset, count);
	return count;
}

Reader::Reader(Source &source)
: m_source(source)
, m_size(std::max<int64_t>(source.size(), 0))
, m_cache(kCacheSize)
{
}

void Reader::read(int64_t offset, void *buffer, size_t n)
{
	if (!has(offset, n))
		throw Malformed{ "read outside the input file" };
	if (++m_reads > kMaxReads)
		throw Malformed{ "read-call budget exhausted" };
	auto *out = static_cast<uint8_t *>(buffer);
	if (n > kCacheSize) {
		m_bytes += n;
		if (m_bytes > kMaxBytes)
			throw Malformed{ "read-byte budget exhausted" };
		if (m_source.readAt(offset, out, n) != n)
			throw Malformed{ "short read from the input file" };
		return;
	}
	if (m_cacheStart < 0 || offset < m_cacheStart ||
		static_cast<uint64_t>(offset - m_cacheStart) + n > m_cacheLen) {
		const size_t want = static_cast<size_t>(std::min<int64_t>(kCacheSize, m_size - offset));
		m_bytes += want;
		if (m_bytes > kMaxBytes)
			throw Malformed{ "read-byte budget exhausted" };
		m_cacheStart = offset;
		m_cacheLen = m_source.readAt(offset, m_cache.data(), want);
		if (m_cacheLen < n)
			throw Malformed{ "short read from the input file" };
	}
	std::memcpy(out, m_cache.data() + (offset - m_cacheStart), n);
}

bool Reader::match(int64_t offset, const char *magic, size_t n)
{
	if (!has(offset, n))
		return false;
	char buf[16];
	read(offset, buf, n);
	return !std::memcmp(buf, magic, n);
}

std::string Reader::bytes(int64_t offset, size_t n)
{
	std::string s(n, '\0');
	if (n)
		read(offset, &s[0], n);
	return s;
}

uint8_t Reader::u8(int64_t o)
{
	uint8_t b;
	read(o, &b, 1);
	return b;
}

uint16_t Reader::be16(int64_t o)
{
	uint8_t b[2];
	read(o, b, 2);
	return uint16_t(b[0] << 8 | b[1]);
}

uint16_t Reader::le16(int64_t o)
{
	uint8_t b[2];
	read(o, b, 2);
	return uint16_t(b[1] << 8 | b[0]);
}

uint32_t Reader::be24(int64_t o)
{
	uint8_t b[3];
	read(o, b, 3);
	return uint32_t(b[0]) << 16 | uint32_t(b[1]) << 8 | b[2];
}

uint32_t Reader::be32(int64_t o)
{
	uint8_t b[4];
	read(o, b, 4);
	return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
}

uint32_t Reader::le32(int64_t o)
{
	uint8_t b[4];
	read(o, b, 4);
	return uint32_t(b[3]) << 24 | uint32_t(b[2]) << 16 | uint32_t(b[1]) << 8 | b[0];
}

uint64_t Reader::be64(int64_t o)
{
	return uint64_t(be32(o)) << 32 | be32(o + 4);
}

uint64_t Reader::le64(int64_t o)
{
	return uint64_t(le32(o + 4)) << 32 | le32(o);
}

namespace
{
bool isMpegAudioSync(Reader &r, int64_t p)
{
	if (!r.has(p, 2))
		return false;
	const unsigned b0 = r.u8(p), b1 = r.u8(p + 1);
	return b0 == 0xFF && (b1 & 0xE0) == 0xE0 && (b1 & 0x06) != 0;
}

bool isAdtsSync(Reader &r, int64_t p)
{
	return r.has(p, 2) && r.u8(p) == 0xFF && (r.u8(p + 1) & 0xF6) == 0xF0;
}

// Annex B H.264/HEVC elementary stream: codec only, no reliable timeline.
bool rawVideo(Reader &r, Result &res)
{
	int64_t p = r.match(0, "\0\0\0\1", 4) ? 4 : r.match(0, "\0\0\1", 3) ? 3 : -1;
	if (p < 0 || !r.has(p, 2))
		return false;
	const unsigned nal = r.u8(p), next = r.u8(p + 1);
	const unsigned hevcType = (nal >> 1) & 0x3F;
	if (!(nal & 0x80) && (hevcType == 32 || hevcType == 35) && next == 0x01)
		res.codec = "hevc";
	else if (!(nal & 0x80) && ((nal & 0x1F) == 7 || (nal & 0x1F) == 9))
		res.codec = "h264";
	else
		return false;
	res.rawStream = true;
	return true;
}

bool isMp4(Reader &r)
{
	static const char *const types[] = { "ftyp", "moov", "mdat", "free", "skip", "wide", "pnot" };
	for (const char *type : types)
		if (r.match(4, type, 4))
			return true;
	return false;
}

// 188 for TS, 192 for M2TS (a 4-byte prefix per packet), or 0 without three sync bytes one
// packet apart.
int64_t mpegTsPacket(Reader &r)
{
	for (int64_t packet : { 188, 192 }) {
		const int64_t lead = packet == 192 ? 4 : 0;
		bool ok = r.has(lead + 2 * packet, 1);
		for (int i = 0; ok && i < 3; ++i)
			ok = r.u8(lead + i * packet) == 0x47;
		if (ok)
			return packet;
	}
	return 0;
}

void dispatch(Reader &r, Result &res)
{
	// Audio formats may carry one or more ID3v2 tags in front.
	int64_t start = 0;
	for (int i = 0; i < 4; ++i) {
		const int64_t n = parseId3v2(r, start, res);
		if (!n)
			break;
		start += n;
	}
	if (start) {
		if (r.match(start, "fLaC", 4))
			return parseFlac(r, start, res);
		if (r.match(start, "TTA1", 4))
			return parseTta(r, start, res);
		if (r.match(start, "wvpk", 4))
			return parseWavPack(r, start, res);
		if (isAdtsSync(r, start))
			return parseAdts(r, start, res);
		return parseMpegAudio(r, start, res);
	}
	if (r.match(0, "RIFF", 4) || r.match(0, "RIFX", 4) || r.match(0, "RF64", 4) || r.match(0, "BW64", 4))
		return parseRiff(r, res);
	if (r.match(0, "FORM", 4))
		return parseAiff(r, res);
	if (r.match(0, "\x1A\x45\xDF\xA3", 4))
		return parseMatroska(r, res);
	if (r.match(0, "OggS", 4))
		return parseOgg(r, res);
	if (r.match(0, "fLaC", 4))
		return parseFlac(r, 0, res);
	if (r.match(0, "\x30\x26\xB2\x75\x8E\x66\xCF\x11", 8))
		return parseAsf(r, res);
	if (r.match(0, "FLV\x01", 4))
		return parseFlv(r, res);
	if (r.match(0, ".RMF", 4))
		return parseRealMedia(r, res);
	if (r.match(0, "#!AMR", 5))
		return parseAmr(r, res);
	if (r.match(0, "TTA1", 4))
		return parseTta(r, 0, res);
	if (r.match(0, "wvpk", 4))
		return parseWavPack(r, 0, res);
	if (isMp4(r))
		return parseMp4(r, res);
	if (r.match(0, "\0\0\1\xBA", 4))
		return parseMpegPs(r, res);
	if (const int64_t packet = mpegTsPacket(r))
		return parseMpegTs(r, packet, res);
	if (rawVideo(r, res))
		return;
	if (isAdtsSync(r, 0))
		return parseAdts(r, 0, res);
	if (isMpegAudioSync(r, 0))
		return parseMpegAudio(r, 0, res);
	throw Malformed{ "unsupported container signature" };
}
} // namespace

int probe(Source &source, Result &res)
{
	Reader r(source);
	try {
		if (r.size() < 4) {
			res = Result{};
			res.failure = "input is too short to identify a container";
			return 11;
		}
		dispatch(r, res);
	} catch (const Malformed &error) {
		res = Result{};
		res.failure = error.reason;
		return 11;
	}
	if (res.rawStream)
		res.seconds = 0;
	if (res.seconds > 0 || !res.codec.empty())
		return 0;
	res.failure = "container has no usable duration or codec";
	return 10;
}

} // namespace probe
