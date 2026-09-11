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

#ifndef SEARCHLISTCTRL_H
#define SEARCHLISTCTRL_H

#include <wx/colour.h>        // Needed for wxColour
#include "MuleDataViewCtrl.h" // Needed for CMuleDataViewCtrl
#include <wx/regex.h>         // Needed for wxRegExp

#include "ListColumnStore.h" // Needed for CListColumnStore, IColumnWidthProvider
#include "MD4Hash.h"         // Needed for CMD4Hash (known-files filter set)
#include "Types.h"           // Needed for uint32

#include <list>
#include <set> // Needed for std::set (known-files filter set)
#include <utility>
#include <vector>

class CSearchList;
class CSearchFile;
class CSearchListModel;

/**
 * Displays search results.
 *
 * Rows are coloured by source count and other parameters (see CSearchListModel::GetAttr). Call
 * ShowResults() first to display the results of a given id; only then do AddResult() and
 * UpdateResult() work.
 *
 * Backed by a wxDataViewCtrl (native tree control) rather than a wxListCtrl, for screen-reader
 * accessibility (#180). Parent/child grouping (same file from several sources) is exposed through
 * CSearchListModel's native tree interface, driven straight from
 * CSearchFile::GetParent()/GetChildren(); expand/collapse state belongs to the control
 * (IsExpanded()), unlike the old hand-drawn tree that faked it with CSearchFile::ShowChildren()
 * plus manual row insertion.
 */
class CSearchListCtrl : public CMuleDataViewCtrl
{
public:
	CSearchListCtrl(wxWindow *parent,
		wxWindowID winid = -1,
		const wxPoint &pos = wxDefaultPosition,
		const wxSize &size = wxDefaultSize,
		const wxString &name = "searchlistctrl");

	virtual ~CSearchListCtrl();

	/// Adds @a toshow to the list. No duplicate checking, so it must be a new result.
	void AddResult(CSearchFile *toshow);

	/// Updates @a toupdate in the list.
	void UpdateResult(CSearchFile *toupdate);

	/// Clears the list and shows the results with ID @a ResultsId; zero just resets it.
	void ShowResults(wxUIntPtr ResultsId);

	wxUIntPtr GetSearchId() const { return m_nResultsID; }

	/// Re-key this control's search ID: the multi-search remote GUI remaps an optimistically-
	/// created tab from its local ID to the daemon-allocated one once the START reply arrives.
	void SetSearchId(wxUIntPtr id) { m_nResultsID = id; }

	/// "View Files" (browse) tabs are keyed by the browsed peer's ECID, stable across a re-
	/// browse and across a search-ID rekey, so a second browse of the same peer refreshes this
	/// tab instead of opening a duplicate. 0 for an ordinary search tab.
	uint32 GetBrowseEcid() const { return m_browseEcid; }
	/**
	 * Marks this tab as browsing @a ecid, and swaps in the model that groups results by the
	 * folders the peer reported.
	 *
	 * The model is chosen here, not in the constructor, because that is where the answer first
	 * exists: CSearchDlg::EnsureBrowseTab creates an ordinary tab and only then says who it is
	 * browsing, with no results yet to migrate. Defined in the .cpp: the swap needs
	 * CBrowseListModel to be complete.
	 */
	void SetBrowseEcid(uint32 ecid);
	bool IsBrowse() const { return m_browseEcid != 0; }

	// Peer display name and lifecycle (EBrowseStatus) for a browse tab. Held here so
	// CSearchDlg::UpdateHitCount can recompose the tab label ("<peer> (N...)" receiving, "(N)"
	// done, "(failed)") on any result or status change without re-parsing it.
	const wxString &GetBrowseName() const { return m_browseName; }
	void SetBrowseName(const wxString &name) { m_browseName = name; }
	uint32 GetBrowseStatus() const { return m_browseStatus; }
	/// Records the browse state, and restarts the rebuild throttle when a browse begins. In the
	/// .cpp so this header need not pull in updownclient.h. See OnIdleHook().
	void SetBrowseStatus(uint32 status);

	/// Sets the filter deciding which results are shown: @a regExp against the filename (an
	/// invalid one shows everything), @a invert to reverse the test, @a filterKnown to drop
	/// files that are queued or already known.
	void SetFilter(const wxString &regExp, bool invert, bool filterKnown);

	/// Toggles filtering on and off.
	void EnableFiltering(bool enabled);

	/// Number of items hidden by the filter.
	size_t GetHiddenItemCount() const;

	/// Number of results shown: one per top-level hit that passes the filter.
	size_t GetItemCount() const;

	/**
	 * The selected result, or NULL if nothing is selected -- or if what is selected is not a
	 * result at all.
	 *
	 * Every item here is a wxDataViewItem holding a bare pointer, and ToFile() turns one back
	 * into a CSearchFile with an unchecked cast. A browse tab's tree also holds folder nodes,
	 * and a handler that casts one anyway reads whatever is at that address. These two
	 * accessors ask the model (IsFolder) first, so the question is answered in one place.
	 */
	CSearchFile *GetFocusedFile() const;

	/// Every selected item that is a result, skipping any that are not.
	/// @see GetFocusedFile
	std::vector<CSearchFile *> GetSelectedFiles() const;

	/// Number of selected rows.
	int GetSelectedItemCount() const;

	/// Downloads every selected item into @a category, or into the drop-down's selection if -1.
	void DownloadSelected(int category = -1);

	static wxString DetermineStatusPrintable(CSearchFile *toshow);

	/// True if @a file passes this list's own filter test -- its own filename and known status,
	/// NOT its children. CSearchListModel uses it to decide whether a row, or for a parent at
	/// least one child, belongs in the tree.
	bool PassesFilter(const CSearchFile *file) const;

	/**
	 * Whether a filter is in force, i.e. whether a row's visibility is a live function of its
	 * values rather than a constant.
	 *
	 * CSearchListModel asks before deciding how to report a change. With no filter every result
	 * is shown, so an arrival is purely an addition and an update keeps its row; with a filter,
	 * an update can make a row appear or disappear (m_filterKnown drops a result the moment its
	 * download status stops being NEW), which only a full re-evaluation of the tree catches.
	 */
	bool HasActiveFilter() const { return m_filterEnabled && m_filter.IsValid(); }

	/// True if @a file belongs in the tree: it passes the filter itself, or for a parent at
	/// least one child does, in which case the parent is shown as a container whether or not it
	/// is expanded -- expand state is display-only and owned by the control, unlike the old
	/// CSearchFile::ShowChildren().
	bool ShouldShow(const CSearchFile *file) const;

	/// Full multi-column comparison, walking this list's sort chain (primary column first, then
	/// the secondary/tertiary columns set by earlier clicks). Used by
	/// CSearchListModel::Compare().
	int CompareFiles(const CSearchFile *f1, const CSearchFile *f2) const;

protected:
	//! Single-column comparison, no chain and no direction. wxDataViewModel::Compare() is only
	//! ever asked to order true siblings, so grouping needs no parent recursion.
	int CompareFilesByColumn(
		const CSearchFile *f1, const CSearchFile *f2, unsigned column, bool alt, int modifier) const;

	// --- CMuleDataViewCtrl hooks ---
	int CompareByColumn(const wxDataViewItem &item1,
		const wxDataViewItem &item2,
		unsigned column,
		bool alt,
		int modifier) const override;
	bool AltSortAllowed(unsigned column) const override;
	void GetDisplayOrder(wxDataViewItemArray &ordered) const override;
	wxString GetRowLabel(const wxDataViewItem &item) const override;
	wxString GetOldColumnOrder() const override;
	void OnIdleHook() override;
	void OnColumnWidthsChanged() override;
	void OnSortingChanged() override;

	/// True if the filename passes the filter and the row should be shown. @see PassesFilter(),
	/// the public wrapper the model uses.
	bool IsFiltered(const CSearchFile *file) const;

	/**
	 * Syncs @a dst's sort column, sort direction and column widths from @a src, resorting @a
	 * dst if the sort changed. Keeps every results list acting as one while still allowing
	 * individual selection.
	 */
	static void SyncLists(CSearchListCtrl *src, CSearchListCtrl *dst);

	/// Syncs every other list against @a src.
	static void SyncOtherLists(CSearchListCtrl *src);

	//! This list contains pointers to all current instances of CSearchListCtrl.
	static std::list<CSearchListCtrl *> s_lists;

	//! The ID of the search-results which the list is displaying or zero if unset.
	wxUIntPtr m_nResultsID;

	//! ECID of the browsed peer for a "View Files" tab, or 0 for a search tab.
	uint32 m_browseEcid;

	//! The folder the context menu was opened on, if any. A right-click does not select the row
	//! on every platform, so the handlers cannot ask for the selection.
	wxDataViewItem m_contextFolder;
	//! Peer display name and lifecycle (EBrowseStatus) for a browse tab.
	wxString m_browseName;
	uint32 m_browseStatus;
	//! Rows at the last rebuild while a browse was streaming; see OnIdleHook().
	unsigned m_lastRebuildRows = 0;

	//! The current filter reg-exp.
	wxRegEx m_filter;
	//! The text from which the filter is compiled.
	wxString m_filterText;
	//! Controls if shared/queued results should be shown.
	bool m_filterKnown;
	//! Controls if the result of filter-hits should be inverted.
	bool m_invert;
	//! Specifies if filtering should be used.
	bool m_filterEnabled;

	/**
	 * Results the user queued from this list, exempt from "Hide Known Files" until the filter
	 * is applied again.
	 *
	 * Queueing a result makes it known, and every result update resets the whole model (see
	 * CSearchListModel::MarkDirty), so a plain status test re-runs immediately and the row
	 * vanishes from under the click that queued it -- before the colour confirming the download
	 * is ever visible, and leaving nothing on screen to say which results were taken (#756).
	 * Listing them here holds those rows in place, in their queued colour, until SetFilter() or
	 * ShowResults() clears the set.
	 *
	 * Keyed on the user's action rather than on when a result became known: in amulegui every
	 * result is constructed NEW and learns its real status from a later poll
	 * (CSearchListRem::ProcessItemUpdate), so "was it known when it arrived" is not a question
	 * the remote GUI can answer.
	 */
	std::set<CMD4Hash> m_userQueued;

	void OnIdle(wxIdleEvent &event);

	CSearchListModel *m_model;

	void OnRightClick(wxDataViewEvent &event);
	void OnItemActivated(wxDataViewEvent &event);

	void OnPopupGetUrl(wxCommandEvent &event);
	void OnRazorStatsCheck(wxCommandEvent &event);
	void OnRelatedSearch(wxCommandEvent &event);
	void OnGetComments(wxCommandEvent &event);
	//! Folders the expand/collapse entries act on: the whole selection when the
	//! clicked row belongs to it, otherwise just that row.
	std::vector<wxDataViewItem> ContextFolders();
	void OnExpandAll(wxCommandEvent &event);
	void OnCollapseAll(wxCommandEvent &event);

	/**
	 * Expands or collapses @a item and everything under it. Only folders are containers worth
	 * walking; a grouped result's children are alternative sources, and opening those wholesale
	 * is not what "expand all" on a folder means.
	 */
	void SetSubtreeExpanded(const wxDataViewItem &item, bool expand);
	void OnPopupDownload(wxCommandEvent &event);

public:

private:
	//! Top-level rows in the order they are displayed under the current sort.
	void BuildDisplayOrder(std::vector<CSearchFile *> &ordered) const;

	wxDECLARE_EVENT_TABLE();
};

#endif // SEARCHLISTCTRL_H
// File_checked_for_headers
