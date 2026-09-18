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

inline bool AcceptObservedEndpoint(const PeerAddressing::UdpEndpoint &claimed,
	const PeerAddressing::UdpEndpoint &observed,
	PeerAddressing::UdpEndpoint &accepted) noexcept
{
	if (!PeerAddressing::MatchesUdpSource(claimed, observed)) {
		return false;
	}
	accepted = observed;
	return true;
}

class CRequesterLimiter
{
public:
	bool Admit(const CNetworkAddress &requester, std::uint64_t now) noexcept
	{
		const CNetworkAddress key = PeerAddressing::RateLimitScope(requester);
		if (key.IsAbsent() || key.IsUnspecified()) {
			return false;
		}

		Evict(now);
		for (auto &entry : m_entries) {
			if (entry.address == key) {
				if (now < entry.lastRequest || now - entry.lastRequest < kRequestThrottleMs) {
					return false;
				}
				entry.lastRequest = now;
				return true;
			}
		}

		if (m_entries.size() >= kMaxTrackedRequesters) {
			return false;
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
			std::remove_if(m_entries.begin(), m_entries.end(), [now](const SEntry &entry) {
				return now >= entry.lastRequest &&
				       now - entry.lastRequest >= kRequestThrottleMs;
			});
		m_entries.erase(firstLive, m_entries.end());
	}

	std::vector<SEntry> m_entries;
};
} // namespace NatRendezvous

#endif // NATRENDEZVOUSPOLICY_H
