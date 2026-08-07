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

#include "FileDetailListCtrl.h" // Interface declarations

#include <common/Format.h> // Needed for CFormat

#include "OtherFunctions.h" // Needed for CmpAny
#include "PartFile.h"       // Needed for SourcenameItem

wxBEGIN_EVENT_TABLE(CFileDetailListCtrl, CMuleVirtualDataViewCtrl)
	EVT_DATAVIEW_SELECTION_CHANGED(wxID_ANY, CFileDetailListCtrl::OnSelectionChanged)
wxEND_EVENT_TABLE()

CFileDetailListCtrl::CFileDetailListCtrl(wxWindow *&parent, int id, const wxPoint &pos, wxSize siz, int flags)
: CMuleVirtualDataViewCtrl(parent, id, pos, siz, flags)
{
	const int columnFlags = wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE;
	AddTextColumn(_("File Name"), COLUMN_FILEDETAIL_NAME, "N", 370, wxALIGN_LEFT, columnFlags);
	AddTextColumn(_("Sources"), COLUMN_FILEDETAIL_SOURCES, "S", 70, wxALIGN_LEFT, columnFlags);

	AppendSpacerColumn(COLUMN_FILEDETAIL_SPACER);
	AssociateVirtualModel();

	// Initial sorting: Sources descending, matching the pre-port default.
	// LoadColumnSettings() replaces it once the config has something saved.
	ApplySorting(COLUMN_FILEDETAIL_SOURCES, SORT_DES);

	// This list gains persistence here: it is the one dataview list that never
	// had it, so a user who widened "File Name" lost it the moment the dialog
	// closed, and the sort reset to Sources-descending on every open. There is
	// no GetOldColumnOrder() override because there is no pre-dataview config
	// to migrate -- nothing was ever written under this name.
	m_columnStore.SetTableName("FileDetail");
	LoadColumnSettings();

	InitColumnState();
}

void CFileDetailListCtrl::AddSource(SourcenameItem *item)
{
	AddItemData(reinterpret_cast<wxUIntPtr>(item));
}

void CFileDetailListCtrl::RefreshSource(SourcenameItem *item)
{
	RefreshItemData(reinterpret_cast<wxUIntPtr>(item));
}

void CFileDetailListCtrl::RemoveSource(SourcenameItem *item)
{
	RemoveItemData(reinterpret_cast<wxUIntPtr>(item));
}

wxString CFileDetailListCtrl::GetItemColumnText(wxUIntPtr item, unsigned column) const
{
	const SourcenameItem *source = reinterpret_cast<const SourcenameItem *>(item);
	switch (column) {
	case COLUMN_FILEDETAIL_NAME:
		return source->name;
	case COLUMN_FILEDETAIL_SOURCES:
		return CFormat("%i") % source->count;
	default:
		return wxEmptyString;
	}
}

int CFileDetailListCtrl::CompareItemData(
	wxUIntPtr data1, wxUIntPtr data2, unsigned column, bool WXUNUSED(alt), int modifier) const
{
	const SourcenameItem *s1 = reinterpret_cast<const SourcenameItem *>(data1);
	const SourcenameItem *s2 = reinterpret_cast<const SourcenameItem *>(data2);

	switch (column) {
	case COLUMN_FILEDETAIL_NAME:
		return modifier * s1->name.CmpNoCase(s2->name);
	case COLUMN_FILEDETAIL_SOURCES:
		return modifier * CmpAny(s1->count, s2->count);
	default:
		return 0;
	}
}

bool CFileDetailListCtrl::IsLiveSortColumn() const
{
	// Only the source count moves on its own; a row's name is the key it was
	// created under and never changes. Answering "yes" for the name column
	// would schedule a re-sort on every refresh tick that could not reorder
	// anything. Same shape as CServerListCtrl.
	if (m_sort_orders.empty()) {
		return false;
	}
	return static_cast<int>(m_sort_orders.front().first) == COLUMN_FILEDETAIL_SOURCES;
}

void CFileDetailListCtrl::OnSelectionChanged(wxDataViewEvent &event)
{
	if (GetSelectedItemsCount() > 1 && event.GetItem().IsOk()) {
		UnselectAll();
		Select(event.GetItem());
	}
}
// File_checked_for_headers
