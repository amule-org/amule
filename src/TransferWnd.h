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

#ifndef TRANSFERWND_H
#define TRANSFERWND_H

#include <wx/panel.h>    // Needed for wxPanel
#include <wx/notebook.h> // needed for wxBookCtrlEvent in wx 2.8
#include "Types.h"       // Needed for uint32
#include "OtherStructs.h"

class CSourceListCtrl;
class CDownloadListCtrl;
class CMuleNotebook;
class wxListCtrl;
class wxSplitterEvent;
class wxCommandEvent;
class wxMouseEvent;
class wxEvent;
class wxMenu;

/**
 * Manages the lists and other controls in the transfer window; primarily, the user-defined
 * categories.
 */
class CTransferWnd : public wxPanel
{
public:
	CTransferWnd(wxWindow *pParent = NULL);

	~CTransferWnd();

	/**
	 * Adds @a category to the end of the list. Call after the category has been added to the
	 * lists of categories: the new one is assumed to be the last, and is appended to the end of
	 * the tabs on the category notebook.
	 */
	void AddCategory(Category_Struct *category);

	/**
	 * Updates the title of the category at notebook index @a index; -1 updates all of them.
	 */
	void UpdateCategory(int index);

	/**
	 * Removes a category.
	 */
	void RemoveCategory(int index);
	//! Everything that follows a category really being gone. Split out so the
	//! monolithic build can run it inline and amulegui from the EC reply.
	void CommitRemoveCategory(int index);
	void RemoveCategoryPage(int index);

	/**
	 * Updates the displayed titles of every existing category.
	 */
	void UpdateCatTabTitles() { UpdateCategory(-1); }

	/**
	 * Call before displaying the dialog: does a few tasks to make sure it looks the right way.
	 */
	void Prepare();

	//! Pointer to the download-queue.
	CDownloadListCtrl *downloadlistctrl;
	//! Pointer to the list of clients.
	CSourceListCtrl *clientlistctrl;

private:
	//! Contains the current (or last if the clientlist is hidden) position of the splitter.
	int m_splitter;
	//! Minimum position of splitter bar
	static const int s_splitterMin = 90;

	/**
	 * Handler for the set-status-by-category menu item.
	 */
	void OnSetCatStatus(wxCommandEvent &event);

	/**
	 * Handler for the set-priority-by-category menu item.
	 */
	void OnSetCatPriority(wxCommandEvent &event);

	/**
	 * Handler for the "Add Category" menu item.
	 */
	void OnAddCategory(wxCommandEvent &event);

	/**
	 * Handler for the "Delete Category" menu item.
	 */
	void OnDelCategory(wxCommandEvent &event);

	/**
	 * Handler for the "Edit Category" menu item.
	 */
	void OnEditCategory(wxCommandEvent &event);

	/**
	 * Handler for manipulating the default category.
	 */
	void OnSetDefaultCat(wxCommandEvent &event);

	/**
	 * Handler for the "Clear Completed" button.
	 */
	void OnBtnClearDownloads(wxCommandEvent &evt);

	/** Live text-filter box changed: push the new text to the download list. */
	void OnFilterChanged(wxCommandEvent &evt);

	/**
	 * Handler for changing categories.
	 */
	void OnCategoryChanged(wxBookCtrlEvent &evt);

	/**
	 * Handler for displaying the category popup menu.
	 */
	void OnNMRclickDLtab(wxMouseEvent &evt);

	/**
	 * Handler for the list-toggle button.
	 */
	void OnToggleClientList(wxCommandEvent &event);

	/**
	 * Handler for changes in the sash divider position.
	 */
	void OnSashPositionChanging(wxSplitterEvent &evt);

	//! Variable used to ensure that the category menu doesn't get displayed twice.
	wxMenu *m_menu;

	//! Pointer to the category tabs.
	CMuleNotebook *m_dlTab;

	wxDECLARE_EVENT_TABLE();
};

#endif

// File_checked_for_headers
