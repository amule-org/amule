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

#ifndef NATRENDEZVOUSPOLICY_H
#define NATRENDEZVOUSPOLICY_H

#include "PeerAddressing.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

/**
 * Pure admission policy for a future NAT rendezvous control path.
 *
 * This deliberately does not define a wire opcode or forward packets. A relay cannot prove that
 * a candidate peer is reachable, so the first slice only validates observed endpoints and bounds
 * request pressure. Production handlers must use the observed source, never a peer-provided hint.
 */
namespace NatRendezvous
{
constexpr std::uint64_t kRequestThrottleMs = 3 * 1000;
constexpr std::size_t kMaxTrackedRequesters = 256;

inline std::optional<PeerAddressing::UdpEndpoint> AcceptObservedEndpoint(
	const PeerAddressing::UdpEndpoint &claimed, const PeerAddressing::UdpEndpoint &observed) noexcept
{
	if (!PeerAddressing::MatchesUdpSource(claimed, observed)) {
		return std::nullopt;
	}
	return observed;
}

class CRequesterLimiter
{
public:
	CRequesterLimiter() = default;
	CRequesterLimiter(std::uint64_t throttleMs, std::size_t maxTracked) noexcept
	: m_throttleMs(throttleMs)
	, m_maxTracked(maxTracked)
	{
	}

	/** @p now is a monotonic millisecond tick; rollback re-anchors the request window. */
	bool Admit(const CNetworkAddress &requester, std::uint64_t now) noexcept
	{
		if (!PeerAddressing::IsSecurityKey(requester)) {
			return false;
		}
		const CNetworkAddress key = PeerAddressing::RateLimitScope(requester);

		Evict(now);
		for (auto &entry : m_entries) {
			if (entry.address == key) {
				if (now >= entry.lastRequest && now - entry.lastRequest < m_throttleMs) {
					return false;
				}
				entry.lastRequest = now;
				return true;
			}
		}

		if (m_entries.size() >= m_maxTracked) {
			const auto oldest = std::min_element(m_entries.begin(),
				m_entries.end(),
				[](const SEntry &left, const SEntry &right) {
					return left.lastRequest < right.lastRequest;
				});
			m_entries.erase(oldest);
		}
		m_entries.push_back({ key, now });
		return true;
	}

private:
	struct SEntry
	{
		CNetworkAddress address;
		std::uint64_t lastRequest;
	};

	void Evict(std::uint64_t now) noexcept
	{
		const auto firstLive =
			std::remove_if(m_entries.begin(), m_entries.end(), [this, now](const SEntry &entry) {
				return now >= entry.lastRequest && now - entry.lastRequest >= m_throttleMs;
			});
		m_entries.erase(firstLive, m_entries.end());
	}

	std::uint64_t m_throttleMs = kRequestThrottleMs;
	std::size_t m_maxTracked = kMaxTrackedRequesters;
	std::vector<SEntry> m_entries;
};
} // namespace NatRendezvous

#endif // NATRENDEZVOUSPOLICY_H
