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

wxString SearchType(LookupType type)
{
	switch (type) {
	case LookupType::Node:
		return _("Node");
	case LookupType::CompleteNode:
		return _("Complete node");
	case LookupType::File:
		return _("File");
	case LookupType::Keyword:
		return _("Keyword");
	case LookupType::Notes:
		return _("Notes");
	case LookupType::PublishFile:
		return _("Publish file");
	case LookupType::PublishKeyword:
		return _("Publish keyword");
	case LookupType::PublishNotes:
		return _("Publish notes");
	case LookupType::FindBuddy:
		return _("Find buddy");
	case LookupType::FindSource:
		return _("Find source");
	case LookupType::SpecialNode:
		return _("Special node");
	case LookupType::UDPFirewallCheck:
		return _("UDP firewall check");
	case LookupType::PublishPointer:
		return _("Publish buddy pointer");
	case LookupType::FindPointer:
		return _("Find buddy pointer");
	default:
		return CFormat(_("type %u")) % static_cast<uint32_t>(type);
	}
}

wxString Address(CLookupTrace::Address address)
{
	const auto ip = address.first;
	return CFormat("%u.%u.%u.%u:%u") % uint8_t(ip >> 24) % uint8_t(ip >> 16) % uint8_t(ip >> 8) %
	       uint8_t(ip) % address.second;
}

wxString DescribeLookup(const LookupReport &snapshot)
{
	wxString text = CFormat(_("Lookup %s (%s, %s): %u peers, %u overdue routing requests, "
				  "%u omitted peers, %u evicted referrals")) %
			snapshot.target % SearchType(snapshot.type) %
			(snapshot.active ? _("active lookup") : _("finished lookup")) %
			static_cast<unsigned>(snapshot.peers.size()) % snapshot.overdue %
			snapshot.omittedPeers % snapshot.evictedReferrals;
	text += "\n";
	if (!snapshot.keyword.empty()) {
		text += "  ";
		text += CFormat(_("Keyword: %s")) % snapshot.keyword;
		text += "\n";
	}
	for (const auto &row : snapshot.peers) {
		text += "  ";
		text += CFormat(_("%s [%s], distance %s, Kad version %u (%s): %u routing requests, "
				  "%u replies, last RTT %u ms, %u item requests, %u result packets, "
				  "%u result records")) %
			Address(row.address) % HexID(row.id) % HexID(row.distance) %
			static_cast<unsigned>(row.kadVersion) %
			(row.pending ? _("awaiting reply") : _("reply received")) % row.requests %
			row.replies % static_cast<unsigned>(row.roundTrip) % row.itemRequests %
			row.itemReplies % row.results;
		text += "\n";
	}
	for (const auto &event : snapshot.referrals) {
		text += "  ";
		text += CFormat(_("+%u ms: %s referred %s (distance %s, %s)")) % event.elapsed %
			Address(event.source) % Address(event.peer) % HexID(event.distance) %
			(event.closer ? _("closer to target") : _("no closer to target"));
		text += "\n";
	}
	return text + "\n";
}
} // namespace
wxString Kademlia::FormatLookupDiagnostics(const LookupReports &reports)
{
	wxString text = _("Overdue means a routing request unanswered for at least 3 seconds; late replies "
			  "may still arrive. Result records are received records, not unique files. Only "
			  "bounded recent history is retained.");
	text += "\n\n";
	for (const auto &snapshot : reports) {
		text += DescribeLookup(snapshot);
	}
	if (reports.empty()) {
		text += _("No Kad lookup history available.");
	}
	return text;
}
