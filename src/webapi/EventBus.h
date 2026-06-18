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

#ifndef WEBAPI_EVENT_BUS_H
#define WEBAPI_EVENT_BUS_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>


namespace webapi {


// Event published over the SSE channel. Wire shape mirrors
// `text/event-stream` per RFC 6202 §4: the SSE-emitter writes
// `event: <name>\nid: <id>\ndata: <data>\n\n` for each event.
//
// `id` is monotonic across the bus's lifetime (uint64, never wraps
// for any realistic uptime — 18 EH). It is NOT stable across
// amuleapi restarts; the bus resets to 1 on each daemon start.
// Phase 8d's `resync` event covers the restart case for SSE
// subscribers: when a client reconnects with Last-Event-ID > the
// bus's current max, it gets a resync event and re-GETs all
// affected collections.
struct Event {
	std::uint64_t id        = 0;
	std::string   name;     // "download_added", "status_changed", etc.
	std::string   data;     // JSON payload (typed per `name`)
};


// In-memory SSE event bus. One instance per amuleapi process; the
// refresher publishes events as cache deltas surface and SSE
// sessions drain them.
//
// **Concurrency:** all public methods are safe for any thread. The
// refresher publishes from the wxApp thread (during a tick);
// streaming-handler threads drain. Internal lock is a regular
// std::mutex — drain operations only hold it long enough to
// copy out the events they want, never across a wire write.
//
// **Capacity:** a 100-event ring (PLAN §12 Q7). When the buffer
// fills, the oldest event is dropped. This matches the SSE
// `Last-Event-ID`-based replay window; clients that miss more
// than 100 events get a `resync` (Phase 8d) instead of a partial
// replay.
class CEventBus {
public:
	static constexpr std::size_t kCapacity = 100;

	// Publish a new event. Assigns the next id and stores it. Wakes
	// all blocked Drain* callers. Drop the oldest event if the ring
	// is full.
	void Publish(const std::string &name, const std::string &data);

	// Drain every event with `id > since_id` into `out`. Returns
	// the highest id we found (== since_id if nothing new). Blocks
	// up to `timeout` if there are no new events; returns early
	// when something becomes available.
	std::uint64_t Drain(std::uint64_t since_id,
	                    std::chrono::milliseconds timeout,
	                    std::vector<Event> &out);

	// The id of the bus's oldest currently-stored event, or 0 if
	// the bus is empty. Used by Phase 8c's Last-Event-ID resolution:
	// if `Last-Event-ID < OldestId()`, the client missed events
	// that have already been evicted and should be sent `resync`
	// instead of an empty replay.
	std::uint64_t OldestId() const;

	// The id of the most recently published event, or 0 if nothing
	// has been published yet. Phase 8c's reconnect path uses this
	// to compute "did I miss anything".
	std::uint64_t NewestId() const;

	// Reset the bus. Wakes any blocked drainers. Used by tests; not
	// called from production code.
	void ResetForTest();

private:
	mutable std::mutex                          m_mu;
	std::condition_variable                     m_cv;
	std::deque<Event>                           m_ring;
	std::atomic<std::uint64_t>                  m_next_id{1};
};


}  // namespace webapi

#endif // WEBAPI_EVENT_BUS_H
