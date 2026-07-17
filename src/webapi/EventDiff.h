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

// One "last seen" snapshot of all the substructs we publish events
// for. Owned by CamuleapiApp; mutated AFTER each successful tick by
// `EmitDiffsAndUpdate`. The first tick fires `_added` for every alive
// entry (cold start); subsequent ticks fire only the deltas.
struct LastSeenState
{
	// `files` mirrors CState::m_files (unified ECID-keyed map with
	// `is_downloading` / `is_shared` flags). Role-flag transitions
	// false→true emit the corresponding `_added` event, true→false
	// the `_removed`; a file may participate in both views and emit
	// both event families.
	std::map<std::uint32_t, FileSnapshot> files;
	std::map<std::uint32_t, ServerSnapshot> servers;
	std::map<std::uint32_t, ClientSnapshot> clients;
	// Status event payload mirrors the REST /status envelope, which
	// pulls from THREE sources (StatusSnapshot + KadSnapshot +
	// ec_connected flag). All three must be diffed against the prior
	// tick to decide whether to fire `status_changed`.
	StatusSnapshot status;
	KadSnapshot kad;
	bool ec_connected = false;
	bool status_initialised = false;

	// log-tail tracking for the `log_appended` event.
	// `amule_log_count` is the size of `state.AmuleLog()` at the
	// previous tick. When the vector grows, the new tail is
	// `log[amule_log_count .. new_size)` and we publish it.
	// First-tick cold-start is gated by `amule_log_initialised` so
	// we don't dump every historical line as one event — clients
	// can GET /api/v0/logs/amule for the history.
	std::size_t amule_log_count = 0;
	bool amule_log_initialised = false;

	// Per-search event baseline (multi-search). One entry per open search_id,
	// diffed against that search's state each tick: new result ECIDs →
	// search_result_added; a percent change, the running→finished edge, or a
	// generation bump → search_progress (the terminal frame, state="finished",
	// supersedes the old standalone search_finished event). Every emitted event
	// carries its `search_id`. Entries for searches no longer present (closed /
	// reset) are pruned each tick.
	struct SearchDiffState
	{
		std::map<std::uint32_t, SearchResult> results;
		bool complete = false;
		std::uint32_t percent = 0;
		// Baseline `generation` from the previous tick. Any bump forces a
		// search_progress emit, so back-to-back searches that start and finish
		// inside one refresher interval still deliver a terminal frame.
		std::uint64_t generation = 0;
	};
	std::map<std::uint32_t, SearchDiffState> searches;
	bool search_initialised = false;
};

// Walk every (old vs current) substruct, publish typed events for
// each delta, then overwrite `prev` with the current snapshot so the
// next tick diffs against the freshest baseline.
//
// Wire event names: download_{added,updated,removed},
// shared_{added,updated,removed}, server_{added,updated,removed},
// client_{added,updated,removed}, status_changed.
//
// `_added` / `_updated` payload: the full snapshot object (matches
// the REST list-item shape byte-for-byte; clients overwrite their
// cache slot from the new object).
// `_removed` payload: `{"ecid": N}` for ECID-keyed types,
// `{"hash": "..."}` for hash-keyed (downloads + shared).
// `status_changed` payload: the nested REST /status envelope
// (ed2k.*, kad.* with kad.network rollup, speeds.*, queue.*, plus
// top-level ec_connected). Pulled from state.Dashboard() so all
// three pieces stay consistent within a tick.
void EmitDiffsAndUpdate(CEventBus &bus, LastSeenState &prev, const CState &state);

} // namespace webapi

#endif // WEBAPI_EVENT_DIFF_H
