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

#ifndef CLIENTMETATRAILER_H
#define CLIENTMETATRAILER_H

#include "ClientCredits.h" // Needed for ClientMetaStruct
#include "MD4Hash.h"       // Needed for CMD4Hash

#include <vector>

class CFileDataIO;

//! The optional metadata block after the credit records in clients.met. Kept apart from
//! CClientCreditsList, which needs the application, so the format can be tested on its own.
namespace ClientMetaTrailer
{
struct Entry
{
	CMD4Hash key;
	ClientMetaStruct meta;
};

enum ReadResult
{
	READ_OK,
	READ_ABSENT,        //!< nothing after the records, or too little to be a block
	READ_FOREIGN,       //!< trailing data without our magic
	READ_OTHER_VERSION, //!< a block from a newer aMule
	READ_DAMAGED        //!< the block ends early or is malformed
};

//! Writes nothing for an empty list, so the file stays as an older build writes it.
void Write(CFileDataIO &file, const std::vector<Entry> &entries);

//! Reads from the current position to the end. On READ_DAMAGED, `entries` still holds
//! every entry read before the damage.
ReadResult Read(CFileDataIO &file, std::vector<Entry> &entries);
} // namespace ClientMetaTrailer

#endif // CLIENTMETATRAILER_H
// File_checked_for_headers
