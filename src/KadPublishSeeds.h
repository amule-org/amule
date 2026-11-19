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

#ifndef KADPUBLISHSEEDS_H
#define KADPUBLISHSEEDS_H

#include "kademlia/utils/KadUDPKey.h" // Needed for CKadUDPKey
#include "kademlia/utils/UInt128.h"   // Needed for CUInt128

#include <common/Macros.h> // Needed for HR2S

#include <cstdint>
#include <map>
#include <vector>

class CFileDataIO;

//! kadseeds.dat: the nodes near each file or keyword that answered its last Kad publish. The
//! next publish asks them first, which saves most of the lookup.
namespace KadPublishSeeds
{
struct Seed
{
	Kademlia::CUInt128 id;
	uint32_t ip = 0;
	uint16_t udpPort = 0;
	uint16_t tcpPort = 0;
	uint8_t version = 0;
	Kademlia::CKadUDPKey key;
	bool verified = false;
};

struct Entry
{
	uint32_t saved = 0;
	std::vector<Seed> seeds;
};

using Seeds = std::map<Kademlia::CUInt128, Entry>;

constexpr const char *FileName = "kadseeds.dat";
constexpr size_t MaxSeeds = 20;
//! Keywords publish once a day, so their seeds are a day old when used again. Older sets have lost
//! more nodes, and a lookup from them misses more of the closest nodes than a full lookup.
constexpr uint32_t MaxAge = HR2S(30);

inline bool IsCurrent(const Entry &entry, uint32_t now)
{
	return entry.saved <= now && now - entry.saved <= MaxAge;
}

//! Writes the current entries only, with at most MaxSeeds seeds each.
void Write(CFileDataIO &file, const Seeds &seeds, uint32_t now);

//! Keeps the current entries, with at most MaxSeeds seeds that have an address. Returns false
//! for a damaged or unknown file; `seeds` then holds what was read before the damage.
bool Read(const CFileDataIO &file, uint32_t now, Seeds &seeds);
} // namespace KadPublishSeeds

#endif // KADPUBLISHSEEDS_H
// File_checked_for_headers
