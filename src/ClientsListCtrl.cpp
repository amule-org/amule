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

#include <map>

#include <common/Format.h>  // Needed for CFormat
#include <common/MenuIDs.h> // Needed for MP_DETAIL etc.

#include "amule.h"                // Needed for theApp
#include "ClientContextActions.h" // Needed for BuildClientContextMenu, ClientAction*
#include "ClientDetailDialog.h"   // Needed for CClientDetailDialog
#include "ClientList.h"           // Needed for CClientList::FindClientByECID
#include "ClientRef.h"            // Needed for CClientRef
#include "DataToText.h"           // Needed for GetSoftName, OriginToText
#include "MuleBarRenderer.h"      // Needed for CBarFillSpec, CMuleBarRenderer
#include "OtherFunctions.h"       // Needed for CastItoXBytes, CastItoSpeed
#include "muuli_wdr.h"            // Needed for ID_CLIENTSLIST

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

wxBEGIN_EVENT_TABLE(CClientsListCtrl, CMuleVirtualDataViewCtrl)
	EVT_DATAVIEW_ITEM_ACTIVATED(wxID_ANY, CClientsListCtrl::OnItemActivated)
	EVT_DATAVIEW_ITEM_CONTEXT_MENU(wxID_ANY, CClientsListCtrl::OnItemRightClicked)

	EVT_MENU(MP_SHOWLIST, CClientsListCtrl::OnViewFiles)
	EVT_MENU(MP_ADDFRIEND, CClientsListCtrl::OnAddFriend)
	EVT_MENU(MP_FRIENDSLOT, CClientsListCtrl::OnSetFriendslot)
	EVT_MENU(MP_SENDMESSAGE, CClientsListCtrl::OnSendMessage)
	EVT_MENU(MP_DETAIL, CClientsListCtrl::OnViewClientInfo)
wxEND_EVENT_TABLE()

CClientsListCtrl::CClientsListCtrl(
	wxWindow *parent, int id, const wxPoint &pos, wxSize size, int flags, const wxString &tableName)
: CMuleVirtualDataViewCtrl(parent, id, pos, size, flags)
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
	// index+1, so 0 stays "no item".
	if (item == 0 || item > m_rows.size()) {
		return nullptr;
	}
	return &m_rows[item - 1];
}

void CClientsListCtrl::SetClients(std::vector<Row> &&rows)
{
	// The common case by far: the same peers as last second, with new numbers.
	// Overwrite the rows where they sit and refresh them. Rebuilding the model
	// instead -- which is all this used to do -- discards the scroll position
	// and the selection every time the poll lands, so a list a person is
	// reading jumps back to the top once a second.
	//
	// Matched on ECID rather than position: the sweep walks a container whose
	// order is not stable, so equal contents can arrive in a different order.
	if (rows.size() == m_rows.size()) {
		std::map<uint32, const Row *> incoming;
		for (const Row &row : rows) {
			incoming[row.ecid] = &row;
		}
		bool sameSet = incoming.size() == m_rows.size();
		for (const Row &row : m_rows) {
			if (!sameSet) {
				break;
			}
			sameSet = incoming.count(row.ecid) != 0;
		}
		if (sameSet) {
			for (Row &row : m_rows) {
				row = *incoming[row.ecid];
			}
			// Per row rather than a blanket Refresh() so the control's own
			// viewport gate applies -- off-screen rows cost nothing -- and so a
			// live sort column still gets the chance to reorder.
			for (size_t i = 0; i < m_rows.size(); ++i) {
				RefreshItemData(static_cast<wxUIntPtr>(i + 1));
			}
			return;
		}
	}

	// A peer arrived or left: the row set itself changed, so rebuild. Selection
	// is carried across; the scroll position is the price of a structural
	// change, and one only happens when the set of peers actually moves.
	const std::vector<wxUIntPtr> selected = GetSelectedItemData();
	ClearItemData();
	m_rows = std::move(rows);
	for (size_t i = 0; i < m_rows.size(); ++i) {
		AppendItemData(static_cast<wxUIntPtr>(i + 1));
	}
	FinishBulkLoad();
	SetSelectedItemData(selected);
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
		return OriginToText(row->sourceFrom);

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

void CClientsListCtrl::GetItemBarFill(wxUIntPtr data, unsigned column, CBarFillSpec &out) const
{
	if (column != COLUMN_CLIENTS_NAME) {
		return;
	}
	const Row *row = RowFor(data);
	if (row == nullptr) {
		return;
	}
	// The renderer needs the snapshot, not spans. Safe to hand out an address
	// into m_rows: the vector is only ever replaced wholesale by SetClients(),
	// which runs on the main thread between paints.
	out = CBarFillSpec(reinterpret_cast<wxUIntPtr>(&row->nameCell), 0, {});
}

std::vector<CClientRef> CClientsListCtrl::SelectedClients() const
{
	std::vector<CClientRef> clients;
	for (wxUIntPtr data : GetSelectedItemData()) {
		const Row *row = RowFor(data);
		if (row == nullptr || row->ecid == 0) {
			continue;
		}
#ifdef CLIENT_GUI
		CClientRef *ref = theApp->clientlist->GetByID(row->ecid);
		CUpDownClient *client = ref != nullptr ? ref->GetClient() : nullptr;
#else
		CUpDownClient *client = theApp->clientlist->FindClientByECID(row->ecid);
#endif
		if (client != nullptr) {
			clients.push_back(CCLIENTREF(client, wxT("CClientsListCtrl::SelectedClients")));
		}
	}
	return clients;
}

void CClientsListCtrl::OnItemRightClicked(wxDataViewEvent &event)
{
	if (event.GetItem().IsOk()) {
		wxDataViewItemArray selection;
		GetSelections(selection);
		if (selection.Index(event.GetItem()) == wxNOT_FOUND) {
			UnselectAll();
			Select(event.GetItem());
		}
	}

	const std::vector<CClientRef> clients = SelectedClients();
	if (clients.empty()) {
		return;
	}

	// No swap-to-file: that acts on an A4AF source of one particular download,
	// which is a per-file notion this list does not have.
	wxMenu *menu = BuildClientContextMenu(clients.front(), false);
	PopupMenu(menu, event.GetPosition());
	delete menu;
}

void CClientsListCtrl::OnViewFiles(wxCommandEvent &WXUNUSED(event))
{
	ClientActionViewFiles(SelectedClients());
}

void CClientsListCtrl::OnAddFriend(wxCommandEvent &WXUNUSED(event))
{
	ClientActionToggleFriend(SelectedClients());
}

void CClientsListCtrl::OnSetFriendslot(wxCommandEvent &evt)
{
	ClientActionSetFriendSlot(this, SelectedClients(), evt.IsChecked());
}

void CClientsListCtrl::OnSendMessage(wxCommandEvent &WXUNUSED(event))
{
	ClientActionSendMessage(SelectedClients());
}

void CClientsListCtrl::OnViewClientInfo(wxCommandEvent &WXUNUSED(event))
{
	ClientActionShowDetails(this, SelectedClients());
}

void CClientsListCtrl::OnItemActivated(wxDataViewEvent &event)
{
	if (!event.GetItem().IsOk()) {
		return;
	}
	const Row *row = RowFor(ItemAt(GetModelRow(event.GetItem())));
	if (row == nullptr || row->ecid == 0) {
		return;
	}

	// Resolve now rather than holding the peer: a row is a snapshot, and the
	// peer it describes may have gone since the last sweep. A miss simply
	// means there is nothing to show.
#ifdef CLIENT_GUI
	CClientRef *ref = theApp->clientlist->GetByID(row->ecid);
	CUpDownClient *client = ref != nullptr ? ref->GetClient() : nullptr;
#else
	CUpDownClient *client = theApp->clientlist->FindClientByECID(row->ecid);
#endif
	if (client == nullptr) {
		return;
	}
	// The CClientRef is what keeps the peer alive for as long as the modal
	// dialog is up.
	CClientDetailDialog(this, CCLIENTREF(client, wxT("CClientsListCtrl::OnItemActivated"))).ShowModal();
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
