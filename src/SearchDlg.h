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

#ifndef SEARCHDLG_H
#define SEARCHDLG_H

#include <wx/panel.h>    // Needed for wxPanel
#include <wx/notebook.h> // needed for wxBookCtrlEvent in wx 2.8

class wxStaticLine;

#include "Types.h" // Needed for uint16 and uint32

#include <map> // Needed for std::map (per-tab progress cache)

class CMuleNotebook;
class CSearchListCtrl;
class CMuleNotebookEvent;
class wxListEvent;
class wxSpinEvent;
class wxGauge;
class wxContextMenuEvent;
class wxButton;
class wxTextEntry;
class CSearchFile;

/**
 * This class represents the Search Dialog, which takes care of
 * enabling the user to search and to display results in a readable
 * manner.
 */
class CSearchDlg : public wxPanel
{
public:
	/**
	 * Constructor.
	 *
	 * @param pParent The parent widget passed to the wxPanel constructor.
	 */
	CSearchDlg(wxWindow *pParent);

	/**
	 * Destructor.
	 */
	~CSearchDlg();

	/**
	 * Adds the provided result to the right result-list.
	 *
	 * Please note that there is no duplicates checking, so the files should
	 * indeed be a new result.
	 */
	void AddResult(CSearchFile *toadd);

	/**
	 * Updates a changed result.
	 *
	 * @param A pointer to the updated CSearchFile.
	 *
	 * This function will update the source-count and color of the result, and
	 * if needed, it will also move the result so that the current sorting
	 * is maintained.
	 */
	void UpdateResult(CSearchFile *toupdate);

	/**
	 * Checks if a result-page with the specified heading exists.
	 *
	 * @param searchString The heading to look for.
	 */
	bool CheckTabNameExists(const wxString &searchString);

	/**
	 * Creates a new tab and displays the specified results.
	 *
	 * @param searchString This will be the heading of the new page.
	 * @param nSearchID The results with this searchId will be displayed.
	 * @param select Whether to bring the new tab to the front. True for a tab
	 *   the local user just asked for (a search they started, a peer they
	 *   chose to browse). False for one that appears on its own -- a search
	 *   discovered from another client -- which must not steal the selection
	 *   from whatever the user is currently looking at, possibly mid-typing
	 *   (got3nks, amule-org/amule#703).
	 */
	void CreateNewTab(const wxString &searchString, wxUIntPtr nSearchID, bool select = true);

	/**
	 * Call this function to signify that the local search is over.
	 */
	void LocalSearchEnd();

	/**
	 * Call this function to signify that the kad search is over.
	 */
	void KadSearchEnd(uint32 id);

	/**
	 * This function updates the category list according to existing categories.
	 */
	void UpdateCatChoice();

	/**
	 * This function displays the the hit-count in the heading for the specified page.
	 *
	 * @param page The page to have its heading updated.
	 */
	void UpdateHitCount(CSearchListCtrl *page);

	/**
	 * Helper function which resets the controls.
	 */
	void ResetControls();

	// Event handler and helper function
	void OnBnClickedDownload(wxCommandEvent &ev);

	CSearchListCtrl *GetSearchList(wxUIntPtr id);

	// Multi-search (remote GUI): remap a tab's search ID from the optimistic
	// local ID to the daemon-allocated one once the START reply arrives.
	void RekeySearch(wxUIntPtr oldID, wxUIntPtr newID);

	// The core rejected something this dialog optimistically opened a tab for:
	// report the reason and undo the tab. Covers both a search start and a
	// "View Files" browse, since in amuleGUI both are optimistic and both come
	// back as EC_OP_FAILED carrying the same EC_TAG_SEARCH_REF (got3nks, PR
	// #680 review).
	//
	// The monolithic build calls it directly from OnBnClickedStart, which has
	// the error string in hand; amuleGUI reaches it from the EC_OP_FAILED
	// reply, since CSearchListRem::StartNewSearch returns "" unconditionally
	// and the rejection only arrives later over EC. Sharing the one path is
	// what keeps a rejected start looking the same in both builds.
	//
	// searchID is the optimistic tab id: amuleGUI has already created that tab
	// and needs it dropped, the monolithic build never created one and
	// CloseSearchTab no-ops there. The search-button reset is skipped for a
	// browse -- the user never pressed Search, so it would wrongly disable
	// Download/Stop for whichever search tab is visible.
	void OnStartRejected(wxUIntPtr searchID, const wxString &error);

	// The core started a search (MuleNotify::Search_Added). Creates a tab for
	// it unless this GUI already has one, or unless it is the local user's own
	// search still inside OnBnClickedStart -- that path creates its own tab,
	// selected, right after StartNewSearch returns, and would otherwise end up
	// with two (amule-org/amule#703).
	void OnSearchAdded(wxUIntPtr searchID, const wxString &name, uint32 kind);

	// This search's results are gone -- close its tab, since a tab left open
	// on a freed search can only mislead (in amuleGUI "Download" would
	// silently do nothing, the daemon's m_results no longer having the hash;
	// in the monolithic build the rows hold raw CSearchFile pointers that
	// have just been deleted). One entry point for both builds, since it is
	// one idea (got3nks, PR #680 review):
	//   - amuleGUI: EC_TAG_SEARCH_EXPIRED, i.e. another client closed the
	//     search or the daemon's LRU evicted it.
	//   - monolithic: MuleNotify::Search_Removed, fired by
	//     CSearchList::RemoveResults whenever a bucket is freed.
	// Goes through the normal DeletePage path so OnSearchClosing does the
	// actual cleanup in one place; m_expiringSearchID tells it to skip
	// StopSearchById, which for both callers would address a search the core
	// has already discarded. No-op if no tab matches id (e.g. it was never
	// opened here), or if OnSearchClosing is already on the stack -- which
	// is what stops the monolithic close path (OnSearchClosing ->
	// RemoveResults -> Search_Removed -> here) from recursing.
	void CloseSearchTab(wxUIntPtr searchID);

	// "View Files" (browse): find-or-create the tab for a peer's shared-file
	// listing, keyed by the peer's ECID so a re-browse refreshes the same tab.
	// searchID is the result-routing ID (daemon-allocated over EC, or the local
	// client pointer in the monolithic client); if the tab already exists its
	// routing ID is rekeyed to searchID. Shared by monolithic and amuleGUI.
	void EnsureBrowseTab(uint32 peerEcid, const wxString &userName, wxUIntPtr searchID);

	// "View Files": if a browse tab for this peer's ECID is already open, bring
	// the Search panel forward, select that tab, and return true. Lets the
	// request sites skip re-browsing a peer whose listing is still on screen --
	// which would otherwise fire a redundant request and duplicate the results
	// in the existing tab. Returns false (and does nothing) for ecid 0 or when
	// no such tab is open. Shared by monolithic and amuleGUI.
	bool ActivateBrowseTabIfOpen(uint32 peerEcid);

	// Remote GUI: allocate a fresh optimistic placeholder tab ID in the reserved
	// high sub-range (bit 30, bottom half) the daemon's allocators never produce.
	wxUIntPtr AllocateOptimisticId();

	// "View Files": update a browse tab's lifecycle marker (EBrowseStatus),
	// keyed by the tab's result-routing search ID (the only key both sides agree
	// on across EC — the daemon's client ECID differs from the GUI's). No-op if
	// the tab isn't an open browse tab.
	void SetBrowseStatus(wxUIntPtr searchID, uint32 status);

	// Search ID of the currently visible tab (0 if none). The single bottom
	// progress bar tracks this tab.
	wxUIntPtr GetVisibleSearchId();

	// Multi-search (remote GUI): apply a per-search progress update. Clears the
	// tab's "!" marker on completion and drives the bottom bar for the visible
	// tab only, so each search's lifecycle is tracked independently.
	void UpdateSearchProgress(uint32 searchID, uint32 status);

	void UpdateProgress(uint32 new_value);

#ifndef CLIENT_GUI
	// Monolithic: drive the bottom bar from the visible tab's core search
	// lifecycle (CSearchList::GetSearchBarStatusById), so the bar follows tab
	// switches and shows the right search's progress — the local-core analogue
	// of the remote GUI's per-search EC progress cache.
	void RefreshVisibleTabProgress();
#endif

	void StartNewSearch();

	void FixSearchTypes();

	// Current ID_SEARCHTYPE selection normalised to a stable code
	// (0 = Local, 1 = Global, 2 = Kad) independent of which networks are
	// enabled, so it can be persisted and restored across restarts.
	// Returns wxNOT_FOUND if nothing is selected.
	int GetSelectedSearchTypeCanonical();

private:
	// Event handlers
	void OnFieldChanged(wxEvent &evt);

	// Search *query* history: past search terms (not their results -- see
	// #641 for that separate, not-yet-implemented follow-up) persisted to
	// a dedicated searchhistory.dat and shown in the IDC_SEARCHNAME combo
	// box's dropdown.
	void LoadSearchHistory();
	void RecordSearchHistory(const wxString &term);
	void ClearSearchHistory();
	// Asks first, then clears. Used by every user-facing route so neither the
	// button nor the context menu can wipe the history without a prompt.
	void ConfirmAndClearSearchHistory();
	void OnSearchNameContextMenu(wxContextMenuEvent &evt);

public:
	// Brings the whole search-history UI in line with the "Remember search
	// history" preference: with it off the Name field is a plain wxTextCtrl
	// (no dropdown), the Clear button is hidden and no stored terms are
	// loaded. searchhistory.dat is deliberately left on disk, so re-enabling
	// the preference restores the previous history rather than starting over.
	// Called at construction and live from PrefsUnifiedDlg::OnOk when the
	// checkbox changes, so no restart is needed (issue #697).
	void ApplySearchHistoryPref();

private:
	// Replaces the Name field in its sizer slot with the control type the
	// preference calls for, carrying the typed value across. Returns the
	// control now in place. A no-op when the right type is already there.
	wxTextEntry *RebuildSearchNameField(bool wantHistory);

	// Whether the combo currently in the Name slot has had the context-menu
	// handler attached. The handler is bound per instance, and the combo built
	// by muuli_wdr is one this class never created, so binding cannot be left
	// to the creation path alone -- that is what made the "Clear search
	// history" item missing until the preference was toggled.
	bool m_searchNameCtxBound = false;

	// Clear-history button, inserted into the action row after "Reset Fields"
	// at construction, together with the divider that precedes it. Both are
	// held rather than looked up by id so the pref-driven show/hide stays
	// cheap and cannot silently miss one of them.
	wxButton *m_clearHistoryBtn = nullptr;
	wxStaticLine *m_clearHistorySep = nullptr;
	void OnBnClickedClearHistory(wxCommandEvent &evt);

	// Greys the button out when there is nothing stored to clear, mirroring
	// what the context-menu item already does with its own Enable() call.
	void UpdateClearHistoryButton();

	// Persists the chosen search type so it survives a restart (amule-org/amule#608).
	void OnSearchTypeChanged(wxCommandEvent &evt);

	void OnListItemSelected(wxListEvent &ev);
	void OnBnClickedReset(wxCommandEvent &ev);
	void OnBnClickedClear(wxCommandEvent &ev);
	void OnExtendedSearchChange(wxCommandEvent &ev);
	void OnFilterCheckChange(wxCommandEvent &ev);
	void OnFilteringChange(wxCommandEvent &ev);
	void OnFilterReset(wxCommandEvent &ev);

	// Reads the three filter controls and pushes the result to every open
	// result page. Shared by OnFilteringChange and OnFilterReset so the reset
	// path applies through exactly the same code as a manual filter change.
	void ApplyFilter();

	void OnSearchClosing(wxBookCtrlEvent &evt);

	void OnBnClickedStart(wxCommandEvent &evt);
	void OnBnClickedStop(wxCommandEvent &evt);
	void OnBnClickedSearchMore(wxCommandEvent &evt);

	/**
	 * Event-handler for page-chages which takes care of enabling/disabling the download button.
	 */
	void OnSearchPageChanged(wxBookCtrlEvent &evt);

	// Multi-search (remote GUI): last progress status per search ID, so the
	// bottom bar can be refreshed instantly when the visible tab changes
	// (rather than waiting for the next poll). Empty on monolithic.
	std::map<wxUIntPtr, uint32> m_searchProgress;

	// Set by CloseSearchTab immediately before DeletePage(), which fires
	// PAGE_CLOSING synchronously and re-enters OnSearchClosing on the same
	// call stack. Tells that handler to skip StopSearchById for this one
	// id -- the core has already discarded it -- while still running its
	// other cleanup (ShowResults, m_searchProgress.erase, RemoveResults,
	// last-tab button disabling), so that cleanup exists in exactly one
	// place (got3nks, PR #680 review). 0 the rest of the time.
	wxUIntPtr m_expiringSearchID;

	// True while OnBnClickedStart is inside StartNewSearch. That call fires
	// MuleNotify::Search_Added before it returns, and this dialog creates the
	// tab for its own search only afterwards -- so without this the local
	// user's every search would get two tabs (amule-org/amule#703).
	bool m_startingLocalSearch;

	// True while OnSearchClosing is on the stack. In the monolithic build
	// that handler calls CSearchList::RemoveResults, which now fires
	// MuleNotify::Search_Removed -> CloseSearchTab for the very tab being
	// closed; without this the pair would recurse (and double-delete the
	// page). Set/cleared by a scoped guard in OnSearchClosing.
	bool m_inSearchClosing;

	// Set the bottom progress bar from a per-search status sentinel: a finished
	// search (0xffff/0xfffe) resets it, otherwise it shows the running percent.
	void ApplyProgressToBar(uint32 status);

	// Find an open "View Files" tab by the browsed peer's ECID (NULL if none).
	// When found and outPage is non-null, outPage receives the tab's page index.
	CSearchListCtrl *GetBrowseList(uint32 ecid, int *outPage = nullptr);

	// Monotonic counter behind search/browse tab IDs. On the remote GUI both a
	// new search and a browse draw their optimistic placeholder from it via
	// AllocateOptimisticId, so their placeholders never collide before the daemon
	// rekeys them; the monolithic build reuses the same counter directly for its
	// (non-remapped) search IDs.
	static uint32 s_optimisticIdCounter;

	uint64 m_last_search_time;

	wxGauge *m_progressbar;

	CMuleNotebook *m_notebook;

	wxArrayString m_searchchoices;

	wxDECLARE_EVENT_TABLE();
};

#endif
// File_checked_for_headers
