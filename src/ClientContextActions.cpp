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

namespace
{

// Asks for the message and hands it to the chat window. Shared so the live and
// stored-row paths cannot drift: all either has is a display name and a GUI_ID.
void PromptAndSendChatMessage(const wxString &userName, uint64 userID)
{
	const wxString message = ::wxGetTextFromUser(_("Send message to user"), _("Message to send:"));
	if (!message.IsEmpty()) {
		theApp->amuledlg->m_chatwnd->SendMessage(message, userName, userID);
	}
}

// Holds the friend list's write open for as long as it is in scope. Works in
// both builds: CFriendListRem answers the same two calls with no-ops, because
// the daemon owns the file there.
class FriendListBatch
{
public:
	FriendListBatch() { theApp->friendlist->BeginBatch(); }
	~FriendListBatch() { theApp->friendlist->EndBatch(); }
	FriendListBatch(const FriendListBatch &) = delete;
	FriendListBatch &operator=(const FriendListBatch &) = delete;
};

// Only one friend slot exists, so a wider selection loses all but the first.
void WarnIfMultipleFriendSlot(wxWindow *parent, size_t selected)
{
	if (selected > 1) {
		wxMessageBox(_("You are not allowed to set more than one friend slot.\n Only one slot was "
			       "assigned."),
			_("Multiple selection"),
			wxOK | wxICON_ERROR,
			parent);
	}
}

} // namespace

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
	//
	// LookupFriend(), never FindFriend(): the latter adopts the hash onto an
	// address-only record and saves the file, and merely opening a menu must
	// not write to disk.
	CFriend *known = theApp->friendlist->LookupFriend(peer.hash, peer.ip, peer.port);

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
	// Only browsing is out of amulegui's reach, because EC names a browse
	// target by ECID and this peer has no daemon-side object. Chat addresses
	// by GUI_ID, which is the address itself, so it works in both builds.
	// Friending is unaffected either way: it is a property of our own list.
	// Unfriending only needs the record we already have; friending needs an
	// address to store, and both connection entries need somewhere to dial.
	menu->Enable(MP_ADDFRIEND, known != nullptr || peer.CanBeFriended());
	menu->Enable(MP_SENDMESSAGE, peer.CanOpenConnection());
	menu->Enable(MP_SHOWLIST, peer.CanOpenConnection() && PeerBrowseIsPossible(peer));

	return menu;
}

// Whether this build can browse a peer it is not already connected to.
//
// Monolithic aMule owns its client list and can make one from an address.
// amulegui cannot: EC names a browse target by ECID, and a peer that is
// neither connected nor a friend has no daemon-side object to have one.
//
// Chat is deliberately NOT covered by this. EC_OP_CHAT_SEND takes a bare
// GUI_ID, which is built from the address alone, so messaging a peer we only
// hold an address for works in both builds.
bool PeerBrowseIsPossible(const PeerIdentity &peer)
{
#ifndef CLIENT_GUI
	(void)peer;
	return true;
#else
	// EC names a browse target by ECID. A peer we are connected to has one as
	// a client, and a friend has one as a friend record, and CFriendListRem
	// already browses both. Only a peer that is neither has no daemon-side
	// object to name, which is the case this cannot reach.
	return peer.client.IsLinked() || PeerIsFriend(peer);
#endif
}

bool PeerActionViewFiles(const PeerIdentity &peer)
{
	if (peer.client.IsLinked()) {
		ClientActionViewFiles({ peer.client });
		return true;
	}
	if (!peer.CanOpenConnection()) {
		return false;
	}
#ifdef CLIENT_GUI
	// A friend can be named on the wire by its ECID even when we are not
	// connected to it, which is the one handle a stored row can have.
	if (CFriend *known = theApp->friendlist->LookupFriend(peer.hash, peer.ip, peer.port)) {
		theApp->friendlist->RequestSharedFileList(known);
		return true;
	}
	// Otherwise there is nothing to name it with, so say so rather than
	// leaving the caller to assume the browse was asked for.
	return false;
#else
	// Only now is a client made and a connection opened: the user asked to
	// browse, which cannot be answered from the stored record. Passing the
	// hash lets the call reuse the client it made last time instead of
	// stacking up a new one per click.
	ClientActionViewFiles(
		{ theApp->clientlist->CreateForAddress(peer.hash, peer.ip, peer.port, peer.name) });
	return true;
#endif
}

void PeerActionSendMessage(const PeerIdentity &peer)
{
	if (peer.client.IsLinked()) {
		ClientActionSendMessage({ peer.client });
		return;
	}
	if (!peer.CanOpenConnection()) {
		return;
	}
	// The address is the whole target: monolithic looks the client up by it,
	// and amulegui sends the GUI_ID over EC for the daemon to resolve. Either
	// way CClientList::SendChatMessage() makes the client if there is none,
	// so there is nothing to pre-create here.
	//
	// The hash stands in for a missing name here, and only here: this label
	// titles the chat tab and is not stored anywhere, unlike the name that
	// goes into a friend record.
	PromptAndSendChatMessage(
		peer.name.IsEmpty() ? peer.hash.Encode() : peer.name, GUI_ID(peer.ip, peer.port));
}

CFriend *FriendFor(const PeerIdentity &peer)
{
	// Through the live client's own linkage first, then the stored identity.
	// One step for both paths: implementing this per caller is how the menu
	// and an action end up disagreeing about whether a peer is a friend.
	if (peer.client.IsLinked()) {
		CClientRef &live = const_cast<CClientRef &>(peer.client);
		if (CFriend *linked = live.GetFriend()) {
			return linked;
		}
	}
	return theApp->friendlist->LookupFriend(peer.hash, peer.ip, peer.port);
}

bool PeerIsFriend(const PeerIdentity &peer)
{
	return FriendFor(peer) != nullptr;
}

void PeerActionSetFriends(const std::vector<PeerIdentity> &peers, bool addThem)
{
	// One write for the whole run. CFriendList saves after every add and
	// remove, so without this a large selection rewrites emfriends.met once
	// per row, on the GUI thread, with the file growing as it goes.
	//
	// Scoped, not a bare pair of calls: a batch left open makes every later
	// SaveList() a silent no-op, including the one in ~CFriendList(), so a
	// single escape from this loop would lose the friend list on exit.
	FriendListBatch batch;
	for (const PeerIdentity &peer : peers) {
		CFriend *known = FriendFor(peer);
		// One direction for the whole run, taken from the entry the user
		// picked. Toggling per row means a selection holding both friends and
		// strangers does the opposite of its own label to half of it, and
		// removing a friend is the loss of something they curated.
		if (addThem) {
			if (known != nullptr) {
				continue;
			}
			if (peer.client.IsLinked()) {
				// The client-aware overload, which links the record to the
				// live client. Storing the address alone leaves the friend
				// looking offline for the rest of a session we are already
				// talking through, and leaves its friend slot unsettable,
				// because that entry is gated on the linkage.
				theApp->friendlist->AddFriend(peer.client);
			} else if (peer.CanBeFriended()) {
				theApp->friendlist->AddFriend(peer.hash, peer.ip, peer.port, peer.name);
			}
		} else if (known != nullptr) {
			theApp->friendlist->RemoveFriend(known);
		}
	}
}

void PeerActionSetFriendSlot(wxWindow *parent, const PeerIdentity &peer, bool checked, size_t selected)
{
	// Resolve the friend the same way the menu decided whether to offer this
	// entry: through the live client's own linkage when there is one, and
	// from the stored record otherwise. Resolving it differently is how the
	// menu ends up describing one peer while the action runs on another.
	// Either way the slot is a property of our own list, so it is settable
	// on a peer that is not connected, which is the case this list covers.
	theApp->friendlist->SetFriendSlot(FriendFor(peer), checked);

	WarnIfMultipleFriendSlot(parent, selected);
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

	WarnIfMultipleFriendSlot(parent, clients.size());
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

	PromptAndSendChatMessage(userName, userID);
}

void ClientActionShowDetails(wxWindow *parent, const std::vector<CClientRef> &clients)
{
	if (clients.size() != 1) {
		return;
	}
	CClientDetailDialog(parent, clients.front()).ShowModal();
}
// File_checked_for_headers
