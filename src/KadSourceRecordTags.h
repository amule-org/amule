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

#ifndef KAD_SOURCE_RECORD_TAGS_H
#define KAD_SOURCE_RECORD_TAGS_H

#include "Tag.h" // Needed for TagPtrList

#include <cstdint>

//! What a Kad source record needs to say about the buddy that relays callbacks to us.
struct SKadBuddyRoute
{
	uint32_t buddyIP = 0;
	uint16_t buddyUDPPort = 0;
	//! Our buddy ID in aMule's layout (CUInt128::ToByteArray); it goes out in eMule's (#1835).
	uint8_t buddyID[16] = {};
	uint16_t tcpPort = 0;
	//! Our internal Kad port, or 0 when storers should take the port the packet came from.
	uint16_t kadPort = 0;
};

//! The buddy fields and our ports, shared by a type 3/5 record and our pointer record.
void AddKadBuddyRouteTags(TagPtrList &taglist, const SKadBuddyRoute &route);

/**
 * A type 3/5 file record up to the tags every source type shares: the type, the buddy route, the
 * size when the storer reads it (0 for none), and the pointer route marker when `marked`.
 */
void AddKadBuddyRecordTags(
	TagPtrList &taglist, const SKadBuddyRoute &route, uint8_t sourceType, uint64_t fileSize, bool marked);

/**
 * Our pointer record (#2513, emule-qt ipv6-spec §5.6.1): type 3 with the buddy route, read by a
 * receiver like a file record. No size, since it names no file; `bpv` gives its version.
 */
void AddKadPointerRecordTags(TagPtrList &taglist, const SKadBuddyRoute &route, uint8_t connectOptions);

#endif // KAD_SOURCE_RECORD_TAGS_H
