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

#include "KadPublishSeeds.h" // Interface declarations

#include <common/DataFileVersion.h> // Needed for KADSEEDS_VERSION

#include "SafeFile.h" // Needed for CFileDataIO

#include <algorithm>

namespace KadPublishSeeds
{
void Write(CFileDataIO &file, const Seeds &seeds, uint32_t now)
{
	file.WriteUInt8(KADSEEDS_VERSION);
	file.WriteUInt32(static_cast<uint32_t>(std::count_if(seeds.begin(),
		seeds.end(),
		[now](const Seeds::value_type &entry) { return IsCurrent(entry.second, now); })));
	for (const auto &entry : seeds) {
		if (!IsCurrent(entry.second, now)) {
			continue;
		}
		const size_t count = std::min(entry.second.seeds.size(), MaxSeeds);
		file.WriteUInt128(entry.first);
		file.WriteUInt32(entry.second.saved);
		file.WriteUInt8(static_cast<uint8_t>(count));
		for (size_t i = 0; i < count; ++i) {
			const Seed &seed = entry.second.seeds[i];
			file.WriteUInt128(seed.id);
			file.WriteUInt32(seed.ip);
			file.WriteUInt16(seed.udpPort);
			file.WriteUInt16(seed.tcpPort);
			file.WriteUInt8(seed.version);
			seed.key.StoreToFile(file);
			file.WriteUInt8(seed.verified);
		}
	}
}

bool Read(const CFileDataIO &file, uint32_t now, Seeds &seeds)
{
	try {
		if (file.ReadUInt8() != KADSEEDS_VERSION) {
			return false;
		}
		for (uint32_t count = file.ReadUInt32(); count; --count) {
			const Kademlia::CUInt128 target = file.ReadUInt128();
			Entry entry;
			entry.saved = file.ReadUInt32();
			for (uint8_t n = file.ReadUInt8(); n; --n) {
				Seed seed;
				seed.id = file.ReadUInt128();
				seed.ip = file.ReadUInt32();
				seed.udpPort = file.ReadUInt16();
				seed.tcpPort = file.ReadUInt16();
				seed.version = file.ReadUInt8();
				const uint32_t key = file.ReadUInt32();
				const uint32_t keyIP = file.ReadUInt32();
				seed.key = Kademlia::CKadUDPKey(key, keyIP);
				seed.verified = file.ReadUInt8() != 0;
				if (seed.ip && seed.udpPort && entry.seeds.size() < MaxSeeds) {
					entry.seeds.push_back(seed);
				}
			}
			if (IsCurrent(entry, now) && !entry.seeds.empty()) {
				seeds[target] = std::move(entry);
			}
		}
	} catch (const CSafeIOException &) {
		return false;
	}
	return true;
}
} // namespace KadPublishSeeds
// File_checked_for_headers
