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

#include "Timer.h"             // Needed for CTimer
#include "ObservableQueue.h"   // Needed for CQueueObserver
#include "SearchFile.h"        // Needed for CSearchFile
#include "SearchResultIndex.h" // Needed for CSearchResultIndex
#include <common/SmartPtr.h>   // Needed for CSmartPtr
#include <set>                 // Needed for std::set (per-search Kad completion)
#include <map>                 // Needed for std::map (per-search start times)
#include <vector>              // Needed for std::vector (same-hash result fan-out)

class CMemFile;
class CMD4Hash;
class CPacket;
class CServer;
class CSearchFile;

namespace Kademlia
{
class CUInt128;
}

/**
 * What kind of search an id names.
 *
 * The values are pinned, not incidental. They are cast straight to uint8 and
 * shipped as EC_TAG_SEARCH_LIFECYCLE_KIND, where they have to line up with
 * EC_SEARCH_TYPE in ECCodes.abstract, and CSearchList::Save() writes the same
 * byte into the saved-searches file. So a member's number is part of both the
 * wire protocol and an on-disk format, and renumbering one silently
 * reinterprets the other.
 *
 * Note the gap: 3 belongs to EC_SEARCH_WEB, which has no counterpart here.
 * Appending BrowseSearch without saying so would have handed it that value and
 * made every browse report itself as a web search.
 */
enum SearchType
{
	LocalSearch = 0,
	GlobalSearch = 1,
	KadSearch = 2,
	// 3 is EC_SEARCH_WEB -- deliberately skipped, see above.
	//! A "View Files" browse of one peer's share. Shares the id space and
	//! the lifecycle machinery with real searches, but is started by
	//! EnsureBrowseTab rather than a query, and is the kind a remote GUI
	//! needs in order to rebuild the right sort of tab for a browse it did
	//! not start itself.
	BrowseSearch = 4
};

typedef std::vector<CSearchFile *> CSearchResultList;

class CSearchList : public wxEvtHandler, public CSearchResultIndex
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

	/**
	 * Allocates a fresh ed2k search ID from the single core counter shared by
	 * the monolithic GUI and the EC daemon path. Returns IDs in the range
	 * [1, 0x3fffffff] (never 0), provably disjoint from Kad's top-half IDs
	 * (>= 0x80000000) and from the remote GUI's optimistic placeholder tab IDs
	 * (0x40000000-0x7fffffff, see CSearchDlg::AllocateOptimisticId).
	 */
	uint32 AllocateEd2kId();

	/**
	 * Advances m_nextEd2kId past a restored ed2k search's persisted id
	 * (issue #641 Phase 3), so the next AllocateEd2kId() call this session
	 * can't reissue it -- m_nextEd2kId restarts at 0 every launch, so
	 * without this a restored search and the first new one after a
	 * restart collide deterministically. No-op if id is already at or
	 * below the current counter.
	 */
	void ReserveEd2kId(uint32_t id)
	{
		const uint32_t masked = id & 0x3fffffff;
		if (masked > m_nextEd2kId) {
			m_nextEd2kId = masked;
		}
	}

	/** True if the given searchID corresponds to an active Kad search. */
	bool IsKadSearch(uint32_t searchID) const;

	/** True if the given searchID is a Kad search, active or already finished. */
	bool IsOrWasKadSearch(uint32_t searchID) const;

	/**
	 * Returns the query string this search was started with, or an empty
	 * string if searchID is unknown. Used to label a search enumerated via
	 * EC_OP_SEARCH_LIST for a client that never started it locally.
	 */
	wxString GetSearchStringById(uint32_t searchID) const;

	/**
	 * True if this core currently routes results/progress for searchID --
	 * either a CSearchList search (m_searchStrings, populated in
	 * StartNewSearch, pruned in RemoveResults) or a "View Files" browse tab
	 * (m_browseBar; browses are not CSearchList searches but share the same
	 * per-ID result-routing and EC lifecycle -- got3nks, PR #680 review).
	 * Use this to gate per-ID EC replies (SEARCH_PROGRESS, single-ID
	 * SEARCH_RESULTS, Stop, Request_More) instead of the EC-only
	 * s_ecSearches registry: a monolithic-started search (or a browse) is
	 * known here but was never Register()'d into that registry, so gating
	 * on it alone reports a live search as EC_TAG_SEARCH_EXPIRED.
	 */
	bool IsKnownSearchId(uint32_t searchID) const;

	/**
	 * Every search ID this core currently knows a name for -- populated in
	 * StartNewSearch and RegisterBrowseSearch, pruned in RemoveResults, so
	 * this covers a search regardless of how it was started (monolithic GUI
	 * or an EC client) and is not bounded by any EC-connection-specific
	 * registry.
	 *
	 * Browses are in here: PR #914 registers one under its peer's name so a
	 * remote GUI listing the core's searches can rebuild it as a browse tab
	 * rather than a nameless search one. PR #680's narrower rule -- that a
	 * browse must never surface on EC_OP_SEARCH_LIST -- was superseded by
	 * that, so this map is no longer searches-only.
	 *
	 * Returned by reference -- called once per explicit EC_OP_SEARCH_LIST
	 * request, but a copy is still needless. Neither caller needs ownership.
	 */
	const std::map<uint32_t, wxString> &GetKnownSearchIds() const { return m_searchStrings; }

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
	// "View Files" browse tabs are not CSearchList searches: CBrowseManager
	// owns their bar, and GetSearchBarStatusById consults it first, so the
	// monolithic bar and the EC PROGRESS reply render the same value.
	// The browse lifecycle (EBrowseStatus: browsing / finished / failed) recorded
	// Echoes m_searchType for the current/last search; meaningful only
	// when state is RUNNING or FINISHED. Returns LocalSearch by default.
	SearchType GetSearchLifecycleKind() const { return m_searchType; }
	// Per-id search kind: the type recorded for THIS search when it started,
	// so the EC PROGRESS reply reports the polled tab's kind rather than the
	// scalar (most-recently-started) one — a remote GUI running several
	// searches needs each tab's real kind (e.g. to enable the Kad-only "More"
	// button). Kad is authoritative via IsOrWasKadSearch even if the recorded
	// entry was pruned; unknown ids fall back to the scalar.
	SearchType GetSearchLifecycleKindById(wxUIntPtr searchID) const;
	/**
	 * Records a browse under @a searchID so it appears in the search list
	 * like any other entry.
	 *
	 * A browse gets a real id from the same space as searches, but nothing
	 * described it: with no entry here its kind fell back to the scalar and
	 * it had no name at all, so a remote GUI listing the daemon's searches
	 * saw a nameless one it could only rebuild as an ordinary search tab.
	 *
	 * Writes all three of the per-id maps, which are maintained as a triple
	 * -- Save() walks m_searchStrings and reaches for the other two with
	 * .at(), so a partial entry turns saving into an exception rather than a
	 * missing field.
	 */
	void RegisterBrowseSearch(uint32 searchID, const wxString &peerName, uint32 peerEcid);
	//! ECID of the peer a browse id is listing, 0 if not a browse. Reported
	//! so a remote GUI can build a browse tab that is actually tied to its
	//! peer -- a tab with no ecid is not a browse at all (IsBrowse()).
	uint32 GetBrowsePeerEcid(uint32 searchID) const;
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

	// GetSearchResults() is inherited from CSearchResultIndex, which holds the
	// per-search result index this class fills through IndexResult().

	/** Removes all results for the specified search. */
	void RemoveResults(wxUIntPtr searchID);

	/**
	 * Persists every currently-held search (query string, kind, start time,
	 * and its full result tree) to StoredSearches.met, so results survive a
	 * restart (issue #641 Phase 3). Called from OnExit() before this object
	 * is destroyed -- the destructor drains everything via RemoveResults(),
	 * so this must run first. "View Files" browse tabs are not searches (see
	 * GetKnownSearchIds()) and are never persisted. Bounded at
	 * MAX_STORED_SEARCHES / MAX_STORED_RESULTS_PER_SEARCH; anything beyond
	 * that is dropped with a log line rather than silently.
	 */
	void StoreSearches() const;

	/**
	 * Reloads whatever StoreSearches() last wrote, reconstructing each
	 * search's query string/kind/start time and its full result tree
	 * (SetDownloadStatus() is re-run on every restored result). A restored
	 * Kad search is recorded as already finished, since the original
	 * in-flight Kad search object cannot survive a restart. Must run after
	 * theApp->downloadqueue/knownfiles/canceledfiles exist (see amule.cpp).
	 *
	 * A malformed file (bad header, or a search/result count exceeding the
	 * write-time caps) is treated as fatal for the whole load, matching
	 * CSearchFile::LoadFromFile()'s own fail-closed contract one level up --
	 * a corrupt file yields zero restored searches rather than a partial,
	 * unpredictable set.
	 *
	 * @return The IDs of every search actually restored, in file order, so
	 *         callers can register them with clients that discover searches
	 *         out-of-band (see RegisterRestoredSearch() in ExternalConn.h).
	 */
	std::vector<uint32_t> LoadSearches();

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
	//! On-disk name of the search-results persistence file, in the config dir.
	static const wxChar *const s_storedSearchesFilename;

	//! Ceiling on how many searches StoreSearches() writes / LoadSearches()
	//! accepts. Matches kMaxEcSearches (ExternalConn.cpp) for consistency,
	//! though m_searchStrings itself isn't EC-bounded -- a purely local,
	//! monolithic-only set of open tabs could exceed it. The oldest (by
	//! m_searchStartTimes) are dropped first, with a log line; never silent.
	static const std::size_t MAX_STORED_SEARCHES = 20;

	//! Ceiling on how many results StoreSearches() writes / LoadSearches()
	//! accepts per search. No existing loader (known.met, server.met) bounds
	//! its record count, so this is a fresh, deliberately generous number --
	//! not one mirrored from elsewhere. Unlike known.met (describes local
	//! files), a search result describes an arbitrary remote peer's claims,
	//! so the read side must fail closed on a record claiming more than this
	//! rather than attempt an unbounded allocation.
	static const std::size_t MAX_STORED_RESULTS_PER_SEARCH = 5000;

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

	//! Monotonic counter for ed2k search IDs, shared by the monolithic GUI and
	//! the EC daemon path. Its range [1, 0x3fffffff] keeps IDs provably disjoint
	//! from Kad's top-half IDs and the remote GUI's optimistic placeholder range.
	uint32 m_nextEd2kId = 0;

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

	//! Per-search kind (multi-search), recorded at start so the EC PROGRESS
	//! reply can report each polled tab's real type instead of the scalar
	//! m_searchType (which only tracks the most-recently-started search).
	//! Pruned in RemoveResults.
	std::map<uint32_t, SearchType> m_searchKinds;
	//! Peer ecid per browse id; see RegisterBrowseSearch().
	std::map<uint32_t, uint32> m_browsePeers;

	//! This search's original query string, keyed by id (same lifetime as
	//! m_searchKinds -- recorded in StartNewSearch, pruned in RemoveResults).
	//! Needed to label a search enumerated via EC_OP_SEARCH_LIST for a
	//! client that didn't start it locally and so has no tab-title string
	//! of its own to fall back on.
	std::map<uint32_t, wxString> m_searchStrings;

	//! Bar value for "View Files" browse tabs, keyed by routing ID and set by
	//! the browsing client (0..100 percent, or 0xffff finished/failed). Read by
	//! GetSearchBarStatusById. Pruned in RemoveResults.
	//! Browse lifecycle (EBrowseStatus) by search ID, kept in lockstep with
	//! m_browseBar so a browse's terminal state survives the client's teardown.
	//! Pruned in RemoveResults.

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

	//! Set by the destructor before it drains m_results, so RemoveResults
	//! skips its MuleNotify::Search_Removed broadcast during teardown --
	//! the GUI is being dismantled around us and there is no tab left worth
	//! closing.
	bool m_shuttingDown;

	//! Queue of servers to ask when doing global searches.
	//! TODO: Replace with 'cookie' system.
	CQueueObserver<CServer *> m_serverQueue;

	// The map of search-results (ResultMap / m_results) lives in
	// CSearchResultIndex, shared with the remote search list.

	//! Contains the results type desired in the current search.
	//! If not empty, results of different types are filtered.
	wxString m_resultType;

	wxDECLARE_EVENT_TABLE();
};

#endif // SEARCHLIST_H
// File_checked_for_headers
