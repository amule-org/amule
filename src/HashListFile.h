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

#ifndef HASHLISTFILE_H
#define HASHLISTFILE_H

#include <set>
#include "MD4Hash.h"

// A set of file hashes saved in a file of its own in the config dir: canceled.met for canceled
// downloads, knownhistory.met for files known.met no longer remembers.
class CHashListFile
{
public:
	// Loads the list from `filename`, if it exists
	explicit CHashListFile(const wxString &filename);
	void Save();
	bool Contains(const CMD4Hash &hash) const;
	// Returns true if added, false if already in the list
	bool Add(const CMD4Hash &hash);
	// Returns true if removed, false if not in the list
	bool Remove(const CMD4Hash &hash);

private:
	std::set<CMD4Hash> m_hashes;
	wxString m_filename;

	bool Init();
};

#endif // HASHLISTFILE_H
// File_checked_for_headers
