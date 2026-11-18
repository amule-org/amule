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

#include "KadSourceRecordTags.h"

#include "KadBuddyIdLayout.h" // Needed for ConvertBuddyIdLayout
#include "KadPointerPolicy.h" // Needed for KAD_POINTER_VERSION
#include "MD4Hash.h"

#include <tags/FileTags.h>

void AddKadBuddyRouteTags(TagPtrList &taglist, const SKadBuddyRoute &route)
{
	taglist.push_back(new CTagVarInt(TAG_SERVERIP, route.buddyIP));
	taglist.push_back(new CTagVarInt(TAG_SERVERPORT, route.buddyUDPPort));
	// eMule, emule-qt and eMuleAI all publish TAG_BUDDYHASH in their own layout (#1835).
	uint8_t emuleLayout[16];
	Kademlia::ConvertBuddyIdLayout(route.buddyID, emuleLayout);
	taglist.push_back(new CTagString(TAG_BUDDYHASH, CMD4Hash(emuleLayout).Encode()));
	taglist.push_back(new CTagVarInt(TAG_SOURCEPORT, route.tcpPort));
	if (route.kadPort != 0) {
		taglist.push_back(new CTagInt16(TAG_SOURCEUPORT, route.kadPort));
	}
}

void AddKadBuddyRecordTags(
	TagPtrList &taglist, const SKadBuddyRoute &route, uint8_t sourceType, uint64_t fileSize, bool marked)
{
	taglist.push_back(new CTagInt8(TAG_SOURCETYPE, sourceType));
	AddKadBuddyRouteTags(taglist, route);
	if (fileSize != 0) {
		taglist.push_back(new CTagVarInt(TAG_FILESIZE, fileSize));
	}
	// A receiver that finds this buddy gone looks up where our pointer says we are now.
	if (marked) {
		taglist.push_back(new CTagInt8(TAG_BUDDYPOINTER, KAD_POINTER_VERSION));
	}
}

void AddKadPointerRecordTags(TagPtrList &taglist, const SKadBuddyRoute &route, uint8_t connectOptions)
{
	taglist.push_back(new CTagInt8(TAG_SOURCETYPE, 3));
	AddKadBuddyRouteTags(taglist, route);
	taglist.push_back(new CTagInt8(TAG_BUDDYPOINTER, KAD_POINTER_VERSION));
	taglist.push_back(new CTagInt8(TAG_ENCRYPTION, connectOptions));
}
