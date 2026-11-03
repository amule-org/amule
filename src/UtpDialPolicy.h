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

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
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
	// A uTP dial to this endpoint fell back to TCP recently (UtpDialFailureMemory), so the
	// legacy connect goes first instead of waiting out the uTP handshake window again.
	bool recentDialFailure = false;
};

inline EUtpDialDecision DecideUtpDial(const SUtpDialFacts &facts)
{
	return facts.peerSupportsUtp && facts.localOutboundService &&
			       (facts.directHighId || facts.serverVouchedLowId) && !facts.proxyEnabled &&
			       facts.routableEndpoint && facts.obfuscationSatisfied &&
			       !facts.recentDialFailure
		       ? EUtpDialDecision::TryUtp
		       : EUtpDialDecision::PreserveLegacy;
}

// Endpoints whose uTP handshake did not complete and fell back to TCP. The cooldown outlasts the
// default source re-ask interval, so the next contact goes straight to TCP; a HighID peer still
// has TCP, so a stale entry costs only the uTP attempt. Keyed by endpoint, not by peer: a peer
// that moves to another address or port is dialled as before.
class UtpDialFailureMemory
{
public:
	static constexpr std::uint64_t CooldownMs = 60 * 60 * 1000;
	static constexpr std::size_t MaxEntries = 512;

	bool IsCoolingDown(std::uint32_t ip, std::uint16_t udpPort, std::uint64_t nowMs)
	{
		PruneExpired(nowMs);
		return Find(ip, udpPort) != m_failures.end();
	}

	void RecordFailure(std::uint32_t ip, std::uint16_t udpPort, std::uint64_t nowMs)
	{
		PruneExpired(nowMs);
		const auto existing = Find(ip, udpPort);
		if (existing != m_failures.end()) {
			m_failures.erase(existing);
		} else if (m_failures.size() >= MaxEntries) {
			m_failures.pop_front();
		}
		m_failures.push_back({ ip, udpPort, nowMs });
	}

private:
	struct Failure
	{
		std::uint32_t ip;
		std::uint16_t udpPort;
		std::uint64_t at;
	};

	std::deque<Failure>::iterator Find(std::uint32_t ip, std::uint16_t udpPort)
	{
		return std::find_if(
			m_failures.begin(), m_failures.end(), [ip, udpPort](const Failure &failure) {
				return failure.ip == ip && failure.udpPort == udpPort;
			});
	}

	// Entries are kept in recording order, so the expired ones are at the front.
	void PruneExpired(std::uint64_t nowMs)
	{
		while (!m_failures.empty() && nowMs >= m_failures.front().at &&
			nowMs - m_failures.front().at >= CooldownMs) {
			m_failures.pop_front();
		}
	}

	std::deque<Failure> m_failures;
};

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

	std::uint32_t Take()
	{
		const std::uint32_t peerToken = m_selected ? m_peerToken : 0;
		Clear();
		return peerToken;
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
	return { true, false };
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

inline std::optional<SUtpEndpoint> MakeLocalUtpEndpoint(
	std::uint32_t publicIp, std::uint16_t externalUdpPort, std::uint16_t udpPort, std::uint16_t tcpPort)
{
	const std::uint16_t bestUdpPort =
		externalUdpPort != 0 ? externalUdpPort : (udpPort != 0 ? udpPort : tcpPort);
	return MakeUtpEndpointFromNetworkOrder(publicIp, bestUdpPort);
}

// A configured UDP port is not evidence of a NAT's public mapping. Before Kad has verified an
// external UDP port, rejecting an incoming simultaneous-open SYN risks both peers retaining their
// outgoing dials based on different local tuples. Accept it and let OP_HELLO make the final
// identity and winner decision instead.
inline bool CanRejectIncomingUtpSynForSimultaneousOpen(std::uint16_t externalUdpPort)
{
	return externalUdpPort != 0;
}

inline bool CanInstallUtpPortTranslation(bool observedEndpointMapped, bool expectedEndpointMapped)
{
	return !observedEndpointMapped && !expectedEndpointMapped;
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
	std::uint32_t clientToken = 0;
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
		const std::uint16_t delta = expectedPort > observedPort ? expectedPort - observedPort
									: observedPort - expectedPort;
		const bool exactEndpoint = expectedPort == observedPort;
		const bool boundedPortChange =
			candidate.remapAllowed && !exactEndpoint && delta <= kUtpPortRemapWindow &&
			nowMs >= candidate.registeredAtMs &&
			nowMs - candidate.registeredAtMs < kUtpPortRemapExpectedPeerTtlMs;
		const bool eligible = candidate.outbound && candidate.handshakePending &&
				      candidate.ip == observedIp && observedIp != 0 && observedPort != 0 &&
				      expectedPort != 0 && (exactEndpoint || boundedPortChange);
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
		const std::uint16_t delta = expectedPort > observedPort ? expectedPort - observedPort
									: observedPort - expectedPort;
		const bool eligible = candidate.outbound && candidate.remapAllowed &&
				      candidate.handshakePending && candidate.ip == observedIp &&
				      observedPort != 0 && expectedPort != 0 &&
				      expectedPort != observedPort && nowMs >= candidate.registeredAtMs &&
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

//! Whether a datagram's source port may be the NAT moving the port this stream already holds. The
//! address is implied: the caller only asks about streams on the source address, and a different
//! address is a different peer rather than a repoint.
inline bool IsUtpPortRepointOf(std::uint16_t observedPort, std::uint16_t heldPort)
{
	if (observedPort == 0 || heldPort == 0 || observedPort == heldPort) {
		return false;
	}
	const std::uint16_t delta =
		observedPort > heldPort ? observedPort - heldPort : heldPort - observedPort;
	return delta <= kUtpPortRemapWindow;
}

//! A connected stream as the repoint decision sees it: the address it holds and whether it is
//! usable at all.
struct SUtpConnectedStream
{
	std::uint32_t ip = 0;
	std::uint16_t port = 0;
	bool connected = false;
};

//! The one connected stream this datagram can belong to, or none when two could be it. libutp
//! takes the address it is given at face value, so an ambiguous candidate must fail closed here,
//! exactly as an ambiguous handshake remap does.
template <typename Range>
std::optional<std::size_t> FindUniqueUtpRepointCandidate(
	const Range &streams, std::uint32_t observedIp, std::uint16_t observedPort)
{
	std::optional<std::size_t> match;
	std::size_t index = 0;
	for (const auto &stream : streams) {
		const bool eligible = stream.connected && observedIp != 0 && stream.ip == observedIp &&
				      IsUtpPortRepointOf(observedPort, stream.port);
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

/**
 * Bounds how often a stream may follow a NAT source-port change: a few moves per stream and a few
 * more for the adapter, both over one sliding minute. The bookkeeping is fixed-capacity, so a peer
 * replaying source ports can neither grow it nor evict another stream's record; a full table
 * refuses, which keeps the failure closed. Time is injected, so the bounds are testable.
 */
class UtpPortRepointLimiter
{
public:
	static constexpr std::size_t MovesPerStream = 3;
	static constexpr std::size_t MovesOverall = 10;
	static constexpr std::uint64_t WindowMs = 60'000;
	//! Below MovesOverall on purpose: the per-stream table is the tighter bound on distinct streams.
	static constexpr std::size_t MaxStreams = 8;

	//! Records this stream's attempt and returns true, or returns false and records nothing.
	bool TryAdmit(std::uint64_t streamId, std::uint64_t nowMs)
	{
		ExpireStreams(nowMs);
		if (CountRecentMoves(nowMs) >= MovesOverall) {
			return false;
		}
		SStreamMoves *stream = FindStream(streamId);
		if (stream == nullptr) {
			stream = ReserveStream(streamId, nowMs);
			if (stream == nullptr) {
				return false;
			}
		}
		if (stream->moves >= MovesPerStream) {
			return false;
		}
		++stream->moves;
		m_moves[m_nextMove] = nowMs;
		m_nextMove = (m_nextMove + 1) % MovesOverall;
		if (m_storedMoves < MovesOverall) {
			++m_storedMoves;
		}
		return true;
	}

private:
	struct SStreamMoves
	{
		std::uint64_t id = 0;
		std::uint64_t windowStart = 0;
		std::size_t moves = 0;
		bool used = false;
	};

	SStreamMoves *FindStream(std::uint64_t streamId)
	{
		for (auto &stream : m_streams) {
			if (stream.used && stream.id == streamId) {
				return &stream;
			}
		}
		return nullptr;
	}

	//! A free slot for a stream that has not moved inside the window, or none at all.
	SStreamMoves *ReserveStream(std::uint64_t streamId, std::uint64_t nowMs)
	{
		for (auto &stream : m_streams) {
			if (stream.used) {
				continue;
			}
			stream = SStreamMoves{};
			stream.used = true;
			stream.id = streamId;
			stream.windowStart = nowMs;
			return &stream;
		}
		return nullptr;
	}

	void ExpireStreams(std::uint64_t nowMs)
	{
		for (auto &stream : m_streams) {
			if (stream.used && IsExpired(stream.windowStart, nowMs)) {
				stream = SStreamMoves{};
			}
		}
	}

	std::size_t CountRecentMoves(std::uint64_t nowMs) const
	{
		std::size_t moves = 0;
		for (std::size_t index = 0; index < m_storedMoves; ++index) {
			if (nowMs >= m_moves[index] && nowMs - m_moves[index] < WindowMs) {
				++moves;
			}
		}
		return moves;
	}

	static bool IsExpired(std::uint64_t sinceMs, std::uint64_t nowMs)
	{
		return nowMs >= sinceMs && nowMs - sinceMs >= WindowMs;
	}

	std::array<SStreamMoves, MaxStreams> m_streams{};
	// The last MovesOverall admissions: the ring can only drop one that has left the window.
	std::array<std::uint64_t, MovesOverall> m_moves{};
	std::size_t m_nextMove = 0;
	std::size_t m_storedMoves = 0;
};

#endif // UTP_DIAL_POLICY_H
