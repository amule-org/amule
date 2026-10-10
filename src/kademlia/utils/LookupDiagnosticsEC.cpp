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

#include "LookupDiagnosticsEC.h"
#include "../../libs/ec/cpp/ECPacket.h"

void Kademlia::AddLookupDiagnosticsTags(CECPacket &packet,
	const std::vector<LookupSnapshot> &active,
	const std::deque<LookupSnapshot> &recent,
	uint64_t now)
{
	auto addLookup = [&](const LookupSnapshot &snapshot, bool isActive) {
		CECEmptyTag lookup(EC_TAG_KAD_LOOKUP);
		lookup.AddTag(CECTag(EC_TAG_KAD_LOOKUP_TARGET, snapshot.target));
		lookup.AddTag(CECTag(EC_TAG_KAD_LOOKUP_TYPE, snapshot.type));
		lookup.AddTag(CECTag(EC_TAG_KAD_LOOKUP_KEYWORD, snapshot.keyword));
		lookup.AddTag(CECTag(EC_TAG_KAD_LOOKUP_ACTIVE, uint8_t(isActive)));
		lookup.AddTag(CECTag(EC_TAG_KAD_LOOKUP_STARTED, snapshot.started));
		lookup.AddTag(CECTag(EC_TAG_KAD_LOOKUP_OVERDUE,
			uint32_t(snapshot.trace.Overdue(isActive ? now : snapshot.finished, 3000))));
		lookup.AddTag(CECTag(EC_TAG_KAD_LOOKUP_OMITTED_PEERS, snapshot.trace.OmittedPeers()));
		lookup.AddTag(CECTag(EC_TAG_KAD_LOOKUP_EVICTED_REFERRALS, snapshot.trace.EvictedReferrals()));
		for (const auto &item : snapshot.trace.Peers()) {
			CECEmptyTag peer(EC_TAG_KAD_LOOKUP_PEER);
			peer.AddTag(CECTag(EC_TAG_KAD_LOOKUP_PEER_IP, item.first.first));
			peer.AddTag(CECTag(EC_TAG_KAD_LOOKUP_PEER_PORT, item.first.second));
			peer.AddTag(CECTag(
				EC_TAG_KAD_LOOKUP_PEER_ID, item.second.id.size(), item.second.id.data()));
			peer.AddTag(CECTag(EC_TAG_KAD_LOOKUP_PEER_DISTANCE,
				item.second.distance.size(),
				item.second.distance.data()));
			peer.AddTag(CECTag(EC_TAG_KAD_LOOKUP_PEER_VERSION, item.second.kadVersion));
			peer.AddTag(CECTag(EC_TAG_KAD_LOOKUP_PEER_REQUESTS, item.second.requests));
			peer.AddTag(CECTag(EC_TAG_KAD_LOOKUP_PEER_REPLIES, item.second.replies));
			peer.AddTag(CECTag(EC_TAG_KAD_LOOKUP_PEER_RTT, item.second.roundTrip));
			peer.AddTag(CECTag(EC_TAG_KAD_LOOKUP_PEER_ITEM_REQUESTS, item.second.itemRequests));
			peer.AddTag(CECTag(EC_TAG_KAD_LOOKUP_PEER_ITEM_REPLIES, item.second.itemReplies));
			peer.AddTag(CECTag(EC_TAG_KAD_LOOKUP_PEER_RESULTS, item.second.results));
			peer.AddTag(CECTag(EC_TAG_KAD_LOOKUP_PEER_PENDING, uint8_t(item.second.pending)));
			lookup.AddTag(peer);
		}
		for (const auto &event : snapshot.trace.Events()) {
			CECEmptyTag referral(EC_TAG_KAD_LOOKUP_REFERRAL);
			referral.AddTag(CECTag(EC_TAG_KAD_LOOKUP_REFERRAL_SOURCE_IP, event.source.first));
			referral.AddTag(CECTag(EC_TAG_KAD_LOOKUP_REFERRAL_SOURCE_PORT, event.source.second));
			referral.AddTag(CECTag(EC_TAG_KAD_LOOKUP_REFERRAL_PEER_IP, event.peer.first));
			referral.AddTag(CECTag(EC_TAG_KAD_LOOKUP_REFERRAL_PEER_PORT, event.peer.second));
			referral.AddTag(CECTag(EC_TAG_KAD_LOOKUP_REFERRAL_ELAPSED,
				event.tick >= snapshot.started ? event.tick - snapshot.started : 0));
			referral.AddTag(CECTag(EC_TAG_KAD_LOOKUP_REFERRAL_DISTANCE,
				event.distance.size(),
				event.distance.data()));
			referral.AddTag(CECTag(EC_TAG_KAD_LOOKUP_REFERRAL_CLOSER, uint8_t(event.closer)));
			lookup.AddTag(referral);
		}
		packet.AddTag(lookup);
	};
	for (const auto &snapshot : active) {
		addLookup(snapshot, true);
	}
	for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
		addLookup(*it, false);
	}
}
