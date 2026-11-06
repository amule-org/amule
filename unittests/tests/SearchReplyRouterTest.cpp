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
#include "SearchReplyRouter.h"

using namespace muleunit;
DECLARE_SIMPLE(SearchReplyRouter)

TEST(SearchReplyRouter, SingleAskRoundTrips)
{
	CSearchReplyRouter router;
	router.Ask(0x0a000001, 11);
	ASSERT_EQUALS(uint32_t(11), router.ClaimReply(0x0a000001, 1000));
	// Nothing outstanding left: a duplicate answer owns no search.
	ASSERT_EQUALS(uint32_t(0), router.ClaimReply(0x0a000001, 1000));
	ASSERT_TRUE(router.Empty());
}

TEST(SearchReplyRouter, LateReplyAcrossTwoSearchesKeepsItsOwnSearch)
{
	CSearchReplyRouter router;
	// Search 1 asks server A, then search 2 replaces it and re-asks the same server.
	router.Ask(0x0a000001, 11);
	router.Ask(0x0a000001, 22);
	// The first answer belongs to search 1 even though search 2 is current: without
	// per-ask routing it would land in search 2's bucket.
	ASSERT_EQUALS(uint32_t(11), router.ClaimReply(0x0a000001, 1000));
	// The second answer belongs to search 2.
	ASSERT_EQUALS(uint32_t(22), router.ClaimReply(0x0a000001, 1000));
	ASSERT_EQUALS(uint32_t(0), router.ClaimReply(0x0a000001, 1000));
}

TEST(SearchReplyRouter, AnswerFromNeverAskedServerOwnsNothing)
{
	CSearchReplyRouter router;
	router.Ask(0x0a000001, 11);
	ASSERT_EQUALS(uint32_t(0), router.ClaimReply(0x0a000002, 1000));
}

TEST(SearchReplyRouter, ExpiredAskIsSkipped)
{
	CSearchReplyRouter router;
	// Search 1's ask expired unanswered (a silent server sends nothing); search 2 asked
	// the same server afterwards. The next answer cannot be search 1's.
	router.Ask(0x0a000001, 11, 100);
	router.Ask(0x0a000001, 22);
	ASSERT_EQUALS(uint32_t(22), router.ClaimReply(0x0a000001, 200));
	ASSERT_EQUALS(uint32_t(0), router.ClaimReply(0x0a000001, 200));
}

TEST(SearchReplyRouter, RemovedAskOwnsNothing)
{
	CSearchReplyRouter router;
	router.Ask(0x0a000001, 11);
	router.Remove(0x0a000001, 11);
	ASSERT_EQUALS(uint32_t(0), router.ClaimReply(0x0a000001, 1000));
	// Removing one search keeps the other's outstanding ask.
	router.Ask(0x0a000001, 11);
	router.Ask(0x0a000001, 22);
	router.Remove(0x0a000001, 11);
	ASSERT_EQUALS(uint32_t(22), router.ClaimReply(0x0a000001, 1000));
}

TEST(SearchReplyRouter, PurgeSearchDropsAllItsAsks)
{
	CSearchReplyRouter router;
	router.Ask(0x0a000001, 11);
	router.Ask(0x0a000002, 11);
	router.Ask(0x0a000001, 22);
	router.PurgeSearch(11);
	ASSERT_EQUALS(uint32_t(22), router.ClaimReply(0x0a000001, 1000));
	ASSERT_EQUALS(uint32_t(0), router.ClaimReply(0x0a000002, 1000));
	ASSERT_TRUE(router.Empty());
}
