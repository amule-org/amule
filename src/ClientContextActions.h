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

#include "ClientRef.h" // Needed for CClientRef

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
