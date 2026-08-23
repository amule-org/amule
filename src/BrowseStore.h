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

#ifndef BROWSESTORE_H
#define BROWSESTORE_H

#include "BrowseLifecycle.h"

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace browse
{

/**
 * What the owner must do about a change the store has just made.
 *
 * The store decides; the caller performs. That split is the point: the rules
 * are then reachable from a test, which the manager holding them was not --
 * and both bugs found after the lifecycle was extracted were rules, not state
 * machine (a terminal record erased instead of retained, a disconnect after
 * success reported as a failure).
 */
enum class Effect
{
	Nothing,
	//! The browse reached a terminal state; tell whoever is watching.
	Announce,
	//! ...and it failed, which is also worth a line in the log.
	AnnounceFailure,
	//! Terminal and announced already: let go of the peer. The RECORD stays,
	//! because the search ID is still listed and still has to answer for its
	//! state; only Remove() disposes of it.
	ReleaseClient
};

/**
 * Every browse the core is tracking, and the rules about them.
 *
 * Clients are identified by an opaque key rather than held: lifetime is the
 * owner's problem, and keeping it out here is what lets the rules be driven
 * from a test with no client, no theApp and no clock.
 */
class Store
{
public:
	//! Opaque stand-in for the peer. The owner maps it back to a real client.
	using ClientKey = const void *;

	/**
	 * Track a browse of `client` under `searchId`.
	 *
	 * Refuses when that client already has one: there is a single exchange
	 * with a peer to report on, so a second record could only ever describe
	 * the same browse. This is the rule the EC handler's "join the browse
	 * already in flight" depends on being true.
	 */
	bool Start(ClientKey client, std::uint32_t searchId, std::uint32_t peerEcid, std::uint64_t now);

	//! Push the silence deadline back; the peer has shown a sign of life.
	void Touch(ClientKey client, std::uint64_t now);

	Effect OnDirectoryList(ClientKey client, int dirCount, std::uint64_t now);
	Effect OnListingReceived(ClientKey client, std::uint64_t now);

	//! Terminalize `client`'s browse. Refused, silently, if it already ended.
	Effect Fail(ClientKey client);
	Effect Finish(ClientKey client);

	/**
	 * The peer is going away: fail a browse still running, then release it.
	 * Returns both effects in order, so the caller reports the failure before
	 * dropping the reference it needs to name the peer.
	 */
	std::vector<Effect> Forget(ClientKey client);

	//! Dispose of a record for good, with its search.
	void Remove(std::uint32_t searchId);

	//! Advance every record; returns what to do, per search ID.
	std::vector<std::pair<std::uint32_t, Effect>> Tick(std::uint64_t now);

	std::uint32_t SearchIdFor(ClientKey client) const;
	bool Has(std::uint32_t searchId) const;
	State StateOf(std::uint32_t searchId) const;
	std::uint16_t BarValue(std::uint32_t searchId) const;
	std::vector<std::uint32_t> Ids() const;
	std::size_t Size() const { return m_records.size(); }

private:
	struct Held
	{
		Record rec;
		ClientKey client = nullptr;
	};

	Effect ApplyTo(Held &held, Action action);
	std::map<std::uint32_t, Held>::iterator FindFor(ClientKey client);
	std::map<std::uint32_t, Held>::const_iterator FindFor(ClientKey client) const;

	std::map<std::uint32_t, Held> m_records;
};

} // namespace browse

#endif // BROWSESTORE_H
