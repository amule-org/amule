//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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

#ifndef SEARCHLIST_H
#define SEARCHLIST_H

#include "Timer.h"           // Needed for CTimer
#include "ObservableQueue.h" // Needed for CQueueObserver
#include "SearchFile.h"      // Needed for CSearchFile
#include <common/SmartPtr.h> // Needed for CSmartPtr
#include <set>               // Needed for std::set (per-search Kad completion)
#include <map>               // Needed for std::map (per-search start times)
#include <vector>            // Needed for std::vector (same-hash result fan-out)

class CMemFile;
class CMD4Hash;
class CPacket;
class CServer;
class CSearchFile;

namespace Kademlia
{
class CUInt128;
}

enum SearchType
{
	LocalSearch,
	GlobalSearch,
	KadSearch
};

typedef std::vector<CSearchFile *> CSearchResultList;

class CSearchList : public wxEvtHandler
{
public:
	//! Structure used to pass search-parameters.
	struct CSearchParams
	{
		/** Prevents accidental use of uninitialized variables. */
		CSearchParams() { minSize = maxSize = availability = 0; }

		//! The actual string to search for.
		wxString searchString;
		//! The keyword selected for Kad search
		wxString strKeyword;
		//! The type of files to search for (may be empty), one of ED2KFTSTR_*
		wxString typeText;
		//! The filename extension. May be empty.
		wxString extension;
		//! The smallest filesize in bytes to accept, zero for any.
		uint64_t minSize;
		//! The largest filesize in bytes to accept, zero for any.
		uint64_t maxSize;
		//! The minimum available (source-count), zero for any.
		uint32_t availability;
	};

	/** Constructor. */
	CSearchList();

	/** Frees any remaining search-results. */
	~CSearchList();

	/**
	 * Starts a new search.
	 *
	 * @param searchID The ID of the search, which may be modified.
	 * @param type The type of search, see SearchType.
	 * @param params The search parameters, see CSearchParams.
	 * @return An empty string on success, otherwise an error-message.
	 */
	wxString StartNewSearch(uint32 *searchID, SearchType type, CSearchParams &params);

	/** Stops the current search (global or Kad), if any is in progress. */
	void StopSearch(bool globalOnly = false);

	/**
	 * Stops network activity for one specific search by ID, keeping its
	 * results (the multi-search EC "stop" — as opposed to RemoveResults,
	 * which also frees the results). Stops the matching Kad keyword search
	 * if this ID is one, and finalizes the ed2k global sweep if this ID is
	 * the in-flight one. A no-op for an already-finished / unknown ID.
	 */
	void StopSearchById(wxUIntPtr searchID);

	/**
	 * Finalizes any in-flight ed2k (local/global) search, keeping its
	 * results. ed2k searches share a single in-flight slot and file their
	 * results under the scalar m_currentSearch, so the multi-search EC layer
	 * calls this before starting a new search to stop the old sweep's late
	 * UDP results from leaking into the new search's bucket. Running Kad
	 * searches are attributed by their own ID and are left untouched.
	 */
	void StopInFlightEd2kSearch();

	/** True if the given searchID corresponds to an active Kad search. */
	bool IsKadSearch(uint32_t searchID) const;

	/** True if the given searchID is a Kad search, active or already finished. */
	bool IsOrWasKadSearch(uint32_t searchID) const;

	/**
	 * Ask the Kad search identified by searchID to widen its frontier
	 * via KADEMLIA_FIND_VALUE_MORE.  Wired to the search dialog "More"
	 * button.  Returns true if a reask was dispatched, false otherwise.
	 */
	bool RequestMoreResults(uint32_t searchID);

	/** Returns the completion percentage of the current search. */
	uint32 GetSearchProgress() const;

	// Unambiguous lifecycle accessors used by the new EC tags
	// (EC_TAG_SEARCH_LIFECYCLE_STATE / _KIND / _RESULT_COUNT). Old
	// consumers still read the overloaded GetSearchProgress() return.
	enum SearchLifecycleState
	{
		SEARCH_LIFECYCLE_IDLE = 0,    // no search started this session
		SEARCH_LIFECYCLE_RUNNING = 1, // active search in flight
		SEARCH_LIFECYCLE_FINISHED = 2 // last search completed; results retained
	};
	SearchLifecycleState GetSearchLifecycleState() const;

	// Per-search-ID lifecycle accessors for the multi-search EC path. For
	// the most-recently-started search (== m_currentSearch) these delegate
	// to the scalar accessors above (accurate live state); for older searches
	// they infer state from the Kad manager (a still-active keyword search is
	// RUNNING) and the retained result bucket (present => FINISHED).
	SearchLifecycleState GetSearchLifecycleStateById(wxUIntPtr searchID) const;
	uint8 GetSearchLifecyclePercentById(wxUIntPtr searchID) const;
	// The overloaded progress-bar sentinel for a search: 0xffff when a finished
	// ed2k search, 0xfffe when a finished Kad search (each resets the bar and,
	// for Kad, clears the "!" marker on the client), otherwise the running
	// percent. Single source of truth for the bottom bar, shared by the EC
	// PROGRESS reply (remote GUI / amuleapi) and the monolithic search dialog.
	uint32 GetSearchBarStatusById(wxUIntPtr searchID) const;
	// Echoes m_searchType for the current/last search; meaningful only
	// when state is RUNNING or FINISHED. Returns LocalSearch by default.
	SearchType GetSearchLifecycleKind() const { return m_searchType; }
	// Result count for the current search; 0 if idle.
	std::size_t GetCurrentSearchResultCount() const;
	// Unified 0..100 completion for the current search, surfaced via
	// EC_TAG_SEARCH_LIFECYCLE_PERCENT. Global uses the real server-queue
	// percent; Kad — which has no measurable progress — gets a cosmetic
	// time-ramp off the fixed keyword-search lifetime that the FINISHED
	// lifecycle state authoritatively snaps to 100. Idle returns 0.
	uint8 GetSearchLifecyclePercent() const;

	/** This function is called once the local (ed2k) search has ended. */
	void LocalSearchEnd();

	/**
	 * Returns the list of results for the specified search.
	 *
	 * If the search is not valid, an empty list is returned.
	 */
	const CSearchResultList &GetSearchResults(wxUIntPtr searchID) const;

	/** Removes all results for the specified search. */
	void RemoveResults(wxUIntPtr searchID);

	/** Finds the search-result (by hash) and downloads it in the given category. */
	void AddFileToDownloadByHash(const CMD4Hash &hash, uint8 category = 0);

	/**
	 * Returns the first search-result (across every search, parents and their
	 * children) matching the given file hash, or NULL if none. Used by the Kad
	 * NOTES machinery to size and attach on-demand community comments to a
	 * result the user has not downloaded.
	 */
	CSearchFile *GetSearchFileByID(const CMD4Hash &hash) const;

	/**
	 * Collect EVERY search result (parents and children) matching the given
	 * file hash, across all concurrent searches. On-demand Kad notes and the
	 * running flag fan out to all of them so the same file shown in more than
	 * one open search tab gets its community comments everywhere, not just in
	 * the first tab that happens to hold it.
	 */
	void GetAllSearchFilesByID(const CMD4Hash &hash, std::vector<CSearchFile *> &out) const;

	/**
	 * Start downloading the specific search result identified by its EC
	 * ECID — used to pick one same-hash/different-name grouped child so
	 * the partfile lands under that chosen filename (issue #431).
	 * Searches parents and their children; a no-op if the ecid is gone.
	 */
	void AddFileToDownloadByEcid(uint32 ecid, uint8 category = 0);

	/**
	 * Processes a list of shared files from a client.
	 *
	 * @param packet The raw packet received from the client.
	 * @param size the length of the packet.
	 * @param sender The sender of the packet.
	 * @param moreResultsAvailable Set to a value specifying if more results are available.
	 * @param directory The directory containing the shared files.
	 */
	void ProcessSharedFileList(const uint8_t *packet,
		uint32 size,
		CUpDownClient *sender,
		bool *moreResultsAvailable,
		const wxString &directory);

	/**
	 * Processes a search-result sent via TCP from the local server. All results are added.
	 *
	 * @param packet The packet containing one or more search-results.
	 * @param size the length of the packet.
	 * @param optUTF8 Specifies if the server supports UTF8.
	 * @param serverIP The IP of the server sending the results.
	 * @param serverPort The Port of the server sending the results.
	 */
	void ProcessSearchAnswer(
		const uint8_t *packet, uint32_t size, bool optUTF8, uint32_t serverIP, uint16_t serverPort);

	/**
	 * Processes a search-result sent via UDP. Only one result is read from the packet.
	 *
	 * @param packet The packet containing one or more search-results.
	 * @param optUTF8 Specifies if the server supports UTF8.
	 * @param serverIP The IP of the server sending the results.
	 * @param serverPort The Port of the server sending the results.
	 */
	void ProcessUDPSearchAnswer(const CMemFile &packet, bool optUTF8, uint32 serverIP, uint16 serverPort);

	/**
	 * Adds a result in the form of a kad search-keyword to the specified result-list.
	 *
	 * @param searchID The search to which this result belongs.
	 * @param fileID The hash of the result-file.
	 * @param name The filename of the result.
	 * @param size The filesize of the result.
	 * @param type The filetype of the result (TODO: Not used?)
	 * @param kadPublishInfo The kademlia publish information of the result.
	 * @param taglist List of additional tags associated with the search-result.
	 */
	void KademliaSearchKeyword(uint32_t searchID,
		const Kademlia::CUInt128 *fileID,
		const wxString &name,
		uint64_t size,
		const wxString &type,
		uint32_t kadPublishInfo,
		const TagPtrList &taglist);

	/** Update a certain search result in all lists */
	void UpdateSearchFileByHash(const CMD4Hash &hash);

	/**
	 * Mark a specific Kad search (by ID) as finished. Records it per-search
	 * (m_finishedKadSearches) so multi-search progress is precise — one search
	 * ending must not report a *different* running search as finished. Also
	 * sets the legacy scalar m_KadSearchFinished for the single-search
	 * (parameterless GetSearchProgress / GetSearchLifecycleState) path.
	 */
	void SetKadSearchFinished(uint32_t searchID);

private:
	/** Event-handler for global searches. */
	void OnGlobalSearchTimer(CTimerEvent &evt);

	/**
	 * Shared cleanup for global-search completion. Releases the search
	 * packet, stops the timer, notifies 100% progress, and sets
	 * m_ed2kSearchFinished. Callers decide whether to also reset
	 * m_currentSearch: StopSearch does (explicit abort), the natural-
	 * drain path in OnGlobalSearchTimer does not (preserving the ID
	 * lets GetSearchLifecycleState report FINISHED instead of IDLE).
	 */
	void FinalizeGlobalSearch();

	/**
	 * Adds the specified file to the current search's results.
	 *
	 * @param toadd The result to add.
	 * @param clientResponse Is the result sent by a client (shared-files list).
	 * @return True if the results were added, false otherwise.
	 *
	 * Note that this function takes ownership of the CSearchFile object,
	 * regardless of whenever or not it was actually added to the results list.
	 */
	bool AddToList(CSearchFile *toadd, bool clientResponse = false);

	//! This smart pointer is used to safely prevent leaks.
	typedef CSmartPtr<CMemFile> CMemFilePtr;

	/** Create a basic search-packet for the given search-type. */
	CMemFilePtr CreateSearchData(
		CSearchParams &params, SearchType type, bool supports64bit, bool &packetUsing64bit);

	//! Timer used for global search intervals.
	CTimer m_searchTimer;

	//! The current search-type, regarding the last/current search.
	SearchType m_searchType;

	//! Specifies if a search is being performed.
	bool m_searchInProgress;

	//! The ID of the current search.
	wxUIntPtr m_currentSearch;

	//! The current packet used for searches.
	CPacket *m_searchPacket;

	//! Does the current search packet contain 64bit values?
	bool m_64bitSearchPacket;

	//! If the current search is a KAD search this signals if it is finished.
	bool m_KadSearchFinished;

	//! Per-search Kad completion (multi-search): the IDs of Kad searches that
	//! have ended (their CSearch was destroyed on the result cap or the 45s
	//! lifetime). Lets GetSearchLifecycleStateById report each search
	//! independently, so one search finishing does not mark a different
	//! still-running search as finished. Pruned in RemoveResults.
	std::set<uint32_t> m_finishedKadSearches;

	//! Per-search start time (multi-search), so each search's cosmetic Kad
	//! progress ramp is computed from its own age rather than the single
	//! m_searchStart of the most-recently-started search. Pruned in RemoveResults.
	std::map<uint32_t, time_t> m_searchStartTimes;

	//! ED2K-side counterpart of m_KadSearchFinished, covering both local
	//! and global searches. Cleared to false in StartNewSearch when an
	//! ED2K search is issued; set back to true in LocalSearchEnd (local)
	//! or FinalizeGlobalSearch (global — both natural drain and
	//! explicit abort). GetSearchLifecycleState uses this as the
	//! RUNNING vs FINISHED signal for the ED2K branch.
	bool m_ed2kSearchFinished;

	//! Wall-clock start of the current/last search. Stamped in
	//! StartNewSearch; feeds the Kad cosmetic progress ramp in
	//! GetSearchLifecyclePercent.
	time_t m_searchStart;

	//! Queue of servers to ask when doing global searches.
	//! TODO: Replace with 'cookie' system.
	CQueueObserver<CServer *> m_serverQueue;

	//! Shorthand for the map of results (key is a SearchID).
	typedef std::map<wxUIntPtr, CSearchResultList> ResultMap;

	//! Map of all search-results added.
	ResultMap m_results;

	//! Contains the results type desired in the current search.
	//! If not empty, results of different types are filtered.
	wxString m_resultType;

	wxDECLARE_EVENT_TABLE();
};

#endif // SEARCHLIST_H
// File_checked_for_headers
