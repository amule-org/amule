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

// AllSearch (issue #1542) adds a fourth search type that simultaneously queries eD2k
// (local + global) and Kad. Its enum value, its EC protocol code, and its interaction
// with CSearchRequest's reuse logic are all pinned here, because:
//
//   - The numeric value is wire format (EC_TAG_SEARCH_LIFECYCLE_KIND) and on-disk format
//     (StoredSearches.met), so a silent renumber would corrupt both.
//   - The EC mapping in ExternalConn.cpp is a ternary that falls through to LocalSearch
//     for unknown codes; a missing AllSearch case would quietly start a local-only search
//     when a remote client asked for "all".
//   - CSearchRequest::CanReuse treats type as a distinguishing filter, so AllSearch must
//     not be reused as (or by) any of the other three types.

#include "SearchEd2kSlot.h"
#include "SearchStartRequests.h"
#include <muleunit/test.h>

#include "SearchList.h"
#include "SearchRequest.h"
#include "SearchTypeChoices.h"

#include <ECCodes.h>

using namespace muleunit;

DECLARE_SIMPLE(AllSearchType)

// The enum value is pinned to 5: it shares the numeric space with EC_SEARCH_TYPE
// (cast straight to uint8 for EC_TAG_SEARCH_LIFECYCLE_KIND) and with StoredSearches.met
// (written as a raw byte). Renumbering silently reinterprets both.
TEST(AllSearchType, EnumValueIsPinned)
{
	ASSERT_EQUALS(5, static_cast<int>(AllSearch));
}

// Every SearchType member must carry its EC_SEARCH_TYPE number. The static_asserts in
// ExternalConn.cpp check this at build time, but a test makes the contract visible and
// catches a drift introduced by a header that ExternalConn.cpp does not include.
TEST(AllSearchType, AlignsWithECSearchType)
{
	ASSERT_EQUALS(static_cast<int>(LocalSearch), static_cast<int>(EC_SEARCH_LOCAL));
	ASSERT_EQUALS(static_cast<int>(GlobalSearch), static_cast<int>(EC_SEARCH_GLOBAL));
	ASSERT_EQUALS(static_cast<int>(KadSearch), static_cast<int>(EC_SEARCH_KAD));
	ASSERT_EQUALS(static_cast<int>(BrowseSearch), static_cast<int>(EC_SEARCH_BROWSE));
	ASSERT_EQUALS(static_cast<int>(AllSearch), static_cast<int>(EC_SEARCH_ALL));
}

// The gap at 3 (EC_SEARCH_WEB) is deliberate and documented in SearchList.h. AllSearch
// must not fill it: doing so would make every "all" search report itself as a web search
// on the wire.
TEST(AllSearchType, DoesNotCollideWithWebSearchValue)
{
	ASSERT_TRUE(static_cast<int>(AllSearch) != 3);
	ASSERT_EQUALS(3, static_cast<int>(EC_SEARCH_WEB));
}

// CSearchRequest uses the type as a distinguishing filter: an AllSearch tab must not be
// reused for a Local/Global/Kad-only request, and vice versa. This is the gate that
// prevents the dialog from silently switching a user's "All" tab to a single-network one.
TEST(AllSearchType, SearchRequestDistinguishesAllFromOthers)
{
	CSearchList::CSearchParams params;
	params.searchString = "ubuntu";
	const CSearchRequest allRequest(AllSearch, params);

	ASSERT_TRUE(allRequest.CanReuse(CSearchRequest(AllSearch, params), 0));
	ASSERT_TRUE(!allRequest.CanReuse(CSearchRequest(LocalSearch, params), 0));
	ASSERT_TRUE(!allRequest.CanReuse(CSearchRequest(GlobalSearch, params), 0));
	ASSERT_TRUE(!allRequest.CanReuse(CSearchRequest(KadSearch, params), 0));
}

// Symmetric: a request for a single-network search must not grab an AllSearch tab.
TEST(AllSearchType, SingleNetworkRequestDoesNotReuseAllTab)
{
	CSearchList::CSearchParams params;
	params.searchString = "ubuntu";
	const CSearchRequest localRequest(LocalSearch, params);
	const CSearchRequest allRequest(AllSearch, params);

	ASSERT_TRUE(!localRequest.CanReuse(allRequest, 0));
	ASSERT_TRUE(!allRequest.CanReuse(localRequest, 0));
}

// FindReusableSearch must pick the AllSearch tab for an AllSearch request, not fall
// through to a same-query Local/Global/Kad tab that happens to be open.
TEST(AllSearchType, FindReusableSearchPicksCorrectType)
{
	CSearchList::CSearchParams params;
	params.searchString = "ubuntu";
	const CSearchRequest allRequest(AllSearch, params);
	const CSearchRequest globalRequest(GlobalSearch, params);

	const std::vector<CSearchReuseCandidate> pages{
		{ &globalRequest, 25 },
		{ &allRequest, 25 },
	};
	ASSERT_EQUALS(size_t(1), FindReusableSearch(pages, allRequest));
	ASSERT_EQUALS(size_t(0), FindReusableSearch(pages, globalRequest));
}

// AllSearch is a distinct type for reuse even before the first progress arrives
// (the pending-submission case).
TEST(AllSearchType, PendingAllSearchIsReusableBeforeProgress)
{
	CSearchList::CSearchParams params;
	params.searchString = "ubuntu";
	const CSearchRequest allRequest(AllSearch, params);
	std::vector<CSearchReuseCandidate> pages{ { &allRequest, std::nullopt } };
	ASSERT_EQUALS(size_t(0), FindReusableSearch(pages, allRequest));

	// A terminal status still blocks reuse, same as every other type.
	pages[0].progress = 0xffffu;
	ASSERT_EQUALS(pages.size(), FindReusableSearch(pages, allRequest));
	pages[0].progress = 0xfffeu;
	ASSERT_EQUALS(pages.size(), FindReusableSearch(pages, allRequest));
}

// Exercise the actual dropdown conversion before matching a submitted request:
// using the raw index 3 previously made the same All query start another tab.
TEST(AllSearchType, AllChoiceReusesSubmittedAllSearch)
{
	CSearchList::CSearchParams params;
	params.searchString = "ubuntu";
	const CSearchRequest submitted(AllSearch, params);
	const CSearchRequest selected(BuildSearchTypeChoices(true, true, true, AllSearch).types[3], params);
	const std::vector<CSearchReuseCandidate> pages{ { &submitted, 25 } };
	ASSERT_EQUALS(size_t(0), FindReusableSearch(pages, selected));
	ASSERT_EQUALS(static_cast<int>(EC_SEARCH_ALL),
		BuildSearchTypeChoices(true, true, true, AllSearch).types[3]);
}

TEST(AllSearchType, OldDaemonOmitsAllAndRejectsSavedAllSelection)
{
	for (long saved : { long(AllSearch), 3L }) {
		const auto choices = BuildSearchTypeChoices(true, true, false, saved);
		ASSERT_EQUALS(size_t(3), choices.types.size());
		ASSERT_EQUALS(int(LocalSearch), int(choices.types[0]));
		ASSERT_EQUALS(int(GlobalSearch), int(choices.types[1]));
		ASSERT_EQUALS(int(KadSearch), int(choices.types[2]));
		ASSERT_EQUALS(0, choices.selection);
	}
}

TEST(AllSearchType, CapableDaemonOffersAndRestoresAll)
{
	for (long saved : { long(AllSearch), 3L }) {
		const auto choices = BuildSearchTypeChoices(true, true, true, saved);
		ASSERT_EQUALS(size_t(4), choices.types.size());
		ASSERT_EQUALS(3, choices.selection);
		ASSERT_EQUALS(int(AllSearch), int(choices.types[choices.selection]));
		ASSERT_EQUALS(int(AllSearch), int(choices.types[choices.selection]));
	}
}

TEST(AllSearchType, AllRequiresBothNetworksEvenWithCapability)
{
	for (bool supported : { false, true }) {
		const auto ed2k = BuildSearchTypeChoices(true, false, supported, AllSearch);
		ASSERT_EQUALS(size_t(2), ed2k.types.size());
		ASSERT_EQUALS(int(LocalSearch), int(ed2k.types[0]));
		ASSERT_EQUALS(int(GlobalSearch), int(ed2k.types[1]));
		ASSERT_EQUALS(0, ed2k.selection);
		const auto kad = BuildSearchTypeChoices(false, true, supported, AllSearch);
		ASSERT_EQUALS(size_t(1), kad.types.size());
		ASSERT_EQUALS(int(KadSearch), int(kad.types[0]));
		ASSERT_EQUALS(0, kad.selection);
		const auto none = BuildSearchTypeChoices(false, false, supported, AllSearch);
		ASSERT_TRUE(none.types.empty());
		ASSERT_EQUALS(-1, none.selection);
	}
}

TEST(AllSearchType, CapabilityDoesNotDisturbOtherSavedModes)
{
	for (bool supported : { false, true }) {
		for (SearchType saved : { LocalSearch, GlobalSearch, KadSearch }) {
			const auto choices = BuildSearchTypeChoices(true, true, supported, saved);
			ASSERT_EQUALS(int(saved), int(choices.types[choices.selection]));
		}
		const auto unknown = BuildSearchTypeChoices(true, true, supported, 99);
		ASSERT_EQUALS(0, unknown.selection);
		const auto disabled = BuildSearchTypeChoices(false, true, supported, GlobalSearch);
		ASSERT_EQUALS(int(KadSearch), int(disabled.types[disabled.selection]));
	}
}

TEST(AllSearchType, AcceptedReplacementInvalidatesPreviousEd2kOwnerBeforePolling)
{
	CSearchEd2kSlot slot;
	ASSERT_EQUALS(uint32_t(0), slot.Accept(1));
	ASSERT_TRUE(slot.IsActive(1));
	ASSERT_EQUALS(uint32_t(1), slot.Accept(2));
	ASSERT_FALSE(slot.IsActive(1));
	ASSERT_TRUE(slot.IsActive(2));
	// An old tab's completion poll cannot clear the new owner.
	slot.Observe(1, false);
	ASSERT_TRUE(slot.IsActive(2));
}

TEST(AllSearchType, KadOnlyAllDoesNotOwnActiveEd2kSlot)
{
	CSearchEd2kSlot slot;
	slot.Accept(1);
	slot.Observe(1, false);
	ASSERT_FALSE(slot.IsActive(1));
	// Retain the last owner for request invalidation on accepted replacement.
	ASSERT_EQUALS(uint32_t(1), slot.Accept(2, false));
	ASSERT_FALSE(slot.IsActive(2));
}

TEST(AllSearchType, LegacyProgressReleasesFinishedEd2kSlot)
{
	CSearchEd2kSlot slot;
	slot.Accept(1);
	slot.ObserveLegacyProgress(1, 50);
	ASSERT_TRUE(slot.IsActive(1));
	slot.ObserveLegacyProgress(1, 0xffff);
	ASSERT_FALSE(slot.IsActive(1));
	// Kad replaces the legacy single bucket; its live progress cannot acquire eD2k.
	slot.Accept(2, false);
	slot.ObserveLegacyProgress(2, 50);
	ASSERT_FALSE(slot.IsActive(2));
}

TEST(AllSearchType, PendingCloseWaitsForRealSearchIdAndCloseAcknowledgment)
{
	CSearchStartRequests requests;
	requests.Begin(1, AllSearch);
	ASSERT_TRUE(requests.DeferStop(1, true));
	// A later Stop must not undo the user's Close intent.
	ASSERT_TRUE(requests.DeferStop(1, false));
	ASSERT_TRUE(requests.DiscoveryBlocked());
	const auto accepted = requests.Take(1);
	ASSERT_TRUE(accepted.has_value());
	ASSERT_EQUALS(int(AllSearch), int(accepted->kind));
	ASSERT_TRUE(accepted->stopRequested);
	ASSERT_TRUE(accepted->closeRequested);
	requests.BeginClose();
	ASSERT_TRUE(requests.DiscoveryBlocked());
	requests.FinishClose();
	ASSERT_FALSE(requests.DiscoveryBlocked());
}

TEST(AllSearchType, PendingStopPreservesTabAndFailedStartReleasesDiscovery)
{
	CSearchStartRequests requests;
	requests.Begin(1, KadSearch);
	ASSERT_TRUE(requests.DeferStop(1, false));
	const auto accepted = requests.Take(1);
	ASSERT_TRUE(accepted->stopRequested);
	ASSERT_FALSE(accepted->closeRequested);
	ASSERT_FALSE(requests.DeferStop(1, true));
	requests.Begin(2, AllSearch);
	requests.Take(2); // A rejected START has no daemon search to close.
	ASSERT_FALSE(requests.DiscoveryBlocked());
}

TEST(AllSearchType, LostRepliesCannotKeepDiscoveryBlockedAcrossReconnect)
{
	CSearchStartRequests requests;
	requests.Begin(1, AllSearch);
	requests.BeginClose();
	ASSERT_EQUALS(size_t(1), requests.PendingIds().size());
	requests.Reset();
	ASSERT_FALSE(requests.DiscoveryBlocked());
	ASSERT_TRUE(requests.PendingIds().empty());
	requests.FinishClose(); // Multiple discarded FIFO callbacks are harmless.
	ASSERT_FALSE(requests.DiscoveryBlocked());
}

TEST(AllSearchType, OneCloseReplyCannotReleaseOtherPendingRequests)
{
	CSearchStartRequests requests;
	requests.Begin(1, AllSearch);
	requests.BeginClose();
	requests.BeginClose();
	requests.FinishClose();
	ASSERT_TRUE(requests.DiscoveryBlocked());
	requests.Take(1);
	ASSERT_TRUE(requests.DiscoveryBlocked());
	requests.FinishClose();
	ASSERT_FALSE(requests.DiscoveryBlocked());
}

TEST(AllSearchType, PendingBrowseCloseNeverReplacesEd2kSlot)
{
	CSearchStartRequests requests;
	requests.Begin(1, BrowseSearch);
	ASSERT_TRUE(requests.DeferStop(1, true));
	const auto accepted = requests.Take(1);
	ASSERT_TRUE(accepted->closeRequested);
	ASSERT_FALSE(accepted->ReplacesEd2kSlot());
	for (SearchType kind : { LocalSearch, GlobalSearch, AllSearch }) {
		requests.Begin(2, kind);
		ASSERT_TRUE(requests.Take(2)->ReplacesEd2kSlot());
	}
	requests.Begin(3, KadSearch);
	ASSERT_FALSE(requests.Take(3)->ReplacesEd2kSlot());
}
