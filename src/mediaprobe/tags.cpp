// SPDX-License-Identifier: GPL-2.0-or-later
// Text decoding and shared tag formats: ID3v2, ID3v1, APEv2 and Vorbis comments.
#include "probe.h"

#include <algorithm>
#include <cstring>

namespace probe
{

namespace
{
constexpr size_t kMaxText = 4096; // Published bytes per tag, as the helper always capped it.

void appendUtf8(std::string &out, uint32_t c)
{
	if (c < 0x80) {
		out += char(c);
	} else if (c < 0x800) {
		out += char(0xC0 | (c >> 6));
		out += char(0x80 | (c & 63));
	} else if (c < 0x10000) {
		out += char(0xE0 | (c >> 12));
		out += char(0x80 | ((c >> 6) & 63));
		out += char(0x80 | (c & 63));
	} else {
		out += char(0xF0 | (c >> 18));
		out += char(0x80 | ((c >> 12) & 63));
		out += char(0x80 | ((c >> 6) & 63));
		out += char(0x80 | (c & 63));
	}
}

bool validUtf8(const std::string &s)
{
	for (size_t i = 0; i < s.size();) {
		const auto c = static_cast<unsigned char>(s[i]);
		size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
		if (!n || i + n > s.size())
			return false;
		uint32_t cp = n == 1 ? c : c & (0x7F >> n);
		for (size_t k = 1; k < n; ++k) {
			const auto d = static_cast<unsigned char>(s[i + k]);
			if ((d & 0xC0) != 0x80)
				return false;
			cp = cp << 6 | (d & 63);
		}
		// Reject overlong forms, surrogates and values above U+10FFFF.
		if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) ||
			(cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
			return false;
		i += n;
	}
	return true;
}

std::string latin1(const std::string &s)
{
	std::string out;
	for (unsigned char c : s)
		appendUtf8(out, c);
	return out;
}

uint32_t synchsafe(const uint8_t *b)
{
	return uint32_t(b[0] & 0x7F) << 21 | uint32_t(b[1] & 0x7F) << 14 | uint32_t(b[2] & 0x7F) << 7 |
	       (b[3] & 0x7F);
}

// Removes the 0x00 that unsynchronisation inserts after every 0xFF.
std::string deunsync(const std::string &s)
{
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i) {
		out += s[i];
		if (static_cast<unsigned char>(s[i]) == 0xFF && i + 1 < s.size() && s[i + 1] == '\0')
			++i;
	}
	return out;
}

std::string id3Text(const std::string &raw)
{
	if (raw.empty())
		return {};
	const auto *p = reinterpret_cast<const uint8_t *>(raw.data()) + 1;
	const size_t n = raw.size() - 1;
	switch (raw[0]) {
	case 1: // UTF-16 with BOM; default to little-endian without one.
		if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF)
			return utf16(p + 2, n - 2, true);
		if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE)
			return utf16(p + 2, n - 2, false);
		return utf16(p, n, false);
	case 2:
		return utf16(p, n, true);
	default: // 0: ISO-8859-1, 3: UTF-8. cleanText() validates both.
		return std::string(reinterpret_cast<const char *>(p), n);
	}
}

std::string lower(std::string s)
{
	for (auto &c : s)
		if (c >= 'A' && c <= 'Z')
			c = char(c - 'A' + 'a');
	return s;
}

void id3Frames(Reader &r, int64_t p, int64_t end, unsigned version, Result &res)
{
	const int64_t header = version == 2 ? 6 : 10;
	for (unsigned guard = 0; p + header <= end && guard < 4096; ++guard) {
		char id[4] = {};
		r.read(p, id, version == 2 ? 3 : 4);
		if (!id[0])
			break; // Padding.
		uint32_t size;
		uint16_t flags = 0;
		if (version == 2) {
			size = r.be24(p + 3);
		} else {
			uint8_t b[4];
			r.read(p + 4, b, 4);
			size = version == 4 ? synchsafe(b) : r.be32(p + 4);
			flags = r.be16(p + 8);
		}
		int64_t data = p + header;
		if (size > static_cast<uint64_t>(end - data))
			break;
		p = data + size;
		std::string *dst = nullptr;
		if (!std::memcmp(id, "TIT2", 4) || !std::memcmp(id, "TT2", 4))
			dst = &res.title;
		else if (!std::memcmp(id, "TPE1", 4) || !std::memcmp(id, "TP1", 4))
			dst = &res.artist;
		else if (!std::memcmp(id, "TALB", 4) || !std::memcmp(id, "TAL", 4))
			dst = &res.album;
		if (!dst || !dst->empty())
			continue;
		bool unsync = false;
		if (version == 3) {
			if (flags & 0x00C0)
				continue; // Compressed or encrypted.
			if (flags & 0x0020)
				++data; // Grouping identity.
		} else if (version == 4) {
			if (flags & 0x000C)
				continue;
			if (flags & 0x0040)
				++data;
			if (flags & 0x0001)
				data += 4; // Data length indicator.
			unsync = flags & 0x0002;
		}
		if (data >= p)
			continue;
		std::string raw =
			r.bytes(data, static_cast<size_t>(std::min<int64_t>(p - data, kMaxTagRead)));
		setTag(*dst, id3Text(unsync ? deunsync(raw) : raw));
	}
}
} // namespace

std::string utf16(const uint8_t *p, size_t n, bool bigEndian)
{
	std::string out;
	for (size_t i = 0; i + 1 < n; i += 2) {
		uint32_t c = bigEndian ? (p[i] << 8 | p[i + 1]) : (p[i + 1] << 8 | p[i]);
		if (!c)
			break;
		if (c >= 0xD800 && c <= 0xDBFF && i + 3 < n) {
			const uint32_t d =
				bigEndian ? (p[i + 2] << 8 | p[i + 3]) : (p[i + 3] << 8 | p[i + 2]);
			if (d >= 0xDC00 && d <= 0xDFFF) {
				c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
				i += 2;
			} else {
				c = 0xFFFD;
			}
		} else if (c >= 0xD800 && c <= 0xDFFF) {
			c = 0xFFFD;
		}
		appendUtf8(out, c);
		if (out.size() > kMaxText + 4)
			break;
	}
	return out;
}

std::string cleanText(std::string s)
{
	const auto nul = s.find('\0');
	if (nul != std::string::npos)
		s.resize(nul);
	const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
	while (!s.empty() && space(s.back()))
		s.pop_back();
	size_t lead = 0;
	while (lead < s.size() && space(s[lead]))
		++lead;
	s.erase(0, lead);
	if (!validUtf8(s))
		s = latin1(s);
	if (s.size() > kMaxText) {
		size_t cut = kMaxText;
		while (cut && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
			--cut;
		s.resize(cut);
	}
	return s;
}

void setTag(std::string &dst, const std::string &value)
{
	if (dst.empty())
		dst = cleanText(value);
}

int64_t parseId3v2(Reader &r, int64_t offset, Result &res)
{
	if (!r.has(offset, 10) || !r.match(offset, "ID3", 3))
		return 0;
	uint8_t h[10];
	r.read(offset, h, 10);
	const unsigned version = h[3], flags = h[5];
	if (version < 2 || version > 4 || h[4] == 0xFF || ((h[6] | h[7] | h[8] | h[9]) & 0x80))
		return 0;
	const uint32_t size = synchsafe(h + 6);
	int64_t total = 10 + int64_t(size) + (version == 4 && (flags & 0x10) ? 10 : 0);
	if (!r.has(offset, total))
		throw Malformed{};
	int64_t body = offset + 10, end = offset + 10 + size;
	if (version == 2 && (flags & 0x40))
		return total; // ID3v2.2 compression has no defined scheme.
	if (flags & 0x40) {   // Extended header.
		uint8_t b[4];
		r.read(body, b, 4);
		const uint32_t ext = version == 4 ? synchsafe(b) : 4 + r.be32(body);
		if (ext > static_cast<uint64_t>(end - body))
			throw Malformed{};
		body += ext;
	}
	if ((flags & 0x80) && version < 4) {
		// Whole-tag unsynchronisation: decode the tag in memory. Text frames come first
		// in practice, so a capped read keeps them even with large cover art.
		const std::string raw =
			deunsync(r.bytes(body, static_cast<size_t>(std::min<int64_t>(end - body, 1 << 20))));
		MemSource mem(raw.data(), raw.size());
		Reader inner(mem);
		id3Frames(inner, 0, inner.size(), version, res);
	} else {
		id3Frames(r, body, end, version, res);
	}
	return total;
}

int64_t trailingTags(Reader &r, Result &res)
{
	int64_t end = r.size();
	Result v1;
	if (end >= 128 && r.match(end - 128, "TAG", 3)) {
		setTag(v1.title, r.bytes(end - 125, 30));
		setTag(v1.artist, r.bytes(end - 95, 30));
		setTag(v1.album, r.bytes(end - 65, 30));
		end -= 128;
	}
	if (end >= 32 && r.match(end - 32, "APETAGEX", 8)) {
		const uint32_t size = r.le32(end - 20), count = r.le32(end - 16), flags = r.le32(end - 12);
		if (size >= 32 && size <= static_cast<uint64_t>(end)) {
			const int64_t itemsEnd = end - 32;
			int64_t p = end - size;
			for (uint32_t i = 0; i < count && i < 256 && p + 9 <= itemsEnd; ++i) {
				const uint32_t length = r.le32(p), itemFlags = r.le32(p + 4);
				int64_t key = p + 8, k = key;
				while (k < itemsEnd && k - key < 256 && r.u8(k))
					++k;
				if (k >= itemsEnd || r.u8(k))
					break;
				const std::string name = lower(r.bytes(key, static_cast<size_t>(k - key)));
				const int64_t value = k + 1;
				if (length > static_cast<uint64_t>(itemsEnd - value))
					break;
				if (!(itemFlags & 6)) { // UTF-8 text item.
					const std::string text = r.bytes(value,
						static_cast<size_t>(std::min<uint64_t>(length, kMaxTagRead)));
					if (name == "title")
						setTag(res.title, text);
					else if (name == "artist")
						setTag(res.artist, text);
					else if (name == "album")
						setTag(res.album, text);
				}
				p = value + length;
			}
			end -= size + (flags & 0x80000000 ? 32 : 0);
			if (end < 0)
				throw Malformed{};
		}
	}
	setTag(res.title, v1.title);
	setTag(res.artist, v1.artist);
	setTag(res.album, v1.album);
	return end;
}

void parseVorbisComments(const uint8_t *p, size_t n, Result &res)
{
	auto le32 = [p](size_t o) {
		return uint32_t(p[o]) | uint32_t(p[o + 1]) << 8 | uint32_t(p[o + 2]) << 16 |
		       uint32_t(p[o + 3]) << 24;
	};
	if (n < 8)
		return;
	size_t o = 4 + size_t(le32(0));
	if (o < 4 || o + 4 > n)
		return;
	const uint32_t count = le32(o);
	o += 4;
	for (uint32_t i = 0; i < count && i < 4096 && o + 4 <= n; ++i) {
		const uint32_t length = le32(o);
		o += 4;
		if (length > n - o)
			break;
		const std::string entry(
			reinterpret_cast<const char *>(p + o), std::min<size_t>(length, kMaxTagRead));
		o += length;
		const auto eq = entry.find('=');
		if (eq == std::string::npos)
			continue;
		const std::string key = lower(entry.substr(0, eq)), value = entry.substr(eq + 1);
		if (key == "title")
			setTag(res.title, value);
		else if (key == "artist")
			setTag(res.artist, value);
		else if (key == "album")
			setTag(res.album, value);
	}
}

} // namespace probe
