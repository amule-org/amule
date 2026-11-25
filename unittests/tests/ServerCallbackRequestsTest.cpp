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
	requests.Clear();
	ASSERT_EQUALS(0U, requests.Count(0));
}
