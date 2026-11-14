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

#include "KadKeywordPublishTimes.h" // Interface declarations

#include <common/DataFileVersion.h> // Needed for KADKEYWORDS_VERSION
#include <protocol/kad/Constants.h> // Needed for KADEMLIAREPUBLISHTIMEK

#include "SafeFile.h" // Needed for CFileDataIO

#include <algorithm>

namespace KadKeywordPublishTimes
{
void Write(CFileDataIO &file, const Times &times)
{
	file.WriteUInt8(KADKEYWORDS_VERSION);
	file.WriteUInt32(times.size());
	for (const auto &time : times) {
		file.WriteUInt128(time.first);
		file.WriteUInt32(time.second);
	}
}

bool Read(const CFileDataIO &file, uint32_t now, Times &times)
{
	try {
		if (file.ReadUInt8() != KADKEYWORDS_VERSION) {
			return false;
		}
		const uint32_t latest = now + KADEMLIAREPUBLISHTIMEK;
		for (uint32_t count = file.ReadUInt32(); count; --count) {
			const Kademlia::CUInt128 keyword = file.ReadUInt128();
			const uint32_t due = file.ReadUInt32();
			if (due > now) {
				times[keyword] = std::min(due, latest);
			}
		}
	} catch (const CSafeIOException &) {
		return false;
	}
	return true;
}
} // namespace KadKeywordPublishTimes
// File_checked_for_headers
