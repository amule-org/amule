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
#include <wx/msgdlg.h>

#include <common/Format.h> // Needed for CFormat
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

namespace
{
// Above this a bulk action asks before running. One row must never cost a
// click and a few is plainly deliberate, but Clients -> Known lists every peer
// we have credit for, so a select-all there is thousands of rows and each one
// costs an outbound connection or a rewrite of the friend list.
const size_t kBulkPeerActionPrompt = 10;
} // namespace

bool CClientRowListCtrl::MenuPeer(PeerIdentity &out) const
{
	return m_menuItemValid && PeerForItem(m_menuItem, out);
}

std::vector<PeerIdentity> CClientRowListCtrl::SelectedPeers() const
{
	std::vector<PeerIdentity> peers;
	for (wxUIntPtr data : GetSelectedItemData()) {
		PeerIdentity peer;
		if (PeerForItem(data, peer)) {
			peers.push_back(std::move(peer));
		}
	}
	return peers;
}

void CClientRowListCtrl::OnItemActivated(wxDataViewEvent &event)
{
	if (!event.GetItem().IsOk()) {
		return;
	}
	ShowDetailsForSelection();
}

// Opening details is a read-only act: it never creates a client and never
// opens a connection. A peer we are talking to is snapshotted live, because
// that knows strictly more; otherwise the row's own record is rendered and
// the session fields show as absent.
void CClientRowListCtrl::ShowDetailsForSelection()
{
	const std::vector<PeerIdentity> peers = SelectedPeers();
	if (peers.size() != 1) {
		return;
	}
	const PeerIdentity &peer = peers.front();
	if (peer.client.IsLinked()) {
		ClientActionShowDetails(this, { peer.client });
	} else if (peer.hasDetail) {
		CClientDetailDialog(this, peer.detail).ShowModal();
	}
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

	// A peer we are not connected to still gets a menu. Friending and the
	// friend slot are persistent and need no connection at all; browsing and
	// messaging open one when the user picks them.
	//
	// Only the row under the cursor is resolved. The menu describes that one,
	// and building the whole selection to read its first entry would resolve
	// every other selected row for nothing.
	if (!event.GetItem().IsOk()) {
		return;
	}
	m_menuItem = ItemAt(GetModelRow(event.GetItem()));
	PeerIdentity peer;
	m_menuItemValid = PeerForItem(m_menuItem, peer);
	if (!m_menuItemValid) {
		return;
	}

	// The builder omits "Swap to this file": it acts on an A4AF source of one
	// particular download, which is a per-file notion neither of these lists
	// has.
	wxMenu *menu = BuildPeerContextMenu(peer);
	PopupMenu(menu, event.GetPosition());
	delete menu;
}

// Both lists are multi-select (CMuleDataViewCtrl forces wxDV_MULTIPLE) and the
// connected-client paths act on the whole selection, so these do too. But the
// history list is the entire credit store rather than the handful of peers we
// happen to be talking to, and the menu is built for one row while the action
// runs on all of them, so a large selection says so first. Same shape as the
// shared-files media refresh: one row never costs a click, and what is about to
// happen is stated in full.
bool CClientRowListCtrl::ConfirmBulkPeerAction(size_t count, const wxString &message)
{
	if (count <= kBulkPeerActionPrompt) {
		return true;
	}
	return wxMessageBox(message, _("Multiple selection"), wxYES_NO | wxICON_QUESTION, this) == wxYES;
}

void CClientRowListCtrl::OnViewFiles(wxCommandEvent &WXUNUSED(event))
{
	const std::vector<PeerIdentity> peers = SelectedPeers();
	if (peers.empty()) {
		return;
	}
	// Each one opens its own connection and its own browse tab.
	wxString message = CFormat(wxPLURAL("Request the shared files of %u client?",
				   "Request the shared files of %u clients?",
				   peers.size())) %
			   peers.size();
	message << wxT("\n\n") << _("A connection is opened to each of them.");
	if (!ConfirmBulkPeerAction(peers.size(), message)) {
		return;
	}
	for (const PeerIdentity &peer : peers) {
		PeerActionViewFiles(peer);
	}
}

void CClientRowListCtrl::OnAddFriend(wxCommandEvent &WXUNUSED(event))
{
	const std::vector<PeerIdentity> peers = SelectedPeers();
	if (peers.empty()) {
		return;
	}
	// Each row is added or removed on its own, and the friend list is written
	// out for every one of them.
	const wxString message = CFormat(wxPLURAL("Add or remove %u client from your friend list?",
					 "Add or remove %u clients from your friend list?",
					 peers.size())) %
				 peers.size();
	if (!ConfirmBulkPeerAction(peers.size(), message)) {
		return;
	}
	PeerActionToggleFriends(peers);
}

void CClientRowListCtrl::OnSetFriendslot(wxCommandEvent &evt)
{
	// The row the menu was built for, not whatever the selection happens to
	// be: the checkbox describes one peer, so the slot has to land on that
	// one. The count comes from the control rather than from resolving every
	// selected row, which the warning is all it is needed for.
	PeerIdentity peer;
	if (!MenuPeer(peer)) {
		return;
	}
	PeerActionSetFriendSlot(this, peer, evt.IsChecked(), GetSelectedItemsCount());
}

void CClientRowListCtrl::OnSendMessage(wxCommandEvent &WXUNUSED(event))
{
	// Single row only, as the connected-client path has always been, and the
	// row the menu named rather than a re-resolved selection.
	PeerIdentity peer;
	if (GetSelectedItemsCount() != 1 || !MenuPeer(peer)) {
		return;
	}
	PeerActionSendMessage(peer);
}

void CClientRowListCtrl::OnViewClientInfo(wxCommandEvent &WXUNUSED(event))
{
	ShowDetailsForSelection();
}
// File_checked_for_headers
