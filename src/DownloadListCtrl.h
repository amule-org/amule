//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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

#ifndef DOWNLOADLISTCTRL_H
#define DOWNLOADLISTCTRL_H

#include "MuleVirtualDataViewCtrl.h" // Needed for CMuleVirtualDataViewCtrl

#include <set> // Needed for std::set

#define COLUMN_DL_PART 0
#define COLUMN_DL_NAME 1
#define COLUMN_DL_SIZE 2
#define COLUMN_DL_TRANSFERRED 3
#define COLUMN_DL_COMPLETED 4
#define COLUMN_DL_SPEED 5
#define COLUMN_DL_PROGRESS 6
#define COLUMN_DL_SOURCES 7
#define COLUMN_DL_PRIORITY 8
#define COLUMN_DL_STATUS 9
#define COLUMN_DL_TIMEREMAINING 10
#define COLUMN_DL_LASTSEENCOMPLETE 11
#define COLUMN_DL_LASTRECEPTION 12
//! Always empty. Absorbs the macOS trailing-column sizing; see
//! CMuleDataViewCtrl::AppendSpacerColumn().
#define COLUMN_DL_SPACER 13

class CPartFile;
class wxMenu;
class wxStaticText;

/**
 * This class is responsible for representing the download queue.
 *
 * The CDownloadListCtrl class is responsible for drawing files being
 * downloaded. It is in many ways the primary widget within the application,
 * since it is here that users can inspect and manipulate their current
 * downloads.
 *
 * Rows are addressed by CPartFile* identity, same as CSharedFilesCtrl's by
 * CKnownFile*. The Progress column is the one graphic cell (a CBarShader
 * chunk/gap bar plus a completed-progress overlay and percent text); see
 * CDownloadBarRenderer in the .cpp.
 */
class CDownloadListCtrl : public CMuleVirtualDataViewCtrl
{
public:
	/**
	 * Constructor.
	 *
	 * @see CMuleVirtualDataViewCtrl::CMuleVirtualDataViewCtrl for documentation of parameters.
	 */
	CDownloadListCtrl(wxWindow *parent,
		wxWindowID winid = wxID_ANY,
		const wxPoint &pos = wxDefaultPosition,
		const wxSize &size = wxDefaultSize,
		long style = 0,
		const wxString &name = "downloadlistctrl");

	/**
	 * Destructor.
	 */
	~CDownloadListCtrl();

	/**
	 * Adds a file to the list, but it wont show unless it matches the current category.
	 *
	 * @param file A valid pointer to a new partfile.
	 * @param deferView If true, only the internal model entry is created;
	 *                  the file is not shown and the list is not sorted.
	 *                  Used for a bulk load (remote GUI first sync), where
	 *                  the caller shows and sorts the whole list once
	 *                  afterwards via ShowFileList() instead of paying an
	 *                  O(n^2) per-item sort. See issue #414.
	 *
	 * Please note that duplicates wont be added.
	 */
	void AddFile(CPartFile *file, bool deferView = false);

	/**
	 * Shows every model item belonging to the current category and sorts
	 * the list a single time, wrapped in Freeze()/Thaw().
	 *
	 * Batch counterpart to AddFile()'s per-item path, used after a
	 * deferred bulk load so a large queue populates in one pass. Mirrors
	 * CSharedFilesCtrl::ShowFileList().
	 */
	void ShowFileList();

	// The live text filter (SetFilterText) is inherited from
	// CMuleVirtualDataViewCtrl; here it is AND-ed with the current category,
	// and the rebuild it triggers is RebuildFilteredView() below. Purely
	// GUI-side, so it behaves the same in the monolithic app and the remote
	// GUI.

	/**
	 * Bracket a burst of AddFile()/UpdateItem() calls (a reconnect resync —
	 * issue #444, or any poll that adds a batch of downloads — issue #615) so
	 * the list repaints once (Freeze) and sorts once, instead of per-item.
	 * Existing rows are updated in place; new rows are appended without a
	 * per-item sort. EndBatchUpdate() does the single SortList(), unless
	 * doSort is false (a pure in-place update needs no re-sort).
	 */
	void BeginBatchUpdate();
	void EndBatchUpdate(bool doSort = true);

	/**
	 * Removes the specified file from the list.
	 *
	 * @param file A valid pointer of the file to be removed.
	 */
	void RemoveFile(CPartFile *file);

	/**
	 * Shows or hides a file's own row, depending on whether it currently
	 * passes the category + text filter.
	 *
	 * @param file A valid pointer to the file to be shown/hidden.
	 * @param show Whether to show the file.
	 */
	void ShowFile(CPartFile *file, bool show);

	/**
	 * Updates the state of the specified file, possibly causing a redrawing.
	 *
	 * @param toupdate The file to be updated (always a CPartFile*, kept as
	 *                 const void* for source-compatibility with existing
	 *                 callers -- see GuiEvents.cpp's DownloadCtrlUpdateItem).
	 *
	 * Calling this function ensures that the file is hidden/shown depending
	 * on its state and the currently selected category.
	 */
	void UpdateItem(const void *toupdate);

	/**
	 * Returns the current category.
	 */
	uint8 GetCategory() const { return m_category; }

	/**
	 * Changes the displayed category and updates the list of shown files.
	 *
	 * @param newCategory The new category to display.
	 */
	void ChangeCategory(int newCategory);

	/**
	 * Clears all completed files from the list.
	 */
	void ClearCompleted();

	/**
	 * Perform client update when item selection has changed.
	 */
	void DoItemSelectionChanged();

	/**
	 * Refreshes the "Free space:" label from the core's figure for the
	 * filesystem holding the part files, red once it no longer covers what
	 * is left to download.
	 *
	 * The warning is measured against the whole queue, not the category on
	 * screen: there is one temp directory for every category (a category
	 * chooses where a file lands when it finishes, not where it downloads),
	 * so every queued file competes for the same space and a per-category
	 * comparison would only warn once it was already too late.
	 *
	 * Driven by the GUI timer rather than by list changes -- the figure
	 * moves on its own as the files grow.
	 */
	void UpdateFreeSpace();

protected:
	/// Return old column order.
	wxString GetOldColumnOrder() const override;

	/**
	 * Type-ahead matches against the filename, not column 0: column 0 is
	 * the part number here, unlike every other ported list where it's the
	 * name. The base's default (GetItemColumnText(item, 0)) would make
	 * type-to-select match part numbers instead.
	 */
	wxString GetRowLabel(const wxDataViewItem &item) const override;

	/// Text of one cell, pulled on demand for the cells being drawn.
	wxString GetItemColumnText(wxUIntPtr item, unsigned column) const override;

	/// Rating/comment smiley on the File Name column, nothing elsewhere.
	bool GetItemIcon(wxUIntPtr item, unsigned column, wxIcon &icon) const override;

	/// Chunk/gap-bar spans for the Progress column.
	void GetItemBarFill(wxUIntPtr item, unsigned column, CBarFillSpec &out) const override;

	/** Live auto-sort: re-order when sorted by a column whose value changes
	 *  as a download progresses (transferred, completed, speed, progress,
	 *  sources, time remaining, last seen complete, last reception). Static
	 *  columns don't auto-resort. */
	bool IsLiveSortColumn() const override;

	/** Pause live auto-sort while the context menu is open. */
	bool IsMenuOpen() const override { return m_menu != nullptr; }

	/// Single-column comparison for the base's sort chain.
	int CompareItemData(
		wxUIntPtr data1, wxUIntPtr data2, unsigned column, bool alt, int modifier) const override;

	/**
	 * @see CMuleVirtualDataViewCtrl::RebuildFilteredView
	 */
	void RebuildFilteredView() override;

private:
	/**
	 * Rebuilds the visible rows from the model in a single pass, keeping the
	 * files that pass the current category + text filter. Used whenever the
	 * visible set changes wholesale (category switch, filter edit, or a
	 * deferred bulk load's first ShowFileList()).
	 */
	void RebuildVisibleList();

	//! Whether @a file should be displayed in @a category: the category
	//! predicate AND-ed with the text filter. Takes a non-const file because
	//! CPartFile::CheckShowItemInGivenCat() is not const.
	bool IsVisibleInCat(CPartFile *file, int category) const;

	/**
	 * Updates the displayed number representing the amount of files currently shown.
	 */
	void ShowFilesCount(int diff);

	/**
	 * Sets the displayed file count to an absolute value.
	 */
	void SetFilesCount(int count);

	/**
	 * Sets the "Total queue size:" label to the combined size of the
	 * currently shown files (category + text filter).
	 */
	void SetTotalSize(uint64 total);

	/**
	 * Delete/F2 key handling; see CMuleDataViewCtrl::OnListKey.
	 */
	bool OnListKey(wxKeyEvent &event) override;

	// Event-handlers for files
	void OnCancelFile(wxCommandEvent &event);
	void OnSetPriority(wxCommandEvent &event);
	void OnSwapSources(wxCommandEvent &event);
	void OnSetCategory(wxCommandEvent &event);
	void OnSetStatus(wxCommandEvent &event);
	void OnClearCompleted(wxCommandEvent &event);
	void OnGetLink(wxCommandEvent &event);
	void OnGetFeedback(wxCommandEvent &event);
	void OnViewFileInfo(wxCommandEvent &event);
	void OnViewFileComments(wxCommandEvent &event);
	void OnPreviewFile(wxCommandEvent &event);
	void OnShowInFolder(wxCommandEvent &event);

	/**
	 * The item the context menu was built for, by identity rather than row.
	 *
	 * The menu's enabled state is decided from this item, so the handlers act
	 * on it. A row index would not survive the menu being open: PopupMenu runs
	 * a nested event loop, so timers and EC updates keep mutating the list, and
	 * removing a row *above* this one shifts every index below it -- the click
	 * would then act on the neighbouring file. HasItemData() re-checks that the
	 * item is still present before it is used.
	 */
	wxUIntPtr m_menuItem = 0;

	void OnItemActivated(wxDataViewEvent &event);
	void OnItemRightClicked(wxDataViewEvent &event);

	/**
	 * Fires DoItemSelectionChanged() (which rebuilds the sources panel), but
	 * only if a notification isn't already pending -- m_ItemSelectionChangePending
	 * guards against a burst of selection events (e.g. a rubber-band
	 * selecting many rows) scheduling more than one.
	 */
	void OnSelectionChanged(wxDataViewEvent &event);
	//! Set while a DoItemSelectionChanged() notification is pending, so a
	//! burst of selection events schedules only one.
	bool m_ItemSelectionChangePending = false;

	/**
	 * Show file detail dialog for the given row.
	 */
	void ShowFileDetailDialog(long row);

	//! Every known download, shown or hidden by the current category/filter.
	//! RebuildVisibleList() walks this to decide what to show; AddFile() uses
	//! it to reject duplicates.
	std::set<CPartFile *> m_files;

	//! Pointer to the current menu object, used to avoid multiple menus.
	wxMenu *m_menu = nullptr;

	//! The currently displayed category
	uint8 m_category = 0;

	//! True between BeginBatchUpdate()/EndBatchUpdate(): AddFile() appends
	//! rows but defers the per-item SortList to one final sort (issue #444).
	bool m_batchUpdate = false;

	//! The number of displayed files
	int m_filecount = 0;

	//! Combined size of the displayed files (drives the "Total queue size:" label)
	uint64 m_shownSize = 0;

	//! The "Free space:" label, resolved by name on first use. Lives in the
	//! sources pane, so it cannot be reached through GetParent().
	wxStaticText *m_freeSpaceLabel = nullptr;

	wxDECLARE_EVENT_TABLE();
};

#endif
// File_checked_for_headers
