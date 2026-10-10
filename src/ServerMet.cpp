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

#include "ServerMet.h" // Interface declarations

#include <common/MuleDebug.h> // Needed for CInvalidPacket
#include <tags/ServerTags.h>  // Needed for ST_VERSION

#include "SafeFile.h" // Needed for CFileDataIO

#include <algorithm> // Needed for std::any_of
#include <utility>   // Needed for std::move

static bool HasVersion(const std::vector<CTag> &tags)
{
	return std::any_of(
		tags.begin(), tags.end(), [](const CTag &tag) { return tag.GetNameID() == ST_VERSION; });
}

// With legacyCounts, a server with no version among its tags holds one tag fewer than declared.
static void ReadRecords(const CFileDataIO &file, bool legacyCounts, std::vector<ServerMetRecord> &records)
{
	records.clear();
	const uint32 count = file.ReadUInt32();
	for (uint32 i = 0; i < count; ++i) {
		ServerMetRecord record;
		record.ip = file.ReadUInt32();
		record.port = file.ReadUInt16();
		const uint32 tagCount = file.ReadUInt32();
		for (uint32 t = 0; t < tagCount; ++t) {
			if (legacyCounts && t + 1 == tagCount && !HasVersion(record.tags)) {
				break;
			}
			record.tags.emplace_back(file, true);
		}
		records.push_back(std::move(record));
	}
}

static bool ReadsToEnd(const CFileDataIO &file, bool legacyCounts, std::vector<ServerMetRecord> &records)
{
	try {
		ReadRecords(file, legacyCounts, records);
	} catch (const CInvalidPacket &) {
		return false;
	} catch (const CSafeIOException &) {
		return false;
	}
	return file.GetPosition() == file.GetLength();
}

bool ReadServerMetRecords(const CFileDataIO &file, std::vector<ServerMetRecord> &records)
{
	const sint64 start = static_cast<sint64>(file.GetPosition());
	if (ReadsToEnd(file, false, records)) {
		return false;
	}
	file.Seek(start);
	if (ReadsToEnd(file, true, records)) {
		return true;
	}
	// Neither way works: read as declared again, so the caller gets the servers before the error,
	// and the error.
	file.Seek(start);
	ReadRecords(file, false, records);
	return false;
}
