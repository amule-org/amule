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
#include "MD4Hash.h"                 // Needed for CMD4Hash in MediaRefreshSelection

#define COLUMN_SHARED_NAME 0
#define COLUMN_SHARED_SIZE 1
#define COLUMN_SHARED_TYPE 2
#define COLUMN_SHARED_PRIO 3
#define COLUMN_SHARED_REQ 4
#define COLUMN_SHARED_AREQ 5
#define COLUMN_SHARED_TRA 6
#define COLUMN_SHARED_RTIO 7
#define COLUMN_SHARED_PART 8
#define COLUMN_SHARED_CMPL 9
#define COLUMN_SHARED_SPEED 10
#define COLUMN_SHARED_SINCE 11
#define COLUMN_SHARED_LASTUP 12
#define COLUMN_SHARED_PATH 13
//! Always empty. Absorbs the macOS trailing-column sizing; see
//! CMuleDataViewCtrl::AppendSpacerColumn().
#define COLUMN_SHARED_SPACER 14

class CSharedFileList;
class CKnownFile;
class wxMenu;
class wxStaticText;

/**
 * This class represents the widget used to list shared files.
 */
class CSharedFilesCtrl : public CMuleVirtualDataViewCtrl
{
public:
	/**
	 * Constructor.
	 *
	 * @see CMuleVirtualDataViewCtrl::CMuleVirtualDataViewCtrl
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
	/**
	 * While the startup hash drain runs, hold back the per-row model
	 * notification and the files-count label.
	 *
	 * Each finished hash arrives as its own queued event, and
	 * ProcessPendingEvents() runs the whole queue in one pass -- so with
	 * thousands of them the main thread never returns to the run loop and the
	 * window, already created by then, cannot paint. Per row the two costs are
	 * wx's Cocoa Add(), which issues a full NSOutlineView reloadData, and
	 * ShowFilesCount(), which searches the window hierarchy by name. Deferred,
	 * both happen once per drain tick instead of once per file.
	 *
	 * Only for that drain: the tick flushes with FinishBulkLoad(), whose
	 * Reset() rebuilds the view and drops the scroll position. That is
	 * unobjectionable while the list is being populated for the first time and
	 * would not be during amulegui's steady-state poll, which shares the same
	 * batch machinery.
	 */
	void SetStartupDrainMode(bool on);

	void BeginBatchUpdate();
	void EndBatchUpdate(bool doSort = true);

	/**
	 * Ends the repaint freeze without ending the batch.
	 *
	 * Startup keeps appending long after the window is worth showing: the
	 * shared-file scan finishes in seconds, but the files it queued for
	 * hashing arrive over the following minutes, and each has to stay an
	 * O(1) append rather than a sorted insert (#853). Thawing separately
	 * lets those rows be seen as they land while the batch runs on.
	 */
	void ThawForDisplay();

	/**
	 * Number of files still queued for hashing, shown next to the count.
	 *
	 * Zero clears it. Only startup sets this, and only until its hash queue
	 * drains.
	 */
	void SetHashingCount(size_t remaining);

	/**
	 * Sorts only if rows have been appended since the last call.
	 *
	 * Startup polls on a timer, but rows arrive on hash completions, and
	 * hashing cost tracks bytes -- one large file is a single append after
	 * minutes of nothing. Sorting per tick regardless would be a full
	 * std::sort plus a row-index rebuild and a model reset, once a second,
	 * for no reordering (#853).
	 */
	void SortIfRowsAppended();

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

	/**
	 * Refreshes the "Free space:" label from the core's figure for the
	 * filesystem finished downloads land on.
	 *
	 * Informational only, with no threshold: nothing here stops when the
	 * disk fills, and this panel has no category selector to scope a
	 * comparison to -- so it reports the default category's incoming
	 * directory. Driven by the GUI timer, like the Downloads panel's.
	 */
	void UpdateFreeSpace();

	/**
	 * Redraws the "Total size of Shared Files:" label.
	 *
	 * Also driven by the GUI timer, because unlike the total itself the
	 * completed figure beside it moves while nothing about the list changes:
	 * part files gain bytes as they download. Sets the label only when the
	 * text actually differs, since this runs once a second for as long as
	 * the panel is up.
	 */
	void UpdateTotalSize();

	/** Map a (virtual) row index to its file, or NULL if out of range. */
	CKnownFile *FileAtRow(long row) const { return reinterpret_cast<CKnownFile *>(ItemAt(row)); }

protected:
	/// Return old column order.
	wxString GetOldColumnOrder() const override;

	/// Text of one cell, pulled on demand for the cells being drawn.
	wxString GetItemColumnText(wxUIntPtr item, unsigned column) const override;

	/// Rating/comment smiley on the Name column, nothing elsewhere.
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
	 * @see CMuleDataViewCtrl::AltSortAllowed
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
	void OnRefreshMediaMetadata(wxCommandEvent &event);

	//! The current selection split by whether a media re-extraction can act on
	//! it. Shared by the menu's enable rule and the handler so the two cannot
	//! disagree about what the action would do.
	struct MediaRefreshSelection
	{
		// Hashes, not CKnownFile pointers. The confirmation dialog runs a
		// nested event loop, and in amulegui the EC poll timer keeps running
		// inside it -- CKnownFilesRem::DeleteItem ends in `delete file` for
		// anything the daemon stops listing, so a pointer collected before the
		// dialog can be dangling after it. A hash cannot dangle, and the
		// refresh call takes one anyway; a file that went away in the meantime
		// simply fails to resolve.
		std::vector<CMD4Hash> eligible;
		unsigned incomplete = 0; //!< in-progress downloads, nothing complete to read
		unsigned notMedia = 0;   //!< not audio or video
	};
	MediaRefreshSelection PartitionForMediaRefresh() const;

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

	//! Whether the batch's Freeze() is still outstanding. Tracked separately
	//! from m_batchUpdate because ThawForDisplay() ends one without the
	//! other, and wx counts Freeze/Thaw -- an unbalanced Thaw() asserts.
	bool m_batchFrozen;

	//! Files still queued for hashing, appended to the count label while
	//! non-zero. See SetHashingCount().
	size_t m_hashingRemaining;

	//! Rows appended since the last SortIfRowsAppended(). The batch appends
	//! without sorting, so this is what makes a later sort worth running.
	bool m_batchRowsAppended;
	bool m_startupDrain = false;

	//! Combined size of the displayed files (drives the "Total size:" label)
	uint64 m_shownSize;

	/**
	 * Bytes the displayed part files have yet to obtain.
	 *
	 * m_shownSize is the sum of the Size column, and that column shows a part
	 * file's final size, not what is on disk yet. Subtracting this gives the
	 * figure the total is shown next to (issue #927).
	 *
	 * Walks the download queue and asks the row index whether each part file
	 * is displayed, rather than walking the displayed files and asking each
	 * whether it is a part file: there are at most as many part files as
	 * there are downloads, against a share that can hold tens of thousands.
	 */
	uint64 ShownIncompleteBytes() const;

	//! The "Free space:" label, resolved by name on first use. Lives in the
	//! statistics box, so it cannot be reached through GetParent().
	wxStaticText *m_freeSpaceLabel = nullptr;

	//! The "Total size of Shared Files:" label, resolved the same way.
	wxStaticText *m_totalSizeLabel = nullptr;

	// The virtual-list model, sorting, live auto-sort and selection
	// preservation all live in CMuleVirtualDataViewCtrl now.

	wxDECLARE_EVENT_TABLE();
};

#endif
// File_checked_for_headers
