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

#include "ClientRowListCtrl.h" // Interface declarations

#include <wx/menu.h>

#include <common/MenuIDs.h>

#include "ClientContextActions.h" // Needed for BuildClientContextMenu, ClientAction*
#include "MuleBarRenderer.h"      // Needed for CBarFillSpec

wxBEGIN_EVENT_TABLE(CClientRowListCtrl, CMuleVirtualDataViewCtrl)
	EVT_DATAVIEW_ITEM_ACTIVATED(wxID_ANY, CClientRowListCtrl::OnItemActivated)
	EVT_DATAVIEW_ITEM_CONTEXT_MENU(wxID_ANY, CClientRowListCtrl::OnItemRightClicked)

	EVT_MENU(MP_SHOWLIST, CClientRowListCtrl::OnViewFiles)
	EVT_MENU(MP_ADDFRIEND, CClientRowListCtrl::OnAddFriend)
	EVT_MENU(MP_FRIENDSLOT, CClientRowListCtrl::OnSetFriendslot)
	EVT_MENU(MP_SENDMESSAGE, CClientRowListCtrl::OnSendMessage)
	EVT_MENU(MP_DETAIL, CClientRowListCtrl::OnViewClientInfo)
wxEND_EVENT_TABLE()

CClientRowListCtrl::CClientRowListCtrl(wxWindow *parent, int id, const wxPoint &pos, wxSize size, int flags)
: CMuleVirtualDataViewCtrl(parent, id, pos, size, flags)
{
}

void CClientRowListCtrl::GetItemBarFill(wxUIntPtr data, unsigned column, CBarFillSpec &out) const
{
	if (column != NameColumn()) {
		return;
	}
	const ClientNameCell *cell = NameCellFor(data);
	if (cell == nullptr) {
		return;
	}
	// The renderer needs the snapshot, not spans. Safe to hand out its address:
	// the rows are only ever replaced wholesale, on the main thread, between
	// paints.
	out = CBarFillSpec(reinterpret_cast<wxUIntPtr>(cell), 0, {});
}

void CClientRowListCtrl::OnItemActivated(wxDataViewEvent &event)
{
	if (!event.GetItem().IsOk()) {
		return;
	}
	ClientActionShowDetails(this, SelectedClients());
}

void CClientRowListCtrl::OnItemRightClicked(wxDataViewEvent &event)
{
	if (event.GetItem().IsOk()) {
		wxDataViewItemArray selection;
		GetSelections(selection);
		if (selection.Index(event.GetItem()) == wxNOT_FOUND) {
			UnselectAll();
			Select(event.GetItem());
		}
	}

	// Nothing resolved means the rows name peers we are not connected to, which
	// every entry on this menu needs. No menu rather than one that cannot act.
	const std::vector<CClientRef> clients = SelectedClients();
	if (clients.empty()) {
		return;
	}

	// The builder omits "Swap to this file": it acts on an A4AF source of one
	// particular download, which is a per-file notion neither of these lists
	// has.
	wxMenu *menu = BuildClientContextMenu(clients.front());
	PopupMenu(menu, event.GetPosition());
	delete menu;
}

void CClientRowListCtrl::OnViewFiles(wxCommandEvent &WXUNUSED(event))
{
	ClientActionViewFiles(SelectedClients());
}

void CClientRowListCtrl::OnAddFriend(wxCommandEvent &WXUNUSED(event))
{
	ClientActionToggleFriend(SelectedClients());
}

void CClientRowListCtrl::OnSetFriendslot(wxCommandEvent &evt)
{
	ClientActionSetFriendSlot(this, SelectedClients(), evt.IsChecked());
}

void CClientRowListCtrl::OnSendMessage(wxCommandEvent &WXUNUSED(event))
{
	ClientActionSendMessage(SelectedClients());
}

void CClientRowListCtrl::OnViewClientInfo(wxCommandEvent &WXUNUSED(event))
{
	ClientActionShowDetails(this, SelectedClients());
}
// File_checked_for_headers
