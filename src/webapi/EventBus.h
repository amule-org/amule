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

namespace webapi
{

// Event published over the SSE channel. Wire shape mirrors
// `text/event-stream` per RFC 6202 §4: the SSE-emitter writes
// `event: <name>\nid: <id>\ndata: <data>\n\n` for each event.
//
// `id` is monotonic across the bus's lifetime (uint64, never wraps
// for any realistic uptime — 18 EH). It is NOT stable across
// amuleapi restarts; the bus resets to 1 on each daemon start. The
// `resync` event covers the restart case for SSE subscribers: when
// a client reconnects with Last-Event-ID > the bus's current max,
// it gets a resync event and re-GETs all affected collections.
struct Event
{
	std::uint64_t id = 0;
	std::string name; // "download_added", "status_changed", etc.
	std::string data; // JSON payload (typed per `name`)
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
// **Capacity:** runtime-configured ring (see `[Streaming]/
// EventBusRingCapacity` in amuleapi.conf; default 16 384). When the
// buffer fills, the oldest event is dropped and clients whose
// Last-Event-ID fell off the ring get a typed `resync` instead of a
// partial replay. The default is sized for a cold-start tick on a
// heavy node (5K downloads + 5K shared can publish ~10K `*_added`
// in a single tick before any subscriber has had a chance to
// drain); worst-case memory ≈ capacity × ~1 KB JSON payload, so
// 16 384 ≈ 16 MB.
class CEventBus
{
public:
	// Compile-time floor + default. Capacity is settable at
	// construction; values below kMinCapacity are clamped up to the
	// floor. Floor exists so an operator can't accidentally
	// effectively disable SSE replay by setting capacity=1.
	static constexpr std::size_t kDefaultCapacity = 16384;
	static constexpr std::size_t kMinCapacity = 16;

	CEventBus()
	: CEventBus(kDefaultCapacity)
	{
	}
	explicit CEventBus(std::size_t capacity);

	// Effective ring capacity actually in use (post-clamp).
	std::size_t Capacity() const { return m_capacity; }

	// Publish a new event. Assigns the next id and stores it. Wakes
	// all blocked Drain* callers. Drop the oldest event if the ring
	// is full.
	void Publish(const std::string &name, const std::string &data);

	// Batch-publish. One lock acquisition + one notify_all for the
	// whole batch, vs N of each from per-event Publish loops. Used
	// by the cold-start tick where a 5K-download library emits one
	// `_added` per item — the per-item Publish was holding the
	// refresher loop for tens of milliseconds (every notify_all
	// goes through cv->mutex wake/sleep cycles on every drainer).
	// Each (name, data) pair is treated identically to a Publish
	// call: monotonic id assignment, evict-oldest if the ring fills.
	void PublishBatch(const std::vector<std::pair<std::string, std::string>> &events);

	// Drain every event with `id > since_id` into `out`. Returns
	// the highest id we found (== since_id if nothing new). Blocks
	// up to `timeout` if there are no new events; returns early
	// when something becomes available.
	std::uint64_t Drain(
		std::uint64_t since_id, std::chrono::milliseconds timeout, std::vector<Event> &out);

	// The id of the bus's oldest currently-stored event, or 0 if the
	// bus is empty. Used by the Last-Event-ID reconnect path: if
	// `Last-Event-ID < OldestId()` the client missed events that
	// have already been evicted and should be sent `resync` instead
	// of an empty replay.
	std::uint64_t OldestId() const;

	// The id of the most recently published event, or 0 if nothing
	// has been published yet. The reconnect path uses this to
	// compute "did I miss anything".
	std::uint64_t NewestId() const;

	// Reset the bus. Wakes any blocked drainers. Used by tests; not
	// called from production code.
	void ResetForTest();

	// Atomically wake every blocked Drain caller and mark the bus as
	// "shutting down". Subsequent Drain calls return immediately
	// (with an empty out vector). Used by the shutdown path:
	// detached SSE worker threads sit inside Drain() blocked on the
	// 15 s heartbeat; without this they'd hold references to the
	// dispatcher across its destruction → UAF. Latches once; idempotent.
	void Shutdown();

	// True if Shutdown() has been called. SSE worker loops poll this
	// between Drain calls and exit cleanly.
	bool IsShutdown() const;

	// --- Subscriber accounting ---------------------------------------
	//
	// A tick's diff means snapshotting every collection and comparing it with
	// the previous tick, which on a large library is most of the refresher's
	// work. Once nothing has been subscribed for a few ticks there is nobody to
	// send the result to, so the refresher skips it and records that
	// (MarkSuspended). Nothing is lost -- subscribers read the same state over
	// REST -- but no collection change is represented on the bus for that
	// period, and a client reconnecting with a Last-Event-ID cannot tell: its
	// cursor can even still be in range, since the chat publisher runs outside
	// this gate and keeps ids moving. The tick that resumes publishes a
	// `resync` for that, after re-baselining, so a client re-GETs against a
	// baseline that is already current -- emitting it at connect instead would
	// leave a tick's worth of changes in neither the GET nor an event.

	// RAII registration for one SSE session.
	class Subscription
	{
	public:
		explicit Subscription(CEventBus &bus);
		~Subscription();
		Subscription(const Subscription &) = delete;
		Subscription &operator=(const Subscription &) = delete;

	private:
		CEventBus &m_bus;
	};

	std::size_t SubscriberCount() const { return m_subscribers.load(std::memory_order_acquire); }

	// Record that a tick's diff was skipped for want of a subscriber.
	void MarkSuspended() { m_suspended.store(true, std::memory_order_release); }

	// Read and clear the suspended flag. True obliges the refresher to
	// re-baseline silently first: diffing against a pre-idle snapshot would
	// emit one event per record.
	bool TakeSuspended() { return m_suspended.exchange(false, std::memory_order_acq_rel); }

private:
	const std::size_t m_capacity;
	mutable std::mutex m_mu;
	std::condition_variable m_cv;
	std::deque<Event> m_ring;
	std::atomic<std::uint64_t> m_next_id{ 1 };
	std::atomic<bool> m_shutdown{ false };
	std::atomic<std::size_t> m_subscribers{ 0 };
	std::atomic<bool> m_suspended{ false };
};

} // namespace webapi

#endif // WEBAPI_EVENT_BUS_H
