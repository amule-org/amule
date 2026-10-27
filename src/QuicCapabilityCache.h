//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// Any parts of this program contributed by third-party developers are copyrighted
// by their respective authors.
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

#ifndef QUIC_CAPABILITY_CACHE_H
#define QUIC_CAPABILITY_CACHE_H

#include "QuicNattProtocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <utility>
#include <vector>

// Pure, process-local capability state.  The caller supplies monotonic time;
// this type deliberately has no knowledge of clients, endpoints, or routing.
enum class QuicCapabilityState
{
	Unknown,
	Compatible,
	Incompatible,
	Cooldown
};

struct QuicCapabilityResult
{
	QuicCapabilityState state;
	uint8_t options;
	QuicCapabilityResult(QuicCapabilityState value = QuicCapabilityState::Unknown, uint8_t bits = 0)
	: state(value)
	, options(bits)
	{
	}
};

class CQuicCapabilityCache
{
public:
	explicit CQuicCapabilityCache(std::size_t maximum)
	: m_maximum(maximum)
	{
	}

	QuicCapabilityResult Lookup(const std::array<uint8_t, 16> &peer, uint64_t nowMs)
	{
		Expire(nowMs);
		const auto found = m_records.find(peer);
		if (found == m_records.end())
			return {};
		Touch(found);
		return { found->second.state, found->second.options };
	}

	void Remember(const std::array<uint8_t, 16> &peer, uint8_t options, uint64_t nowMs, uint64_t ttlMs)
	{
		Store(peer,
			options,
			(options & QuicNatt::EAQC_CAP_QUIC) != 0 ? QuicCapabilityState::Compatible
								 : QuicCapabilityState::Incompatible,
			nowMs,
			ttlMs);
	}

	void RememberFailure(const std::array<uint8_t, 16> &peer, uint64_t nowMs, uint64_t cooldownMs)
	{
		Store(peer, 0, QuicCapabilityState::Cooldown, nowMs, cooldownMs);
	}

	std::size_t Size(uint64_t nowMs)
	{
		Expire(nowMs);
		return m_records.size();
	}

private:
	using Peer = std::array<uint8_t, 16>;
	using LruList = std::list<Peer>;
	struct Record
	{
		QuicCapabilityState state;
		uint8_t options;
		uint64_t expiresAt;
		LruList::iterator lru;
	};
	using Map = std::map<Peer, Record>;
	using Iterator = Map::iterator;

	void Expire(uint64_t nowMs)
	{
		for (auto it = m_records.begin(); it != m_records.end();) {
			if (nowMs >= it->second.expiresAt) {
				m_lru.erase(it->second.lru);
				it = m_records.erase(it);
			} else
				++it;
		}
	}

	void Touch(Iterator it)
	{
		m_lru.erase(it->second.lru);
		m_lru.push_front(it->first);
		it->second.lru = m_lru.begin();
	}

	static uint64_t Expiration(uint64_t nowMs, uint64_t ttlMs)
	{
		const auto maximum = UINT64_MAX;
		return ttlMs > maximum - nowMs ? maximum : nowMs + ttlMs;
	}

	void Store(const std::array<uint8_t, 16> &peer,
		uint8_t options,
		QuicCapabilityState state,
		uint64_t nowMs,
		uint64_t ttlMs)
	{
		Expire(nowMs);
		if (m_maximum == 0 || ttlMs == 0)
			return;
		const auto existing = m_records.find(peer);
		if (existing != m_records.end()) {
			m_lru.erase(existing->second.lru);
			m_records.erase(existing);
		}
		m_lru.push_front(peer);
		m_records.emplace(peer, Record{ state, options, Expiration(nowMs, ttlMs), m_lru.begin() });
		while (m_records.size() > m_maximum) {
			const auto old = m_lru.back();
			m_lru.pop_back();
			m_records.erase(old);
		}
	}

	std::size_t m_maximum;
	Map m_records;
	std::list<std::array<uint8_t, 16>> m_lru;
};

// Pure CAPS resend/resolve policy for a peer whose capability was still Unknown when a
// server-vouched LowID dial started: resends CAPS up to twice more and holds the transport
// choice for the same timeout as eMuleAI's DIRECT_NATT_CAPS_LEGACY_TIMEOUT_MS before giving up
// on QUIC. Pure inputs/outputs only: the caller owns the clock, the socket, and the client
// reference.
constexpr uint64_t kQuicCapsHoldoffDeadlineMs = 1500;
constexpr uint64_t kQuicCapsHoldoffResendIntervalMs = 500;
constexpr int kQuicCapsHoldoffMaxProbes = 3;

struct QuicCapsHoldoffDecision
{
	bool resend = false;
	bool resolveQuic = false;
	bool resolveUtp = false;
};

inline QuicCapsHoldoffDecision DecideQuicCapsHoldoff(
	QuicCapabilityState state, int probesSent, uint64_t elapsedMs)
{
	if (state == QuicCapabilityState::Compatible)
		return { false, true, false };
	if (state == QuicCapabilityState::Incompatible || state == QuicCapabilityState::Cooldown ||
		elapsedMs >= kQuicCapsHoldoffDeadlineMs)
		return { false, false, true };
	// Still Unknown and within the deadline: resend once per 500 ms window, up to three
	// probes total (the first one is already sent before the holdoff is registered).
	const bool resend = probesSent < kQuicCapsHoldoffMaxProbes &&
			    elapsedMs >= static_cast<uint64_t>(probesSent) * kQuicCapsHoldoffResendIntervalMs;
	return { resend, false, false };
}

//! Peers whose transport choice waits for a CAPS answer. Owner keeps the dialling client alive.
template <typename Owner> class CQuicCapsHoldoffs
{
public:
	struct STarget
	{
		std::array<uint8_t, 16> peer;
		uint32_t ipv4;
		uint16_t udpPort;
		std::array<uint8_t, 16> fileHash;
	};

	struct SCallbacks
	{
		std::function<bool(Owner &)> isAlive;
		std::function<QuicCapabilityState(const std::array<uint8_t, 16> &, uint64_t)> lookup;
		std::function<void(const STarget &)> sendCaps;
		//! False when the dial did not start.
		std::function<bool(const STarget &, Owner &, uint64_t)> dialQuic;
		std::function<void(const STarget &, Owner &, uint64_t)> quicDialFailed;
		std::function<void(Owner &)> fallBackToUtp;
	};

	explicit CQuicCapsHoldoffs(SCallbacks callbacks)
	: m_callbacks(std::move(callbacks))
	{
	}

	//! Call after sending the first CAPS.
	void Add(const STarget &target, uint64_t nowMs, Owner owner)
	{
		m_entries.push_back(SEntry{ target, nowMs, 1, std::move(owner) });
	}

	bool Empty() const { return m_entries.empty(); }

	void Service(uint64_t nowMs)
	{
		// Indexed, and each entry is removed before its callbacks run: they can call Add().
		for (std::size_t i = 0; i < m_entries.size();) {
			SEntry &entry = m_entries[i];
			if (!m_callbacks.isAlive(entry.owner)) {
				RemoveAt(i);
				continue;
			}
			const auto decision =
				DecideQuicCapsHoldoff(m_callbacks.lookup(entry.target.peer, nowMs),
					entry.probesSent,
					nowMs - entry.startedAt);
			if (decision.resend) {
				++entry.probesSent;
				const STarget target = entry.target;
				++i;
				m_callbacks.sendCaps(target);
				continue;
			}
			if (!decision.resolveQuic && !decision.resolveUtp) {
				++i;
				continue;
			}
			SEntry resolved = std::move(entry);
			RemoveAt(i);
			if (decision.resolveUtp) {
				m_callbacks.fallBackToUtp(resolved.owner);
			} else if (!m_callbacks.dialQuic(resolved.target, resolved.owner, nowMs)) {
				m_callbacks.quicDialFailed(resolved.target, resolved.owner, nowMs);
			}
		}
	}

private:
	struct SEntry
	{
		STarget target;
		uint64_t startedAt;
		int probesSent;
		Owner owner;
	};

	void RemoveAt(std::size_t i) { m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(i)); }

	SCallbacks m_callbacks;
	std::vector<SEntry> m_entries;
};

#endif
