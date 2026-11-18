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

#ifndef AMULE_LOOKUPDIAGNOSTICS_H
#define AMULE_LOOKUPDIAGNOSTICS_H
#include "LookupTrace.h"
#include <wx/string.h>
#include <vector>
namespace Kademlia
{
struct LookupSnapshot
{
	uint32_t type;
	wxString target;
	wxString keyword;
	uint64_t started;
	uint64_t finished;
	CLookupTrace trace;
};
// Stable EC values; independent of CSearch's internal enum order.
enum class LookupType : uint32_t
{
	Node = 0,
	CompleteNode = 1,
	File = 2,
	Keyword = 3,
	Notes = 4,
	PublishFile = 5,
	PublishKeyword = 6,
	PublishNotes = 7,
	FindBuddy = 8,
	FindSource = 9,
	SpecialNode = 10,
	UDPFirewallCheck = 11,
	PublishPointer = 12,
	Unknown = 0xffffffff
};
struct LookupPeer
{
	CLookupTrace::Address address;
	CLookupTrace::ID id{};
	CLookupTrace::ID distance{};
	uint8_t kadVersion = 0;
	uint32_t requests = 0;
	uint32_t replies = 0;
	uint64_t roundTrip = 0;
	uint32_t itemRequests = 0;
	uint32_t itemReplies = 0;
	uint32_t results = 0;
	bool pending = false;
};
struct LookupReferral
{
	CLookupTrace::Address source;
	CLookupTrace::Address peer;
	uint64_t elapsed = 0;
	CLookupTrace::ID distance{};
	bool closer = false;
};
struct LookupReport
{
	LookupType type = LookupType::Unknown;
	wxString target;
	wxString keyword;
	bool active = false;
	uint32_t overdue = 0;
	uint32_t omittedPeers = 0;
	uint32_t evictedReferrals = 0;
	std::vector<LookupPeer> peers;
	std::vector<LookupReferral> referrals;
};
using LookupReports = std::vector<LookupReport>;
constexpr size_t MaxLookupHistory = 16;
LookupType LookupTypeFromSearch(uint32_t type);
LookupReports MakeLookupReports(
	const std::vector<LookupSnapshot> &active, const std::deque<LookupSnapshot> &recent, uint64_t now);
// Shared local/remote GUI formatter, translated in the caller's locale.
wxString FormatLookupDiagnostics(const LookupReports &reports);
} // namespace Kademlia
#endif
