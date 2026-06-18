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


namespace webapi {


// C++14 requires an out-of-class definition for a static constexpr
// member used by reference (test asserts that bind kCapacity to a
// const auto& parameter). C++17 inlined that, but the project is
// pinned to C++14 (PLAN.md §14 Phase 2).
constexpr std::size_t CEventBus::kCapacity;


void CEventBus::Publish(const std::string &name, const std::string &data)
{
	Event ev;
	ev.id   = m_next_id.fetch_add(1, std::memory_order_acq_rel);
	ev.name = name;
	ev.data = data;
	{
		std::lock_guard<std::mutex> g(m_mu);
		if (m_ring.size() >= kCapacity) m_ring.pop_front();
		m_ring.push_back(std::move(ev));
	}
	// notify_all so every blocked drainer wakes and races to its
	// own copy-out. The shared-mutex critical section is short
	// (just walks the deque) so contention is negligible.
	m_cv.notify_all();
}


std::uint64_t CEventBus::Drain(std::uint64_t since_id,
                               std::chrono::milliseconds timeout,
                               std::vector<Event> &out)
{
	out.clear();
	std::unique_lock<std::mutex> lk(m_mu);

	// Quick path: any events with id > since_id already in ring?
	auto has_newer = [&]() {
		return !m_ring.empty() && m_ring.back().id > since_id;
	};
	if (!has_newer()) {
		// Wait up to `timeout` for someone to publish. wait_for
		// returns no_timeout on a notify, timeout otherwise. We
		// re-check the predicate either way.
		m_cv.wait_for(lk, timeout, has_newer);
	}

	std::uint64_t max_seen = since_id;
	for (const auto &ev : m_ring) {
		if (ev.id > since_id) {
			out.push_back(ev);
			if (ev.id > max_seen) max_seen = ev.id;
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
	}
	m_cv.notify_all();
}


}  // namespace webapi
