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

#ifndef PEERIDENTITY_H
#define PEERIDENTITY_H

#include "ClientDetailDialog.h" // Needed for ClientDetailInfo
#include "ClientRef.h"          // Needed for CClientRef
#include "MD4Hash.h"            // Needed for CMD4Hash
#include "Types.h"              // Needed for uint16/uint32

/**
 * A peer a row names, whether or not we are talking to it right now.
 *
 * The identity fields come from the row and are enough to act on a peer that
 * is offline: to friend it, or to open a connection when the user asks for one.
 * `client` is linked only when the peer happens to be connected.
 */
struct PeerIdentity
{
	CMD4Hash hash;
	wxString name;
	uint32 ip = 0;
	uint16 port = 0;
	CClientRef client;

	/**
	 * What the details dialog should show when `client` is not linked.
	 *
	 * Only a list that keeps enough of a record to render it fills this in;
	 * `hasDetail` says whether it did. A live peer needs none of it -- the
	 * dialog snapshots the client instead, which knows strictly more.
	 */
	ClientDetailInfo detail;

	/**
	 * Whether a connection to this peer can be opened at all.
	 *
	 * Either we are already talking to it, or we hold an address to dial.
	 */
	bool CanOpenConnection() const { return client.IsLinked() || (ip != 0 && port != 0); }

	/**
	 * Whether a live client carries an address a friend record could use.
	 *
	 * The one place that answers this for a client, so the menu that offers
	 * an action and the action itself cannot read it differently.
	 */
	static bool Addressable(const CClientRef &live)
	{
		CClientRef &c = const_cast<CClientRef &>(live);
		return c.GetIP() != 0 && c.GetUserPort() != 0;
	}

	/**
	 * Whether a friend record can be stored for this peer.
	 *
	 * A friend is reached by address, and CAddFriend refuses to store one
	 * without a usable ip and port. A record written from here has to meet
	 * the same bar: an address-less friend keys every chat tab on GUI_ID(0,0)
	 * and can never be dialled.
	 *
	 * For a peer we are connected to the live client is the source of truth,
	 * not the row. A row only learns an address once the peer has told us its
	 * name, so a connected peer with an empty nickname has one the row does
	 * not, and CFriend's client constructor copies it from the client anyway.
	 */
	bool CanBeFriended() const
	{
		return client.IsLinked() ? Addressable(client) : (ip != 0 && port != 0);
	}
	bool hasDetail = false;
};

#endif // PEERIDENTITY_H
