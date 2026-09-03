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

#ifndef CLIENTCONTEXTACTIONS_H
#define CLIENTCONTEXTACTIONS_H

#include <vector>

#include "ClientRef.h"    // Needed for CClientRef
#include "PeerIdentity.h" // Needed for PeerIdentity

class wxMenu;
class wxWindow;

/**
 * The right-click menu offered on a peer, and what its entries do.
 *
 * Free functions rather than a base class because the lists that offer this
 * menu do not share a row type: the per-file lists hold an owning CClientRef
 * per row, the global clients list holds a value snapshot and resolves the peer
 * by ECID when something is actually done to it. All they have in common is the
 * CClientRef they end up with, which is exactly what these take.
 */

/**
 * Build the menu for `client`, with the entries every caller can act on.
 *
 * "Swap to this file" is not among them: it needs a file in context, so the
 * per-file lists append it themselves.
 *
 * Caller owns the returned menu.
 */
wxMenu *BuildClientContextMenu(const CClientRef &client);

/**
 * The same menu for a peer we are not connected to.
 *
 * Everything on it works from the stored record: friending and the friend
 * slot are persistent, and browsing or messaging opens a connection when the
 * user picks them. Only the friend slot needs the peer to be a friend
 * already, which is a property of our own list rather than of the connection.
 */
wxMenu *BuildPeerContextMenu(const PeerIdentity &peer);

/**
 * Whether this build can browse a peer it is not already connected to.
 *
 * False in amulegui: EC names a browse target by ECID, which a peer that is
 * neither connected nor a friend does not have. Chat is not covered by this
 * and works in both builds, because EC_OP_CHAT_SEND addresses by GUI_ID.
 */
bool PeerBrowseIsPossible();

//! Browse a peer we are not connected to, opening a connection to do it.
void PeerActionViewFiles(const PeerIdentity &peer);

//! Friend or unfriend a peer from its stored identity. Opens no connection.
void PeerActionToggleFriend(const PeerIdentity &peer);

//! Message a peer we are not connected to, opening a connection to do it.
void PeerActionSendMessage(const PeerIdentity &peer);

/**
 * Grant or revoke the friend slot for the first selected peer.
 *
 * Resolves the friend record from the peer's own identity rather than from a
 * live client, so it works for a friend that is currently offline. Warns, as
 * the connected-client path does, when more than one row was selected.
 */
/**
 * Grant or revoke the friend slot for one peer.
 *
 * Resolves the friend record the same way the menu did, so the entry and the
 * action cannot describe different peers. `selected` is the size of the
 * selection, used only to warn that a wider one still got a single slot.
 */
void PeerActionSetFriendSlot(wxWindow *parent, const PeerIdentity &peer, bool checked, size_t selected);

//! Friend or unfriend every peer given, writing the friend list once.
void PeerActionToggleFriends(const std::vector<PeerIdentity> &peers);

//! Browse each peer's shared files, reusing an already-open tab per peer.
void ClientActionViewFiles(const std::vector<CClientRef> &clients);

//! Add each peer to the friend list, or remove it if it is already a friend.
void ClientActionToggleFriend(const std::vector<CClientRef> &clients);

/**
 * Give the first selected peer the friend slot.
 *
 * Only one peer can hold it, so a multiple selection is applied to the first
 * and the caller's window is told about it.
 */
void ClientActionSetFriendSlot(wxWindow *parent, const std::vector<CClientRef> &clients, bool checked);

//! Prompt for a message and send it. Single selection only.
void ClientActionSendMessage(const std::vector<CClientRef> &clients);

//! Open the details dialog. Single selection only.
void ClientActionShowDetails(wxWindow *parent, const std::vector<CClientRef> &clients);

#endif // CLIENTCONTEXTACTIONS_H
// File_checked_for_headers
