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

#ifndef KAD_BUDDY_CALLBACK_H
#define KAD_BUDDY_CALLBACK_H

#include "MemFile.h"
#include "KadBuddyIdLayout.h"
#include "kademlia/utils/UInt128.h"

namespace Kademlia
{

// Validate the destination before building the TCP relay. The UDP request stays untouched.
// The requester's ID may carry either the aMule or the eMule TAG_BUDDYHASH layout (#1835); on a
// match in either layout, relay the ID stored from KADEMLIA_FINDBUDDY_REQ, not the received form,
// so the source's own (layout-aware) check always sees the value it actually accepts.
inline bool BuildBuddyCallback(
	CMemFile &request, const CUInt128 &buddyID, uint32_t requesterIP, CMemFile &callback)
{
	const CUInt128 destination = request.ReadUInt128();
	uint8_t destinationBytes[16];
	destination.ToByteArray(destinationBytes);
	uint8_t buddyIDBytes[16];
	buddyID.ToByteArray(buddyIDBytes);
	if (!MatchesBuddyIdEitherLayout(destinationBytes, buddyIDBytes)) {
		return false;
	}
	const CUInt128 file = request.ReadUInt128();
	const uint16_t tcpPort = request.ReadUInt16();
	callback.WriteUInt128(buddyID);
	callback.WriteUInt128(file);
	callback.WriteUInt32(requesterIP);
	callback.WriteUInt16(tcpPort);
	return true;
}

} // namespace Kademlia

#endif // KAD_BUDDY_CALLBACK_H
