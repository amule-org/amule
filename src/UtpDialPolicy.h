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

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>

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

struct SUtpIncomingSynWinner
{
	void Select(std::uint32_t peerToken)
	{
		m_selected = true;
		m_peerToken = peerToken;
	}

	bool Consume(std::uint32_t peerToken)
	{
		const bool matches = m_selected && m_peerToken == peerToken;
		Clear();
		return matches;
	}

	void Clear()
	{
		m_selected = false;
		m_peerToken = 0;
	}

private:
	bool m_selected = false;
	std::uint32_t m_peerToken = 0;
};

struct SUtpHelloAttachmentDecision
{
	bool keepFound = false;
	bool socketSwap = false;
};

inline SUtpHelloAttachmentDecision DecideUtpHelloAttachment(bool incomingSynWinnerMatches,
	bool foundInbound,
	bool hashesValid,
	const uint8_t *localHash,
	const uint8_t *peerHash)
{
	if (incomingSynWinnerMatches) {
		return { false, true };
	}
	if (hashesValid && localHash != nullptr && peerHash != nullptr) {
		const bool keepFound = ShouldKeepFoundUtp(foundInbound, localHash, peerHash);
		return { keepFound, !keepFound };
	}
	return {};
}

template <typename NoteFileRequestLost, typename CloseFoundSocket, typename AttachIncomingSocket>
inline bool ApplyUtpHelloSocketSwap(const SUtpHelloAttachmentDecision &decision,
	bool downloadConnected,
	NoteFileRequestLost &&noteFileRequestLost,
	CloseFoundSocket &&closeFoundSocket,
	AttachIncomingSocket &&attachIncomingSocket)
{
	if (decision.keepFound || !decision.socketSwap) {
		return false;
	}
	if (downloadConnected) {
		noteFileRequestLost();
	}
	closeFoundSocket();
	attachIncomingSocket();
	return true;
}

struct SUtpHelloAttachmentResult
{
	SUtpHelloAttachmentDecision decision;
	bool socketAttached = false;
};

template <typename NoteFileRequestLost, typename CloseFoundSocket, typename AttachIncomingSocket>
inline SUtpHelloAttachmentResult AttachUtpSocketAfterVerifiedHello(bool winnerMatchesVerifiedPeer,
	bool foundInbound,
	bool hashesValid,
	const uint8_t *localHash,
	const uint8_t *peerHash,
	bool downloadConnected,
	NoteFileRequestLost &&noteFileRequestLost,
	CloseFoundSocket &&closeFoundSocket,
	AttachIncomingSocket &&attachIncomingSocket)
{
	const SUtpHelloAttachmentDecision decision = DecideUtpHelloAttachment(
		winnerMatchesVerifiedPeer, foundInbound, hashesValid, localHash, peerHash);
	const bool socketAttached = ApplyUtpHelloSocketSwap(decision,
		downloadConnected,
		std::forward<NoteFileRequestLost>(noteFileRequestLost),
		std::forward<CloseFoundSocket>(closeFoundSocket),
		std::forward<AttachIncomingSocket>(attachIncomingSocket));
	return { decision, socketAttached };
}

inline bool ConsumeUtpFileRequestRetry(bool &fileRequestLost, bool downloadConnected)
{
	if (!fileRequestLost) {
		return false;
	}
	fileRequestLost = false;
	return downloadConnected;
}

template <typename SendFileRequest>
inline bool RetryFileRequestAfterUtpSocketSwap(
	bool &fileRequestLost, bool downloadConnected, SendFileRequest &&sendFileRequest)
{
	if (!ConsumeUtpFileRequestRetry(fileRequestLost, downloadConnected)) {
		return false;
	}
	sendFileRequest();
	return true;
}

struct SUtpEndpoint
{
	std::array<uint8_t, 4> address;
	uint16_t port;
};

inline std::optional<SUtpEndpoint> MakeUtpEndpointFromNetworkOrder(std::uint32_t ip, std::uint16_t port)
{
	if (ip == 0 || port == 0) {
		return std::nullopt;
	}
	return SUtpEndpoint{ { static_cast<uint8_t>(ip),
		static_cast<uint8_t>(ip >> 8),
		static_cast<uint8_t>(ip >> 16),
		static_cast<uint8_t>(ip >> 24) },
		port };
}

enum class EUtpSimultaneousOpenDecision
{
	KeepOutgoing,
	KeepIncoming
};

inline EUtpSimultaneousOpenDecision DecideUtpSimultaneousOpen(
	const std::optional<SUtpEndpoint> &localEndpoint,
	const std::optional<SUtpEndpoint> &remoteEndpoint,
	const uint8_t *localHash,
	bool localHashValid,
	const uint8_t *peerHash,
	bool peerHashValid)
{
	if (localEndpoint && remoteEndpoint && localEndpoint->port != 0 && remoteEndpoint->port != 0) {
		if (localEndpoint->address != remoteEndpoint->address) {
			return localEndpoint->address < remoteEndpoint->address
				? EUtpSimultaneousOpenDecision::KeepOutgoing
				: EUtpSimultaneousOpenDecision::KeepIncoming;
		}
		if (localEndpoint->port != remoteEndpoint->port) {
			return localEndpoint->port < remoteEndpoint->port
				? EUtpSimultaneousOpenDecision::KeepOutgoing
				: EUtpSimultaneousOpenDecision::KeepIncoming;
		}
	}

	if (localHashValid && peerHashValid && localHash != nullptr && peerHash != nullptr) {
		const int comparison = std::memcmp(localHash, peerHash, 16);
		if (comparison != 0) {
			return comparison < 0 ? EUtpSimultaneousOpenDecision::KeepOutgoing
					     : EUtpSimultaneousOpenDecision::KeepIncoming;
		}
	}
	return EUtpSimultaneousOpenDecision::KeepOutgoing;
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
	bool outbound = false;
};

enum class EUtpSynCandidateStatus
{
	None,
	Unique,
	Ambiguous
};

struct SUtpSynCandidateMatch
{
	EUtpSynCandidateStatus status = EUtpSynCandidateStatus::None;
	std::size_t index = 0;
};

//! Identify one outgoing handshake that could be the peer sending this simultaneous-open SYN.
template <typename Range>
SUtpSynCandidateMatch FindUniqueUtpSynCandidate(
	const Range &candidates, std::uint32_t observedIp, std::uint16_t observedPort, std::uint64_t nowMs)
{
	std::optional<std::size_t> match;
	std::size_t index = 0;
	for (const auto &candidate : candidates) {
		const std::uint16_t expectedPort = candidate.port;
		const std::uint16_t delta = expectedPort > observedPort
			? expectedPort - observedPort
			: observedPort - expectedPort;
		const bool exactEndpoint = expectedPort == observedPort;
		const bool boundedPortChange = candidate.remapAllowed && !exactEndpoint && delta <= kUtpPortRemapWindow &&
					       nowMs >= candidate.registeredAtMs &&
					       nowMs - candidate.registeredAtMs < kUtpPortRemapExpectedPeerTtlMs;
		const bool eligible = candidate.outbound && candidate.handshakePending && candidate.ip == observedIp &&
				      observedIp != 0 && observedPort != 0 && expectedPort != 0 &&
				      (exactEndpoint || boundedPortChange);
		if (eligible) {
			if (match.has_value()) {
				return { EUtpSynCandidateStatus::Ambiguous, 0 };
			}
			match = index;
		}
		++index;
	}
	return match.has_value() ? SUtpSynCandidateMatch{ EUtpSynCandidateStatus::Unique, *match }
				 : SUtpSynCandidateMatch{};
}

//! Return the only eligible peer whose observed source port is within the bounded remap window.
template <typename Range>
std::optional<std::size_t> FindUniqueUtpPortRemapCandidate(
	const Range &candidates, std::uint32_t observedIp, std::uint16_t observedPort, std::uint64_t nowMs)
{
	std::optional<std::size_t> match;
	std::size_t index = 0;
	for (const auto &candidate : candidates) {
		const std::uint16_t expectedPort = candidate.port;
		const std::uint16_t delta = expectedPort > observedPort
			? expectedPort - observedPort
			: observedPort - expectedPort;
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
