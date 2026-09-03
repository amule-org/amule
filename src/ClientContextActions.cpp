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

#include "ClientContextActions.h" // Interface declarations

#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/textdlg.h>

#include <common/MenuIDs.h>

#include "amule.h"              // Needed for theApp
#include "amuleDlg.h"           // Needed for CamuleDlg
#include "ChatWnd.h"            // Needed for CChatWnd::SendMessage
#include "ClientDetailDialog.h" // Needed for CClientDetailDialog
#include "ClientList.h"         // Needed for CClientList::CreateForAddress
#include "Friend.h"             // Needed for CFriend
#include "FriendList.h"         // Needed for CFriendList
#include "OtherFunctions.h"     // Needed for GUI_ID
#include "SearchDlg.h"          // Needed for CSearchDlg::ActivateBrowseTabIfOpen

wxMenu *BuildClientContextMenu(const CClientRef &client)
{
	// const_cast because the accessors this menu reads are non-const on
	// CClientRef; nothing here modifies the peer.
	CClientRef &c = const_cast<CClientRef &>(client);

	wxMenu *menu = new wxMenu(_("Clients"));
	menu->Append(MP_DETAIL, _("Show &Details"));
	menu->Append(MP_ADDFRIEND, c.IsFriend() ? _("Remove from friends") : _("Add to Friends"));

	menu->AppendCheckItem(MP_FRIENDSLOT, _("Establish Friend Slot"));
	if (c.IsFriend()) {
		menu->Enable(MP_FRIENDSLOT, true);
		menu->Check(MP_FRIENDSLOT, c.GetFriendSlot());
	} else {
		menu->Enable(MP_FRIENDSLOT, false);
	}

	menu->Append(MP_SHOWLIST, _("View Files"));
	menu->Append(MP_SENDMESSAGE, _("Send message"));

	// We need a valid IP if we are to message the client.
	menu->Enable(MP_SENDMESSAGE, c.GetIP() != 0);
	menu->Enable(MP_SHOWLIST, !c.HasDisabledSharedFiles());

	return menu;
}

wxMenu *BuildPeerContextMenu(const PeerIdentity &peer)
{
	// A peer we are connected to knows strictly more about itself than the
	// stored record does, so defer to the existing builder whenever there is
	// one rather than keeping two versions of these rules.
	if (peer.client.IsLinked()) {
		return BuildClientContextMenu(peer.client);
	}

	// Friendship and the friend slot are ours, not the peer's: both live in
	// emfriends.met and apply next time it connects.
	CFriend *known = theApp->friendlist->FindFriend(peer.hash, peer.ip, peer.port);

	wxMenu *menu = new wxMenu(_("Clients"));
	menu->Append(MP_DETAIL, _("Show &Details"));
	menu->Append(MP_ADDFRIEND, known ? _("Remove from friends") : _("Add to Friends"));

	menu->AppendCheckItem(MP_FRIENDSLOT, _("Establish Friend Slot"));
	if (known) {
		menu->Enable(MP_FRIENDSLOT, true);
		menu->Check(MP_FRIENDSLOT, known->HasFriendSlot());
	} else {
		menu->Enable(MP_FRIENDSLOT, false);
	}

	menu->Append(MP_SHOWLIST, _("View Files"));
	menu->Append(MP_SENDMESSAGE, _("Send message"));

	// Both open a connection, so both need somewhere to open it to. Whether
	// the peer forbids browsing is live state we do not have, and an unknown
	// is not a refusal, so View Files stays offered on that count.
	//
	// amulegui cannot offer either for a peer that is not connected: the
	// daemon owns the clients, and there is no EC operation for "browse this
	// address" or "chat to this address". Friending is unaffected -- it is a
	// property of our own list and CFriendListRem carries it over EC.
	const bool canOpenConnection = PeerConnectionsArePossible() && peer.ip != 0;
	menu->Enable(MP_SENDMESSAGE, canOpenConnection);
	menu->Enable(MP_SHOWLIST, canOpenConnection);

	return menu;
}

// Whether this build can reach a peer it is not already connected to.
//
// Monolithic aMule owns its client list and can make one from an address.
// amulegui does not: the daemon owns the clients, and EC has no "browse this
// address" or "chat to this address" operation to stand in for it.
bool PeerConnectionsArePossible()
{
#ifdef CLIENT_GUI
	return false;
#else
	return true;
#endif
}

void PeerActionViewFiles(const PeerIdentity &peer)
{
	if (peer.client.IsLinked()) {
		ClientActionViewFiles({ peer.client });
		return;
	}
#ifndef CLIENT_GUI
	if (peer.ip == 0) {
		return;
	}
	// Only now is a client made and a connection opened: the user asked to
	// browse, which cannot be answered from the stored record.
	ClientActionViewFiles({ theApp->clientlist->CreateForAddress(peer.ip, peer.port, peer.name) });
#endif
}

void PeerActionSendMessage(const PeerIdentity &peer)
{
	if (peer.client.IsLinked()) {
		ClientActionSendMessage({ peer.client });
		return;
	}
#ifndef CLIENT_GUI
	if (peer.ip == 0) {
		return;
	}
	ClientActionSendMessage({ theApp->clientlist->CreateForAddress(peer.ip, peer.port, peer.name) });
#endif
}

void PeerActionToggleFriend(const PeerIdentity &peer)
{
	if (peer.client.IsLinked()) {
		ClientActionToggleFriend({ peer.client });
		return;
	}
	// No connection either way: a friend record is hash, name and last known
	// address, all of which the row already has.
	CFriend *known = theApp->friendlist->FindFriend(peer.hash, peer.ip, peer.port);
	if (known) {
		theApp->friendlist->RemoveFriend(known);
	} else {
		theApp->friendlist->AddFriend(peer.hash, peer.ip, peer.port, peer.name);
	}
}

void ClientActionViewFiles(const std::vector<CClientRef> &clients)
{
	// Browse each selected peer, opening one result tab per peer. If a peer's
	// listing is already open in the Search panel, switch to that tab instead
	// of re-requesting -- a second request would duplicate the results in the
	// existing tab. Only once the tab is closed does a fresh request go out.
	for (const CClientRef &client : clients) {
		CClientRef &c = const_cast<CClientRef &>(client);
		if (!(theApp->amuledlg && theApp->amuledlg->m_searchwnd &&
			    theApp->amuledlg->m_searchwnd->ActivateBrowseTabIfOpen(c.ECID()))) {
			c.RequestSharedFileList();
		}
	}
}

void ClientActionToggleFriend(const std::vector<CClientRef> &clients)
{
	for (const CClientRef &client : clients) {
		CClientRef &c = const_cast<CClientRef &>(client);
		if (c.IsFriend()) {
			theApp->friendlist->RemoveFriend(c.GetFriend());
		} else {
			theApp->friendlist->AddFriend(c);
		}
	}
}

void ClientActionSetFriendSlot(wxWindow *parent, const std::vector<CClientRef> &clients, bool checked)
{
	if (clients.empty()) {
		return;
	}
	CClientRef &first = const_cast<CClientRef &>(clients.front());
	theApp->friendlist->SetFriendSlot(first.GetFriend(), checked);

	if (clients.size() > 1) {
		wxMessageBox(_("You are not allowed to set more than one friend slot.\n Only one slot was "
			       "assigned."),
			_("Multiple selection"),
			wxOK | wxICON_ERROR,
			parent);
	}
}

void ClientActionSendMessage(const std::vector<CClientRef> &clients)
{
	if (clients.size() != 1) {
		return;
	}
	CClientRef &source = const_cast<CClientRef &>(clients.front());

	// These values are cached, since calling wxGetTextFromUser will start an
	// event-loop, in which the client may be deleted.
	const wxString userName = source.GetUserName();
	const uint64 userID = GUI_ID(source.GetIP(), source.GetUserPort());

	const wxString message = ::wxGetTextFromUser(_("Send message to user"), _("Message to send:"));
	if (!message.IsEmpty()) {
		theApp->amuledlg->m_chatwnd->SendMessage(message, userName, userID);
	}
}

void ClientActionShowDetails(wxWindow *parent, const std::vector<CClientRef> &clients)
{
	if (clients.size() != 1) {
		return;
	}
	CClientDetailDialog(parent, clients.front()).ShowModal();
}
// File_checked_for_headers
