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

#ifndef KAD_POINTER_LOOKUP_H
#define KAD_POINTER_LOOKUP_H

#include "KadPointerCache.h"

class CMD4Hash;
namespace Kademlia
{
class CUInt128;
}

// The pointer lookups of this client: one cache, fed by FINDPOINTER searches (#2513).
namespace KadPointerLookup
{
CKadPointerCache &Cache();
CKadPointerCache::Key KeyOf(const CMD4Hash &userHash);
//! Starts the lookup at ~userHash for a source whose record came from `recordIP`.
bool Start(const CMD4Hash &userHash, uint32_t recordIP);
//! A record a FINDPOINTER search at `target` returned.
void AddRecord(const Kademlia::CUInt128 &target, const SKadPointerRecord &record);
//! That search ended.
void Finished(const Kademlia::CUInt128 &target);
} // namespace KadPointerLookup

#endif // KAD_POINTER_LOOKUP_H
