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

#include "EventBus.h"

namespace webapi
{

// C++14 requires an out-of-class definition for static constexpr
// members used by reference (test code may bind them through a
// const auto& parameter). C++17 made static constexpr members
// implicitly inline, so the same lines emit -Wdeprecated on
// GCC in C++17 mode ("redundant redeclaration of constexpr static
// data member"). Guard so it compiles clean under both language
// levels.
#if __cplusplus < 201703L
constexpr std::size_t CEventBus::kDefaultCapacity;
constexpr std::size_t CEventBus::kMinCapacity;
#endif

CEventBus::CEventBus(std::size_t capacity)
: m_capacity(capacity < kMinCapacity ? kMinCapacity : capacity)
{
}

void CEventBus::Publish(const std::string &name, const std::string &data)
{
	Event ev;
	ev.name = name;
	ev.data = data;
	{
		std::lock_guard<std::mutex> g(m_mu);
		// ID assignment INSIDE the lock. With fetch_add outside,
		// two concurrent publishers can swap their lock-order vs
		// their id-order: thread A gets id=N, thread B gets id=N+1,
		// then thread B grabs the lock first and pushes id=N+1
		// before thread A pushes id=N. The drainer then iterates
		// the deque (which is publish order, NOT id order) and
		// reports `id=N+1, id=N` — failing the strict-monotonicity
		// invariant every subscriber depends on.
		// Single-lock-section publish keeps fetch+push atomic.
		ev.id = m_next_id.fetch_add(1, std::memory_order_relaxed);
		if (m_ring.size() >= m_capacity)
			m_ring.pop_front();
		m_ring.push_back(std::move(ev));
	}
	// notify_all so every blocked drainer wakes and races to its
	// own copy-out. The mutex critical section is short (just walks
	// the deque) so contention is negligible.
	m_cv.notify_all();
}

void CEventBus::PublishBatch(const std::vector<std::pair<std::string, std::string>> &events)
{
	if (events.empty())
		return;
	{
		std::lock_guard<std::mutex> g(m_mu);
		// Same id-monotonicity invariant as Publish: assign + push
		// inside the lock. Doing the whole batch under one lock
		// also collapses N notify_all wake-ups into one — the cold
		// start tick on a 5K-download library used to fire 5K
		// individual notify_all cycles inside the refresher loop
		// (each going through every drainer's cv mutex), which
		// dominated the tick's wall-clock.
		for (const auto &kv : events) {
			Event ev;
			ev.name = kv.first;
			ev.data = kv.second;
			ev.id = m_next_id.fetch_add(1, std::memory_order_relaxed);
			if (m_ring.size() >= m_capacity)
				m_ring.pop_front();
			m_ring.push_back(std::move(ev));
		}
	}
	m_cv.notify_all();
}

std::uint64_t CEventBus::Drain(
	std::uint64_t since_id, std::chrono::milliseconds timeout, std::vector<Event> &out)
{
	out.clear();
	// Fast-fail on shutdown so a freshly-spawned SSE worker that
	// raced past the IsShutdown poll doesn't block in wait_for.
	if (m_shutdown.load(std::memory_order_acquire))
		return since_id;
	std::unique_lock<std::mutex> lk(m_mu);

	// Quick path: any events with id > since_id already in ring?
	auto has_newer = [&]() {
		return m_shutdown.load(std::memory_order_acquire) ||
		       (!m_ring.empty() && m_ring.back().id > since_id);
	};
	if (!has_newer()) {
		// Wait up to `timeout` for someone to publish OR for the
		// shutdown latch to fire. wait_for returns no_timeout on a
		// notify, timeout otherwise. We re-check the predicate
		// either way.
		m_cv.wait_for(lk, timeout, has_newer);
	}

	if (m_shutdown.load(std::memory_order_acquire))
		return since_id;

	std::uint64_t max_seen = since_id;
	for (const auto &ev : m_ring) {
		if (ev.id > since_id) {
			out.push_back(ev);
			if (ev.id > max_seen)
				max_seen = ev.id;
		}
	}
	return max_seen;
}

std::uint64_t CEventBus::OldestId() const
{
	std::lock_guard<std::mutex> g(m_mu);
	return m_ring.empty() ? 0 : m_ring.front().id;
}

std::uint64_t CEventBus::NewestId() const
{
	std::lock_guard<std::mutex> g(m_mu);
	return m_ring.empty() ? 0 : m_ring.back().id;
}

void CEventBus::ResetForTest()
{
	{
		std::lock_guard<std::mutex> g(m_mu);
		m_ring.clear();
		m_next_id.store(1, std::memory_order_release);
		m_shutdown.store(false, std::memory_order_release);
	}
	m_cv.notify_all();
}

void CEventBus::Shutdown()
{
	// Latch the shutdown flag and broadcast. Drain callers wake on
	// the cv either way (even with no events pending) and exit the
	// predicate. notify_all under the lock so a drainer that's
	// about to wait_for can't miss the wake.
	{
		std::lock_guard<std::mutex> g(m_mu);
		m_shutdown.store(true, std::memory_order_release);
	}
	m_cv.notify_all();
}

bool CEventBus::IsShutdown() const
{
	return m_shutdown.load(std::memory_order_acquire);
}

} // namespace webapi
