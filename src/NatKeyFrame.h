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

#ifndef AMULE_NAT_KEY_FRAME_H
#define AMULE_NAT_KEY_FRAME_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>

/**
 * The NAT-T key frame: a NAT-T frame (type 0xFF) carrying the sender's 16-byte user hash, which
 * tells the receiver what to obfuscate its datagrams to that endpoint with.
 *
 * Both containers are bounded and take the clock as an argument, so a flood of endpoints can neither
 * grow them nor depend on wall time. Neither persists anything: a hash learned here selects an
 * obfuscation key for one live exchange and is not an identity (the hello stays authoritative).
 */
namespace NatKeyFrame
{
constexpr std::size_t kHashBytes = 16;
using Hash = std::array<std::uint8_t, kHashBytes>;

inline Hash BuildPayload(const Hash &userHash)
{
	return userHash;
}

//! At least a hash's worth of bytes; surplus is ignored, as eMuleAI does. An all-zero hash is its
//! marker for "no key", so it is not accepted as one.
inline std::optional<Hash> ParsePayload(const std::uint8_t *payload, std::size_t length)
{
	if (payload == nullptr || length < kHashBytes) {
		return std::nullopt;
	}
	Hash hash;
	std::copy_n(payload, kHashBytes, hash.begin());
	if (std::all_of(hash.begin(), hash.end(), [](std::uint8_t byte) { return byte == 0; })) {
		return std::nullopt;
	}
	return hash;
}

namespace Detail
{
inline std::uint64_t EndpointKey(std::uint32_t ip, std::uint16_t port)
{
	return (static_cast<std::uint64_t>(ip) << 16) | port;
}

//! Tolerates a clock that moves backwards: an entry is then not yet expired.
inline bool Elapsed(std::uint64_t now, std::uint64_t since, std::uint64_t window)
{
	return now >= since && now - since >= window;
}

template <typename Map>
void EvictOldest(Map &entries)
{
	auto oldest = entries.begin();
	for (auto it = entries.begin(); it != entries.end(); ++it) {
		if (it->second.stamp < oldest->second.stamp) {
			oldest = it;
		}
	}
	entries.erase(oldest);
}
} // namespace Detail

//! Hash a peer announced for its endpoint, kept only while that endpoint stays in use.
class CPeerKeyCache
{
public:
	CPeerKeyCache(std::size_t capacity, std::uint64_t idleMs)
	: m_capacity(capacity)
	, m_idleMs(idleMs)
	{
	}

	void Learn(std::uint32_t ip, std::uint16_t port, const Hash &hash, std::uint64_t nowMs)
	{
		if (m_capacity == 0) {
			return;
		}
		const std::uint64_t key = Detail::EndpointKey(ip, port);
		auto existing = m_entries.find(key);
		if (existing == m_entries.end()) {
			PurgeExpired(nowMs);
			while (m_entries.size() >= m_capacity) {
				Detail::EvictOldest(m_entries);
			}
		}
		m_entries[key] = Entry{ hash, nowMs };
	}

	//! A hit counts as use and extends the entry's life.
	std::optional<Hash> Lookup(std::uint32_t ip, std::uint16_t port, std::uint64_t nowMs)
	{
		auto found = m_entries.find(Detail::EndpointKey(ip, port));
		if (found == m_entries.end()) {
			return std::nullopt;
		}
		if (Detail::Elapsed(nowMs, found->second.stamp, m_idleMs)) {
			m_entries.erase(found);
			return std::nullopt;
		}
		found->second.stamp = nowMs;
		return found->second.hash;
	}

	std::size_t Size() const { return m_entries.size(); }

private:
	struct Entry
	{
		Hash hash;
		std::uint64_t stamp;
	};

	void PurgeExpired(std::uint64_t nowMs)
	{
		for (auto it = m_entries.begin(); it != m_entries.end();) {
			it = Detail::Elapsed(nowMs, it->second.stamp, m_idleMs) ? m_entries.erase(it) : std::next(it);
		}
	}

	std::size_t m_capacity;
	std::uint64_t m_idleMs;
	std::map<std::uint64_t, Entry> m_entries;
};

//! At most one key frame per endpoint per window, so a peer cannot make us send them in volume.
class CSendGate
{
public:
	CSendGate(std::size_t capacity, std::uint64_t windowMs)
	: m_capacity(capacity)
	, m_windowMs(windowMs)
	{
	}

	bool TryAcquire(std::uint32_t ip, std::uint16_t port, std::uint64_t nowMs)
	{
		if (m_capacity == 0) {
			return false;
		}
		const std::uint64_t key = Detail::EndpointKey(ip, port);
		auto existing = m_entries.find(key);
		if (existing != m_entries.end()) {
			if (!Detail::Elapsed(nowMs, existing->second.stamp, m_windowMs)) {
				return false;
			}
			existing->second.stamp = nowMs;
			return true;
		}
		PurgeExpired(nowMs);
		while (m_entries.size() >= m_capacity) {
			Detail::EvictOldest(m_entries);
		}
		m_entries[key] = Entry{ nowMs };
		return true;
	}

	std::size_t Size() const { return m_entries.size(); }

private:
	struct Entry
	{
		std::uint64_t stamp;
	};

	void PurgeExpired(std::uint64_t nowMs)
	{
		for (auto it = m_entries.begin(); it != m_entries.end();) {
			it = Detail::Elapsed(nowMs, it->second.stamp, m_windowMs) ? m_entries.erase(it)
										   : std::next(it);
		}
	}

	std::size_t m_capacity;
	std::uint64_t m_windowMs;
	std::map<std::uint64_t, Entry> m_entries;
};
} // namespace NatKeyFrame

#endif // AMULE_NAT_KEY_FRAME_H
