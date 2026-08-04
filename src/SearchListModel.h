//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
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

#ifndef SEARCHLISTMODEL_H
#define SEARCHLISTMODEL_H

#include <wx/dataview.h>

class CSearchFile;
class CSearchListCtrl;

/**
 * wxDataViewModel backing a CSearchListCtrl. Presents the CSearchFile
 * parent/children forest for one search-id as a native tree.
 *
 * Two things that used to be hand-rolled in CSearchListCtrl::OnDrawItem /
 * ShowChildren are deliberately NOT reimplemented here:
 *  - Expand/collapse state is owned by wxDataViewCtrl itself (per-row, via
 *    Expand()/Collapse()/IsExpanded()), so GetChildren() always reports the
 *    full child set -- CSearchFile::ShowChildren()/SetShowChildren(), which
 *    the old hand-drawn tree used to fake this, is not consulted.
 *  - Tree connector lines and the expander glyph are drawn natively by the
 *    control from IsContainer()/GetChildren(), replacing
 *    CSearchListCtrl::OnDrawItem's manual DrawLine/DrawCircle code.
 *
 * Regex/known-file filtering (CSearchListCtrl::SetFilter) is a different
 * concept from expand/collapse -- a filtered-out result must not appear at
 * all, so it is excluded at the IsContainer()/GetChildren() level.
 *
 * The model does not own or cache the result set: GetChildren() queries
 * CSearchList::GetSearchResults() (via the owning CSearchListCtrl) live, so
 * there is a single source of truth and no separate bookkeeping to keep in
 * sync. AddFile()/RemoveFile()/UpdateFile() only need to notify the control
 * that something changed -- they are not the place new data is stored.
 */
class CSearchListModel : public wxDataViewModel
{
public:
	explicit CSearchListModel(CSearchListCtrl *owner);

	//! Notify the control that a new result is now visible (already added
	//! to CSearchList's own storage by the caller).
	void NotifyFileAdded(CSearchFile *file);
	//! Notify the control that a result should no longer be shown.
	void NotifyFileRemoved(CSearchFile *file);
	//! Notify the control that a result's displayed values changed.
	void NotifyFileUpdated(CSearchFile *file);
	//! Notify the control that filtering criteria changed and the whole
	//! tree should be re-evaluated (some rows may appear/disappear).
	void NotifyFilterChanged();

	static CSearchFile *ToFile(const wxDataViewItem &item)
	{
		return static_cast<CSearchFile *>(item.GetID());
	}
	static wxDataViewItem ToItem(const CSearchFile *file)
	{
		return wxDataViewItem(const_cast<CSearchFile *>(file));
	}

	//! Column indices, matching the wxDataViewColumn order set up by
	//! CSearchListCtrl -- shared with CSearchListCtrl::SortProc-equivalent
	//! Compare() logic below.
	enum Column
	{
		COL_NAME = 0,
		COL_SIZE,
		COL_SOURCES,
		COL_TYPE,
		COL_RATING,
		COL_FILEID,
		COL_STATUS,
		COL_LENGTH,
		COL_BITRATE,
		COL_CODEC,
		COL_DIRECTORY,
		COL_COUNT
	};

	// wxDataViewModel interface
	unsigned int GetColumnCount() const wxOVERRIDE;
	wxString GetColumnType(unsigned int col) const wxOVERRIDE;
	void GetValue(wxVariant &variant, const wxDataViewItem &item, unsigned int col) const wxOVERRIDE;
	bool SetValue(const wxVariant &variant, const wxDataViewItem &item, unsigned int col) wxOVERRIDE;
	bool GetAttr(const wxDataViewItem &item, unsigned int col, wxDataViewItemAttr &attr) const wxOVERRIDE;
	wxDataViewItem GetParent(const wxDataViewItem &item) const wxOVERRIDE;
	bool IsContainer(const wxDataViewItem &item) const wxOVERRIDE;
	unsigned int GetChildren(const wxDataViewItem &item, wxDataViewItemArray &children) const wxOVERRIDE;
	int Compare(const wxDataViewItem &item1,
		const wxDataViewItem &item2,
		unsigned int column,
		bool ascending) const wxOVERRIDE;

private:
	CSearchListCtrl *m_owner;
};

#endif // SEARCHLISTMODEL_H
// File_checked_for_headers
