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

#include "BanRecord.h"

using namespace muleunit;

DECLARE_SIMPLE(BanRecord)

namespace
{
// A tick well clear of zero, so an expiry comparison cannot pass by accident
// on an uninitialised value.
const uint64 T0 = 1000000;
const uint32 IP_A = 0x0100007f;
const uint32 IP_B = 0x0200007f;
} // namespace

// The whole point of the class: the caller increments a counter when this returns true, so a second
// ban of the same address must answer false or the statistic drifts.
// CUpDownClient::SetSpammer(true) calls Ban() with no IsBanned() check, which is how that second
// call happens in practice.
// ASSERT_EQUALS takes its arguments by const reference, so this odr-uses BAN_DURATION_MS. With
// the member declared static const and defined nowhere, it does not link; constexpr is what makes
// it work. The assertion itself is almost beside the point, the reference binding is the test.
TEST(BanRecord, TheBanDurationConstantCanBeReferenced)
{
	ASSERT_EQUALS(static_cast<uint64>(CLIENTBANTIME), CBanRecord::BAN_DURATION_MS);
}

TEST(BanRecord, BanningTheSameAddressTwiceCountsOnce)
{
	CBanRecord record;

	ASSERT_TRUE(record.Ban(IP_A, T0));
	ASSERT_EQUALS(1u, (unsigned)record.Size());

	ASSERT_FALSE(record.Ban(IP_A, T0 + 10));
	ASSERT_EQUALS(1u, (unsigned)record.Size());

	// The tick is still refreshed, so the ban is extended rather than left to
	// expire on the first one's schedule.
	ASSERT_TRUE(record.IsBanned(IP_A, T0 + 10 + CBanRecord::BAN_DURATION_MS - 1));
	ASSERT_FALSE(record.IsBanned(IP_A, T0 + 10 + CBanRecord::BAN_DURATION_MS));
}

// The mirror. Unbanning an address that is not banned must answer false, or
// the same counter drifts the other way.
TEST(BanRecord, UnbanningAnAddressThatIsNotBannedCountsNothing)
{
	CBanRecord record;

	ASSERT_FALSE(record.Unban(IP_A));
	ASSERT_EQUALS(0u, (unsigned)record.Size());

	ASSERT_TRUE(record.Ban(IP_A, T0));
	ASSERT_TRUE(record.Unban(IP_A));
	ASSERT_EQUALS(0u, (unsigned)record.Size());

	// And again, now that it is genuinely gone.
	ASSERT_FALSE(record.Unban(IP_A));
}

// Zero is not an address. CUpDownClient's constructor sets the address to zero when there is no
// socket, so one entry under that key would make every such client read back as banned.
TEST(BanRecord, ZeroIsNeverBannedAndNeverAKey)
{
	CBanRecord record;

	ASSERT_FALSE(record.Ban(0, T0));
	ASSERT_EQUALS(0u, (unsigned)record.Size());
	ASSERT_FALSE(record.IsBanned(0, T0));

	// A real ban does not make the zero key readable either.
	ASSERT_TRUE(record.Ban(IP_A, T0));
	ASSERT_FALSE(record.IsBanned(0, T0));
}

// Expiry is read at the lookup rather than swept, so a lapsed ban must answer false the moment it
// lapses -- and must stop being counted, because the caller decrements on the transition.
TEST(BanRecord, ALapsedBanIsForgottenOnLookup)
{
	CBanRecord record;
	ASSERT_TRUE(record.Ban(IP_A, T0));

	const uint64 lapsed = T0 + CBanRecord::BAN_DURATION_MS;
	ASSERT_FALSE(record.IsBanned(IP_A, lapsed));
	// Forgotten, not merely reported false: the entry is gone, so the caller
	// that decremented on this transition will not decrement again.
	ASSERT_EQUALS(0u, (unsigned)record.Size());
	ASSERT_FALSE(record.Unban(IP_A));
}

// The lookup reports whether it dropped an entry, so the caller knows whether to decrement.
// Reporting the drop is the only way it can: it has no other view of the map.
TEST(BanRecord, TheLookupReportsWhetherItDroppedALapsedEntry)
{
	CBanRecord record;
	ASSERT_TRUE(record.Ban(IP_A, T0));

	bool dropped = false;
	ASSERT_TRUE(record.IsBanned(IP_A, T0 + 1, &dropped));
	ASSERT_FALSE(dropped);

	ASSERT_FALSE(record.IsBanned(IP_A, T0 + CBanRecord::BAN_DURATION_MS, &dropped));
	ASSERT_TRUE(dropped);

	// Nothing left to drop the second time.
	ASSERT_FALSE(record.IsBanned(IP_A, T0 + CBanRecord::BAN_DURATION_MS, &dropped));
	ASSERT_FALSE(dropped);
}

// Two addresses are two bans. Trivial, but it is what makes Size() a
// meaningful stand-in for the statistic the caller keeps.
TEST(BanRecord, DistinctAddressesAreCountedSeparately)
{
	CBanRecord record;

	ASSERT_TRUE(record.Ban(IP_A, T0));
	ASSERT_TRUE(record.Ban(IP_B, T0));
	ASSERT_EQUALS(2u, (unsigned)record.Size());

	ASSERT_TRUE(record.Unban(IP_A));
	ASSERT_EQUALS(1u, (unsigned)record.Size());
	ASSERT_TRUE(record.IsBanned(IP_B, T0 + 1));
	ASSERT_FALSE(record.IsBanned(IP_A, T0 + 1));
}

// The sweep exists so a table of long-lapsed entries does not grow without bound when nobody looks
// those addresses up again. It reports how many it dropped, for the same reason the lookup does.
TEST(BanRecord, TheSweepDropsOnlyLapsedEntriesAndReportsHowMany)
{
	CBanRecord record;
	ASSERT_TRUE(record.Ban(IP_A, T0));
	ASSERT_TRUE(record.Ban(IP_B, T0 + CBanRecord::BAN_DURATION_MS));

	// Only the first has lapsed at this point.
	const uint64 now = T0 + CBanRecord::BAN_DURATION_MS + 1;
	ASSERT_EQUALS(1u, (unsigned)record.DropLapsed(now));
	ASSERT_EQUALS(1u, (unsigned)record.Size());
	ASSERT_TRUE(record.IsBanned(IP_B, now));

	// A second sweep at the same instant has nothing left to do.
	ASSERT_EQUALS(0u, (unsigned)record.DropLapsed(now));
}
