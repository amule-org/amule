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
#include "ServerCallbackRequests.h"

#include <vector>

using namespace muleunit;

DECLARE_SIMPLE(ServerCallbackRequests)

namespace
{
constexpr std::uint64_t kWindowMs = 1000;
}

TEST(ServerCallbackRequests, ALoneRequestTakesTheRefusal)
{
	CServerCallbackRequests<int> requests;
	requests.Add(1, 0, kWindowMs);
	const std::optional<int> refused = requests.TakeRefused(10);
	ASSERT_TRUE(refused.has_value());
	ASSERT_EQUALS(1, *refused);
	ASSERT_FALSE(requests.TakeRefused(20).has_value());
}

TEST(ServerCallbackRequests, TwoClientsLeaveTheRefusalUnmatched)
{
	CServerCallbackRequests<int> requests;
	requests.Add(1, 0, kWindowMs);
	requests.Add(2, 0, kWindowMs);
	ASSERT_FALSE(requests.TakeRefused(10).has_value());
	ASSERT_EQUALS(2U, requests.Count(10));
}

TEST(ServerCallbackRequests, TwoRequestsForOneClientLeaveTheRefusalUnmatched)
{
	// The upload and download queues ask for the same client; the server answers both.
	CServerCallbackRequests<int> requests;
	requests.Add(1, 0, kWindowMs);
	requests.Add(1, 1, kWindowMs);
	ASSERT_FALSE(requests.TakeRefused(10).has_value());
	// A request for another client before the second refusal: that refusal is not its own.
	requests.Add(2, 20, kWindowMs);
	ASSERT_FALSE(requests.TakeRefused(30).has_value());
}

TEST(ServerCallbackRequests, RemoveDropsEveryRequestOfTheClient)
{
	CServerCallbackRequests<int> requests;
	requests.Add(1, 0, kWindowMs);
	requests.Add(1, 1, kWindowMs);
	requests.Add(2, 2, kWindowMs);
	requests.Remove(1);
	const std::optional<int> refused = requests.TakeRefused(10);
	ASSERT_TRUE(refused.has_value());
	ASSERT_EQUALS(2, *refused);
}

TEST(ServerCallbackRequests, ARequestCountsUntilItsWindowEnds)
{
	CServerCallbackRequests<int> requests;
	requests.Add(1, 0, kWindowMs);
	requests.Add(2, 500, kWindowMs);
	ASSERT_EQUALS(2U, requests.Count(kWindowMs));
	// Past the first request's window, the refusal can only be the second's.
	const std::optional<int> refused = requests.TakeRefused(kWindowMs + 1);
	ASSERT_TRUE(refused.has_value());
	ASSERT_EQUALS(2, *refused);
	// The expired one is still handed over once.
	const std::vector<int> expired = requests.TakeExpired(kWindowMs + 1);
	ASSERT_EQUALS(1U, expired.size());
	ASSERT_EQUALS(1, expired.front());
	ASSERT_TRUE(requests.TakeExpired(kWindowMs + 2).empty());
}

TEST(ServerCallbackRequests, EachRequestHasItsOwnWindow)
{
	CServerCallbackRequests<int> requests;
	requests.Add(1, 0, 4 * kWindowMs);
	requests.Add(2, 0, kWindowMs);
	const std::vector<int> expired = requests.TakeExpired(kWindowMs + 1);
	ASSERT_EQUALS(1U, expired.size());
	ASSERT_EQUALS(2, expired.front());
	ASSERT_EQUALS(1U, requests.Count(kWindowMs + 1));
}

TEST(ServerCallbackRequests, ARefusalThatNamesItsTargetFindsItAmongMany)
{
	CServerCallbackRequests<int> requests;
	requests.Add(1, 0, kWindowMs);
	requests.Add(2, 0, kWindowMs);
	requests.Add(3, 0, kWindowMs);
	const auto isTwo = [](int client) { return client == 2; };
	const std::optional<int> refused = requests.TakeFirst(10, isTwo);
	ASSERT_TRUE(refused.has_value());
	ASSERT_EQUALS(2, *refused);
	ASSERT_EQUALS(2U, requests.Count(10));
	ASSERT_FALSE(requests.TakeFirst(10, isTwo).has_value());
}

TEST(ServerCallbackRequests, ARefusalThatNamesItsTargetSkipsAnEndedWindow)
{
	CServerCallbackRequests<int> requests;
	requests.Add(1, 0, kWindowMs);
	ASSERT_FALSE(requests.TakeFirst(kWindowMs + 1, [](int client) { return client == 1; }).has_value());
	// The ended request still goes out once as expired.
	ASSERT_EQUALS(1U, requests.TakeExpired(kWindowMs + 1).size());
}

TEST(ServerCallbackRequests, ARefusalNamesItsTargetAndReason)
{
	const std::vector<std::uint8_t> data{ 0x77, 0x77, 0x00, 0x00, ServerCallbackRefusal::TargetGone };
	const std::optional<SServerCallbackRefusal> refusal =
		ParseServerCallbackRefusal(data.data(), data.size());
	ASSERT_TRUE(refusal.has_value());
	// Little-endian, as in OP_LOWID_HOLEPUNCH_FAIL.
	ASSERT_EQUALS(0x7777U, refusal->targetId);
	ASSERT_EQUALS(ServerCallbackRefusal::TargetGone, refusal->reason);
	// A server other than ed2k-server and eNode-go refuses OP_CALLBACKREQUEST with an empty one.
	ASSERT_FALSE(ParseServerCallbackRefusal(data.data(), 0).has_value());
	ASSERT_FALSE(ParseServerCallbackRefusal(data.data(), 4).has_value());
	ASSERT_FALSE(ParseServerCallbackRefusal(nullptr, 5).has_value());
}
