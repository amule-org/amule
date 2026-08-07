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

#ifndef FILEDETAILLISTCTRL_H
#define FILEDETAILLISTCTRL_H

#include "MuleVirtualDataViewCtrl.h" // Needed for CMuleVirtualDataViewCtrl

#define COLUMN_FILEDETAIL_NAME 0
#define COLUMN_FILEDETAIL_SOURCES 1

class SourcenameItem;

class CFileDetailListCtrl : public CMuleVirtualDataViewCtrl
{
public:
	CFileDetailListCtrl(wxWindow *&parent, int id, const wxPoint &pos, wxSize siz, int flags);

	void AddSource(SourcenameItem *item);
	void RefreshSource(SourcenameItem *item);
	void RemoveSource(SourcenameItem *item);
	void ClearSources() { ClearItemData(); }

protected:
	/// Text of a cell, pulled on demand for the rows being drawn.
	wxString GetItemColumnText(wxUIntPtr item, unsigned column) const override;

	/// Name compare on column 0, source-count compare on column 1.
	int CompareItemData(
		wxUIntPtr data1, wxUIntPtr data2, unsigned column, bool alt, int modifier) const override;

	/**
	 * The Sources column's value (a live source count) changes on every
	 * 5-second refresh timer tick while the dialog is open, so a
	 * sources-sorted list needs the coalesced live re-sort.
	 */
	bool IsLiveSortColumn() const override;

private:
	/**
	 * The base always constructs with wxDV_MULTIPLE (CMuleDataViewCtrl has
	 * no single-selection mode), but this list is meant to only ever have
	 * one row selected -- the "take over filename" actions assume it.
	 * Collapses the selection down to the just-clicked row whenever more
	 * than one ends up selected, the wxDataViewCtrl equivalent of the old
	 * list's own OnSelect() enforcement.
	 */
	void OnSelectionChanged(wxDataViewEvent &event);

	wxDECLARE_EVENT_TABLE();
};
#endif // FILEDETAILLISTCTRL_H
// File_checked_for_headers
