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
#include "SearchList.h"
#include "SearchRequest.h"
#include "SearchStartBookkeeping.h"

using namespace muleunit;
DECLARE_SIMPLE(SearchRequest)

TEST(SearchRequest, FailedStartKeepsPreviousSearchBookkeeping)
{
	SearchType type = GlobalSearch;
	time_t start = 1234;
	{
		CSearchStartBookkeeping pending(type, start, KadSearch, false);
		// A late startup failure exits without committing the proposed state.
	}
	ASSERT_EQUALS(GlobalSearch, type);
	ASSERT_EQUALS(time_t(1234), start);

	CSearchStartBookkeeping successful(type, start, KadSearch, false);
	successful.Commit(5678);
	ASSERT_EQUALS(KadSearch, type);
	ASSERT_EQUALS(time_t(5678), start);

	type = GlobalSearch;
	CSearchStartBookkeeping parallelKad(type, start, KadSearch, true);
	parallelKad.Commit(9012);
	ASSERT_EQUALS(GlobalSearch, type);
	ASSERT_EQUALS(time_t(5678), start);
}

TEST(SearchRequest, OnlyRunningRequestsAreReused)
{
	CSearchList::CSearchParams params;
	params.searchString = "ubuntu";
	const CSearchRequest request(GlobalSearch, params);
	ASSERT_TRUE(request.CanReuse(request, 0));
	ASSERT_TRUE(request.CanReuse(request, 50));
	ASSERT_TRUE(request.CanReuse(request, 100));
	ASSERT_TRUE(!request.CanReuse(request, 0xffff));
	ASSERT_TRUE(!request.CanReuse(request, 0xfffe));
	// Progress and reuse share the protocol's two terminal sentinels.
	ASSERT_TRUE(request.CanReuse(request, 101));
}

TEST(SearchRequest, EverySubmittedFilterDistinguishesRequests)
{
	CSearchList::CSearchParams params;
	params.searchString = "ubuntu";
	params.extension = "iso";
	params.typeText = "Pro";
	params.minSize = UINT64_C(1) << 33;
	params.maxSize = UINT64_C(1) << 34;
	params.availability = 3;
	const CSearchRequest request(GlobalSearch, params);
	ASSERT_TRUE(request.CanReuse(CSearchRequest(GlobalSearch, params), 25));
	ASSERT_TRUE(!request.CanReuse(CSearchRequest(LocalSearch, params), 25));
	ASSERT_TRUE(!request.CanReuse(CSearchRequest(KadSearch, params), 25));
	{
		auto different = params;
		different.searchString = "debian";
		ASSERT_TRUE(!request.CanReuse(CSearchRequest(GlobalSearch, different), 25));
	}
	{
		auto different = params;
		different.extension = "zip";
		ASSERT_TRUE(!request.CanReuse(CSearchRequest(GlobalSearch, different), 25));
	}
	{
		auto different = params;
		different.typeText = "Audio";
		ASSERT_TRUE(!request.CanReuse(CSearchRequest(GlobalSearch, different), 25));
	}
	{
		auto different = params;
		different.minSize = params.minSize + 1;
		ASSERT_TRUE(!request.CanReuse(CSearchRequest(GlobalSearch, different), 25));
	}
	{
		auto different = params;
		different.maxSize = params.maxSize + 1;
		ASSERT_TRUE(!request.CanReuse(CSearchRequest(GlobalSearch, different), 25));
	}
	{
		auto different = params;
		different.availability = 4;
		ASSERT_TRUE(!request.CanReuse(CSearchRequest(GlobalSearch, different), 25));
	}
}

TEST(SearchRequest, RequestOwnsItsValuesAndPreservesQuerySyntax)
{
	CSearchList::CSearchParams params;
	params.searchString = "ubuntu OR debian";
	const CSearchRequest submitted(KadSearch, params);
	params.searchString = "ubuntu or debian";
	ASSERT_TRUE(!submitted.CanReuse(CSearchRequest(KadSearch, params), 0));
	params.searchString = "ubuntu OR debian";
	ASSERT_TRUE(submitted.CanReuse(CSearchRequest(KadSearch, params), 0));
	// The Kad keyword is derived by the core after submission, not another filter.
	params.strKeyword = "ubuntu";
	ASSERT_TRUE(submitted.CanReuse(CSearchRequest(KadSearch, params), 0));
}

TEST(SearchRequest, FindsMatchingPageAmongFinishedAndForeignTabs)
{
	CSearchList::CSearchParams params;
	params.searchString = "ubuntu";
	const CSearchRequest request(GlobalSearch, params);
	const CSearchRequest other(KadSearch, params);
	const std::vector<CSearchReuseCandidate> pages{
		{ nullptr, 10 }, // Externally discovered search: no submitted filters.
		{ &request, 0xffff },
		{ &other, 25 },
		{ &request, 45 },
		{ &request, 60 }
	};
	ASSERT_EQUALS(size_t(3), FindReusableSearch(pages, request));
	ASSERT_EQUALS(size_t(2), FindReusableSearch(pages, other));
	ASSERT_EQUALS(size_t(0), FindReusableSearch({}, request));
}

TEST(SearchRequest, PendingSubmissionIsReusableBeforeFirstProgress)
{
	CSearchList::CSearchParams params;
	params.searchString = "ubuntu";
	for (const auto type : { LocalSearch, GlobalSearch, KadSearch }) {
		const CSearchRequest request(type, params);
		std::vector<CSearchReuseCandidate> pages{ { nullptr,
								  std::nullopt }, // Restored or browse page.
			{ &request, std::nullopt } };
		ASSERT_EQUALS(size_t(1), FindReusableSearch(pages, request));
		// Rekeying the page does not change its owned request; progress can follow later.
		pages[1].progress = 0;
		ASSERT_EQUALS(size_t(1), FindReusableSearch(pages, request));
		pages[1].progress = 100;
		ASSERT_EQUALS(size_t(1), FindReusableSearch(pages, request));
		for (const uint32_t terminal : { 0xffffu, 0xfffeu }) {
			pages[1].progress = terminal;
			ASSERT_EQUALS(pages.size(), FindReusableSearch(pages, request));
		}
	}
}

TEST(SearchRequest, InvalidatedRequestCannotBeRevivedByDelayedProgress)
{
	CSearchList::CSearchParams params;
	params.searchString = "ubuntu";
	const CSearchRequest request(GlobalSearch, params);
	std::vector<CSearchReuseCandidate> pages{ { &request, 35 } };
	ASSERT_EQUALS(size_t(0), FindReusableSearch(pages, request));
	// Explicit or implicit stop clears the request before the next daemon poll.
	pages[0].request = nullptr;
	pages[0].progress = 40;
	ASSERT_EQUALS(pages.size(), FindReusableSearch(pages, request));
	// A daemon restart also drops progress; it must not turn a restored tab into pending work.
	pages[0].progress.reset();
	ASSERT_EQUALS(pages.size(), FindReusableSearch(pages, request));
	// Rejection and close remove the candidate entirely.
	pages.clear();
	ASSERT_EQUALS(pages.size(), FindReusableSearch(pages, request));
}

TEST(SearchRequest, PendingRequestStillRequiresEveryFilterToMatch)
{
	CSearchList::CSearchParams params;
	params.searchString = "ubuntu";
	params.extension = "iso";
	const CSearchRequest submitted(GlobalSearch, params);
	const std::vector<CSearchReuseCandidate> pages{ { &submitted, std::nullopt } };
	params.extension = "zip";
	ASSERT_EQUALS(pages.size(), FindReusableSearch(pages, CSearchRequest(GlobalSearch, params)));
	params.extension = "iso";
	ASSERT_EQUALS(pages.size(), FindReusableSearch(pages, CSearchRequest(KadSearch, params)));
}
