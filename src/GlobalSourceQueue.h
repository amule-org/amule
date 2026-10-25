//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
//

#ifndef GLOBALSOURCEQUEUE_H
#define GLOBALSOURCEQUEUE_H

#include "MD4Hash.h"                 // Needed for CMD4Hash
#include "Refill.h"                  // Needed for CRefill
#include "Types.h"                   // Needed for uint64, needed by Constants.h
#include <protocol/ed2k/Constants.h> // Needed for UDPSERVERREASKTIME

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <vector>

// The global source requests one non-connected server gets: which downloads to ask it about, and
// when. Lugdunum 17.15 stops answering an IP, for an hour or more, once its requests cost too much:
// about 124 hashes in a few minutes at any pace, or about 25 single-hash requests sent 10 to 15 s
// apart. It kept answering one request a minute. The old pass over every download sent a server
// at most 35 hashes per UDPSERVERREASKTIME; the hash budget keeps that volume, and the packet
// budget keeps the long-run pace at one request a minute.
//
// Downloads just added or resumed go first, newest first, so a new one is not stuck behind a
// large batch added earlier. The others are asked in turn, in hash order from where the last
// request stopped, so the one asked longest ago is next; one asked within UDPSERVERREASKTIME waits.
class CGlobalSourceQueue
{
public:
	static constexpr uint32_t HASHES = 35;
	static constexpr uint64_t MS_PER_HASH = UDPSERVERREASKTIME / HASHES;
	static constexpr uint32_t PACKETS = 5;
	static constexpr uint64_t MS_PER_PACKET = 60000;
	// Kept for downloads just added or resumed, so the others cannot use it all.
	static constexpr uint32_t RESERVE = 4;
	static constexpr uint32_t PACKET_RESERVE = 1;
	// Lugdunum drops a request sent a few seconds after another. A download just added or
	// resumed may go before the others' gap is over.
	static constexpr uint64_t GAP_MS = 60000;
	static constexpr uint64_t NEW_GAP_MS = 15000;
	static constexpr size_t MAX_NEW = 1000;

	explicit CGlobalSourceQueue(const CMD4Hash &start = CMD4Hash())
	: m_cursor(start)
	{
	}

	// A download was added or resumed.
	void Add(const CMD4Hash &hash)
	{
		m_new.erase(std::remove(m_new.begin(), m_new.end(), hash), m_new.end());
		if (m_new.size() >= MAX_NEW) {
			m_new.pop_front();
		}
		m_new.push_back(hash);
	}

	uint32_t Available(uint64_t now) const { return m_hashes.Available(now); }
	uint32_t PacketsAvailable(uint64_t now) const { return m_packets.Available(now); }

	// The downloads to ask about now, at most perPacket; empty if none may be asked yet. Takes
	// them out of the queue and spends the budget. downloads must be sorted; wanted() says
	// whether one may be asked about at all.
	std::vector<CMD4Hash> Next(uint64_t now,
		const std::vector<CMD4Hash> &downloads,
		const std::function<bool(const CMD4Hash &)> &wanted,
		uint32_t perPacket)
	{
		std::vector<CMD4Hash> asked;
		const uint32_t budget = m_hashes.Available(now);
		const uint32_t packets = m_packets.Available(now);
		const size_t room = std::min(budget, perPacket);
		const uint64_t sinceLast = m_asked ? now - m_lastRequest : UINT64_MAX;

		if (sinceLast >= NEW_GAP_MS && packets > 0) {
			while (asked.size() < room && !m_new.empty()) {
				const CMD4Hash hash = m_new.back();
				m_new.pop_back();
				if (wanted(hash)) {
					asked.push_back(hash);
				}
			}
		}

		if ((sinceLast >= GAP_MS && packets > PACKET_RESERVE) || !asked.empty()) {
			ForgetOlderThan(now);
			const size_t unreserved =
				budget > asked.size() + RESERVE ? budget - asked.size() - RESERVE : 0;
			const size_t limit = std::min(room, asked.size() + unreserved);
			const auto after = std::upper_bound(downloads.begin(), downloads.end(), m_cursor);
			for (size_t i = 0; i < downloads.size() && asked.size() < limit; ++i) {
				const size_t pos = (static_cast<size_t>(after - downloads.begin()) + i) %
						   downloads.size();
				const CMD4Hash &hash = downloads[pos];
				if (!m_recent.count(hash) &&
					std::find(asked.begin(), asked.end(), hash) == asked.end() &&
					wanted(hash)) {
					asked.push_back(hash);
					m_cursor = hash;
				}
			}
		}

		if (!asked.empty()) {
			m_asked = true;
			m_lastRequest = now;
			m_hashes.Spend(now, static_cast<uint32_t>(asked.size()));
			m_packets.Spend(now, 1);
			for (const CMD4Hash &hash : asked) {
				m_recent[hash] = now;
			}
		}
		return asked;
	}

private:
	void ForgetOlderThan(uint64_t now)
	{
		for (auto it = m_recent.begin(); it != m_recent.end();) {
			it = (now - it->second >= UDPSERVERREASKTIME) ? m_recent.erase(it) : std::next(it);
		}
	}

	std::deque<CMD4Hash> m_new;
	// Asked within UDPSERVERREASKTIME, and when.
	std::map<CMD4Hash, uint64_t> m_recent;
	// The last download asked about in turn.
	CMD4Hash m_cursor;
	CRefill m_hashes{ HASHES, MS_PER_HASH };
	CRefill m_packets{ PACKETS, MS_PER_PACKET };
	uint64_t m_lastRequest = 0;
	bool m_asked = false;
};

#endif // GLOBALSOURCEQUEUE_H
// File_checked_for_headers
