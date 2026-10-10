//								-*- C++ -*-
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
#include <kademlia/utils/LookupTrace.h>
using namespace muleunit;
using Kademlia::CLookupTrace;
DECLARE_SIMPLE(LookupTrace)
TEST(LookupTrace, LateAndDuplicateReplies)
{
	CLookupTrace trace;
	trace.Query({ 1, 2 }, {}, 100);
	ASSERT_EQUALS(size_t(0), trace.Overdue(3099, 3000));
	ASSERT_EQUALS(size_t(1), trace.Overdue(3100, 3000));
	ASSERT_FALSE(trace.Reply({ 1, 3 }, 4000));
	ASSERT_TRUE(trace.Reply({ 1, 2 }, 4100));
	ASSERT_FALSE(trace.Reply({ 1, 2 }, 4200));
	ASSERT_EQUALS(size_t(0), trace.Overdue(5000, 3000));
	trace.Query({ 1, 2 }, {}, 6000);
	ASSERT_EQUALS(size_t(1), trace.Overdue(9000, 3000));
	ASSERT_EQUALS(2u, trace.Peers().begin()->second.requests);
}
TEST(LookupTrace, BoundsAndOwnedSnapshots)
{
	CLookupTrace trace;
	for (uint32_t i = 1; i <= 10000; ++i) {
		trace.Query({ i, 1 }, {}, i);
	}
	ASSERT_EQUALS(size_t(128), trace.Peers().size());
	ASSERT_TRUE(trace.Omitted() > 0);
	for (int i = 0; i < 10000; ++i) {
		trace.Referral({ 2, 1 }, { 1, 1 }, 10000 + i, true);
	}
	ASSERT_EQUALS(size_t(256), trace.Events().size());
	auto snapshot = trace;
	trace.Reply({ 1, 1 }, 21000);
	ASSERT_TRUE(snapshot.Peers().begin()->second.pending);
	ASSERT_FALSE(trace.Peers().begin()->second.pending);
}
TEST(LookupTrace, UnsolicitedResultsAndClockRegression)
{
	CLookupTrace trace;
	trace.Result({ 1, 1 }, 50);
	trace.Referral({ 2, 1 }, { 1, 1 }, 50, true);
	ASSERT_TRUE(trace.Events().empty());
	trace.Query({ 1, 1 }, {}, 100);
	ASSERT_EQUALS(size_t(0), trace.Overdue(0, 3000));
	trace.Result({ 1, 1 }, 101);
	ASSERT_EQUALS(0u, trace.Peers().begin()->second.results);
	trace.ItemRequest({ 1, 1 }, 102);
	trace.Result({ 1, 1 }, 103);
	ASSERT_EQUALS(1u, trace.Peers().begin()->second.results);
	ASSERT_TRUE(trace.Peers().begin()->second.pending);
}

TEST(LookupTrace, EmptyAndMultipartItemResponses)
{
	CLookupTrace trace;
	trace.Query({ 1, 2 }, {}, 100);
	trace.ItemReply({ 1, 2 }, 101);
	ASSERT_EQUALS(0u, trace.Peers().begin()->second.itemReplies);
	trace.Reply({ 1, 2 }, 125);
	ASSERT_EQUALS(uint64_t(25), trace.Peers().begin()->second.roundTrip);
	trace.ItemRequest({ 1, 2 }, 130);
	trace.ItemReply({ 1, 2 }, 140); // empty packet is still a reply
	trace.Result({ 1, 2 }, 150);
	trace.ItemReply({ 1, 2 }, 151); // result packets may be split
	ASSERT_EQUALS(1u, trace.Peers().begin()->second.itemRequests);
	ASSERT_EQUALS(2u, trace.Peers().begin()->second.itemReplies);
	ASSERT_EQUALS(1u, trace.Peers().begin()->second.results);
}

TEST(LookupTrace, SustainedMixedTrafficStaysBounded)
{
	CLookupTrace trace;
	for (uint64_t tick = 1; tick <= 100000; ++tick) {
		const CLookupTrace::Address peer{ static_cast<uint32_t>(tick % 128 + 1), 4665 };
		trace.Query(peer, {}, tick);
		trace.Reply(peer, tick + 1);
		trace.ItemRequest(peer, tick + 2);
		trace.Result(peer, tick + 3);
		trace.ItemReply(peer, tick + 4);
		trace.Referral({ 0x01020304, 4665 }, peer, tick + 5, tick % 2 == 0);
	}
	ASSERT_EQUALS(size_t(128), trace.Peers().size());
	ASSERT_EQUALS(size_t(256), trace.Events().size());
	ASSERT_EQUALS(99744u, trace.Omitted());
	ASSERT_EQUALS(0u, trace.OmittedPeers());
	ASSERT_EQUALS(99744u, trace.EvictedReferrals());
	ASSERT_EQUALS(size_t(0), trace.Overdue(200000, 3000));
	ASSERT_EQUALS(uint64_t(1), trace.Peers().begin()->second.roundTrip);
	ASSERT_EQUALS(100005ull, static_cast<unsigned long long>(trace.Events().back().tick));
}
