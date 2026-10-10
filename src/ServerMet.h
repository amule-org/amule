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

#ifndef SERVERMET_H
#define SERVERMET_H

#include "Tag.h" // Needed for CTag

#include <vector>

class CFileDataIO;

// One server as server.met stores it.
struct ServerMetRecord
{
	uint32 ip = 0;
	uint16 port = 0;
	std::vector<CTag> tags;
};

/**
 * Reads the server count and the server records that follow the header byte of a server.met.
 *
 * Older aMule versions declared one tag too many for a server without a version. A file that
 * reads to its end only with that tag left out is read that way, and the function returns true.
 *
 * Throws CInvalidPacket or CSafeIOException if the file reads to its end neither way. records
 * then holds the servers read before the error.
 */
bool ReadServerMetRecords(const CFileDataIO &file, std::vector<ServerMetRecord> &records);

#endif // SERVERMET_H
