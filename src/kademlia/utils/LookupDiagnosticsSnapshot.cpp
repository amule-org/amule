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

#include "LookupDiagnostics.h"
#include "../kademlia/Search.h"

Kademlia::LookupType Kademlia::LookupTypeFromSearch(uint32_t type)
{
	switch (type) {
	case CSearch::NODE:
		return LookupType::Node;
	case CSearch::NODECOMPLETE:
		return LookupType::CompleteNode;
	case CSearch::FILE:
		return LookupType::File;
	case CSearch::KEYWORD:
		return LookupType::Keyword;
	case CSearch::NOTES:
		return LookupType::Notes;
	case CSearch::STOREFILE:
		return LookupType::PublishFile;
	case CSearch::STOREKEYWORD:
		return LookupType::PublishKeyword;
	case CSearch::STORENOTES:
		return LookupType::PublishNotes;
	case CSearch::FINDBUDDY:
		return LookupType::FindBuddy;
	case CSearch::FINDSOURCE:
		return LookupType::FindSource;
	case CSearch::NODESPECIAL:
		return LookupType::SpecialNode;
	case CSearch::NODEFWCHECKUDP:
		return LookupType::UDPFirewallCheck;
	case CSearch::STOREPOINTER:
		return LookupType::PublishPointer;
	case CSearch::FINDPOINTER:
		return LookupType::FindPointer;
	default:
		return LookupType::Unknown;
	}
}

Kademlia::LookupReports Kademlia::MakeLookupReports(
	const std::vector<LookupSnapshot> &active, const std::deque<LookupSnapshot> &recent, uint64_t now)
{
	LookupReports reports;
	reports.reserve(active.size() + recent.size());
	auto add = [&](const LookupSnapshot &snapshot, bool isActive) {
		LookupReport report;
		report.type = LookupTypeFromSearch(snapshot.type);
		report.target = snapshot.target;
		report.keyword = snapshot.keyword;
		report.active = isActive;
		report.overdue = snapshot.trace.Overdue(isActive ? now : snapshot.finished, 3000);
		report.omittedPeers = snapshot.trace.OmittedPeers();
		report.evictedReferrals = snapshot.trace.EvictedReferrals();
		report.peers.reserve(snapshot.trace.Peers().size());
		report.referrals.reserve(snapshot.trace.Events().size());
		for (const auto &item : snapshot.trace.Peers()) {
			const auto &row = item.second;
			report.peers.push_back({ item.first,
				row.id,
				row.distance,
				row.kadVersion,
				row.requests,
				row.replies,
				row.roundTrip,
				row.itemRequests,
				row.itemReplies,
				row.results,
				row.pending });
		}
		for (const auto &event : snapshot.trace.Events()) {
			report.referrals.push_back({ event.source,
				event.peer,
				event.tick >= snapshot.started ? event.tick - snapshot.started : 0,
				event.distance,
				event.closer });
		}
		reports.push_back(std::move(report));
	};
	for (const auto &snapshot : active) {
		add(snapshot, true);
	}
	for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
		add(*it, false);
	}
	return reports;
}
