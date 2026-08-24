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
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA
//

#include "SearchProgressReport.h"

#include <ec/cpp/ECPacket.h>
#include <ec/cpp/ECCodes.h>
#include <muleunit/test.h>

using namespace muleunit;

DECLARE_SIMPLE(SearchProgressReport)

namespace
{

// One union child: the entry's own value is the search id, its fields nested.
CECTag MakeEntry(uint32 id, uint8 state, uint8 percent, uint32 results, const wxString &name = wxEmptyString)
{
	CECTag entry(EC_TAG_SEARCH_ID, id);
	if (!name.IsEmpty()) {
		entry.AddTag(EC_TAG_SEARCH_NAME, name);
	}
	entry.AddTag(CECTag(EC_TAG_SEARCH_STATUS, static_cast<uint16>(percent)));
	entry.AddTag(CECTag(EC_TAG_SEARCH_ID, id));
	entry.AddTag(CECTag(EC_TAG_SEARCH_LIFECYCLE_STATE, state));
	entry.AddTag(CECTag(EC_TAG_SEARCH_RESULT_COUNT, results));
	entry.AddTag(CECTag(EC_TAG_SEARCH_LIFECYCLE_PERCENT, percent));
	return entry;
}

} // namespace

// The shape amulecmd actually receives, and the one it used to misread.
TEST(SearchProgressReport, UnionReportsEverySearchByItsOwnId)
{
	CECPacket p(EC_OP_SEARCH_PROGRESS);
	p.AddTag(MakeEntry(1, 2, 100, 44));
	p.AddTag(MakeEntry(3, 1, 84, 12));

	const wxString out = ecprogress::FormatSearchProgress(p);
	ASSERT_TRUE(out.Contains(wxT("Search 1")));
	ASSERT_TRUE(out.Contains(wxT("100 %")));
	ASSERT_TRUE(out.Contains(wxT("44 results")));
	ASSERT_TRUE(out.Contains(wxT("Search 3")));
	ASSERT_TRUE(out.Contains(wxT("84 %")));
	ASSERT_TRUE(out.Contains(wxT("12 results")));
	// One line per search, not one for the reply.
	ASSERT_EQUALS(2, static_cast<int>(out.Freq(wxT('\n'))));
}

// The regression itself: a running search must not read as 0 %.
TEST(SearchProgressReport, AUnionEntryMidSweepIsNotReportedAsZero)
{
	CECPacket p(EC_OP_SEARCH_PROGRESS);
	p.AddTag(MakeEntry(7, 1, 84, 3));
	ASSERT_TRUE(ecprogress::FormatSearchProgress(p).Contains(wxT("84 %")));
	ASSERT_FALSE(ecprogress::FormatSearchProgress(p).Contains(wxT("0 %")));
}

TEST(SearchProgressReport, LifecycleStatesAreNamed)
{
	CECPacket running(EC_OP_SEARCH_PROGRESS);
	running.AddTag(MakeEntry(1, 1, 50, 0));
	ASSERT_TRUE(ecprogress::FormatSearchProgress(running).Contains(wxT("running")));

	CECPacket done(EC_OP_SEARCH_PROGRESS);
	done.AddTag(MakeEntry(1, 2, 100, 9));
	ASSERT_TRUE(ecprogress::FormatSearchProgress(done).Contains(wxT("finished")));

	CECPacket idle(EC_OP_SEARCH_PROGRESS);
	idle.AddTag(MakeEntry(1, 0, 0, 0));
	ASSERT_TRUE(ecprogress::FormatSearchProgress(idle).Contains(wxT("idle")));
}

// A daemon that echoed multi-search but not the union answers about one
// search with the lifecycle tags at the top level.
TEST(SearchProgressReport, SingleSearchReplyUsesTheLifecycleTags)
{
	CECPacket p(EC_OP_SEARCH_PROGRESS);
	p.AddTag(CECTag(EC_TAG_SEARCH_STATUS, static_cast<uint16>(0xffff)));
	p.AddTag(CECTag(EC_TAG_SEARCH_ID, static_cast<uint32>(5)));
	p.AddTag(CECTag(EC_TAG_SEARCH_LIFECYCLE_STATE, static_cast<uint8>(2)));
	p.AddTag(CECTag(EC_TAG_SEARCH_RESULT_COUNT, static_cast<uint32>(17)));
	p.AddTag(CECTag(EC_TAG_SEARCH_LIFECYCLE_PERCENT, static_cast<uint8>(100)));

	const wxString out = ecprogress::FormatSearchProgress(p);
	// Read through the lifecycle tags, so a finished search says so rather
	// than rendering the 0xffff sentinel as "not available".
	ASSERT_TRUE(out.Contains(wxT("Search 5")));
	ASSERT_TRUE(out.Contains(wxT("finished")));
	ASSERT_TRUE(out.Contains(wxT("100 %")));
}

// Oldest daemons send only the overloaded sentinel.
TEST(SearchProgressReport, SentinelOnlyReplyKeepsTheLegacyLine)
{
	CECPacket p(EC_OP_SEARCH_PROGRESS);
	p.AddTag(CECTag(EC_TAG_SEARCH_STATUS, static_cast<uint16>(42)));
	ASSERT_TRUE(ecprogress::FormatSearchProgress(p).Contains(wxT("Search progress: 42 %")));
}

TEST(SearchProgressReport, SentinelAboveOneHundredIsNotAPercentage)
{
	CECPacket p(EC_OP_SEARCH_PROGRESS);
	p.AddTag(CECTag(EC_TAG_SEARCH_STATUS, static_cast<uint16>(0xffff)));
	ASSERT_TRUE(ecprogress::FormatSearchProgress(p).Contains(wxT("not available")));
}

TEST(SearchProgressReport, ExpiredIsReportedAsExpired)
{
	CECPacket p(EC_OP_SEARCH_PROGRESS);
	p.AddTag(CECEmptyTag(EC_TAG_SEARCH_EXPIRED));
	ASSERT_TRUE(ecprogress::FormatSearchProgress(p).Contains(wxT("expired")));
}

// The discriminator trap: an expired verdict echoes the id it is about, as a
// bare leaf. Testing for EC_TAG_SEARCH_ID alone would read that as a union.
TEST(SearchProgressReport, AnExpiredVerdictEchoingItsIdIsStillExpired)
{
	CECPacket p(EC_OP_SEARCH_PROGRESS);
	p.AddTag(CECEmptyTag(EC_TAG_SEARCH_EXPIRED));
	p.AddTag(CECTag(EC_TAG_SEARCH_ID, static_cast<uint32>(9)));
	const wxString out = ecprogress::FormatSearchProgress(p);
	ASSERT_TRUE(out.Contains(wxT("expired")));
	ASSERT_FALSE(out.Contains(wxT("Search 9")));
}

// Same trap on the single-search reply, which echoes a bare id leaf beside
// its top-level fields.
TEST(SearchProgressReport, ABareIdLeafIsNotAUnionEntry)
{
	CECPacket p(EC_OP_SEARCH_PROGRESS);
	p.AddTag(CECTag(EC_TAG_SEARCH_ID, static_cast<uint32>(4)));
	p.AddTag(CECTag(EC_TAG_SEARCH_STATUS, static_cast<uint16>(30)));
	// No nested percent anywhere, so this is the sentinel shape, not a union.
	ASSERT_TRUE(ecprogress::FormatSearchProgress(p).Contains(wxT("Search progress: 30 %")));
}

// An empty union means the daemon holds no searches, which is not 0 %.
TEST(SearchProgressReport, AnEmptyUnionIsNotZeroPercent)
{
	CECPacket p(EC_OP_SEARCH_PROGRESS);
	const wxString out = ecprogress::FormatSearchProgress(p);
	ASSERT_FALSE(out.Contains(wxT("0 %")));
	ASSERT_TRUE(out.Contains(wxT("No search")));
}

// A search that has genuinely made no progress still reports 0 %.
TEST(SearchProgressReport, AJustStartedSearchStillReportsZero)
{
	CECPacket p(EC_OP_SEARCH_PROGRESS);
	p.AddTag(MakeEntry(2, 1, 0, 0));
	ASSERT_TRUE(ecprogress::FormatSearchProgress(p).Contains(wxT("0 %")));
	ASSERT_TRUE(ecprogress::FormatSearchProgress(p).Contains(wxT("running")));
}

// The daemon now sends the query alongside the progress, so a poll is readable
// without a second request for the search list.
TEST(SearchProgressReport, AnEntryIsLabelledWithItsQuery)
{
	CECPacket p(EC_OP_SEARCH_PROGRESS);
	p.AddTag(MakeEntry(1, 1, 40, 5, wxT("freebsd")));
	p.AddTag(MakeEntry(2, 2, 100, 77, wxT("ubuntu")));

	const wxString out = ecprogress::FormatSearchProgress(p);
	ASSERT_TRUE(out.Contains(wxT("Search 1 \"freebsd\"")));
	ASSERT_TRUE(out.Contains(wxT("Search 2 \"ubuntu\"")));
}

// A daemon too old to send the name must not produce an empty pair of quotes.
TEST(SearchProgressReport, AnEntryWithoutAQueryKeepsThePlainLine)
{
	CECPacket p(EC_OP_SEARCH_PROGRESS);
	p.AddTag(MakeEntry(1, 1, 40, 5));
	const wxString out = ecprogress::FormatSearchProgress(p);
	ASSERT_TRUE(out.Contains(wxT("Search 1:")));
	ASSERT_FALSE(out.Contains(wxT("\"\"")));
}

// A browse is named by its peer, through the same tag.
TEST(SearchProgressReport, ABrowseIsLabelledByItsPeer)
{
	CECPacket p(EC_OP_SEARCH_PROGRESS);
	p.AddTag(MakeEntry(4, 1, 10, 0, wxT("some peer")));
	ASSERT_TRUE(ecprogress::FormatSearchProgress(p).Contains(wxT("Search 4 \"some peer\"")));
}
