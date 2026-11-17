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

#ifndef REQUESTPACING_H
#define REQUESTPACING_H

#include "Types.h"                   // Needed for uint64, needed by Constants.h
#include <protocol/ed2k/Constants.h> // Needed for MIN_REQUESTTIME

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>

// A source's CheckForAggressive() scores an OP_STARTUPLOADREQ, OP_REASKFILEPING or OP_AICHREQUEST
// sent less than MIN_REQUESTTIME after the previous one, and bans at 10. Returns the last-asked
// time that makes CPartFile::Process(), which asks reaskTime after it, ask at the first moment
// that is not scored: now, or MIN_REQUESTTIME after lastPacedRequest (0 if there was none).
inline uint64_t PacedLastAskedTime(uint64_t now, uint64_t lastPacedRequest, uint64_t reaskTime)
{
	const uint64_t askAt = lastPacedRequest ? lastPacedRequest + MIN_REQUESTTIME : 0;
	if (askAt <= now) {
		return 0; // Process() asks on its next tick
	}
	return askAt > reaskTime ? askAt - reaskTime : 1;
}

// How long before its TCP ask CPartFile::Process() sends a queued source the UDP reask.
const uint64_t UDP_REASK_LEAD = 20000; // ms

// What CheckForAggressive() adds for an early request.
const unsigned EARLY_REQUEST_SCORE = 3;

// How high our requests may lift a source's score: two early requests in a row. aMule bans at 10,
// so one early request we did not count stays below it. eMule counts only OP_STARTUPLOADREQ, 1 for
// each early one, and bans at 4; its count stays at 2 or less. Our copy is not saved, so right
// after a restart a source can still hold up to this much from before it.
const unsigned REQUEST_SCORE_BUDGET = 6;

inline bool IsEarlyRequest(uint64_t now, uint64_t lastPacedRequest)
{
	return lastPacedRequest != 0 && now - lastPacedRequest < MIN_REQUESTTIME;
}

// The source's CheckForAggressive() score after one more request.
inline unsigned AggressiveScoreAfter(unsigned score, uint64_t now, uint64_t lastPacedRequest)
{
	if (IsEarlyRequest(now, lastPacedRequest)) {
		return score + EARLY_REQUEST_SCORE;
	}
	return score > 0 ? score - 1 : 0;
}

// Every OP_STARTUPLOADREQ, OP_REASKFILEPING and OP_AICHREQUEST we send must pass this. A request
// that is not early always may: it lowers the score.
inline bool FitsRequestBudget(unsigned score, uint64_t now, uint64_t lastPacedRequest)
{
	return !IsEarlyRequest(now, lastPacedRequest) || score + EARLY_REQUEST_SCORE <= REQUEST_SCORE_BUDGET;
}

// An aMule source drops a waiting client MAX_PURGEQUEUETIME after its last request. Its client
// list cleanup then deletes the client, and the score, within CLIENTLIST_CLEANUP_TIME.
const uint64_t REQUEST_SCORE_MEMORY = MAX_PURGEQUEUETIME + CLIENTLIST_CLEANUP_TIME;

// The scores of deleted clients, so that a client found again does not start at 0.
template <typename Key> class CRequestScoreMemory
{
public:
	void Remember(const Key &key, unsigned score, uint64_t lastPacedRequest, uint64_t now)
	{
		if (lastPacedRequest == 0 || (score == 0 && !IsEarlyRequest(now, lastPacedRequest))) {
			return; // a new client gets the same score for its next request
		}
		SEntry &entry = m_entries[key];
		entry.score = std::max(entry.score, score);
		entry.lastPacedRequest = std::max(entry.lastPacedRequest, lastPacedRequest);
	}

	// Raises score and lastPacedRequest to what was remembered for the key, and forgets it.
	void Recall(const Key &key, uint64_t now, unsigned &score, uint64_t &lastPacedRequest)
	{
		const auto it = m_entries.find(key);
		if (it == m_entries.end()) {
			return;
		}
		if (!IsExpired(it->second, now)) {
			score = std::max(score, it->second.score);
			lastPacedRequest = std::max(lastPacedRequest, it->second.lastPacedRequest);
		}
		m_entries.erase(it);
	}

	void Prune(uint64_t now)
	{
		for (auto it = m_entries.begin(); it != m_entries.end();) {
			it = IsExpired(it->second, now) ? m_entries.erase(it) : std::next(it);
		}
	}

	size_t Size() const { return m_entries.size(); }

private:
	struct SEntry
	{
		unsigned score = 0;
		uint64_t lastPacedRequest = 0;
	};

	static bool IsExpired(const SEntry &entry, uint64_t now)
	{
		return now - entry.lastPacedRequest >= REQUEST_SCORE_MEMORY;
	}

	std::map<Key, SEntry> m_entries;
};

#endif // REQUESTPACING_H
// File_checked_for_headers
