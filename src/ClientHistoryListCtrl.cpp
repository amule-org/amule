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

#include "ClientHistoryListCtrl.h" // Interface declarations

#include <wx/datetime.h>

#include <common/Format.h> // Needed for CFormat

#include "DataToText.h"     // Needed for GetSoftName, OriginToText
#include "OtherFunctions.h" // Needed for CastItoXBytes, Uint32toStringIP

CClientHistoryListCtrl::CClientHistoryListCtrl(
	wxWindow *parent, int id, const wxPoint &pos, wxSize size, int flags)
: CMuleVirtualDataViewCtrl(parent, id, pos, size, flags)
, m_loaded(false)
{
	const int colFlags = wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE;
	AddTextColumn(_("Name"), COLUMN_HISTORY_NAME, "N", 200, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Software"), COLUMN_HISTORY_SOFTWARE, "S", 110, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Version"), COLUMN_HISTORY_VERSION, "V", 90, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("IP Address"), COLUMN_HISTORY_ADDRESS, "I", 140, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Origin"), COLUMN_HISTORY_ORIGIN, "O", 110, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("First seen"), COLUMN_HISTORY_FIRST_SEEN, "F", 130, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Last seen"), COLUMN_HISTORY_LAST_SEEN, "L", 130, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Sessions"), COLUMN_HISTORY_SESSIONS, "n", 80, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Total Uploaded"), COLUMN_HISTORY_TOTAL_UP, "T", 110, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Total Downloaded"), COLUMN_HISTORY_TOTAL_DOWN, "t", 110, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Ratio"), COLUMN_HISTORY_RATIO, "R", 70, wxALIGN_LEFT, colFlags);

	AppendSpacerColumn(COLUMN_HISTORY_SPACER);

	AssociateVirtualModel();

	// Most-recently-seen first: on a store holding tens of thousands of
	// records, the ones worth looking at are the ones from today.
	ApplySorting(COLUMN_HISTORY_LAST_SEEN, 1);

	m_columnStore.SetTableName("ClientHistory");
	LoadColumnSettings();
	InitColumnState();
}

CClientHistoryListCtrl::~CClientHistoryListCtrl() {}

const ClientHistoryRow *CClientHistoryListCtrl::RowFor(wxUIntPtr item) const
{
	// Stored as index+1 so that 0 stays available as "no item", which is what
	// the base returns for an unknown row.
	if (item == 0 || item > m_rows.size()) {
		return NULL;
	}
	return &m_rows[item - 1];
}

void CClientHistoryListCtrl::SetRows(std::vector<ClientHistoryRow> &&rows)
{
	ClearItemData();
	m_rows = std::move(rows);
	m_loaded = true;

	// One bulk load rather than an insert per row: the store can hold tens of
	// thousands of records and AddItemData() sorts on every insert.
	for (size_t i = 0; i < m_rows.size(); ++i) {
		AppendItemData(static_cast<wxUIntPtr>(i + 1));
	}
	FinishBulkLoad();
}

wxString CClientHistoryListCtrl::GetItemColumnText(wxUIntPtr item, unsigned column) const
{
	const ClientHistoryRow *row = RowFor(item);
	if (row == NULL) {
		return wxEmptyString;
	}

	switch (column) {
	case COLUMN_HISTORY_NAME:
		// Without metadata the hash is all we know it by, which is still
		// more useful than an empty cell.
		return row->name.IsEmpty() ? row->hash.Encode() : row->name;

	case COLUMN_HISTORY_SOFTWARE:
		return row->hasMeta ? GetSoftName(row->clientSoft) : wxString();

	case COLUMN_HISTORY_VERSION:
		return row->version;

	case COLUMN_HISTORY_ADDRESS:
		if (row->ip == 0) {
			return wxEmptyString;
		}
		return CFormat(wxT("%s:%u")) % Uint32toStringIP(row->ip) % row->port;

	case COLUMN_HISTORY_ORIGIN:
		return row->hasMeta ? OriginToText(row->sourceFrom) : wxString();

	case COLUMN_HISTORY_FIRST_SEEN:
		return row->firstSeen == 0 ? wxString()
					   : wxDateTime(static_cast<time_t>(row->firstSeen)).FormatDate();

	case COLUMN_HISTORY_LAST_SEEN:
		// A date is the wrong answer for a peer that is here now -- and the
		// stored last-seen for a connected peer is whenever it previously
		// disconnected, which reads as though it were long gone.
		if (row->online) {
			return _("Online now");
		}
		if (row->lastSeen == 0) {
			return wxEmptyString;
		}
		return wxDateTime(static_cast<time_t>(row->lastSeen)).FormatDate();

	case COLUMN_HISTORY_SESSIONS:
		if (row->sessions == 0) {
			return wxEmptyString;
		}
		return CFormat(wxT("%u")) % row->sessions;

	case COLUMN_HISTORY_TOTAL_UP:
		return CastItoXBytes(row->uploaded);

	case COLUMN_HISTORY_TOTAL_DOWN:
		return CastItoXBytes(row->downloaded);

	case COLUMN_HISTORY_RATIO:
		// Blank unless both directions moved -- see the same reasoning in
		// CClientsListCtrl.
		if (row->uploaded == 0 || row->downloaded == 0) {
			return wxEmptyString;
		}
		return CFormat(wxT("%.2f")) %
		       (static_cast<double>(row->downloaded) / static_cast<double>(row->uploaded));

	default:
		return wxEmptyString;
	}
}

int CClientHistoryListCtrl::CompareItemData(
	wxUIntPtr data1, wxUIntPtr data2, unsigned column, bool WXUNUSED(alt), int modifier) const
{
	const ClientHistoryRow *r1 = RowFor(data1);
	const ClientHistoryRow *r2 = RowFor(data2);
	if (r1 == NULL || r2 == NULL) {
		return 0;
	}

	switch (column) {
	case COLUMN_HISTORY_FIRST_SEEN:
		return modifier * CmpAny(r1->firstSeen, r2->firstSeen);
	case COLUMN_HISTORY_LAST_SEEN:
		// Peers that are here now sort as the most recent thing there is, so
		// the column reads as "when was this peer last around" throughout
		// instead of stranding the live ones at their stale timestamps.
		if (r1->online != r2->online) {
			return modifier * (r1->online ? 1 : -1);
		}
		return modifier * CmpAny(r1->lastSeen, r2->lastSeen);
	case COLUMN_HISTORY_SESSIONS:
		return modifier * CmpAny(r1->sessions, r2->sessions);
	case COLUMN_HISTORY_TOTAL_UP:
		return modifier * CmpAny(r1->uploaded, r2->uploaded);
	case COLUMN_HISTORY_TOTAL_DOWN:
		return modifier * CmpAny(r1->downloaded, r2->downloaded);
	default:
		return modifier *
		       GetItemColumnText(data1, column).CmpNoCase(GetItemColumnText(data2, column));
	}
}
