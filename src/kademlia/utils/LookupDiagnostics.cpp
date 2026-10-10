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
#include "../../NetworkFunctions.h"
#include <common/Format.h>
#include <wx/intl.h>
using namespace Kademlia;
namespace
{
wxString HexID(const CLookupTrace::ID &id)
{
	wxString text;
	for (auto byte : id) {
		text += wxString::Format("%02x", byte);
	}
	return text;
}

wxString SearchType(uint32_t type)
{
	switch (type) {
	case CSearch::NODE:
		return _("Node");
	case CSearch::NODECOMPLETE:
		return _("Complete node");
	case CSearch::FILE:
		return _("File");
	case CSearch::KEYWORD:
		return _("Keyword");
	case CSearch::NOTES:
		return _("Notes");
	case CSearch::STOREFILE:
		return _("Publish file");
	case CSearch::STOREKEYWORD:
		return _("Publish keyword");
	case CSearch::STORENOTES:
		return _("Publish notes");
	case CSearch::FINDBUDDY:
		return _("Find buddy");
	case CSearch::FINDSOURCE:
		return _("Find source");
	case CSearch::NODESPECIAL:
		return _("Special node");
	case CSearch::NODEFWCHECKUDP:
		return _("UDP firewall check");
	default:
		return CFormat(_("type %u")) % type;
	}
}

wxString DescribeLookup(const LookupSnapshot &snapshot, bool active, uint64_t now)
{
	const auto &trace = snapshot.trace;
	wxString text = CFormat(_("Lookup %s (%s, %s): %u peers, %u overdue routing requests, "
				  "%u omitted peers, %u evicted referrals")) %
			snapshot.target % SearchType(snapshot.type) %
			(active ? _("active lookup") : _("finished lookup")) %
			static_cast<unsigned>(trace.Peers().size()) %
			static_cast<unsigned>(trace.Overdue(now, 3000)) % trace.OmittedPeers() %
			trace.EvictedReferrals();
	text += "\n";
	if (!snapshot.keyword.empty()) {
		text += "  ";
		text += CFormat(_("Keyword: %s")) % snapshot.keyword;
		text += "\n";
	}
	for (const auto &item : trace.Peers()) {
		const auto &row = item.second;
		text += "  ";
		text += CFormat(_("%s [%s], distance %s, Kad version %u (%s): %u routing requests, "
				  "%u replies, last RTT %u ms, %u item requests, %u result packets, "
				  "%u result records")) %
			KadIPPortToString(item.first.first, item.first.second) % HexID(row.id) %
			HexID(row.distance) % static_cast<unsigned>(row.kadVersion) %
			(row.pending ? _("awaiting reply") : _("reply received")) % row.requests %
			row.replies % static_cast<unsigned>(row.roundTrip) % row.itemRequests %
			row.itemReplies % row.results;
		text += "\n";
	}
	for (const auto &event : trace.Events()) {
		if (event.kind == Kademlia::CLookupTrace::Kind::Referral) {
			text += "  ";
			text += CFormat(_("+%u ms: %s referred %s (distance %s, %s)")) %
				static_cast<unsigned>(
					event.tick >= snapshot.started ? event.tick - snapshot.started : 0) %
				KadIPPortToString(event.source.first, event.source.second) %
				KadIPPortToString(event.peer.first, event.peer.second) %
				HexID(event.distance) %
				(event.closer ? _("closer to target") : _("no closer to target"));
			text += "\n";
		}
	}
	return text + "\n";
}
} // namespace
wxString Kademlia::FormatLookupDiagnostics(
	const std::vector<LookupSnapshot> &active, const std::deque<LookupSnapshot> &recent, uint64_t now)
{
	wxString text = _("Overdue means a routing request unanswered for at least 3 seconds; late replies "
			  "may still arrive. Result records are received records, not unique files. Only "
			  "bounded recent history is retained.");
	text += "\n\n";
	for (const auto &snapshot : active) {
		text += DescribeLookup(snapshot, true, now);
	}
	for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
		text += DescribeLookup(*it, false, it->finished);
	}
	if (active.empty() && recent.empty()) {
		text += _("No Kad lookup history available.");
	}
	return text;
}
