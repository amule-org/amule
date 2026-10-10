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
#include <kademlia/utils/LookupDiagnostics.h>
using namespace muleunit;
using namespace Kademlia;
DECLARE_SIMPLE(LookupDiagnostics)
TEST(LookupDiagnostics, ExplainsRoutingAndItemStages)
{
	LookupSnapshot lookup{ 3, "fixture", "example", 100, 300, {} };
	lookup.trace.Query({ 0x01020304, 4665 }, {}, 100);
	lookup.trace.Reply({ 0x01020304, 4665 }, 125);
	lookup.trace.ItemRequest({ 0x01020304, 4665 }, 130);
	lookup.trace.ItemReply({ 0x01020304, 4665 }, 140);
	lookup.trace.Referral({ 0x02030405, 4665 }, { 0x01020304, 4665 }, 150, true);
	auto text = FormatLookupDiagnostics({ lookup }, {}, 4000);
	ASSERT_TRUE(text.Contains("1 routing requests, 1 replies, last RTT 25 ms"));
	ASSERT_TRUE(text.Contains("1 item requests, 1 result packets, 0 result records"));
	ASSERT_TRUE(text.Contains("1.2.3.4:4665"));
	ASSERT_TRUE(text.Contains("Keyword: example"));
	ASSERT_TRUE(text.Contains("+50 ms"));
	ASSERT_TRUE(text.Contains("closer to target"));
}
TEST(LookupDiagnostics, UncappedUnicodeOutput)
{
	LookupSnapshot lookup{ 3, wxString(200, wxUniChar(0x1f600)), "", 100, 300, {} };
	for (uint32_t i = 1; i <= 128; ++i) {
		lookup.trace.Query({ i, 1 }, {}, 100);
	}
	for (int i = 0; i < 256; ++i) {
		lookup.trace.Referral({ 2, 1 }, { 1, 1 }, 150, true);
	}
	std::vector<LookupSnapshot> active(16, lookup);
	std::deque<LookupSnapshot> recent(16, lookup);
	const auto text = FormatLookupDiagnostics(active, recent, 4000);
	ASSERT_FALSE(text.Contains("Diagnostic output truncated"));
	ASSERT_TRUE(text.utf8_str().length() > 65535);
}
TEST(LookupDiagnostics, EmptyHistory)
{
	ASSERT_TRUE(FormatLookupDiagnostics({}, {}, 0).Contains("No Kad lookup history available"));
}
