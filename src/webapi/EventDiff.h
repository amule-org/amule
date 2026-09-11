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
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
//

#ifndef WEBAPI_EVENT_DIFF_H
#define WEBAPI_EVENT_DIFF_H

#include "State.h"

#include <map>
#include <cstdint>

namespace webapi
{

class CEventBus;

// One "last seen" snapshot of every substruct we publish events for. Owned by CamuleapiApp; mutated
// AFTER each successful tick by `EmitDiffsAndUpdate`. The first tick fires `_added` for every alive
// entry (cold start); later ticks fire only the deltas.
struct LastSeenState
{
	// `files` is a comparison baseline against CState::m_files, not a mirror of it: an entry is
	// rewritten exactly when one of EventDiff's predicates reports a difference, so the fields
	// those predicates read stay fresh and others may lag. Adding a predicate means adding its
	// field to the write-back condition too, not only to the emit condition.
	//
	// Role-flag transitions false->true emit the matching `_added` event, true->false the
	// `_removed`; a file may emit both families.
	std::map<std::uint32_t, FileSnapshot> files;
	std::map<std::uint32_t, ServerSnapshot> servers;
	std::map<std::uint32_t, ClientSnapshot> clients;
	std::map<std::uint32_t, FriendSnapshot> friends;
	// The status event payload mirrors the REST /status envelope, which pulls from THREE
	// sources (StatusSnapshot + KadSnapshot + ec_connected). All three must be diffed against
	// the prior tick to decide on `status_changed`.
	StatusSnapshot status;
	KadSnapshot kad;
	bool ec_connected = false;
	bool status_initialised = false;

	// Log-tail tracking for `log_appended`. `amule_log_count` is how many lines the previous
	// tick accounted for, and is what the next passes to CState::AmuleLogFrom() as its cursor.
	// `amule_log_initialised` gates the cold start so history is not dumped as one event.
	std::size_t amule_log_count = 0;
	bool amule_log_initialised = false;
	// Last clear-generation seen. A bump means DELETE /logs/amule emptied the
	// buffer since the previous tick, which the count alone cannot tell.
	std::uint64_t amule_log_generation = 0;

	// Per-search event baseline (multi-search). One entry per open search_id, diffed against
	// that search's state each tick: new result ECIDs -> search_result_added; a percent change,
	// the running->finished edge, or a generation bump -> search_progress. Every event carries
	// its `search_id`, and entries for searches no longer present are pruned.
	struct SearchDiffState
	{
		std::map<std::uint32_t, SearchResult> results;
		bool complete = false;
		std::uint32_t percent = 0;
		// Baseline `generation` from the previous tick. Any bump forces a search_progress
		// emit, so back-to-back searches that start and finish inside one refresher
		// interval still deliver a terminal frame.
		std::uint64_t generation = 0;
	};
	std::map<std::uint32_t, SearchDiffState> searches;
	bool search_initialised = false;
};

// Walk every (old vs current) substruct, publish typed events for each delta, then overwrite `prev`
// with the current snapshot so the next tick diffs against the freshest baseline.
//
// `_added` / `_updated` payload: the full snapshot object, matching the REST list-item shape byte
// for byte, so clients overwrite their cache slot from it. `_removed` payload: `{"ecid": N}` for
// ECID-keyed types, `{"hash": "..."}` for hash-keyed ones. `status_changed` payload: the nested
// REST /status envelope, from state.Dashboard() so all three pieces stay consistent.
void EmitDiffsAndUpdate(CEventBus &bus, LastSeenState &prev, const CState &state);

// Chat events, published straight from the refresher's chat roundtrip rather than by diffing a
// prior snapshot: the walker already knows what is new (only messages past the cursor come back)
// and which sessions vanished.
//
// One `chat_message` per message, inbound AND outbound alike -- an outbound one is how a message
// sent from amulegui reaches every other viewer. A session that did not exist is implied by the
// first message carrying its `peer`.
void PublishChatEvents(CEventBus &bus,
	const std::vector<ChatSessionSnapshot> &new_messages,
	const std::vector<std::uint64_t> &closed);

} // namespace webapi

#endif // WEBAPI_EVENT_DIFF_H
