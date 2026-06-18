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


namespace webapi {


class CEventBus;


// One "last seen" snapshot of all the substructs we publish events
// for. Owned by CamuleapiApp; mutated AFTER each successful tick by
// `EmitDiffsAndUpdate`. The first tick fires `_added` for every alive
// entry (cold start); subsequent ticks fire only the deltas.
struct LastSeenState {
	std::map<std::uint32_t, DownloadSnapshot> downloads;
	std::map<std::uint32_t, SharedSnapshot>   shared;
	std::map<std::uint32_t, ServerSnapshot>   servers;
	std::map<std::uint32_t, ClientSnapshot>   clients;
	StatusSnapshot                            status;
	bool                                      status_initialised = false;

	// Phase 8d: log-tail tracking for the `log_appended` event.
	// `amule_log_count` is the size of `state.AmuleLog()` at the
	// previous tick. When the vector grows, the new tail is
	// `log[amule_log_count .. new_size)` and we publish it.
	// First-tick cold-start is gated by `amule_log_initialised` so
	// we don't dump every historical line as one event — clients
	// can GET /api/v0/logs/amule for the history.
	std::size_t                               amule_log_count = 0;
	bool                                      amule_log_initialised = false;
};


// Walk every (old vs current) substruct, publish typed events for
// each delta, then overwrite `prev` with the current snapshot so the
// next tick's diff is against the freshest baseline.
//
// Wire event names emitted (Phase 8b):
//   download_added / download_updated / download_removed
//   shared_added   / shared_updated   / shared_removed
//   server_added   / server_updated   / server_removed
//   client_added   / client_updated   / client_removed
//   status_changed
//
// `_added` payload: the full snapshot object (so clients can
// materialise a cache entry from one event).
// `_updated` payload: the full snapshot object too — Phase 8b doesn't
// compute per-field deltas; clients overwrite their cache slot from
// the new object.
// `_removed` payload: `{"ecid": N}` for ECID-keyed types,
// `{"hash": "..."}` for hash-keyed (downloads + shared).
// `status_changed` payload: the full StatusSnapshot JSON.
void EmitDiffsAndUpdate(CEventBus &bus,
                        LastSeenState &prev,
                        const CState &state);


}  // namespace webapi

#endif // WEBAPI_EVENT_DIFF_H
