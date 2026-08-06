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

#ifndef SEARCHRESULTINDEX_H
#define SEARCHRESULTINDEX_H

#include <algorithm> // Needed for std::find
#include <map>       // Needed for std::map

#include "SearchFile.h" // Needed for CSearchFile, CSearchResultList

/**
 * The per-search result index shared by the two search-list implementations.
 *
 * The search list has one implementation per build: CSearchList (monolithic,
 * fed by the network stack) and CSearchListRem (amulegui, fed over EC). Both
 * are reached through the same theApp->searchlist seam, and the GUI reads its
 * rows from GetSearchResults() -- the wxDataViewCtrl search model builds every
 * row from it, so a result that is not indexed here is invisible, no matter
 * how correctly it arrived.
 *
 * That contract used to be satisfied by each implementation keeping its own
 * copy of this map, which is how the remote one came to never populate it at
 * all (the pre-wxDataViewCtrl list control held its own rows, so nothing broke
 * visibly until the port made this map the row source). Keeping the map here,
 * with IndexResult() as the only way to add to it, means a new implementation
 * cannot answer GetSearchResults() correctly and still forget to fill it.
 *
 * Ownership deliberately stays with the derived class: the monolithic list
 * owns its CSearchFile objects, while the remote one only borrows pointers
 * owned by its CRemoteContainer. This index therefore never deletes anything
 * -- DropResultIndex() drops the bookkeeping and leaves freeing to the owner.
 */
class CSearchResultIndex
{
public:
	/**
	 * Returns the list of results for the specified search.
	 *
	 * If the search is not known, an empty list is returned.
	 */
	const CSearchResultList &GetSearchResults(wxUIntPtr searchID) const
	{
		ResultMap::const_iterator it = m_results.find(searchID);
		if (it != m_results.end()) {
			return it->second;
		}

		static const CSearchResultList emptyList;
		return emptyList;
	}

protected:
	//! Not polymorphic: derived classes are never deleted through this type.
	~CSearchResultIndex() = default;

	//! Shorthand for the map of results (key is a SearchID).
	typedef std::map<wxUIntPtr, CSearchResultList> ResultMap;

	/**
	 * Makes a result visible to the GUI, under its own search id.
	 *
	 * This is the single point where a result enters the index, and every
	 * search-list implementation has to route new results through it.
	 */
	void IndexResult(CSearchFile *file) { m_results[file->GetSearchID()].push_back(file); }

	/**
	 * Drops a single result from the index, without freeing it.
	 *
	 * Call this before the owner frees a result, so no freed pointer is left
	 * behind for GetSearchResults() to hand out.
	 */
	void UnindexResult(CSearchFile *file)
	{
		ResultMap::iterator it = m_results.find(file->GetSearchID());
		if (it == m_results.end()) {
			return;
		}

		CSearchResultList &list = it->second;
		CSearchResultList::iterator pos = std::find(list.begin(), list.end(), file);
		if (pos != list.end()) {
			list.erase(pos);
		}
	}

	/**
	 * Drops a whole search from the index, without freeing its results.
	 *
	 * The results belong to the derived class, which frees them itself where
	 * it owns them.
	 */
	void DropResultIndex(wxUIntPtr searchID) { m_results.erase(searchID); }

	//! Map of all indexed search-results, keyed by search id.
	ResultMap m_results;
};

#endif // SEARCHRESULTINDEX_H
