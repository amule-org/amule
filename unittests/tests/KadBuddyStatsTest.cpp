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
using Kademlia::CBuddyRelayCounts;
using Kademlia::CMinutePeak;
using Kademlia::FormatBuddyRelaySummary;

DECLARE_SIMPLE(KadBuddyStats)

TEST(KadBuddyStats, PeakIsTheBusiestKeyInOneWindow)
{
	CMinutePeak peak;
	const uint64_t start = uint64_t(1) << 33;
	ASSERT_EQUALS(0u, peak.Peak());
	peak.Add(1, 100, start);
	peak.Add(2, 30, start + 1000);
	peak.Add(1, 50, start + CMinutePeak::WindowMs - 1);
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
	peak.Add(CMinutePeak::MaxKeys, 5, now);
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
				  "another buddy 1"),
		FormatBuddyRelaySummary(now, before));
}

TEST(KadBuddyStats, SummaryWithOnlyDrops)
{
	CBuddyRelayCounts before;
	CBuddyRelayCounts now;
	now.reasksNoBuddy = 3;
	now.reasksMalformed = 1;
	ASSERT_EQUALS(std::string("Relays in the last minute: none. Dropped: reasks without a buddy 3, "
				  "malformed reasks 1"),
		FormatBuddyRelaySummary(now, before));
}
