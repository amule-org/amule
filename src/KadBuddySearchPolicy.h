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

#ifndef KAD_BUDDY_SEARCH_POLICY_H
#define KAD_BUDDY_SEARCH_POLICY_H

#include "kademlia/utils/UInt128.h"

#include <ctime>

namespace Kademlia
{

// The wait before a firewalled node searches for a buddy again. Stock eMule waits 20 minutes.
constexpr time_t BuddySearchRetrySeconds = 10 * 60;

// The BuddyID a firewalled node announces in KADEMLIA_FINDBUDDY_REQ and publishes in
// TAG_BUDDYHASH. A buddy offer must echo it, whatever target the search looked up.
inline CUInt128 OwnBuddyID(const CUInt128 &kadID)
{
	return CUInt128(true) ^ kadID;
}

// The first search looks up our BuddyID, as stock does. Later ones look up a random target: the
// nodes near our BuddyID answered the first search already, and the search for it may still run.
inline CUInt128 BuddySearchTarget(const CUInt128 &kadID, bool firstSearch, const CUInt128 &random)
{
	return firstSearch ? OwnBuddyID(kadID) : random;
}

} // namespace Kademlia

#endif // KAD_BUDDY_SEARCH_POLICY_H
