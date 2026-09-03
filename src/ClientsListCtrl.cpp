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

#include "ClientsListCtrl.h" // Interface declarations

#include <algorithm>
#include <unordered_map>

#include <common/Format.h> // Needed for CFormat

#include "amule.h"              // Needed for theApp
#include "ClientDetailDialog.h" // Needed for CClientDetailDialog
#include "ClientList.h"         // Needed for CClientList::FindClientByECID
#include "ClientRef.h"          // Needed for CClientRef
#include "DataToText.h"         // Needed for GetSoftName, OriginToText
#include "MuleBarRenderer.h"    // Needed for CBarFillSpec, CMuleBarRenderer
#include "OtherFunctions.h"     // Needed for CastItoXBytes, CastItoSpeed
#include "muuli_wdr.h"          // Needed for ID_CLIENTSLIST

namespace
{
/**
 * Renders the Name column.
 *
 * Same extension point the per-file client lists use (see
 * CGenericClientListCtrl's renderer of the same name): CMuleBarRenderer is
 * borrowed for its identity-carrying CBarFillSpec, not to draw a bar. The
 * identity here is the row's own snapshot rather than a client, since that is
 * what this list holds, and the drawing is DrawClientNameCell() either way.
 */
class CClientsNameRenderer : public CMuleBarRenderer
{
public:
	bool Render(wxRect cell, wxDC *dc, int WXUNUSED(state)) override
	{
		const ClientNameCell *data =
			reinterpret_cast<const ClientNameCell *>(GetSpec().GetIdentity());
		if (data != nullptr) {
			DrawClientNameCell(*data, cell, dc);
		}
		return true;
	}
};
} // namespace

CClientsListCtrl::CClientsListCtrl(
	wxWindow *parent, int id, const wxPoint &pos, wxSize size, int flags, const wxString &tableName)
: CClientRowListCtrl(parent, id, pos, size, flags)
{
	const int colFlags = wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE;
	// Owns its drawing so the badge icons match the per-file client lists;
	// still sorts and compares on the plain name text.
	AddBarColumn(_("Name"), COLUMN_CLIENTS_NAME, "N", 200, colFlags, new CClientsNameRenderer());
	AddTextColumn(_("Software"), COLUMN_CLIENTS_SOFTWARE, "S", 110, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Version"), COLUMN_CLIENTS_VERSION, "V", 90, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("IP Address"), COLUMN_CLIENTS_ADDRESS, "I", 140, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Origin"), COLUMN_CLIENTS_ORIGIN, "O", 110, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Files"), COLUMN_CLIENTS_FILES, "F", 220, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Upload Speed"), COLUMN_CLIENTS_UP_SPEED, "U", 100, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Download Speed"), COLUMN_CLIENTS_DOWN_SPEED, "D", 100, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Uploaded"), COLUMN_CLIENTS_SESSION_UP, "u", 100, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Downloaded"), COLUMN_CLIENTS_SESSION_DOWN, "d", 100, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Total Uploaded"), COLUMN_CLIENTS_TOTAL_UP, "T", 110, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Total Downloaded"), COLUMN_CLIENTS_TOTAL_DOWN, "t", 110, wxALIGN_LEFT, colFlags);
	AddTextColumn(_("Ratio"), COLUMN_CLIENTS_RATIO, "R", 70, wxALIGN_LEFT, colFlags);

	AppendSpacerColumn(COLUMN_CLIENTS_SPACER);

	AssociateVirtualModel();

	ApplySorting(COLUMN_CLIENTS_NAME, 0);

	m_columnStore.SetTableName(tableName);
	LoadColumnSettings();
	InitColumnState();
}

CClientsListCtrl::~CClientsListCtrl() = default;

const CClientsListCtrl::Row *CClientsListCtrl::RowFor(wxUIntPtr item) const
{
	// The item data is the peer's ECID. CECID hands out ++counter starting at
	// 1, so it never collides with 0, which the base returns for "no item".
	const auto it = m_rowOfEcid.find(static_cast<uint32>(item));
	return it != m_rowOfEcid.end() ? &m_rows[it->second] : nullptr;
}

void CClientsListCtrl::ReindexRows()
{
	m_rowOfEcid.clear();
	m_rowOfEcid.reserve(m_rows.size());
	for (size_t i = 0; i < m_rows.size(); ++i) {
		m_rowOfEcid[m_rows[i].ecid] = i;
	}
}

void CClientsListCtrl::SetClients(std::vector<Row> &&rows)
{
	// A peer with no ECID cannot be addressed as an item, and every live peer
	// has one, so this only ever drops a row the sweep could not identify.
	rows.erase(std::remove_if(rows.begin(), rows.end(), [](const Row &row) { return row.ecid == 0; }),
		rows.end());

	// The common case by far: the same peers as last second, with new numbers.
	// Overwrite the rows where they sit and refresh them. Rebuilding the model
	// instead -- which is all this used to do -- discards the scroll position
	// every time the poll lands, so a list a person is reading jumps back to
	// the top once a second.
	if (rows.size() == m_rows.size()) {
		std::unordered_map<uint32, const Row *> incoming;
		incoming.reserve(rows.size());
		for (const Row &row : rows) {
			incoming[row.ecid] = &row;
		}
		bool sameSet = incoming.size() == m_rows.size();
		for (const Row &row : m_rows) {
			if (incoming.count(row.ecid) == 0) {
				sameSet = false;
				break;
			}
		}
		if (sameSet) {
			// Only the rows that actually moved. Refreshing all of them cost a
			// repaint per visible row on every tick even when nothing had
			// changed, which on a page that never sits still is most of the
			// work it was doing (issue #920).
			//
			// Per row rather than a blanket Refresh() so the control's own
			// viewport gate applies -- off-screen rows cost nothing -- and so a
			// live sort column still gets the chance to reorder.
			for (Row &row : m_rows) {
				const Row &fresh = *incoming[row.ecid];
				if (row == fresh) {
					continue;
				}
				row = fresh;
				RefreshItemData(static_cast<wxUIntPtr>(row.ecid));
			}
			return;
		}
	}

	// A peer arrived or left, so the row set itself changed and the model has
	// to be rebuilt. The selection is carried across by ECID and therefore
	// still names the same peers afterwards, whatever order the sweep produced.
	const std::vector<wxUIntPtr> selected = GetSelectedItemData();
	ClearItemData();
	m_rows = std::move(rows);
	ReindexRows();
	for (const Row &row : m_rows) {
		AppendItemData(static_cast<wxUIntPtr>(row.ecid));
	}
	FinishBulkLoad();
	SetSelectedItemData(selected);
}

const ClientNameCell *CClientsListCtrl::NameCellFor(wxUIntPtr item) const
{
	const Row *row = RowFor(item);
	return row != nullptr ? &row->nameCell : nullptr;
}

std::vector<PeerIdentity> CClientsListCtrl::SelectedPeers() const
{
	// By ECID: within one daemon process that names exactly this peer. A row is
	// only ever as current as the last sweep, so a miss means the peer has gone.
	//
	// Every row here is a peer we are connected to, so identity is taken from
	// the live client rather than from the row -- there is no case where one is
	// known and the other is not.
	std::vector<PeerIdentity> peers;
	for (wxUIntPtr data : GetSelectedItemData()) {
		const Row *row = RowFor(data);
		if (row == nullptr || row->ecid == 0) {
			continue;
		}
		CClientRef found;
#ifdef CLIENT_GUI
		// The container already holds a reference; copying it links another.
		CClientRef *ref = theApp->clientlist->GetByID(row->ecid);
		if (ref != nullptr && ref->GetClient() != nullptr) {
			found = *ref;
		}
#else
		CUpDownClient *client = theApp->clientlist->FindClientByECID(row->ecid);
		if (client != nullptr) {
			found = CCLIENTREF(client, wxT("CClientsListCtrl::SelectedPeers"));
		}
#endif
		if (!found.IsLinked()) {
			continue;
		}
		PeerIdentity peer;
		peer.hash = found.GetUserHash();
		peer.name = found.GetUserName();
		peer.ip = found.GetIP();
		peer.port = found.GetUserPort();
		peer.client = found;
		peers.push_back(std::move(peer));
	}
	return peers;
}

wxString CClientsListCtrl::GetItemColumnText(wxUIntPtr item, unsigned column) const
{
	const Row *row = RowFor(item);
	if (row == nullptr) {
		return wxEmptyString;
	}

	switch (column) {
	case COLUMN_CLIENTS_NAME:
		if (!row->name.IsEmpty()) {
			return row->name;
		}
		return Uint32toStringIP(row->ip);

	case COLUMN_CLIENTS_SOFTWARE:
		return row->software;

	case COLUMN_CLIENTS_VERSION:
		return row->version;

	case COLUMN_CLIENTS_ADDRESS:
		return CFormat(wxT("%s:%u")) % Uint32toStringIP(row->ip) % row->port;

	case COLUMN_CLIENTS_ORIGIN:
		// OriginToText() hands back the wxTRANSLATE() marker, which is only an
		// extraction hint -- the lookup has to happen here, the way the download
		// list's source column already does it.
		return wxGetTranslation(OriginToText(row->sourceFrom));

	case COLUMN_CLIENTS_FILES:
		return row->files;

	case COLUMN_CLIENTS_UP_SPEED:
		return row->upSpeed ? CastItoSpeed(row->upSpeed) : wxString();

	case COLUMN_CLIENTS_DOWN_SPEED:
		return row->downSpeed > 0.001 ? CastItoSpeed(static_cast<uint32>(row->downSpeed * 1024))
					      : wxString();

	case COLUMN_CLIENTS_SESSION_UP:
		return CastItoXBytes(row->sessionUp);

	case COLUMN_CLIENTS_SESSION_DOWN:
		return CastItoXBytes(row->sessionDown);

	case COLUMN_CLIENTS_TOTAL_UP:
		return CastItoXBytes(row->totalUp);

	case COLUMN_CLIENTS_TOTAL_DOWN:
		return CastItoXBytes(row->totalDown);

	case COLUMN_CLIENTS_RATIO:
		if (row->totalUp == 0 || row->totalDown == 0) {
			return wxEmptyString;
		}
		return CFormat(wxT("%.2f")) %
		       (static_cast<double>(row->totalDown) / static_cast<double>(row->totalUp));

	default:
		return wxEmptyString;
	}
}

bool CClientsListCtrl::IsLiveSortColumn() const
{
	if (m_sort_orders.empty()) {
		return false;
	}
	switch (m_sort_orders.front().first) {
	case COLUMN_CLIENTS_UP_SPEED:
	case COLUMN_CLIENTS_DOWN_SPEED:
	case COLUMN_CLIENTS_SESSION_UP:
	case COLUMN_CLIENTS_SESSION_DOWN:
	case COLUMN_CLIENTS_TOTAL_UP:
	case COLUMN_CLIENTS_TOTAL_DOWN:
	case COLUMN_CLIENTS_RATIO:
		return true;
	default:
		return false;
	}
}

int CClientsListCtrl::CompareItemData(
	wxUIntPtr data1, wxUIntPtr data2, unsigned column, bool WXUNUSED(alt), int modifier) const
{
	const Row *r1 = RowFor(data1);
	const Row *r2 = RowFor(data2);
	if (r1 == nullptr || r2 == nullptr) {
		return 0;
	}

	switch (column) {
	case COLUMN_CLIENTS_UP_SPEED:
		return modifier * CmpAny(r1->upSpeed, r2->upSpeed);
	case COLUMN_CLIENTS_DOWN_SPEED:
		return modifier * CmpAny(r1->downSpeed, r2->downSpeed);
	case COLUMN_CLIENTS_SESSION_UP:
		return modifier * CmpAny(r1->sessionUp, r2->sessionUp);
	case COLUMN_CLIENTS_SESSION_DOWN:
		return modifier * CmpAny(r1->sessionDown, r2->sessionDown);
	case COLUMN_CLIENTS_TOTAL_UP:
		return modifier * CmpAny(r1->totalUp, r2->totalUp);
	case COLUMN_CLIENTS_TOTAL_DOWN:
		return modifier * CmpAny(r1->totalDown, r2->totalDown);
	default:
		return modifier *
		       GetItemColumnText(data1, column).CmpNoCase(GetItemColumnText(data2, column));
	}
}
