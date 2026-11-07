// SPDX-License-Identifier: GPL-2.0-or-later
// Native container parsers for amule-media-probe.
//
// Input comes from untrusted peers. Every read goes through Reader, which checks the
// range against the file size and charges a fixed read budget. Structural errors throw
// Malformed, which the helper reports as an unreadable file. Parsers only read container
// headers and indexes; they never decode audio or video.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace probe
{

// Malformed structure, an out-of-range read or an exhausted read budget.
struct Malformed
{
	const char *reason = "malformed container structure";
};

struct Result
{
	double seconds = 0;     // Exact duration; 0 means unknown.
	bool rawStream = false; // Elementary stream without a timeline: publish no duration.
	std::string codec, artist, album, title;
	const char *failure = nullptr; // Fixed diagnostic text, never bytes from the media file.
};

class Source
{
public:
	virtual ~Source() = default;
	virtual int64_t size() const = 0;
	// Reads up to n bytes at offset and returns the count; short only at end of file.
	virtual size_t readAt(int64_t offset, void *buffer, size_t n) = 0;
};

class MemSource : public Source
{
public:
	MemSource(const void *data, size_t size)
	: m_data(static_cast<const uint8_t *>(data))
	, m_size(size)
	{
	}
	int64_t size() const override { return static_cast<int64_t>(m_size); }
	size_t readAt(int64_t offset, void *buffer, size_t n) override;

private:
	const uint8_t *m_data;
	size_t m_size;
};

class Reader
{
public:
	explicit Reader(Source &source);
	int64_t size() const { return m_size; }
	bool has(int64_t offset, uint64_t n) const
	{
		return offset >= 0 && offset <= m_size && n <= static_cast<uint64_t>(m_size - offset);
	}
	void read(int64_t offset, void *buffer, size_t n);
	bool match(int64_t offset, const char *magic, size_t n);
	std::string bytes(int64_t offset, size_t n); // Caller bounds n.
	uint8_t u8(int64_t o);
	uint16_t be16(int64_t o);
	uint16_t le16(int64_t o);
	uint32_t be24(int64_t o);
	uint32_t be32(int64_t o);
	uint32_t le32(int64_t o);
	uint64_t be64(int64_t o);
	uint64_t le64(int64_t o);

private:
	Source &m_source;
	int64_t m_size;
	std::vector<uint8_t> m_cache;
	int64_t m_cacheStart = -1;
	size_t m_cacheLen = 0;
	uint64_t m_reads = 0, m_bytes = 0;
};

// Text and tag helpers (tags.cpp).
constexpr size_t kMaxTagRead = 64 * 1024; // Raw bytes read for one tag value.
std::string cleanText(std::string s);     // Cut at NUL, trim, UTF-8 or Latin-1, cap 4096 bytes.
std::string utf16(const uint8_t *p, size_t n, bool bigEndian);
void setTag(std::string &dst, const std::string &value);    // Fills only empty fields.
int64_t parseId3v2(Reader &r, int64_t offset, Result &res); // Tag size, or 0 if none.
int64_t trailingTags(Reader &r, Result &res);               // APEv2 and ID3v1; returns the audio end.
void parseVorbisComments(const uint8_t *p, size_t n, Result &res);

// Codec vocabulary: container identifiers to FFmpeg codec names (codecs.cpp).
std::string fourccCodec(const char fourcc[4]);
std::string waveCodec(unsigned formatTag, unsigned bits, bool bigEndian);
std::string matroskaCodec(const std::string &id, unsigned bits, const std::string &codecPrivate);

// Container parsers. Each starts at offset 0 unless it takes a start offset.
void parseRiff(Reader &r, Result &res);
void parseAiff(Reader &r, Result &res);
void parseMatroska(Reader &r, Result &res);
void parseMp4(Reader &r, Result &res);
void parseOgg(Reader &r, Result &res);
void parseAsf(Reader &r, Result &res);
void parseFlv(Reader &r, Result &res);
void parseMpegTs(Reader &r, int64_t packet, Result &res);
void parseMpegPs(Reader &r, Result &res);
void parseRealMedia(Reader &r, Result &res);
void parseFlac(Reader &r, int64_t start, Result &res);
void parseMpegAudio(Reader &r, int64_t start, Result &res);
void parseAdts(Reader &r, int64_t start, Result &res);
void parseAmr(Reader &r, Result &res);
void parseTta(Reader &r, int64_t start, Result &res);
void parseWavPack(Reader &r, int64_t start, Result &res);

// Returns 0 with metadata, 10 when the file has no usable metadata, 11 when the format is
// unknown or the structure is malformed.
int probe(Source &source, Result &res);

} // namespace probe
