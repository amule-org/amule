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

#ifndef SHAREDFILESCTRL_H
#define SHAREDFILESCTRL_H

#include "MuleVirtualDataViewCtrl.h" // Needed for CMuleVirtualDataViewCtrl

#define COLUMN_SHARED_NAME 0
#define COLUMN_SHARED_RATING 1
#define COLUMN_SHARED_SIZE 2
#define COLUMN_SHARED_TYPE 3
#define COLUMN_SHARED_PRIO 4
#define COLUMN_SHARED_REQ 5
#define COLUMN_SHARED_AREQ 6
#define COLUMN_SHARED_TRA 7
#define COLUMN_SHARED_RTIO 8
#define COLUMN_SHARED_PART 9
#define COLUMN_SHARED_CMPL 10
#define COLUMN_SHARED_SPEED 11
#define COLUMN_SHARED_SINCE 12
#define COLUMN_SHARED_LASTUP 13
#define COLUMN_SHARED_PATH 14
//! Always empty. Absorbs the macOS trailing-column sizing; see
//! CMuleDataViewCtrl::AppendSpacerColumn().
#define COLUMN_SHARED_SPACER 15

class CSharedFileList;
class CKnownFile;
class wxMenu;

/**
 * This class represents the widget used to list shared files.
 */
class CSharedFilesCtrl : public CMuleVirtualDataViewCtrl
{
public:
	/**
	 * Constructor.
	 *
	 * @see CMuleListCtrl::CMuleListCtrl
	 */
	CSharedFilesCtrl(wxWindow *parent, int id, const wxPoint &pos, wxSize size, int flags);

	/**
	 * Destructor.
	 */
	~CSharedFilesCtrl();

	/** Reloads the list of shared files. */
	void ShowFileList();

	/**
	 * Sets the live text filter. Only files whose name contains @a text
	 * (case-insensitive) are shown; an empty string clears the filter. Purely
	 * GUI-side, so it works the same in the monolithic app and amulegui.
	 */
	// SetFilterText() is inherited from CMuleVirtualDataViewCtrl; the rebuild
	// it triggers is RebuildFilteredView() below.

	/** Empties the list (virtual-mode: clears the model + row index). */
	void ClearList();

	// Bracket a reconnect resync (issue #444) so the list repaints once
	// (Freeze) and sorts once at the end rather than per updated/added row.
	void BeginBatchUpdate();
	void EndBatchUpdate(bool doSort = true);

	/**
	 * Adds the specified file to the list, updating filecount and more.
	 *
	 * @param file The new file to be shown.
	 *
	 * Note that the item is inserted in sorted order.
	 */
	void ShowFile(CKnownFile *file);

	/**
	 * Removes a file from the list.
	 *
	 * @param toremove The file to be removed.
	 */
	void RemoveFile(CKnownFile *toremove);

	/**
	 * Updates a file on the list.
	 *
	 * @param toupdate The file to be updated.
	 */
	void UpdateItem(CKnownFile *toupdate);

	/**
	 * Begin a bulk update. While in this mode, UpdateItem() is a no-op
	 * and the per-row FindItem/RefreshItem cost is skipped. EndBulkUpdate()
	 * issues a single full Refresh() to repaint every row at once. Used by
	 * CSharedFileList::ClearED2KPublishInfo to convert what was an O(N²)
	 * GUI cascade (per-file SetPublishedED2K() -> notify -> linear-scan
	 * UpdateItem) into O(N) bookkeeping plus one full repaint.
	 */
	void BeginBulkUpdate();
	void EndBulkUpdate();

	/**
	 * Updates the number of shared files displayed above the list.
	 */
	void ShowFilesCount();

	/** Map a (virtual) row index to its file, or NULL if out of range. */
	CKnownFile *FileAtRow(long row) const { return reinterpret_cast<CKnownFile *>(ItemAt(row)); }

protected:
	/// Return old column order.
	wxString GetOldColumnOrder() const override;

	/// Text of one cell, pulled on demand for the cells being drawn.
	wxString GetItemColumnText(wxUIntPtr item, unsigned column) const override;

	/// Rating/comment smiley on the Rating column, nothing elsewhere.
	bool GetItemIcon(wxUIntPtr item, unsigned column, wxIcon &icon) const override;

	/// Availability-bar spans for the Obtained Parts column.
	void GetItemBarFill(wxUIntPtr item, unsigned column, CBarFillSpec &out) const override;

	/** Whether the current primary sort column changes value during
	 *  operation (drives the base's live auto-sort). */
	bool IsLiveSortColumn() const override;

	/** Pause live auto-sort while the context menu is open. */
	bool IsMenuOpen() const override { return m_menu != nullptr; }

	/// Single-column comparison for the base's sort chain.
	int CompareItemData(
		wxUIntPtr data1, wxUIntPtr data2, unsigned column, bool alt, int modifier) const override;

	/**
	 * Function that specifies which columns have alternate sorting.
	 *
	 * @see CMuleListCtrl::AltSortAllowed
	 */
	bool AltSortAllowed(unsigned column) const override;

	//! True if @a file passes the current text filter (name substring match).
	/**
	 * @see CMuleVirtualDataViewCtrl::RebuildFilteredView
	 */
	void RebuildFilteredView() override;

private:
	/**
	 * Adds the specified file to the list.
	 *
	 * If 'batch' is true, the item will be inserted last,
	 * and the files-count will not be updated, nor is
	 * the list checked for dupes.
	 */
	void DoShowFile(CKnownFile *file, bool batch);

	/**
	 * Event-handler for right-clicks on the list-items.
	 */
	void OnItemRightClicked(wxDataViewEvent &event);

	void OnGetFeedback(wxCommandEvent &event);
	void OnOpenFile(wxCommandEvent &event);
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

	/**
	 * Event-handler for the Set Priority menu items.
	 */
	void OnSetPriority(wxCommandEvent &event);

	/**
	 * Event-handler for the Auto-Priority menu item.
	 */
	void OnSetPriorityAuto(wxCommandEvent &event);

	/**
	 * Event-handler for the Create ED2K/Magnet URI items.
	 */
	void OnCreateURI(wxCommandEvent &event);

	/**
	 * Event-handler for the "Export selected files" menu item: writes the
	 * selected files' eD2k links to an .emulecollection text collection.
	 */
	void OnExportCollection(wxCommandEvent &WXUNUSED(evt));

	/**
	 * The link for one file, in the flavour the given menu id asks for.
	 * Anything other than the ids the URI menu items use yields the plain
	 * eD2k link.
	 */
	wxString LinkForFile(const CKnownFile *file, int menuId) const;

	/**
	 * Every selected row's link, one per line, with a trailing newline.
	 *
	 * One walk of the selection serves the clipboard items and the collection
	 * export alike; @a menuId picks the flavour, as in LinkForFile().
	 */
	wxString SelectedLinks(int menuId) const;

	/**
	 * Event-handler for the Edit Comment menu item.
	 */
	void OnEditComment(wxCommandEvent &event);

	/**
	 * Event-handler for the Rename menu item.
	 */
	void OnRename(wxCommandEvent &event);

	/**
	 * Checks for renaming via F2.
	 */
	bool OnListKey(wxKeyEvent &event) override;

	/**
	 * Adds links in a collection to transfers
	 */
	void OnAddCollection(wxCommandEvent &WXUNUSED(evt));

	void OnVerifyLocalData(wxCommandEvent &WXUNUSED(evt));

	/**
	 * Opens the file-details dialog for the selected shared file. Reuses the
	 * download list's CFileDetailDialog, which shows the sharing-side rows and
	 * hides the download-only ones based on each file's state.
	 */
	void OnViewFileDetails(wxCommandEvent &event);

	/**
	 * Double-click / Enter on a row also opens the file-details dialog, for
	 * parity with the downloads list.
	 */
	void OnItemActivated(wxDataViewEvent &event);

	/** Shared helper: open CFileDetailDialog anchored on the clicked row. */
	void ShowFileDetailDialog(long focused);

	//! Pointer used to ensure that the menu isn't displayed twice.
	wxMenu *m_menu;

	//! When true, UpdateItem() short-circuits and the bulk caller is
	//! responsible for issuing a single Refresh() at end-of-bulk.
	bool m_inBulkUpdate;

	//! True between BeginBatchUpdate()/EndBatchUpdate(): ShowFile() appends
	//! the row without sorting; EndBatchUpdate() does the single SortList().
	bool m_batchUpdate;

	//! Combined size of the displayed files (drives the "Total size:" label)
	uint64 m_shownSize;

	// The virtual-list model, sorting, live auto-sort and selection
	// preservation all live in CMuleVirtualDataViewCtrl now.

	wxDECLARE_EVENT_TABLE();
};

#endif
// File_checked_for_headers
