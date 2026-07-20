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

#include "MuleVirtualListCtrl.h" // Needed for CMuleVirtualListCtrl

class CSharedFileList;
class CKnownFile;
class wxMenu;

/**
 * This class represents the widget used to list shared files.
 */
class CSharedFilesCtrl : public CMuleVirtualListCtrl
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

	/** Empties the list (virtual-mode: clears the model + row index). */
	void ClearList();

	// Bracket a reconnect resync (issue #444) so the list repaints once
	// (Freeze) and sorts once at the end rather than per updated/added row.
	void BeginBatchUpdate();
	void EndBatchUpdate();

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
	wxString GetOldColumnOrder() const;

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
	 * Draws the graph of file-part availability.
	 *
	 * @param file The file to make a graph over.
	 * @param dc The wcDC to draw on.
	 * @param rect The drawing area.
	 *
	 * This function draws a barspan showing the availability of the parts of
	 * a file, for both Part-files and Known-files. Availability for Part-files
	 * is determined using the currently known sources, while availability for
	 * Known-files is determined using the sources requesting that file.
	 */
	void DrawAvailabilityBar(CKnownFile *file, wxDC *dc, const wxRect &rect) const;

	/**
	 * Overloaded function needed to do custom drawing of the items.
	 */
	virtual void OnDrawItem(
		int item, wxDC *dc, const wxRect &rect, const wxRect &rectHL, bool highlighted);

	/**
	 * @see CMuleListCtrl::GetTTSText
	 */
	virtual wxString GetTTSText(unsigned item) const;

	/**
	 * The list is owner-drawn (OnDrawItem paints every cell from the file), so
	 * this only feeds the generic control's keyboard type-ahead: the file name.
	 */
	virtual wxString GetItemColumnText(wxUIntPtr item, long column) const;

	/** Whether the current primary sort column changes value during operation
	 *  (drives the base's live auto-sort). */
	virtual bool IsLiveSortColumn() const;

	/** Pause live auto-sort while the context menu is open. */
	virtual bool IsMenuOpen() const { return m_menu != nullptr; }

	/**
	 * Sorter-function.
	 *
	 * @see wxListCtrl::SortItems
	 */
	static int wxCALLBACK SortProc(wxUIntPtr item1, wxUIntPtr item2, wxIntPtr sortData);

	/**
	 * Function that specifies which columns have alternate sorting.
	 *
	 * @see CMuleListCtrl::AltSortAllowed
	 */
	virtual bool AltSortAllowed(unsigned column) const;

	/**
	 * Event-handler for right-clicks on the list-items.
	 */
	void OnRightClick(wxListEvent &event);

	/**
	 * Event-handler for right-clicks on the list-items.
	 */
	void OnGetFeedback(wxCommandEvent &event);

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
	void OnKeyPressed(wxKeyEvent &event);

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
	void OnItemActivated(wxListEvent &event);

	/** Shared helper: open CFileDetailDialog anchored on the clicked row. */
	void ShowFileDetailDialog(long focused);

	//! Pointer used to ensure that the menu isn't displayed twice.
	wxMenu *m_menu;

	//! When true, UpdateItem() short-circuits and the bulk caller is
	//! responsible for issuing a single Refresh() at end-of-bulk.
	bool m_inBulkUpdate;

	// The virtual-list model, sorting, live auto-sort and selection
	// preservation all live in CMuleVirtualListCtrl now.

	wxDECLARE_EVENT_TABLE();
};

#endif
// File_checked_for_headers
