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

#ifndef KADKEYWORDPUBLISHTIMES_H
#define KADKEYWORDPUBLISHTIMES_H

#include "kademlia/utils/UInt128.h" // Needed for CUInt128

#include <cstdint>
#include <map>

class CFileDataIO;

//! kadkeywords.dat: when each keyword is due for its next Kad publish, so a restart does not
//! republish every keyword at once.
namespace KadKeywordPublishTimes
{
using Times = std::map<Kademlia::CUInt128, uint32_t>;

constexpr const char *FileName = "kadkeywords.dat";

void Write(CFileDataIO &file, const Times &times);

//! Drops times already due, and caps the rest at one republish interval from `now`, so a
//! clock set back cannot hold a keyword longer. Returns false for a damaged or unknown file;
//! `times` then holds what was read before the damage.
bool Read(const CFileDataIO &file, uint32_t now, Times &times);
} // namespace KadKeywordPublishTimes

#endif // KADKEYWORDPUBLISHTIMES_H
// File_checked_for_headers
