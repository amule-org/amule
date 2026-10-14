//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// Any parts of this program derived from the xMule, lMule or eMule project,
// or contributed by third-party developers are copyrighted by their
// respective authors.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
//

#ifndef CHATCAPTCHA_H
#define CHATCAPTCHA_H

#include <cstddef>
#include <cstdint>

// Accept the small, uncompressed Windows BMPs used by eMule chat CAPTCHA.
// Validate before retaining or decoding peer-controlled data. No GUI dependency.
inline bool IsValidChatCaptcha(const unsigned char *data, std::size_t size)
{
	if (!data || size <= 128 || size >= 4096 || data[0] != 'B' || data[1] != 'M') {
		return false;
	}
	const auto read16 = [data](std::size_t pos) {
		return static_cast<std::uint32_t>(data[pos]) |
		       (static_cast<std::uint32_t>(data[pos + 1]) << 8);
	};
	const auto read32 = [&read16](std::size_t pos) { return read16(pos) | (read16(pos + 2) << 16); };
	const auto header = read32(14);
	const auto offset = read32(10);
	const auto width = read32(18);
	const auto encodedHeight = read32(22);
	// Windows BMP allows a negative height for top-down rows.
	const auto height = (encodedHeight & 0x80000000U) ? 0U - encodedHeight : encodedHeight;
	const auto bits = read16(28);
	if (header < 40 || header > size - 14 || offset < 14 + header || offset > size || read32(2) != size ||
		width <= 10 || width >= 150 || height <= 10 || height >= 50 || read16(26) != 1 ||
		read32(30) != 0 ||
		(bits != 1 && bits != 4 && bits != 8 && bits != 16 && bits != 24 && bits != 32)) {
		return false;
	}
	if (bits <= 8) {
		const auto colors = read32(46) ? read32(46) : (1U << bits);
		if (colors > (1U << bits) || colors * 4 > offset - (14 + header)) {
			return false;
		}
	}
	const auto stride = ((width * bits + 31) / 32) * 4;
	return stride * height <= size - offset;
}

#endif // CHATCAPTCHA_H
