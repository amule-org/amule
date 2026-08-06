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
 *
 * What the index holds -- part of the contract, not an implementation detail:
 * TOP-LEVEL RESULTS ONLY. A result grouped under another one (same hash, a
 * different filename) is reachable through its parent's GetChildren() and must
 * not be indexed here, or GetSearchResults(id).size() stops meaning "hits for
 * this search" and consumers see each grouped result twice.
 *
 * Performance note for whenever result counts grow: UnindexResult() is a linear
 * scan of one search's list, so tearing down a whole search one result at a
 * time (CRemoteContainer::FullReload, which calls DeleteItem per item) costs
 * O(N^2) in results per search. That is irrelevant at the few hundred results a
 * search returns today; if the caps ever rise, drop the whole index for the
 * search up front with DropResultIndex() rather than unindexing item by item.
 */
class CSearchResultIndex
{
public:
	/**
	 * Returns the list of top-level results for the specified search.
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
	 * True if this search is present in the index at all.
	 *
	 * Distinct from GetSearchResults(id).empty(): a search that is known but
	 * currently holds no results answers true here and empty there.
	 */
	bool HasSearchResults(wxUIntPtr searchID) const
	{
		return m_results.find(searchID) != m_results.end();
	}

	/**
	 * Every indexed search, for the few operations that have to walk them all
	 * (find a result by hash across open searches, drain the index).
	 *
	 * Read-only on purpose: the map is private so that IndexResult() is the
	 * only way a result can enter it. Walking the lists to call non-const
	 * methods on the results themselves is still fine -- the constness applies
	 * to the index, not to what it points at.
	 */
	const ResultMap &AllResults() const { return m_results; }

	/**
	 * Makes a top-level result visible to the GUI, under its own search id.
	 *
	 * This is the single point where a result enters the index, and every
	 * search-list implementation has to route new results through it.
	 *
	 * Passing a grouped result is a no-op rather than a caller error: the
	 * top-level rule is the index's to keep, so an implementation that hands
	 * over everything it receives still ends up with a correct index instead
	 * of one that is wrong in a way only a consumer notices.
	 */
	void IndexResult(CSearchFile *file)
	{
		if (file->GetParent() != nullptr) {
			return;
		}

		m_results[file->GetSearchID()].push_back(file);
	}

	/**
	 * Drops a single result from the index, without freeing it.
	 *
	 * Call this before the owner frees a result, so no freed pointer is left
	 * behind for GetSearchResults() to hand out.
	 */
	void UnindexResult(CSearchFile *file)
	{
		// Symmetric with IndexResult(): a grouped result was never indexed, so
		// searching for it would scan the whole list to find nothing. A result
		// is parented when it is constructed or loaded, before it could have
		// been indexed, and nothing re-parents an indexed one -- so this can
		// never skip a result that is actually in the index.
		if (file->GetParent() != nullptr) {
			return;
		}

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

private:
	//! Map of all indexed top-level search-results, keyed by search id.
	//! Private so the only way in is IndexResult(): an implementation that
	//! answers GetSearchResults() cannot quietly stop filling what it answers
	//! from, which is exactly the drift this class exists to prevent.
	ResultMap m_results;
};

#endif // SEARCHRESULTINDEX_H
