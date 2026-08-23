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

#ifndef BROWSEMANAGER_H
#define BROWSEMANAGER_H

#include "BrowseLifecycle.h"
#include "BrowseStore.h"
#include "ClientRef.h"

#include <cstdint>
#include <map>

class CUpDownClient;

/**
 * Owns every "View Files" browse the core is running.
 *
 * Before this existed a browse was four fields borrowed on CUpDownClient plus
 * two maps on CSearchList plus a list on CClientList, and the same fact was
 * written independently in seven files. Five separate defects were that one
 * invariant broken somewhere different -- a successful browse never marked
 * finished, a peer we declined to contact, a terminal browse re-examined on
 * every tick forever. One owner is the point: the lifecycle is decided by
 * browse::Tick and stored here, and every other subsystem reads it rather than
 * keeping a copy that has to be made to agree.
 *
 * Core-only. Everything that touches browse state -- CUpDownClient,
 * CClientTCPSocket, CSearchList, the EC handlers, and the monolithic search
 * dialog's #ifndef CLIENT_GUI paths -- is core; amulegui learns a browse's
 * state over EC instead.
 */
class CBrowseManager
{
public:
	/**
	 * Begin tracking a browse of `client` under `searchId`.
	 *
	 * The ID is allocated by the caller before the request goes out, for both
	 * the EC and the local path, so a browse is addressable from the moment it
	 * exists. That is what lets this map be keyed by ID: the old code had no
	 * ID for a local browse until its first result arrived and keyed the
	 * interim state on the client pointer instead, which left entries behind
	 * that nothing could prune and made two browses of different peers
	 * collide when an address was reused.
	 *
	 * Refuses to start a second browse of a client that already has one, so
	 * the caller's own "already in flight" check and this cannot disagree.
	 */
	bool Start(CUpDownClient *client, std::uint32_t searchId, std::uint32_t peerEcid, std::uint64_t now);

	/**
	 * The ask is on the wire. Restarts the silence budget, because the click
	 * may have been a connect ago and the peer's clock starts here.
	 */
	void OnRequestSent(CUpDownClient *client, std::uint64_t now);

	//! The peer answered OP_ASKSHAREDDIRS with its directory count.
	void OnDirectoryList(CUpDownClient *client, int dirCount, std::uint64_t now);

	/**
	 * One listing arrived, of either protocol form. Completes the browse when
	 * nothing more is expected -- which is where the flat (single-answer) form
	 * used to be left hanging, because only the directory form's packet
	 * handler marked anything.
	 */
	void OnListingReceived(CUpDownClient *client, std::uint64_t now);

	//! Terminalize the browse of `client`, if it has one. Safe to call for a
	//! client that never had one, which is most of them.
	void Fail(CUpDownClient *client);
	void Finish(CUpDownClient *client);

	/**
	 * The client is being destroyed: fail its browse if one is still running,
	 * then let go of the reference. The record itself outlives the peer --
	 * the search ID is still listed and still has to answer for its state.
	 */
	void Forget(CUpDownClient *client);

	/**
	 * Release a browse for good, when its search is freed.
	 *
	 * Records are deliberately kept after they terminate: every consumer asks
	 * here what a browse's state is, so a record dropped at completion would
	 * send the listing back to guessing from whether results were retained --
	 * which reports a failed browse as idle, forever.
	 */
	void Remove(std::uint32_t searchId);

	//! Expire the silent, complete the finished, drop the terminal. Driven by
	//! the core timer; `now` is injected so the policy stays testable.
	void Process(std::uint64_t now);

	//! Search ID of `client`'s browse, or 0. This is the "already browsing
	//! this peer" test the EC handler joins on.
	std::uint32_t SearchIdFor(const CUpDownClient *client) const;

	//! See browse::Store::AwaitingDirectoryList.
	bool AwaitingDirectoryList(const CUpDownClient *client) const;
	bool Has(std::uint32_t searchId) const;
	//! browse::State of `searchId`; InProgress for an unknown ID is never
	//! reported -- callers gate on Has() first.
	browse::State StateOf(std::uint32_t searchId) const;
	//! 0..100 while running, 0xffff once terminal: the value space the search
	//! list's bar and the EC progress reply already use.
	std::uint16_t BarValue(std::uint32_t searchId) const;

private:
	//! Perform what the store asked for, on the browse named by `searchId`.
	void Perform(const browse::Outcome &outcome);
	//! Tell the GUI a browse reached its terminal state.
	void Announce(std::uint32_t searchId);

	//! Every rule about browses. Kept separate so it can be driven by a test:
	//! it identifies peers by an opaque key and never reaches theApp.
	browse::Store m_store;

	/**
	 * The references that keep browsed peers alive, keyed the same way.
	 *
	 * Not a second copy of the browse -- the store owns that. This holds only
	 * lifetime, and entries leave it as soon as the store says the peer is no
	 * longer needed, which is well before the record itself is disposed of.
	 */
	std::map<std::uint32_t, CClientRef> m_clients;
};

#endif // BROWSEMANAGER_H
