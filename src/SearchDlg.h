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

#include <wx/dataview.h> // Needed for wxDataViewEvent
#include <wx/notebook.h> // needed for wxBookCtrlEvent in wx 2.8
#include <wx/panel.h>    // Needed for wxPanel

class wxStaticLine;

#include "Types.h" // Needed for uint16 and uint32

#include <map>        // Needed for std::map (per-tab progress cache)
#include <wx/timer.h> // Needed for wxTimer (debounced live filtering)

#include <set> // Needed for std::set (pending hit-count recomputes)

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

/// The Search panel: lets the user search and shows the results.
class CSearchDlg : public wxPanel
{
public:
	CSearchDlg(wxWindow *pParent);

	~CSearchDlg();

	/**
	 * Adds the result to the right result-list. No duplicate checking, so the file must really
	 * be a new result.
	 */
	void AddResult(CSearchFile *toadd);

	/**
	 * Updates a changed result: its source count and colour, moving the row if needed to keep
	 * the current sorting.
	 */
	void UpdateResult(CSearchFile *toupdate);

	/**
	 * Is there a result-page headed `searchString`?
	 */
	bool CheckTabNameExists(const wxString &searchString);

	/**
	 * Creates a new tab headed `searchString` showing the results for `nSearchID`.
	 *
	 * @param select Bring the new tab to the front. True for a tab the local user just asked
	 * for. False for one that appears on its own -- a search discovered from another client --
	 * which must not steal the selection from whatever the user is looking at, possibly mid-
	 * typing (got3nks, amule-org/amule#703).
	 */
	void CreateNewTab(const wxString &searchString, wxUIntPtr nSearchID, bool select = true);

	/// The local search is over.
	void LocalSearchEnd();

	/// The kad search is over.
	void KadSearchEnd(uint32 id);

	/// Refreshes the category list from the existing categories.
	void UpdateCatChoice();

	/**
	 * Shows the hit-count in `page`'s heading.
	 */
	void UpdateHitCount(CSearchListCtrl *page);

	/// Resets the controls.
	void ResetControls();

	// Event handler and helper function
	void OnBnClickedDownload(wxCommandEvent &ev);

	CSearchListCtrl *GetSearchList(wxUIntPtr id);

	// Drop every open tab's rows, keeping the tabs themselves. For a reconnect to a RESTARTED
	// daemon: the stored searches come back under their original ids, so the tabs still
	// correspond to something, but every result behind them has been freed and a tab's rows are
	// raw CSearchFile pointers. Resetting the models makes them re-read the now-empty index.
	void ResetResultViews();

	// Multi-search (remote GUI): remap a tab's search ID from the optimistic
	// local ID to the daemon-allocated one once the START reply arrives.
	void RekeySearch(wxUIntPtr oldID, wxUIntPtr newID);

	// The core rejected something this dialog optimistically opened a tab for: report the
	// reason and undo the tab. Covers both a search start and a "View Files" browse, since in
	// amuleGUI both are optimistic and both come back as EC_OP_FAILED carrying the same
	// EC_TAG_SEARCH_REF.
	//
	// The monolithic build calls it from OnBnClickedStart, which has the error string in hand;
	// amuleGUI reaches it from the EC_OP_FAILED reply, since CSearchListRem::StartNewSearch
	// returns "" unconditionally and the rejection arrives later over EC.
	//
	// searchID is the optimistic tab id: amuleGUI has already created that tab and needs it
	// dropped, while CloseSearchTab no-ops in the monolithic build. The search-button reset is
	// skipped for a browse, where the user never pressed Search and it would wrongly disable
	// Download/Stop for the visible tab.
	void OnStartRejected(wxUIntPtr searchID, const wxString &error);

	// The core started a search (MuleNotify::Search_Added). Creates a tab unless this GUI
	// already has one, or unless it is the local user's own search still inside
	// OnBnClickedStart -- that path creates its own tab, selected, right after StartNewSearch
	// returns, and would otherwise end up with two (#703).
	void OnSearchAdded(wxUIntPtr searchID, const wxString &name, uint32 kind);

	// This search's results are gone, so close its tab: one left open on a freed search can only
	// mislead (in amuleGUI "Download" silently does nothing, the daemon's m_results no longer
	// having the hash; in the monolithic build the rows hold raw CSearchFile pointers that have
	// just been deleted). One entry point for both builds:
	//   - amuleGUI: EC_TAG_SEARCH_EXPIRED, another client closed the search or the daemon's LRU
	//     evicted it.
	//   - monolithic: MuleNotify::Search_Removed from CSearchList::RemoveResults.
	// Goes through the normal DeletePage path so OnSearchClosing does the cleanup in one place;
	// m_expiringSearchID tells it to skip StopSearchById, which for both callers would address a
	// search the core has already discarded. No-op if no tab matches, or if OnSearchClosing is
	// already on the stack -- which is what stops the monolithic close path from recursing.
	void CloseSearchTab(wxUIntPtr searchID);

	/**
	 * Finds or creates the "View Files" tab for a peer's shared-file listing, keyed by the
	 * peer's ECID so a re-browse refreshes the same tab. searchID is the result-routing ID
	 * (daemon-allocated over EC, or the local client pointer in the monolithic build); an
	 * existing tab is rekeyed to it.
	 *
	 * @param reveal Select the tab and bring the Search panel forward. True when the local user
	 * asked for this browse, which is the point of clicking View Files. False for one this
	 * client did not initiate -- another GUI's browse, or one already running when we attached
	 * -- which must not pull the panel or the selection away from what the user is doing (same
	 * rule as a discovered search, amule-org/amule#703).
	 */
	void EnsureBrowseTab(
		uint32 peerEcid, const wxString &userName, wxUIntPtr searchID, bool reveal = true);

	// "View Files": if a browse tab for this peer's ECID is open, bring the Search panel
	// forward, select that tab and return true. Lets callers skip re-browsing a peer whose
	// listing is still on screen, which would duplicate the results. False for ecid 0 or no
	// such tab.
	bool ActivateBrowseTabIfOpen(uint32 peerEcid);

	// Remote GUI: allocate a fresh optimistic placeholder tab ID in the reserved
	// high sub-range (bit 30, bottom half) the daemon's allocators never produce.
	wxUIntPtr AllocateOptimisticId();

	// "View Files": update a browse tab's lifecycle marker (EBrowseStatus), keyed by the tab's
	// result-routing search ID -- the only key both sides agree on across EC, the daemon's
	// client ECID differing from the GUI's. No-op if the tab isn't an open browse tab.
	void SetBrowseStatus(wxUIntPtr searchID, uint32 status);

	// Search ID of the currently visible tab (0 if none). The single bottom
	// progress bar tracks this tab.
	wxUIntPtr GetVisibleSearchId();

	// Multi-search (remote GUI): apply a per-search progress update. Clears the tab's "!"
	// marker on completion and drives the bottom bar for the visible tab only, so each search's
	// lifecycle is tracked independently.
	void UpdateSearchProgress(uint32 searchID, uint32 status);

	/**
	 * Is one of the open tabs an ed2k search that is still running?
	 *
	 * eD2k replies carry no search identifier -- neither the server's TCP reply nor the global
	 * UDP results -- so a client can only attribute incoming results to the query it sent most
	 * recently. Starting a second ed2k search therefore finalises the one in flight, and the
	 * user gets no say. This answers whether there is such a search to warn about.
	 *
	 * Kad tabs and "View Files" browse tabs are excluded: Kad results carry their own search ID
	 * so those genuinely run in parallel, and a browse is not an ed2k search at all.
	 *
	 * Where the running/finished answer comes from differs between the two builds, so it is
	 * resolved here and every caller asks the same question.
	 */
	bool HasRunningEd2kSearch() const;

	void UpdateProgress(uint32 new_value);

#ifndef CLIENT_GUI
	// Monolithic: drive the bottom bar from the visible tab's core search lifecycle so the bar
	// follows tab switches -- the local-core analogue of the remote GUI's per-search EC
	// progress cache.
	void RefreshVisibleTabProgress();
#endif

	void StartNewSearch();

	void FixSearchTypes();

	// Current ID_SEARCHTYPE selection normalised to a stable code (0 = Local, 1 = Global, 2 =
	// Kad) independent of which networks are enabled, so it can be persisted across restarts.
	// wxNOT_FOUND if nothing is selected.
	int GetSelectedSearchTypeCanonical();

private:
	// Event handlers
	void OnFieldChanged(wxEvent &evt);

	// Search QUERY history: past search terms, not their results, persisted to a
	// dedicated searchhistory.dat and shown in the IDC_SEARCHNAME combo dropdown.
	void LoadSearchHistory();
	void RecordSearchHistory(const wxString &term);
	void ClearSearchHistory();
	// Asks first, then clears. Used by every user-facing route so neither the
	// button nor the context menu can wipe the history without a prompt.
	void ConfirmAndClearSearchHistory();
	void OnSearchNameContextMenu(wxContextMenuEvent &evt);

public:
	// Brings the search-history UI in line with the "Remember search history" preference: with
	// it off the Name field is a plain wxTextCtrl, the Clear button is hidden and no stored
	// terms are loaded. searchhistory.dat is left on disk, so re-enabling restores it. Called
	// at construction and live from PrefsUnifiedDlg::OnOk, so no restart is needed.
	void ApplySearchHistoryPref();
	//! Show or hide the rule above the filter row, tracking the row itself.
	void ApplyFilterSeparator(bool shown);

private:
	// Replaces the Name field in its sizer slot with the control type the preference calls for,
	// carrying the typed value across. Returns the control now in place, and no-ops when the
	// right type is already there.
	wxTextEntry *RebuildSearchNameField(bool wantHistory);

	// Whether the combo currently in the Name slot has had the context-menu handler attached.
	// The handler is bound per instance and the combo built by muuli_wdr is one this class
	// never created, so binding cannot be left to the creation path alone -- which is what made
	// "Clear search history" missing until the preference was toggled.
	bool m_searchNameCtxBound = false;

	// Clear-history button, inserted into the action row after "Reset Fields" at construction
	// together with the divider before it. Both are held rather than looked up by id, so the
	// pref-driven show/hide cannot silently miss one.
	wxButton *m_clearHistoryBtn = nullptr;
	wxStaticLine *m_clearHistorySep = nullptr;
	void OnBnClickedClearHistory(wxCommandEvent &evt);

	// Greys the button out when there is nothing stored to clear, mirroring
	// what the context-menu item already does with its own Enable() call.
	void UpdateClearHistoryButton();

	// Persists the chosen search type so it survives a restart (amule-org/amule#608).
	void OnSearchTypeChanged(wxCommandEvent &evt);

	// Recomputes the Download button from the visible list's selection; see
	// the definition for why both callers go through it.
	void UpdateDownloadButtonState();
	void OnListItemSelected(wxDataViewEvent &ev);
	void OnBnClickedReset(wxCommandEvent &ev);
	void OnBnClickedClear(wxCommandEvent &ev);
	void OnExtendedSearchChange(wxCommandEvent &ev);
	void OnFilterCheckChange(wxCommandEvent &ev);
	void OnFilteringChange(wxCommandEvent &ev);
	void OnFilterReset(wxCommandEvent &ev);

	// Reads the three filter controls and pushes the result to every open result page. Shared
	// by OnFilteringChange and OnFilterReset so the reset path applies through exactly the same
	// code as a manual filter change.
	void ApplyFilter();

	void OnSearchClosing(wxBookCtrlEvent &evt);

	void OnBnClickedStart(wxCommandEvent &evt);
	void OnBnClickedStop(wxCommandEvent &evt);
	void OnBnClickedSearchMore(wxCommandEvent &evt);

	/// Page-change handler; enables or disables the download button.
	void OnSearchPageChanged(wxBookCtrlEvent &evt);

	// Multi-search (remote GUI): last progress status per search ID, so the bottom bar can be
	// refreshed instantly on a tab change rather than at the next poll. Empty on monolithic.
	std::map<wxUIntPtr, uint32> m_searchProgress;

	// Set by CloseSearchTab immediately before DeletePage(), which fires PAGE_CLOSING
	// synchronously and re-enters OnSearchClosing on the same stack. Tells that handler to skip
	// StopSearchById for this one id -- the core has already discarded it -- while still
	// running its other cleanup, so that cleanup lives in one place. 0 the rest of the time.
	wxUIntPtr m_expiringSearchID;

	// True while OnBnClickedStart is inside StartNewSearch. That call fires
	// MuleNotify::Search_Added before it returns and this dialog creates the tab for its own
	// search only afterwards, so without this every local search would get two tabs (#703).
	bool m_startingLocalSearch;

	// True while OnSearchClosing is on the stack. In the monolithic build that handler calls
	// CSearchList::RemoveResults, which fires Search_Removed -> CloseSearchTab for the very tab
	// being closed; without this the pair would recurse and double-delete the page.
	bool m_inSearchClosing;

	// Set the bottom progress bar from a per-search status sentinel: a finished
	// search (0xffff/0xfffe) resets it, otherwise it shows the running percent.
	void ApplyProgressToBar(uint32 status);

	// Find an open "View Files" tab by the browsed peer's ECID (NULL if none).
	// When found and outPage is non-null, outPage receives the tab's page index.
	CSearchListCtrl *GetBrowseList(uint32 ecid, int *outPage = nullptr);

	// Monotonic counter behind search/browse tab IDs. On the remote GUI both a new search and a
	// browse draw their optimistic placeholder from it via AllocateOptimisticId, so the
	// placeholders cannot collide before the daemon rekeys them; the monolithic build uses the
	// counter directly.
	static uint32 s_optimisticIdCounter;

	uint64 m_last_search_time;

	wxGauge *m_progressbar;

	CMuleNotebook *m_notebook;

	wxArrayString m_searchchoices;

	/**
	 * Tabs whose hit-count label needs recomputing, flushed once per idle.
	 *
	 * UpdateHitCount() walks the whole result list twice (GetItemCount() and
	 * GetHiddenItemCount(), each looping every result's children), so calling it per arriving
	 * result is quadratic over a search. Results arrive in bursts and only the final label of a
	 * burst is ever seen, so the work is coalesced here the same way the tree rebuild is in
	 * CSearchListCtrl::OnIdle. Entries are only compared against the notebook's live pages in
	 * OnIdle(), never dereferenced, so a tab closing before the flush is harmless.
	 */
	std::set<CSearchListCtrl *> m_pendingHitCount;

	// Kad searches whose "More" budget is spent, by searchID. The daemon reports this once per
	// press and it never un-spends, so it is remembered rather than re-asked: the button's
	// enabled state is recomputed on every tab switch and progress tick, and without this a
	// switch away and back would re-enable a dead control. Entries die with the tab, and re-
	// running a search yields a new searchID.
	std::set<uint32_t> m_moreExhausted;

	// Whether the "More" button should be live for this search: Kad-only, and not already
	// reported as un-widenable. Single home for a predicate five separate sites otherwise have
	// to keep in step.
	bool MoreAllowed(uint32_t searchID) const;

public:
	// The daemon says this search can no longer be widened. Called from the remote GUI's reply
	// handler (amuleGUI); the monolithic build reaches the same state straight from
	// RequestMoreResults' return value.
	void MarkMoreExhausted(uint32_t searchID);

private:
	void OnIdle(wxIdleEvent &evt);

	/**
	 * Live filtering, debounced.
	 *
	 * ApplyFilter() re-filters every open search tab, and each one re-runs the regex over its
	 * results and resets its model, so applying on every keystroke would do that work per
	 * character across all tabs. The text box therefore only restarts a short timer; the filter
	 * is applied once the typing pauses. Enter and the Filter button still apply immediately.
	 */
	void OnFilterTextChanged(wxCommandEvent &evt);
	void OnFilterDebounceTimer(wxTimerEvent &evt);

	wxTimer m_filterTimer;

	wxDECLARE_EVENT_TABLE();
};

#endif
// File_checked_for_headers
