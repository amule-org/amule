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

#ifndef UTP_DIAL_POLICY_H
#define UTP_DIAL_POLICY_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

// Preserve the TCP/callback/buddy/refusal path unless direct uTP is eligible.
// This policy does not replace security checks or authorize NAT rendezvous.
enum class EUtpDialDecision
{
	PreserveLegacy,
	TryUtp
};

struct SUtpDialFacts
{
	bool peerSupportsUtp = false;
	bool localOutboundService = false;
	bool directHighId = false;
	bool proxyEnabled = false;
	bool routableEndpoint = false;
	// The stream handshake never runs over a transport, so a peer owed
	// obfuscation may only be dialled when the frames carry it themselves.
	bool obfuscationSatisfied = false;
	// ed2k-server vouched for this LowID peer's endpoint via OP_LOWID_HOLEPUNCH_INFO: an
	// alternative to directHighId, not an addition to it -- either is sufficient grounds to
	// try uTP.
	bool serverVouchedLowId = false;
};

inline EUtpDialDecision DecideUtpDial(const SUtpDialFacts &facts)
{
	return facts.peerSupportsUtp && facts.localOutboundService &&
			       (facts.directHighId || facts.serverVouchedLowId) && !facts.proxyEnabled &&
			       facts.routableEndpoint && facts.obfuscationSatisfied
		       ? EUtpDialDecision::TryUtp
		       : EUtpDialDecision::PreserveLegacy;
}

/** Whether this side keeps the already-found transport in a simultaneous dial. */
inline bool ShouldKeepFoundUtp(bool foundInbound, const uint8_t *localHash, const uint8_t *peerHash)
{
	return foundInbound == (std::memcmp(localHash, peerHash, 16) > 0);
}

constexpr std::uint64_t kUtpPortRemapExpectedPeerTtlMs = 30'000;
constexpr std::uint16_t kUtpPortRemapWindow = 512;

struct SUtpRemapCandidate
{
	std::uint32_t ip = 0;
	std::uint16_t port = 0;
	std::uint64_t registeredAtMs = 0;
	bool handshakePending = false;
	bool remapAllowed = false;
};

//! Return the only eligible peer whose observed source port is within the bounded remap window.
template <typename Range>
std::optional<std::size_t> FindUniqueUtpPortRemapCandidate(
	const Range &candidates, std::uint32_t observedIp, std::uint16_t observedPort, std::uint64_t nowMs)
{
	std::optional<std::size_t> match;
	std::size_t index = 0;
	for (const auto &candidate : candidates) {
		const std::uint16_t expectedPort = candidate.port;
		const std::uint16_t delta = expectedPort > observedPort ? expectedPort - observedPort : observedPort - expectedPort;
		const bool eligible = candidate.remapAllowed && candidate.handshakePending && candidate.ip == observedIp &&
				      observedPort != 0 && expectedPort != 0 && expectedPort != observedPort &&
				      nowMs >= candidate.registeredAtMs &&
				      nowMs - candidate.registeredAtMs < kUtpPortRemapExpectedPeerTtlMs &&
				      delta <= kUtpPortRemapWindow;
		if (eligible) {
			if (match.has_value()) {
				return std::nullopt;
			}
			match = index;
		}
		++index;
	}
	return match;
}

#endif // UTP_DIAL_POLICY_H
