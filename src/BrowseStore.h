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
 * A change, and the browse it happened to.
 *
 * The two travel together because deriving the ID separately is how the effect
 * got lost: the manager asked which browse a peer had, and the answer excluded
 * the terminal ones -- exactly the records whose peer most needs releasing.
 */
struct Outcome
{
	std::uint32_t searchId = 0;
	Effect effect = Effect::Nothing;
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
	 * The answer to Start: whether it took, and any peer it displaced.
	 *
	 * A peer whose last browse has ended may be browsed again straight away,
	 * before the tick that would have released it. Two records would then name
	 * the same peer, and a lookup by peer could answer with either -- so the
	 * old one lets go here, and says so, since its owner is holding a
	 * reference on the strength of it.
	 */
	struct StartResult
	{
		bool started = false;
		Outcome displaced;
	};

	/**
	 * Track a browse of `client` under `searchId`.
	 *
	 * Refuses when that client already has one STILL RUNNING: there is a
	 * single exchange with a peer to report on, so a second record could only
	 * ever describe the same browse. This is the rule the EC handler's "join
	 * the browse already in flight" depends on being true.
	 *
	 * A browse that has ended is no obstacle, even before its peer has been
	 * released -- matching SearchIdFor, which also answers only for a running
	 * one. Disagreeing about that left a peer un-rebrowsable for the second
	 * or so between its browse ending and the next tick.
	 *
	 * Refuses an ID already tracked, whatever its state: a second record on
	 * one key would replace the first and everything it still has to answer
	 * for. No caller reuses an ID -- both routes pass a freshly allocated one
	 * -- so this guards an invariant rather than a case.
	 */
	StartResult Start(ClientKey client, std::uint32_t searchId, std::uint64_t now);

	//! Push the silence deadline back; the peer has shown a sign of life.
	void Touch(ClientKey client, std::uint64_t now);

	Outcome OnDirectoryList(ClientKey client, int dirCount, std::uint64_t now);
	Outcome OnListingReceived(ClientKey client, std::uint64_t now);

	//! Terminalize `client`'s browse. Refused, silently, if it already ended.
	Outcome Fail(ClientKey client);
	Outcome Finish(ClientKey client);

	/**
	 * The peer is going away: fail a browse still running, then release it.
	 * Returns both effects in order, so the caller reports the failure before
	 * dropping the reference it needs to name the peer.
	 */
	std::vector<Outcome> Forget(ClientKey client);

	//! Dispose of a record for good, with its search.
	void Remove(std::uint32_t searchId);

	//! Advance every record; returns what to do about each.
	std::vector<Outcome> Tick(std::uint64_t now);

	std::uint32_t SearchIdFor(ClientKey client) const;

	/**
	 * Whether `client`'s browse is still waiting for its directory list.
	 *
	 * True exactly once per browse, between the request going out and the
	 * peer's OP_ASKSHAREDDIRSANS. The handler needs that: its old guard was
	 * single-shot by accident (it compared an outstanding count to 1), and
	 * widening it to "this peer has a browse" let a peer resend the answer as
	 * often as it liked, each time re-asking for every directory and pushing
	 * the silence deadline back.
	 */
	bool AwaitingDirectoryList(ClientKey client) const;
	bool Has(std::uint32_t searchId) const;
	State StateOf(std::uint32_t searchId) const;
	std::uint16_t BarValue(std::uint32_t searchId) const;
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
