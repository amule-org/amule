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
#include <KadBuddySearchPolicy.h>

using namespace muleunit;
using namespace Kademlia;

DECLARE_SIMPLE(KadBuddySearchPolicy)

TEST(KadBuddySearchPolicy, TheRetryIsTenMinutes)
{
	ASSERT_EQUALS(600, static_cast<int>(BuddySearchRetrySeconds));
}

TEST(KadBuddySearchPolicy, TheBuddyIDIsTheInverseOfTheKadID)
{
	const CUInt128 kadID(0x12345678u);
	CUInt128 expected(true);
	expected ^= kadID;
	ASSERT_TRUE(OwnBuddyID(kadID) == expected);
	ASSERT_TRUE((OwnBuddyID(kadID) ^ kadID) == CUInt128(true));
}

TEST(KadBuddySearchPolicy, OnlyTheFirstSearchLooksUpTheBuddyID)
{
	const CUInt128 kadID(0x12345678u);
	const CUInt128 random(0x0badf00du);
	ASSERT_TRUE(BuddySearchTarget(kadID, true, random) == OwnBuddyID(kadID));
	ASSERT_TRUE(BuddySearchTarget(kadID, false, random) == random);
}
