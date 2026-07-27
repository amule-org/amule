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

#include "SearchHistory.h"

wxArrayString ApplySearchHistoryEntry(const wxArrayString &existing, const wxString &term, size_t maxEntries)
{
	if (term.IsEmpty()) {
		return existing;
	}

	wxArrayString result;
	result.Add(term);
	for (const wxString &candidate : existing) {
		if (candidate.CmpNoCase(term) != 0) {
			result.Add(candidate);
		}
	}
	if (result.GetCount() > maxEntries) {
		result.RemoveAt(maxEntries, result.GetCount() - maxEntries);
	}
	return result;
}
