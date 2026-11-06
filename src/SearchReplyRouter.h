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

#ifndef SEARCHREPLYROUTER_H
#define SEARCHREPLYROUTER_H

#include <cstdint>
#include <ctime>
#include <deque>
#include <iterator>
#include <map>

// Routes eD2k server answers to the search that asked for them.
//
// Neither TCP OP_SEARCHRESULT nor UDP OP_GLOBSEARCHRES carries a search id on the wire;
// historically both were filed into a single "current search" scalar. Starting a second
// search while late answers to the first are still in flight therefore files the old
// hits into the new search's bucket (e.g. "nord sud ovest est" hits showing up inside
// a "georgie mandy" tab).
//
// Servers answer each query at most once, in the order asked, so every ask pushes the
// owning search id and every answer pops the oldest outstanding one. A server asked by
// two searches in a row attributes its first answer to the first search even though a
// newer search is current. Entries expire (asks a server never answers, e.g. a
// Lugdunum server with no match sends nothing) and explicit stops/timeouts remove
// their search, so an answer arriving after its ask expired is attributed to the next
// outstanding ask instead of a search that can no longer own it.
//
// Pure logic, no application dependencies, so it is unit-testable on its own.
class CSearchReplyRouter
{
public:
	//! Record that @a serverKey was asked on behalf of @a searchId.
	//! @a expiresAt is when an unanswered ask stops owning answers (0 = never).
	void Ask(uint32_t serverKey, uint32_t searchId, time_t expiresAt = 0)
	{
		m_pending[serverKey].push_back(Outstanding{ searchId, expiresAt });
	}

	//! Attribute one incoming answer from @a serverKey: drop expired fronts, pop and
	//! return the oldest outstanding search id, or 0 when nothing outstanding owns it.
	uint32_t ClaimReply(uint32_t serverKey, time_t now)
	{
		auto it = m_pending.find(serverKey);
		if (it == m_pending.end()) {
			return 0;
		}
		std::deque<Outstanding> &queue = it->second;
		while (!queue.empty() && queue.front().expiresAt != 0 && queue.front().expiresAt < now) {
			queue.pop_front();
		}
		if (queue.empty()) {
			m_pending.erase(it);
			return 0;
		}
		const uint32_t searchId = queue.front().searchId;
		queue.pop_front();
		if (queue.empty()) {
			m_pending.erase(it);
		}
		return searchId;
	}

	//! The timed-out or explicitly stopped search no longer expects this server's answer.
	//! Removes its oldest outstanding ask, since a search asks a server at most once per
	//! sweep but may re-ask it from a later search (both entries coexist).
	void Remove(uint32_t serverKey, uint32_t searchId)
	{
		auto it = m_pending.find(serverKey);
		if (it == m_pending.end()) {
			return;
		}
		std::deque<Outstanding> &queue = it->second;
		for (auto entry = queue.begin(); entry != queue.end(); ++entry) {
			if (entry->searchId == searchId) {
				queue.erase(entry);
				break;
			}
		}
		if (queue.empty()) {
			m_pending.erase(it);
		}
	}

	//! Drop every outstanding ask owned by @a searchId (tab closed, search stopped/freed).
	void PurgeSearch(uint32_t searchId)
	{
		for (auto it = m_pending.begin(); it != m_pending.end();) {
			std::deque<Outstanding> &queue = it->second;
			for (auto entry = queue.begin(); entry != queue.end();) {
				entry = (entry->searchId == searchId) ? queue.erase(entry) : entry + 1;
			}
			it = queue.empty() ? m_pending.erase(it) : std::next(it);
		}
	}

	void Clear() { m_pending.clear(); }

	bool Empty() const { return m_pending.empty(); }

private:
	struct Outstanding
	{
		uint32_t searchId;
		time_t expiresAt;
	};
	std::map<uint32_t, std::deque<Outstanding>> m_pending;
};

#endif // SEARCHREPLYROUTER_H
