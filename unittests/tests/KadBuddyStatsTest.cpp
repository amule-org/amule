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
#include <KadBuddyStats.h>

using namespace muleunit;
using Kademlia::BuddyId;
using Kademlia::BuddyIdHex;
using Kademlia::CBuddyRelayCounts;
using Kademlia::CDepartedBuddies;
using Kademlia::CDepartedBuddy;
using Kademlia::CMinutePeak;
using Kademlia::ConvertBuddyIdLayout;
using Kademlia::CServedBuddyLink;
using Kademlia::FormatBuddyDuration;
using Kademlia::FormatBuddyRelaySummary;
using Kademlia::FormatServedBuddyRelays;

namespace
{
BuddyId Id(uint8_t first)
{
	BuddyId id{};
	for (std::size_t i = 0; i < id.size(); ++i) {
		id[i] = static_cast<uint8_t>(first + i);
	}
	return id;
}

CDepartedBuddy Departed(uint8_t first, uint64_t endedAt, uint32_t links = 1)
{
	CDepartedBuddy buddy;
	buddy.buddyId = Id(first);
	buddy.address = "192.0.2." + std::to_string(first);
	buddy.endedAt = endedAt;
	buddy.links = links;
	return buddy;
}
} // namespace

DECLARE_SIMPLE(KadBuddyStats)

TEST(KadBuddyStats, PeakIsTheBusiestKeyInOneWindow)
{
	CMinutePeak peak;
	const uint64_t start = uint64_t(1) << 33;
	ASSERT_EQUALS(0u, peak.Peak());
	ASSERT_EQUALS(100u, peak.Add(1, 100, start));
	peak.Add(2, 30, start + 1000);
	ASSERT_EQUALS(150u, peak.Add(1, 50, start + CMinutePeak::WindowMs - 1));
	ASSERT_EQUALS(150u, peak.Peak());
	// A new window starts each key from zero, and the peak stays.
	peak.Add(1, 20, start + CMinutePeak::WindowMs);
	peak.Add(2, 140, start + CMinutePeak::WindowMs + 1);
	ASSERT_EQUALS(150u, peak.Peak());
	peak.Add(2, 20, start + CMinutePeak::WindowMs + 2);
	ASSERT_EQUALS(160u, peak.Peak());
}

TEST(KadBuddyStats, PeakIgnoresNewKeysPastTheCap)
{
	CMinutePeak peak;
	const uint64_t now = uint64_t(1) << 33;
	for (uint32_t key = 0; key < CMinutePeak::MaxKeys; ++key) {
		peak.Add(key, 1, now);
	}
	ASSERT_EQUALS(0u, peak.Add(CMinutePeak::MaxKeys, 5, now));
	ASSERT_EQUALS(1u, peak.Peak());
	// A key already in the window still counts.
	peak.Add(0, 5, now);
	ASSERT_EQUALS(6u, peak.Peak());
	// The next window has room again.
	peak.Add(CMinutePeak::MaxKeys, 9, now + CMinutePeak::WindowMs);
	ASSERT_EQUALS(9u, peak.Peak());
}

TEST(KadBuddyStats, SummaryIsEmptyWithoutChanges)
{
	CBuddyRelayCounts counts;
	counts.callbacks = 4;
	ASSERT_EQUALS(std::string(), FormatBuddyRelaySummary(counts, counts));
}

TEST(KadBuddyStats, SummaryListsOnlyWhatChanged)
{
	CBuddyRelayCounts before;
	before.callbacks = 1;
	before.callbackBytes = 96;
	CBuddyRelayCounts now = before;
	now.reasks = 2;
	now.reaskBytes = 186;
	now.callbacksMismatched = 1;
	ASSERT_EQUALS(std::string("Relays in the last minute: reasks 2 (186 B). Dropped: callbacks for "
				  "an unknown buddy 1"),
		FormatBuddyRelaySummary(now, before, "unused"));
}

TEST(KadBuddyStats, SummarySplitsDropsForABuddyThatLeft)
{
	CBuddyRelayCounts before;
	CBuddyRelayCounts now;
	now.callbacksNoBuddy = 2;
	now.callbacksMismatched = 1;
	now.callbacksDeparted = 2;
	now.callbacksRateLimited = 1;
	ASSERT_EQUALS(std::string("Relays in the last minute: none. Dropped: callbacks for a buddy that "
				  "left 2, callbacks for an unknown buddy 1, callbacks over the rate "
				  "limit 1. Latest buddy that left: 192.0.2.7, 13 min 0 s ago"),
		FormatBuddyRelaySummary(now, before, "192.0.2.7, 13 min 0 s ago"));
}

TEST(KadBuddyStats, SummaryWithOnlyDrops)
{
	CBuddyRelayCounts before;
	CBuddyRelayCounts now;
	now.reasksNoBuddy = 3;
	now.reasksMalformed = 1;
	ASSERT_EQUALS(std::string("Relays in the last minute: none. Dropped: reasks for an unknown buddy "
				  "3, malformed reasks 1"),
		FormatBuddyRelaySummary(now, before));
}

TEST(KadBuddyStats, DurationPicksItsUnits)
{
	ASSERT_EQUALS(std::string("0 s"), FormatBuddyDuration(999));
	ASSERT_EQUALS(std::string("59 s"), FormatBuddyDuration(59999));
	ASSERT_EQUALS(std::string("5 min 2 s"), FormatBuddyDuration(302000));
	ASSERT_EQUALS(std::string("3 h 12 min"), FormatBuddyDuration((3 * 3600 + 12 * 60 + 5) * 1000ULL));
}

TEST(KadBuddyStats, LinkCountsRelaysAndItsBusiestMinute)
{
	CServedBuddyLink link;
	const uint64_t start = uint64_t(1) << 33;
	ASSERT_EQUALS(38u, link.Relay(true, 38, start));
	ASSERT_EQUALS(131u, link.Relay(false, 93, start + 1000));
	// A new window starts from zero; the busiest minute stays.
	ASSERT_EQUALS(38u, link.Relay(true, 38, start + CMinutePeak::WindowMs));
	ASSERT_EQUALS(std::string("callbacks 2 (76 B), reasks 1 (93 B), busiest minute 131 B"),
		FormatServedBuddyRelays(link));
}

TEST(KadBuddyStats, LinkBudgetCountsOnlyTheCurrentMinute)
{
	CServedBuddyLink link;
	const uint64_t start = uint64_t(1) << 33;
	link.Relay(false, 900, start);
	ASSERT_TRUE(link.HasBudgetFor(100, 1000, start + 1000));
	ASSERT_FALSE(link.HasBudgetFor(101, 1000, start + 1000));
	// The next window starts empty.
	ASSERT_TRUE(link.HasBudgetFor(1000, 1000, start + CMinutePeak::WindowMs));
	ASSERT_FALSE(link.HasBudgetFor(1001, 1000, start + CMinutePeak::WindowMs));
}

TEST(KadBuddyStats, LinkAllowsOneCallbackAMinuteFromEachSender)
{
	CServedBuddyLink link;
	const uint64_t start = uint64_t(1) << 33;
	ASSERT_TRUE(link.AllowCallbackFrom(1, start));
	ASSERT_FALSE(link.AllowCallbackFrom(1, start + CMinutePeak::WindowMs - 1));
	ASSERT_TRUE(link.AllowCallbackFrom(2, start + 1000));
	ASSERT_TRUE(link.AllowCallbackFrom(1, start + CMinutePeak::WindowMs));
	ASSERT_FALSE(link.AllowCallbackFrom(1, start + CMinutePeak::WindowMs + 1));
}

TEST(KadBuddyStats, LinkCallbackTableMakesRoomFromExpiredSenders)
{
	CServedBuddyLink link;
	const uint64_t start = uint64_t(1) << 33;
	for (uint32_t sender = 0; sender < CServedBuddyLink::MaxCallbackSenders; ++sender) {
		link.AllowCallbackFrom(sender, start);
	}
	const uint64_t later = start + CMinutePeak::WindowMs;
	ASSERT_TRUE(link.AllowCallbackFrom(1000, later));
	ASSERT_EQUALS(1u, link.callbackSenders.size());
	// The new sender is tracked, so a repeat is limited.
	ASSERT_FALSE(link.AllowCallbackFrom(1000, later + 1));
}

TEST(KadBuddyStats, LinkCallbackTableFullOfLiveSendersFailsOpen)
{
	CServedBuddyLink link;
	const uint64_t start = uint64_t(1) << 33;
	for (uint32_t sender = 0; sender < CServedBuddyLink::MaxCallbackSenders; ++sender) {
		link.AllowCallbackFrom(sender, start);
	}
	ASSERT_TRUE(link.AllowCallbackFrom(1000, start + 1));
	ASSERT_TRUE(link.AllowCallbackFrom(1000, start + 2));
	ASSERT_EQUALS(CServedBuddyLink::MaxCallbackSenders, link.callbackSenders.size());
	// A sender already in the table is still limited.
	ASSERT_FALSE(link.AllowCallbackFrom(5, start + 3));
}

TEST(KadBuddyStats, DepartedLookupOnAnEmptyListFindsNothing)
{
	CDepartedBuddies departed;
	ASSERT_FALSE(departed.Find(Id(1).data(), 0).has_value());
}

TEST(KadBuddyStats, DepartedLookupMissesAnUnknownId)
{
	CDepartedBuddies departed;
	departed.Record(Departed(1, 1000), 2);
	ASSERT_FALSE(departed.Find(Id(50).data(), 2000).has_value());
	ASSERT_EQUALS(std::string("192.0.2.1"), departed.Find(Id(1).data(), 2000)->address);
}

TEST(KadBuddyStats, DepartedLookupMatchesTheEmuleLayout)
{
	CDepartedBuddies departed;
	departed.Record(Departed(1, 1000), 2);
	uint8_t emule[16];
	ConvertBuddyIdLayout(Id(1).data(), emule);
	ASSERT_TRUE(departed.Find(emule, 2000).has_value());
}

TEST(KadBuddyStats, DepartedEntriesAgeOut)
{
	CDepartedBuddies departed;
	departed.Record(Departed(1, 1000), 2);
	ASSERT_TRUE(departed.Find(Id(1).data(), 1000 + CDepartedBuddies::MaxAgeMs - 1).has_value());
	ASSERT_FALSE(departed.Find(Id(1).data(), 1000 + CDepartedBuddies::MaxAgeMs).has_value());
	// The next record removes it.
	departed.Record(Departed(2, 1000 + CDepartedBuddies::MaxAgeMs), 2);
	ASSERT_EQUALS(1u, departed.Size());
}

TEST(KadBuddyStats, DepartedEntriesOutliveTheKadSourceLifetime)
{
	CDepartedBuddies departed;
	departed.Record(Departed(1, 1000), 2);
	// The latest drop measured live, 7 h 37 min after the buddy left.
	const uint64_t measured = (7 * 60 + 37) * 60 * 1000ULL;
	ASSERT_TRUE(departed.Find(Id(1).data(), 1000 + measured).has_value());
}

TEST(KadBuddyStats, DepartedKeepsTheNewestWithinCapacity)
{
	CDepartedBuddies departed;
	for (uint8_t i = 1; i <= 4; ++i) {
		departed.Record(Departed(i * 20, i * 1000), 2);
	}
	ASSERT_EQUALS(2u, departed.Size());
	ASSERT_FALSE(departed.Find(Id(20).data(), 5000).has_value());
	ASSERT_TRUE(departed.Find(Id(80).data(), 5000).has_value());
	// A lower capacity, as from a lowered limit, trims at the next record.
	departed.Record(Departed(100, 6000), 1);
	ASSERT_EQUALS(1u, departed.Size());
	ASSERT_FALSE(departed.Find(Id(80).data(), 6000).has_value());
	departed.Record(Departed(120, 7000), 0);
	ASSERT_EQUALS(0u, departed.Size());
	ASSERT_FALSE(departed.Find(Id(120).data(), 7000).has_value());
}

TEST(KadBuddyStats, DepartedRelinkReplacesTheEntry)
{
	CDepartedBuddies departed;
	departed.Record(Departed(1, 1000, 1), 2);
	departed.Record(Departed(40, 2000, 1), 2);
	departed.Record(Departed(1, 3000, 2), 2);
	ASSERT_EQUALS(2u, departed.Size());
	const auto found = departed.Find(Id(1).data(), 4000);
	ASSERT_EQUALS(2u, found->links);
	ASSERT_EQUALS(3000u, found->endedAt);
	ASSERT_TRUE(departed.Find(Id(40).data(), 4000).has_value());
}

TEST(KadBuddyStats, BuddyIdHexIsUpperCaseInStoredOrder)
{
	const uint8_t id[16] = {
		0x00, 0x01, 0x0A, 0xFF, 0x10, 0x2B, 0x3C, 0x4D, 0x5E, 0x6F, 0x70, 0x81, 0x92, 0xA3, 0xB4, 0xC5
	};
	ASSERT_EQUALS(std::string("00010AFF102B3C4D5E6F708192A3B4C5"), BuddyIdHex(id));
}
