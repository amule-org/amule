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

class CFriend;

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
 * Whether this build can browse this particular peer.
 *
 * Always true in monolithic. In amulegui it depends on the peer: EC names a
 * browse target by ECID, so a connected peer or a friend can be named, and
 * one that is neither cannot. Chat is not covered by this and works in both
 * builds, because EC_OP_CHAT_SEND addresses by GUI_ID.
 */
bool PeerBrowseIsPossible(const PeerIdentity &peer);

//! The friend record for a live client, or nullptr. Linkage, then identity.
CFriend *FriendForClient(const CClientRef &client);

//! The friend record for this peer, or nullptr. Live linkage first.
CFriend *FriendFor(const PeerIdentity &peer);

//! Whether this peer is already on our friend list.
bool PeerIsFriend(const PeerIdentity &peer);

/**
 * Browse a peer, opening a connection to it if we are not already talking.
 *
 * False when nothing was asked: amulegui cannot name a peer it has no ECID
 * for, and the caller reports what it skipped rather than failing silently.
 */
bool PeerActionViewFiles(const PeerIdentity &peer);

//! Message a peer we are not connected to, opening a connection to do it.
void PeerActionSendMessage(const PeerIdentity &peer);

/**
 * Grant or revoke the friend slot for one peer.
 *
 * Resolves the friend record the same way the menu did, so the entry and the
 * action cannot describe different peers. `selected` is the size of the
 * selection, used only to warn that a wider one still got a single slot.
 */
void PeerActionSetFriendSlot(wxWindow *parent, const PeerIdentity &peer, bool checked, size_t selected);

/**
 * Friend or unfriend every peer given, writing the friend list once.
 *
 * One direction for the whole run rather than a per-row toggle: the menu
 * entry is labelled from a single row, and a selection holding both friends
 * and strangers would otherwise do the opposite of that label to half of it.
 *
 * Returns how many were left out for having no address to store, so the
 * caller can say so rather than reporting a count it did not act on.
 */
size_t PeerActionSetFriends(const std::vector<PeerIdentity> &peers, bool addThem);

//! Browse each peer's shared files, reusing an already-open tab per peer.
void ClientActionViewFiles(const std::vector<CClientRef> &clients);

/**
 * Whether a bulk action over `count` rows should go ahead.
 *
 * True without asking for a small selection; larger ones are confirmed.
 */
bool ConfirmBulkPeerAction(wxWindow *parent, size_t count, const wxString &message);

//! Asks before a wide friend action, wording it for the direction taken.
bool ConfirmFriendAction(wxWindow *parent, size_t count, bool addThem);

//! Says how many rows a friend action left out for having no address.
void ReportFriendSkips(size_t skipped);

/**
 * Asks before browsing a wide selection.
 *
 * Always says a connection is opened to each peer: whether one is depends on
 * the peer rather than on the list asking, and every list can hold a peer we
 * hold no socket for.
 */
bool ConfirmBrowseAction(wxWindow *parent, size_t count);

/**
 * Friend or unfriend a selection of live clients.
 *
 * Confirms, then runs the same action the row-backed lists use, so both paths
 * share one set of rules instead of accumulating their own.
 */
void PeerActionSetFriendsForClients(wxWindow *parent, const std::vector<CClientRef> &clients, bool addThem);

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
