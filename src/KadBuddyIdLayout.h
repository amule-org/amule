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

#ifndef KAD_BUDDY_ID_LAYOUT_H
#define KAD_BUDDY_ID_LAYOUT_H

#include <cstdint>
#include <cstring>

namespace Kademlia
{

// A Kad buddy ID is 16 bytes. aMule's layout is CUInt128::ToByteArray's big-endian byte array.
// eMule, emule-qt and eMuleAI instead write md4str() over a CUInt128 that keeps four 32-bit
// words, most significant word first, each in host byte order -- on every current (little-endian)
// host that reverses each 4-byte group of the aMule layout. The transformation is its own
// inverse, so one function converts either direction (#1835).
inline void ConvertBuddyIdLayout(const uint8_t *src, uint8_t *dst) noexcept
{
	for (unsigned group = 0; group < 4; ++group) {
		for (unsigned i = 0; i < 4; ++i) {
			dst[group * 4 + i] = src[group * 4 + (3 - i)];
		}
	}
}

// True if the 16-byte `received` ID equals `expected` in either the aMule or the eMule layout.
inline bool MatchesBuddyIdEitherLayout(const uint8_t *received, const uint8_t *expected) noexcept
{
	if (memcmp(received, expected, 16) == 0) {
		return true;
	}
	uint8_t converted[16];
	ConvertBuddyIdLayout(expected, converted);
	return memcmp(received, converted, 16) == 0;
}

// Which reading of a source's published buddy hash the next KADEMLIA_CALLBACK_REQ uses: true for the
// aMule layout a pre-fix aMule source published. The form changes only after a request went
// unanswered, so the one that produced a callback is kept (#1835).
inline bool NextBuddyIdUsesAmuleLayout(bool usesAmuleLayout, bool previousRequestUnanswered) noexcept
{
	return previousRequestUnanswered ? !usesAmuleLayout : usesAmuleLayout;
}

} // namespace Kademlia

#endif // KAD_BUDDY_ID_LAYOUT_H
