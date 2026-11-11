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
#include <KadServedBuddyPolicy.h>

using namespace muleunit;
using namespace Kademlia;

DECLARE_SIMPLE(KadServedBuddyPolicy)

TEST(KadServedBuddyPolicy, TheSettingIsCappedByTheConnectionLimit)
{
	ASSERT_EQUALS(8u, EffectiveServedBuddyLimit(8, 500));
	ASSERT_EQUALS(16u, EffectiveServedBuddyLimit(16, 500));
	// A tenth of the connection limit, rounded down, wins over the setting.
	ASSERT_EQUALS(4u, EffectiveServedBuddyLimit(8, 40));
	ASSERT_EQUALS(7u, EffectiveServedBuddyLimit(8, 79));
	ASSERT_EQUALS(2u, EffectiveServedBuddyLimit(8, 20));
	// Never fewer than one, the stock count, even below a connection limit of 10.
	ASSERT_EQUALS(1u, EffectiveServedBuddyLimit(8, 19));
	ASSERT_EQUALS(1u, EffectiveServedBuddyLimit(8, 5));
	// A setting outside 4..16, as from a hand-edited amule.conf, is clamped first.
	ASSERT_EQUALS(4u, EffectiveServedBuddyLimit(1, 500));
	ASSERT_EQUALS(16u, EffectiveServedBuddyLimit(100, 500));
}

TEST(KadServedBuddyPolicy, NatTSlotsAreAThirdOnlyWhenWeRelayTheRendezvous)
{
	ASSERT_EQUALS(0u, ReservedNatTSlots(8, false));
	ASSERT_EQUALS(1u, ReservedNatTSlots(4, true));
	ASSERT_EQUALS(2u, ReservedNatTSlots(8, true));
	ASSERT_EQUALS(5u, ReservedNatTSlots(16, true));
	ASSERT_EQUALS(0u, ReservedNatTSlots(2, true));
}

TEST(KadServedBuddyPolicy, TheNatTBitIsEitherTransport)
{
	ASSERT_TRUE(IsNatTCapableRequest(0x80));
	ASSERT_TRUE(IsNatTCapableRequest(0x40));
	ASSERT_TRUE(IsNatTCapableRequest(0xC8));
	ASSERT_FALSE(IsNatTCapableRequest(0x08));
	ASSERT_FALSE(IsNatTCapableRequest(0x00));
}

TEST(KadServedBuddyPolicy, WithoutKeptSlotsEveryRequesterFillsTheLimit)
{
	ASSERT_TRUE(AdmitsServedBuddy(7, 0, 8, 0, false));
	ASSERT_FALSE(AdmitsServedBuddy(8, 0, 8, 0, false));
	ASSERT_FALSE(AdmitsServedBuddy(8, 3, 8, 0, true));
}

TEST(KadServedBuddyPolicy, KeptSlotsAreForNatTNodesOnly)
{
	// 8 slots, 2 kept. Six other buddies fill the open slots.
	ASSERT_FALSE(AdmitsServedBuddy(6, 0, 8, 2, false));
	ASSERT_TRUE(AdmitsServedBuddy(6, 0, 8, 2, true));
	ASSERT_TRUE(AdmitsServedBuddy(7, 1, 8, 2, true));
	ASSERT_FALSE(AdmitsServedBuddy(8, 2, 8, 2, true));
}

TEST(KadServedBuddyPolicy, NatTNodesFillKeptSlotsFirstThenOpenOnes)
{
	// 8 slots, 2 kept, 3 NAT-T buddies: the third takes an open slot, so 5 open remain for 5 others.
	ASSERT_TRUE(AdmitsServedBuddy(7, 3, 8, 2, false));
	ASSERT_FALSE(AdmitsServedBuddy(8, 3, 8, 2, false));
	// 2 NAT-T in the kept slots leave all 6 open slots to others.
	ASSERT_TRUE(AdmitsServedBuddy(7, 2, 8, 2, false));
	ASSERT_FALSE(AdmitsServedBuddy(8, 2, 8, 2, false));
}
