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

#include <map> // Needed for std::multimap
#include <wx/brush.h>

#include "Types.h"        // Needed for uint8
#include "Constants.h"    // Needed for DownloadItemType
#include "MuleListCtrl.h" // Needed for CMuleListCtrl

class CPartFile;
class wxBitmap;
class wxRect;
class wxDC;

struct FileCtrlItem_Struct;

/**
 * This class is responsible for representing the download queue.
 *
 * The CDownlodListCtrl class is responsible for drawing files being downloaded.
 * It is in many ways primary widget within the application, since it is here that
 * users can inspect and manipulate their current downloads.
 *
 */
class CDownloadListCtrl : public CMuleListCtrl
{
public:
	/**
	 * Constructor.
	 *
	 * @see CMuleListCtrl::CMuleListCtrl for documentation of parameters.
	 */
	CDownloadListCtrl(wxWindow *parent,
		wxWindowID winid = -1,
		const wxPoint &pos = wxDefaultPosition,
		const wxSize &size = wxDefaultSize,
		long style = wxLC_ICON,
		const wxValidator &validator = wxDefaultValidator,
		const wxString &name = "downloadlistctrl");

	/**
	 * Destructor.
	 */
	virtual ~CDownloadListCtrl();

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

	/**
	 * Bracket a burst of AddFile()/UpdateItem() calls (a reconnect resync —
	 * issue #444) so the list repaints once (Freeze) and sorts once, instead
	 * of per-item. Existing rows are updated in place; new rows are appended
	 * without a per-item sort. EndBatchUpdate() does the single SortList().
	 */
	void BeginBatchUpdate();
	void EndBatchUpdate();

	/**
	 * Removes the specified file from the list.
	 *
	 * @param file A valid pointer of the file to be removed.
	 *
	 * This function also removes any sources associated with the file.
	 */
	void RemoveFile(CPartFile *file);

	/**
	 * Shows or hides the sources of a specific file.
	 *
	 * @param file A valid pointer to the file to be shown/hidden.
	 * @param show Whenever or not to show the file.
	 *
	 * If the file is hidden, then its sources will also be hidden.
	 */
	void ShowFile(CPartFile *file, bool show);

	/**
	 * Updates the state of the specified item, possibly causing a redrawing.
	 *
	 * @param toupdate The source or file to be updated.
	 *
	 * Calling this function with a file as the argument will ensure that the
	 * file is hidden/shown depending on its state and the currently selected
	 * category.
	 */
	void UpdateItem(const void *toupdate);

	/**
	 * Returns the current category.
	 */
	uint8 GetCategory() const;

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

protected:
	/// Return old column order.
	wxString GetOldColumnOrder() const;

private:
	/**
	 * Updates the displayed number representing the amount of files currently shown.
	 */
	void ShowFilesCount(int diff);

	/**
	 * @see CMuleListCtrl::GetTTSText
	 */
	virtual wxString GetTTSText(unsigned item) const;

	/**
	 * Overloaded function needed for custom drawing of items.
	 */
	virtual void OnDrawItem(
		int item, wxDC *dc, const wxRect &rect, const wxRect &rectHL, bool highlighted);

	/**
	 * Draws a file item.
	 */
	void DrawFileItem(wxDC *dc, int nColumn, const wxRect &rect, FileCtrlItem_Struct *item) const;

	/**
	 * Draws the status (chunk) bar for a file.
	 */
	void DrawFileStatusBar(const CPartFile *file, wxDC *dc, const wxRect &rect, bool bFlat) const;

	static int wxCALLBACK SortProc(wxUIntPtr item1, wxUIntPtr item2, wxIntPtr sortData);
	static int Compare(const CPartFile *file1, const CPartFile *file2, long lParamSort);

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

	// Misc event-handlers
	void OnItemActivated(wxListEvent &event);
	void OnMouseRightClick(wxListEvent &event);
	void OnMouseMiddleClick(wxListEvent &event);
	void OnKeyPressed(wxKeyEvent &event);
	void OnItemSelectionChanged(wxListEvent &event);

	/**
	 * Executes the user-selected preview command on the specified file.
	 *
	 * @file The file to be previewed.
	 */
	void PreviewFile(CPartFile *file);

	/**
	 * Show file detail dialog for item at index
	 */
	void ShowFileDetailDialog(long index);

	//! The type of list used to store items on the listctrl.
	typedef std::multimap<const void *, FileCtrlItem_Struct *> ListItems;
	//! Shortcut to the pair-type used on the list.
	typedef ListItems::value_type ListItemsPair;
	//! This pair is used when searching for equal-ranges.
	typedef std::pair<ListItems::iterator, ListItems::iterator> ListIteratorPair;

	//! This list contains everything shown on the list. Sources are only to
	//! be found on this list if they are being displayed, whereas files can
	//! always be found on this list, even if they are currently hidden.
	ListItems m_ListItems;

	//! Pointer to the current menu object, used to avoid multiple menus.
	wxMenu *m_menu;
	//! Cached brush object.
	wxBrush m_hilightBrush;
	//! Cached brush object.
	wxBrush m_hilightUnfocusBrush;

	//! The currently displayed category
	uint8 m_category;

	//! Flag if change of item selection is pending
	bool m_ItemSelectionChangePending;

	//! True between BeginBatchUpdate()/EndBatchUpdate(): AddFile() appends
	//! rows but defers the per-item SortList to one final sort (issue #444).
	bool m_batchUpdate = false;

	//! The number of displayed files
	int m_filecount;

	wxDECLARE_EVENT_TABLE();
};

#endif
// File_checked_for_headers
