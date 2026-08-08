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
 * This class is used to display search results.
 *
 * Results added to the list are colored according to the number of sources
 * and other parameters (see CSearchListModel::GetAttr).
 *
 * To display results, first use the ShowResults function, which will display
 * all current results with the specified id and afterwards you can use the
 * AddResult function to add new results or the UpdateResult function to update
 * already present results. Please note that it is not possible to add results
 * with the AddResult function before calling ShowResults.
 *
 * Backed by a wxDataViewCtrl (native tree control) rather than a wxListCtrl,
 * for screen-reader accessibility (#180). Parent/child grouping (same file
 * from multiple sources/variants) is exposed via CSearchListModel's native
 * tree interface -- CSearchFile::GetParent()/GetChildren() drives it
 * directly, and expand/collapse state is owned by the control itself
 * (IsExpanded()), unlike the old hand-drawn tree which faked it with
 * CSearchFile::ShowChildren()/SetShowChildren() plus manual row insertion.
 */
class CSearchListCtrl : public CMuleDataViewCtrl
{
public:
	/**
	 * Constructor.
	 */
	CSearchListCtrl(wxWindow *parent,
		wxWindowID winid = -1,
		const wxPoint &pos = wxDefaultPosition,
		const wxSize &size = wxDefaultSize,
		const wxString &name = "searchlistctrl");

	/**
	 * Destructor.
	 */
	virtual ~CSearchListCtrl();

	/**
	 * Adds the specified file to the list.
	 *
	 * @param toshow The new result to be shown.
	 *
	 * Please note that no duplicates checking is done, so the pointer should
	 * point to a new file in order to avoid problems.
	 */
	void AddResult(CSearchFile *toshow);

	/**
	 * Updates the specified source.
	 *
	 * @param toupdate The search result to be updated.
	 */
	void UpdateResult(CSearchFile *toupdate);

	/**
	 * Clears the list and inserts all results with the specified Id instead.
	 *
	 * @param ResultsId The ID of the results or Zero to simply reset the list.
	 */
	void ShowResults(wxUIntPtr ResultsId);

	/**
	 * Returns the current Search Id.
	 */
	wxUIntPtr GetSearchId() const { return m_nResultsID; }

	/**
	 * Re-key this control's search ID. Used by the multi-search remote GUI to
	 * remap an optimistically-created tab from its local ID to the
	 * daemon-allocated one once the START reply arrives.
	 */
	void SetSearchId(wxUIntPtr id) { m_nResultsID = id; }

	/**
	 * "View Files" (browse) tabs are keyed by the browsed peer's ECID (stable
	 * across a re-browse and across a search-ID rekey), so a second browse of
	 * the same peer refreshes this tab instead of opening a duplicate. 0 for an
	 * ordinary search tab.
	 */
	uint32 GetBrowseEcid() const { return m_browseEcid; }
	void SetBrowseEcid(uint32 ecid) { m_browseEcid = ecid; }
	bool IsBrowse() const { return m_browseEcid != 0; }

	// Peer display name and lifecycle (EBrowseStatus) for a browse tab. Held on
	// the control so CSearchDlg::UpdateHitCount can recompose the tab label
	// ("<peer> (N…)" while receiving, "(N)" done, "(failed)") on every result
	// or status change without re-parsing the label text.
	const wxString &GetBrowseName() const { return m_browseName; }
	void SetBrowseName(const wxString &name) { m_browseName = name; }
	uint32 GetBrowseStatus() const { return m_browseStatus; }
	void SetBrowseStatus(uint32 status) { m_browseStatus = status; }

	/**
	 * Sets the filter which decides which results should be shown.
	 *
	 * @param regExp A regular expression targeting the filenames.
	 * @param invert If true, invert the results of the filter-test.
	 * @param filterKnown Should files that are queued or known be filtered out.
	 *
	 * An invalid regExp will result in all results being displayed.
	 */
	void SetFilter(const wxString &regExp, bool invert, bool filterKnown);

	/**
	 * Toggles the use of filtering on and off.
	 */
	void EnableFiltering(bool enabled);

	/**
	 * Returns the number of items hidden due to filtering.
	 */
	size_t GetHiddenItemCount() const;

	/**
	 * Returns the number of results currently shown (root results plus
	 * children of expanded parents) -- equivalent to the old
	 * wxListCtrl::GetItemCount() this tab used to report in its title.
	 */
	size_t GetItemCount() const;

	/**
	 * Returns the number of currently selected rows.
	 */
	int GetSelectedItemCount() const;

	/**
	 * Attempts to download all selected items.
	 *
	 * @param category The target category, or -1 to use the drop-down selection.
	 */
	void DownloadSelected(int category = -1);

	static wxString DetermineStatusPrintable(CSearchFile *toshow);

	/**
	 * True if `file` currently passes this list's own filter test (own
	 * filename/known-status match -- does NOT consider children). Used by
	 * CSearchListModel to decide whether a row (or, for a parent, at least
	 * one of its children) should be exposed in the tree.
	 */
	bool PassesFilter(const CSearchFile *file) const;

	/**
	 * Whether a filter is in force, i.e. whether a row's visibility is a
	 * live function of its values rather than a constant.
	 *
	 * CSearchListModel asks before deciding how to report a change. With no
	 * filter every result is shown, so an arriving one is purely an addition
	 * and an updated one keeps its row; with a filter, an update can make a
	 * row have to appear or disappear (m_filterKnown drops a result the
	 * moment its download status stops being NEW), which only a full
	 * re-evaluation of the tree catches.
	 */
	bool HasActiveFilter() const { return m_filterEnabled && m_filter.IsValid(); }

	/**
	 * True if `file` should be exposed as a tree row: it passes the filter
	 * itself, or (for a parent) at least one of its children does -- in
	 * which case the parent is still shown as a container, regardless of
	 * whether it's currently expanded (expand state is a pure display
	 * concern now, owned by the control, so it no longer gates filtering
	 * the way CSearchFile::ShowChildren() used to).
	 */
	bool ShouldShow(const CSearchFile *file) const;

	/**
	 * Full multi-column comparison of two results, walking this list's sort
	 * chain (primary column first, falling back to secondary/tertiary
	 * columns set by earlier clicks) exactly as CMuleListCtrl's generic
	 * chain-walking used to. Used by CSearchListModel::Compare().
	 */
	int CompareFiles(const CSearchFile *f1, const CSearchFile *f2) const;

protected:
	//! Single-column comparison (no chain, no direction), mirroring the old
	//! CSearchListCtrl::SortProc switch minus the parent-recursion hack
	//! (wxDataViewModel::Compare() is only ever asked to order true
	//! siblings, so grouping is handled by the tree structure itself).
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

	/**
	 * Returns true if the filename is filtered (i.e. passes the filter and
	 * should be shown) -- see PassesFilter(), the public wrapper used by
	 * the model.
	 */
	bool IsFiltered(const CSearchFile *file) const;

	/**
	 * Helper function which syncs two lists.
	 *
	 * @param src The source list.
	 * @param dst The list to be synced with the source list.
	 *
	 * This function synchronises the following settings of two lists:
	 *  - Sort column
	 *  - Sort direction
	 *  - Column widths
	 *
	 * If either sort column or direction is changed, then the dst list will
	 * be resorted. This function is used to ensure that all results list act
	 * as one, while still allowing individual selection.
	 */
	static void SyncLists(CSearchListCtrl *src, CSearchListCtrl *dst);

	/**
	 * Helper function which syncs all other lists against the specified one.
	 */
	static void SyncOtherLists(CSearchListCtrl *src);

	//! This list contains pointers to all current instances of CSearchListCtrl.
	static std::list<CSearchListCtrl *> s_lists;

	//! The ID of the search-results which the list is displaying or zero if unset.
	wxUIntPtr m_nResultsID;

	//! ECID of the browsed peer for a "View Files" tab, or 0 for a search tab.
	uint32 m_browseEcid;
	//! Peer display name and lifecycle (EBrowseStatus) for a browse tab.
	wxString m_browseName;
	uint32 m_browseStatus;

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
	 * Results the user queued from this list, exempted from "Hide Known
	 * Files" until the filter is applied again.
	 *
	 * Queueing a result makes it known, and every result update resets the
	 * whole model (see CSearchListModel::MarkDirty), so a plain status test
	 * re-runs immediately and the row vanishes from under the click that
	 * queued it -- before the colour confirming the download was added is
	 * ever visible, and leaving nothing on screen to say which results were
	 * taken (#756). Keeping them listed here holds those rows in place, in
	 * their queued colour, until SetFilter() or ShowResults() clears the set.
	 *
	 * Deliberately keyed on the user's action rather than on when a result
	 * became known: in amulegui every result is constructed NEW and learns
	 * its real status from a later poll (CSearchListRem::ProcessItemUpdate),
	 * so "was it known when it arrived" is not a question the remote GUI can
	 * answer, and hiding already-known results has to stay a live test there.
	 */
	std::set<CMD4Hash> m_userQueued;

	//! Last-seen column widths, used to detect a user drag-resize (there is
	//! no portable wxDataViewCtrl "column resized" event to hook directly)
	//! so it can be propagated to the other open search tabs, same as the
	//! old EVT_LIST_COL_END_DRAG-driven sync.
	void OnIdle(wxIdleEvent &event);

	CSearchListModel *m_model;

	void OnRightClick(wxDataViewEvent &event);
	void OnItemActivated(wxDataViewEvent &event);
	void OnSelectionChanged(wxDataViewEvent &event);

	void OnPopupGetUrl(wxCommandEvent &event);
	void OnRazorStatsCheck(wxCommandEvent &event);
	void OnRelatedSearch(wxCommandEvent &event);
	void OnGetComments(wxCommandEvent &event);
	void OnPopupDownload(wxCommandEvent &event);

	/**
	 * Header right-click: the column show/hide menu every other list gets
	 * from CMuleListCtrl::OnColumnRClick(). Hiding calls SetHidden() and
	 * records it in m_columnHidden; the persisted form stays a width of
	 * zero, which CListColumnStore already understands.
	 */

	//! Keeps the group expander on the leftmost visible column.

	/**
	 * Columns the user owns, excluding the macOS spacer, which must stay
	 * out of the header menu, the persisted widths and the cross-tab sync.
	 */

public:

private:
	//! Top-level rows in the order they are displayed under the current sort.
	void BuildDisplayOrder(std::vector<CSearchFile *> &ordered) const;

	wxDECLARE_EVENT_TABLE();
};

#endif // SEARCHLISTCTRL_H
// File_checked_for_headers
