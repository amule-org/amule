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
#include <kademlia/utils/LookupDiagnosticsEC.h>
#include <kademlia/kademlia/Search.h>
#include "libs/ec/cpp/ECPacket.h"
#include <limits>
using namespace muleunit;
using namespace Kademlia;
DECLARE_SIMPLE(LookupDiagnostics)
TEST(LookupDiagnostics, ExplainsRoutingAndItemStages)
{
	LookupSnapshot lookup{ 3, "fixture", "example", 100, 300, {} };
	lookup.trace.Query({ 0x01020304, 4665 }, {}, 100);
	lookup.trace.Reply({ 0x01020304, 4665 }, 125);
	lookup.trace.ItemRequest({ 0x01020304, 4665 });
	lookup.trace.ItemReply({ 0x01020304, 4665 });
	lookup.trace.Referral({ 0x02030405, 4665 }, { 0x01020304, 4665 }, 150, true);
	auto text = FormatLookupDiagnostics(MakeLookupReports({ lookup }, {}, 4000));
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
	const auto text = FormatLookupDiagnostics(MakeLookupReports(active, recent, 4000));
	ASSERT_FALSE(text.Contains("Diagnostic output truncated"));
	ASSERT_TRUE(text.utf8_str().length() > 65535);
}
TEST(LookupDiagnostics, EmptyHistory)
{
	ASSERT_TRUE(FormatLookupDiagnostics({}).Contains("No Kad lookup history available"));
}

TEST(LookupDiagnostics, PopulatedReportRoundTrip)
{
	LookupSnapshot lookup{
		CSearch::KEYWORD, "target", wxString::FromUTF8("query \xc3\xa9"), 100, 5000, {}
	};
	CLookupTrace::ID id{}, distance{};
	id[0] = 0xab;
	distance[15] = 7;
	const CLookupTrace::Address address{ 0x01020304, 65535 };
	lookup.trace.Query(address, id, 100, 8, distance);
	lookup.trace.Reply(address, 125);
	lookup.trace.ItemRequest(address);
	lookup.trace.ItemReply(address);
	lookup.trace.ItemReply(address);
	lookup.trace.Result(address);
	lookup.trace.Referral({ 0x02030405, 1234 }, address, 150, true, distance);
	lookup.trace.Referral({ 0x03040506, 2345 }, address, 50, false, id);
	lookup.trace.Query({ 0x04050607, 3456 }, distance, 200, 9, id);
	for (uint32_t i = 1; i <= 128; ++i) {
		lookup.trace.Query({ i, 1 }, {}, 100);
	}
	for (unsigned i = 0; i < CLookupTrace::MaxEvents; ++i) {
		lookup.trace.Referral({ i, 4665 }, address, 200 + i, i % 2 == 0, distance);
	}
	const auto original = MakeLookupReports({ lookup }, { lookup }, 4000);
	CECPacket packet(EC_OP_GET_KAD_LOOKUPS);
	AddLookupDiagnosticsTags(packet, original);
	LookupReports decoded;
	ASSERT_TRUE(DecodeLookupDiagnosticsTags(packet, decoded));
	ASSERT_EQUALS(original.size(), decoded.size());
	ASSERT_EQUALS(FormatLookupDiagnostics(original), FormatLookupDiagnostics(decoded));
	for (size_t i = 0; i < original.size(); ++i) {
		const auto &a = original[i];
		const auto &b = decoded[i];
		ASSERT_TRUE(a.type == b.type);
		ASSERT_EQUALS(a.target, b.target);
		ASSERT_EQUALS(a.keyword, b.keyword);
		ASSERT_EQUALS(a.active, b.active);
		ASSERT_EQUALS(a.overdue, b.overdue);
		ASSERT_EQUALS(a.omittedPeers, b.omittedPeers);
		ASSERT_EQUALS(a.evictedReferrals, b.evictedReferrals);
		ASSERT_EQUALS(a.peers.size(), b.peers.size());
		ASSERT_EQUALS(a.referrals.size(), b.referrals.size());
		for (size_t j = 0; j < a.peers.size(); ++j) {
			const auto &x = a.peers[j];
			const auto &y = b.peers[j];
			ASSERT_TRUE(x.address == y.address && x.id == y.id && x.distance == y.distance);
			ASSERT_EQUALS(x.kadVersion, y.kadVersion);
			ASSERT_EQUALS(x.requests, y.requests);
			ASSERT_EQUALS(x.replies, y.replies);
			ASSERT_EQUALS(x.roundTrip, y.roundTrip);
			ASSERT_EQUALS(x.itemRequests, y.itemRequests);
			ASSERT_EQUALS(x.itemReplies, y.itemReplies);
			ASSERT_EQUALS(x.results, y.results);
			ASSERT_EQUALS(x.pending, y.pending);
		}
		for (size_t j = 0; j < a.referrals.size(); ++j) {
			const auto &x = a.referrals[j];
			const auto &y = b.referrals[j];
			ASSERT_TRUE(x.source == y.source && x.peer == y.peer && x.distance == y.distance);
			ASSERT_EQUALS(x.elapsed, y.elapsed);
			ASSERT_EQUALS(x.closer, y.closer);
		}
	}
	// A bad row after a valid one must not expose a partially decoded report.
	packet.AddTag(CECTag(EC_TAG_KAD_LOOKUP, uint32_t(42)));
	ASSERT_FALSE(DecodeLookupDiagnosticsTags(packet, decoded));
	ASSERT_TRUE(decoded.empty());
	CECPacket empty(EC_OP_GET_KAD_LOOKUPS);
	ASSERT_TRUE(DecodeLookupDiagnosticsTags(empty, decoded));
	ASSERT_TRUE(decoded.empty());
}

TEST(LookupDiagnostics, MapsEveryCoreSearchType)
{
	const std::pair<uint32_t, LookupType> types[] = { { CSearch::NODE, LookupType::Node },
		{ CSearch::NODECOMPLETE, LookupType::CompleteNode },
		{ CSearch::FILE, LookupType::File },
		{ CSearch::KEYWORD, LookupType::Keyword },
		{ CSearch::NOTES, LookupType::Notes },
		{ CSearch::STOREFILE, LookupType::PublishFile },
		{ CSearch::STOREKEYWORD, LookupType::PublishKeyword },
		{ CSearch::STORENOTES, LookupType::PublishNotes },
		{ CSearch::FINDBUDDY, LookupType::FindBuddy },
		{ CSearch::FINDSOURCE, LookupType::FindSource },
		{ CSearch::NODESPECIAL, LookupType::SpecialNode },
		{ CSearch::NODEFWCHECKUDP, LookupType::UDPFirewallCheck },
		{ CSearch::STOREPOINTER, LookupType::PublishPointer },
		{ CSearch::FINDPOINTER, LookupType::FindPointer } };
	for (const auto &type : types) {
		ASSERT_TRUE(LookupTypeFromSearch(type.first) == type.second);
	}
	ASSERT_TRUE(LookupTypeFromSearch(999) == LookupType::Unknown);
}

TEST(LookupDiagnostics, RejectsOutOfRangeWireIntegers)
{
	LookupReport report;
	report.target = "target";
	report.peers.push_back({ { 0x01020304, 4665 } });
	report.referrals.push_back({ { 0x01020304, 4665 }, { 0x02030405, 4665 } });
	struct Boundary
	{
		ec_tagname_t parent;
		ec_tagname_t field;
		uint64_t maximum;
	};
	const uint64_t max32 = std::numeric_limits<uint32_t>::max();
	const Boundary boundaries[] = { { EC_TAG_KAD_LOOKUP, EC_TAG_KAD_LOOKUP_TYPE, max32 },
		{ EC_TAG_KAD_LOOKUP, EC_TAG_KAD_LOOKUP_ACTIVE, 1 },
		{ EC_TAG_KAD_LOOKUP, EC_TAG_KAD_LOOKUP_OVERDUE, max32 },
		{ EC_TAG_KAD_LOOKUP, EC_TAG_KAD_LOOKUP_OMITTED_PEERS, max32 },
		{ EC_TAG_KAD_LOOKUP, EC_TAG_KAD_LOOKUP_EVICTED_REFERRALS, max32 },
		{ EC_TAG_KAD_LOOKUP_PEER, EC_TAG_KAD_LOOKUP_PEER_IP, max32 },
		{ EC_TAG_KAD_LOOKUP_PEER, EC_TAG_KAD_LOOKUP_PEER_PORT, 65535 },
		{ EC_TAG_KAD_LOOKUP_PEER, EC_TAG_KAD_LOOKUP_PEER_VERSION, 255 },
		{ EC_TAG_KAD_LOOKUP_PEER, EC_TAG_KAD_LOOKUP_PEER_REQUESTS, max32 },
		{ EC_TAG_KAD_LOOKUP_PEER, EC_TAG_KAD_LOOKUP_PEER_REPLIES, max32 },
		{ EC_TAG_KAD_LOOKUP_PEER, EC_TAG_KAD_LOOKUP_PEER_ITEM_REQUESTS, max32 },
		{ EC_TAG_KAD_LOOKUP_PEER, EC_TAG_KAD_LOOKUP_PEER_ITEM_REPLIES, max32 },
		{ EC_TAG_KAD_LOOKUP_PEER, EC_TAG_KAD_LOOKUP_PEER_RESULTS, max32 },
		{ EC_TAG_KAD_LOOKUP_PEER, EC_TAG_KAD_LOOKUP_PEER_PENDING, 1 },
		{ EC_TAG_KAD_LOOKUP_REFERRAL, EC_TAG_KAD_LOOKUP_REFERRAL_SOURCE_IP, max32 },
		{ EC_TAG_KAD_LOOKUP_REFERRAL, EC_TAG_KAD_LOOKUP_REFERRAL_SOURCE_PORT, 65535 },
		{ EC_TAG_KAD_LOOKUP_REFERRAL, EC_TAG_KAD_LOOKUP_REFERRAL_PEER_IP, max32 },
		{ EC_TAG_KAD_LOOKUP_REFERRAL, EC_TAG_KAD_LOOKUP_REFERRAL_PEER_PORT, 65535 },
		{ EC_TAG_KAD_LOOKUP_REFERRAL, EC_TAG_KAD_LOOKUP_REFERRAL_CLOSER, 1 } };
	for (const auto &boundary : boundaries) {
		CECPacket packet(EC_OP_GET_KAD_LOOKUPS);
		AddLookupDiagnosticsTags(packet, { report });
		auto *lookup = packet.GetTagByName(EC_TAG_KAD_LOOKUP);
		auto *parent =
			boundary.parent == EC_TAG_KAD_LOOKUP ? lookup : lookup->GetTagByName(boundary.parent);
		auto *field = parent->GetTagByName(boundary.field);
		*field = CECTag(boundary.field, boundary.maximum);
		LookupReports decoded;
		ASSERT_TRUE(DecodeLookupDiagnosticsTags(packet, decoded));
		*field = CECTag(boundary.field, boundary.maximum + 1);
		ASSERT_FALSE(DecodeLookupDiagnosticsTags(packet, decoded));
		ASSERT_TRUE(decoded.empty());
		*field = CECTag(boundary.field, wxString("wrong type"));
		ASSERT_FALSE(DecodeLookupDiagnosticsTags(packet, decoded));
		ASSERT_TRUE(decoded.empty());
	}
	// RTT and referral offsets remain 64-bit, rather than inheriting the counter bound.
	report.peers[0].roundTrip = std::numeric_limits<uint64_t>::max();
	report.referrals[0].elapsed = std::numeric_limits<uint64_t>::max();
	CECPacket packet(EC_OP_GET_KAD_LOOKUPS);
	AddLookupDiagnosticsTags(packet, { report });
	LookupReports decoded;
	ASSERT_TRUE(DecodeLookupDiagnosticsTags(packet, decoded));
	ASSERT_EQUALS(report.peers[0].roundTrip, decoded[0].peers[0].roundTrip);
	ASSERT_EQUALS(report.referrals[0].elapsed, decoded[0].referrals[0].elapsed);
}
