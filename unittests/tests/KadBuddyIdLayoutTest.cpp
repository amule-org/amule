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

#include <muleunit/test.h>
#include "KadBuddyIdLayout.h"

#include <cstring>

using namespace muleunit;
using namespace Kademlia;

DECLARE_SIMPLE(KadBuddyIdLayout)

namespace
{
// #1835 vector: source Kad ID 123456789ABCDEF00FEDCBA987654321, buddy ID (its inverse)
// EDCBA9876543210FF0123456789ABCDE.
const uint8_t amuleLayout[16] = {
	0xED, 0xCB, 0xA9, 0x87, 0x65, 0x43, 0x21, 0x0F, 0xF0, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE
};
// Same buddy ID, eMule layout: each 4-byte group of amuleLayout reversed.
const uint8_t emuleLayout[16] = {
	0x87, 0xA9, 0xCB, 0xED, 0x0F, 0x21, 0x43, 0x65, 0x56, 0x34, 0x12, 0xF0, 0xDE, 0xBC, 0x9A, 0x78
};
} // namespace

TEST(KadBuddyIdLayout, ConvertsAmuleLayoutToEmuleLayout)
{
	uint8_t converted[16];
	ConvertBuddyIdLayout(amuleLayout, converted);
	ASSERT_EQUALS(0, memcmp(converted, emuleLayout, 16));
}

TEST(KadBuddyIdLayout, ConvertsEmuleLayoutToAmuleLayout)
{
	uint8_t converted[16];
	ConvertBuddyIdLayout(emuleLayout, converted);
	ASSERT_EQUALS(0, memcmp(converted, amuleLayout, 16));
}

TEST(KadBuddyIdLayout, ConversionRoundTrips)
{
	uint8_t once[16];
	uint8_t twice[16];
	ConvertBuddyIdLayout(amuleLayout, once);
	ConvertBuddyIdLayout(once, twice);
	ASSERT_EQUALS(0, memcmp(twice, amuleLayout, 16));
}

TEST(KadBuddyIdLayout, MatchesExpectedInAmuleLayout)
{
	ASSERT_TRUE(MatchesBuddyIdEitherLayout(amuleLayout, amuleLayout));
}

TEST(KadBuddyIdLayout, MatchesExpectedInEmuleLayout)
{
	ASSERT_TRUE(MatchesBuddyIdEitherLayout(emuleLayout, amuleLayout));
}

TEST(KadBuddyIdLayout, RejectsAnIdThatMatchesNeitherLayout)
{
	uint8_t wrong[16];
	memcpy(wrong, amuleLayout, 16);
	wrong[0] ^= 0xFF;
	ASSERT_FALSE(MatchesBuddyIdEitherLayout(wrong, amuleLayout));
}
