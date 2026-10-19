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
#include "KadBuddyCallback.h"

#include <array>
#include <algorithm>

using namespace muleunit;
using namespace Kademlia;

DECLARE_SIMPLE(KadBuddyCallback)

namespace
{
// Stored IDs are big-endian bytes; Kad writes four little-endian 32-bit chunks.
const uint8_t buddyID[16] = {
	0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10
};
const uint8_t requestBytes[] = { 0x67,
	0x45,
	0x23,
	0x01,
	0xef,
	0xcd,
	0xab,
	0x89,
	0x98,
	0xba,
	0xdc,
	0xfe,
	0x10,
	0x32,
	0x54,
	0x76,
	0x11,
	0x22,
	0x33,
	0x44,
	0x55,
	0x66,
	0x77,
	0x88,
	0x99,
	0xaa,
	0xbb,
	0xcc,
	0xdd,
	0xee,
	0xff,
	0x00,
	0x36,
	0x12 };
} // namespace

TEST(KadBuddyCallback, MatchingDestinationPreservesWirePayload)
{
	std::array<uint8_t, sizeof(requestBytes)> bytes{};
	std::copy_n(requestBytes, bytes.size(), bytes.begin());
	CMemFile request(static_cast<const uint8_t *>(bytes.data()), bytes.size());
	CMemFile callback;
	ASSERT_TRUE(BuildBuddyCallback(request, CUInt128(buddyID), 0x04030201, callback));
	ASSERT_EQUALS(uint64_t(38), callback.GetLength());
	std::array<uint8_t, 38> actual{};
	callback.Seek(0, wxFromStart);
	callback.Read(actual.data(), actual.size());
	std::array<uint8_t, 38> expected{};
	std::copy_n(requestBytes, 32, expected.begin());
	const uint8_t endpoint[] = { 1, 2, 3, 4, 0x36, 0x12 };
	std::copy_n(endpoint, 6, expected.begin() + 32);
	ASSERT_TRUE(actual == expected);
	ASSERT_TRUE(std::equal(bytes.begin(), bytes.end(), requestBytes));
}

TEST(KadBuddyCallback, EveryIDByteMustMatchBeforeBuildingRelay)
{
	for (size_t i = 0; i < sizeof(buddyID); ++i) {
		std::array<uint8_t, 16> wrongID{};
		std::copy_n(buddyID, wrongID.size(), wrongID.begin());
		wrongID[i] ^= 1;
		CMemFile request(requestBytes, sizeof(requestBytes));
		CMemFile callback;
		ASSERT_FALSE(BuildBuddyCallback(request, CUInt128(wrongID.data()), 0x04030201, callback));
		ASSERT_EQUALS(uint64_t(0), callback.GetLength());
		ASSERT_EQUALS(uint64_t(16), request.GetPosition());
	}
}

TEST(KadBuddyCallback, PreviousDestinationIsRejectedAfterBuddyReplacement)
{
	CMemFile originalRequest(requestBytes, sizeof(requestBytes));
	CMemFile originalCallback;
	ASSERT_TRUE(BuildBuddyCallback(originalRequest, CUInt128(buddyID), 0x04030201, originalCallback));
	std::array<uint8_t, 16> replacementID{};
	std::copy_n(buddyID, replacementID.size(), replacementID.begin());
	replacementID.back() ^= 1;
	CMemFile staleRequest(requestBytes, sizeof(requestBytes));
	CMemFile staleCallback;
	ASSERT_FALSE(
		BuildBuddyCallback(staleRequest, CUInt128(replacementID.data()), 0x04030201, staleCallback));
	ASSERT_EQUALS(uint64_t(0), staleCallback.GetLength());
}

// An eMule-family requester sends the ID read from an aMule source's TAG_BUDDYHASH in its own
// layout (#1835). The buddy accepts it and relays the ID it stored, which the source accepts.
TEST(KadBuddyCallback, DestinationInTheOtherLayoutRelaysTheStoredID)
{
	uint8_t otherLayout[16];
	ConvertBuddyIdLayout(buddyID, otherLayout);
	CMemFile request;
	request.WriteUInt128(CUInt128(otherLayout));
	request.Write(requestBytes + 16, sizeof(requestBytes) - 16);
	request.Seek(0, wxFromStart);

	CMemFile callback;
	ASSERT_TRUE(BuildBuddyCallback(request, CUInt128(buddyID), 0x04030201, callback));
	std::array<uint8_t, 16> relayedID{};
	callback.Seek(0, wxFromStart);
	callback.Read(relayedID.data(), relayedID.size());
	ASSERT_TRUE(std::equal(relayedID.begin(), relayedID.end(), requestBytes));
}
