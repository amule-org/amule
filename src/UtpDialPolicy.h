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
 * Bounds how often a stream may follow a NAT source-port change: one move per stream per ten seconds.
 * A stream keeps its slot for that window, so the fixed table is also the bound on how many streams
 * can move at once: a peer replaying source ports can neither grow it nor evict a live record, and a
 * full table refuses, which keeps the failure closed. Time is injected, so the bound is testable.
 */
class UtpPortRepointLimiter
{
public:
	static constexpr std::size_t MovesPerStream = 1;
	static constexpr std::uint64_t WindowMs = 10'000;
	static constexpr std::size_t MaxStreams = 16;

	//! Records this stream's attempt and returns true, or returns false and records nothing.
	bool TryAdmit(std::uint64_t streamId, std::uint64_t nowMs)
	{
		ExpireStreams(nowMs);
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

	static bool IsExpired(std::uint64_t sinceMs, std::uint64_t nowMs)
	{
		return nowMs >= sinceMs && nowMs - sinceMs >= WindowMs;
	}

	std::array<SStreamMoves, MaxStreams> m_streams{};
};

//! How far a frame's sequence number may sit from the peer's last one, in either direction, and still
//! be that stream's. Mirrors libutp's own receive bound (REORDER_BUFFER_MAX_SIZE, utp_internal.cpp:55,
//! enforced at :1894-1907): anything further apart is not a packet this connection could have in
//! flight.
constexpr std::uint16_t kUtpMoveSeqWindow = 1024;

//! Half the sequence space. The ack range runs from the peer's last ack minus one up to the last
//! number we sent, so a span that wide means our facts disagree: the peer acknowledged more than we
//! sent. Anything narrower is a legitimate window and is admitted, however wide our sends run ahead.
constexpr std::uint16_t kUtpMoveAckSpanLimit = 32'768;

//! The circular distance between two sequence numbers, counting modulo 2^16 (utp_internal.cpp:79).
//! libutp measures a packet the same way against the one it expects next - `seqnr = (pk_seq_nr -
//! conn->ack_nr - 1) & SEQ_NR_MASK` (:1894) - and admits it while that is below
//! REORDER_BUFFER_MAX_SIZE (:1897).
inline std::uint16_t UtpSeqDistance(std::uint16_t left, std::uint16_t right)
{
	const std::uint16_t forward = static_cast<std::uint16_t>(left - right);
	const std::uint16_t backward = static_cast<std::uint16_t>(right - left);
	return forward < backward ? forward : backward;
}

//! Whether @p value lies in the modular range [@p lowest, @p lowest + @p span], counting modulo
//! 2^16. Comparison has to be modular because a long-lived stream wraps past 65535.
inline bool IsUtpSeqWithin(std::uint16_t value, std::uint16_t lowest, std::uint16_t span)
{
	return static_cast<std::uint16_t>(value - lowest) <= span;
}

//! libutp packet types (utp_internal.cpp:157-163). A data frame is the one that carries payload, and
//! only a reset or a SYN change what a move may do.
constexpr std::uint8_t kUtpTypeData = 0;
constexpr std::uint8_t kUtpTypeState = 2;
constexpr std::uint8_t kUtpTypeReset = 3;
constexpr std::uint8_t kUtpTypeSyn = 4;

//! How long a connected stream may stay silent before we poke libutp into sending an ack, and how
//! stale its last data may be before poking is pointless.
constexpr std::uint64_t kUtpProbeIdleMs = 3'000;
constexpr std::uint64_t kUtpProbeFreshMs = 15'000;

//! What the probe decision needs: when this stream last received data, when it last sent any, and
//! when it was last probed.
struct SUtpProbeFacts
{
	bool receivedData = false;
	std::uint64_t lastDataReceivedMs = 0;
	bool sentData = false;
	std::uint64_t lastDataSentMs = 0;
	bool probed = false;
	std::uint64_t lastProbeMs = 0;
};

/**
 * Whether a silent downloader should be poked so the peer learns where the stream went. A receiver
 * whose NAT dropped the incoming data has nothing to acknowledge, so it stays quiet until its 29 s
 * keepalive, while the sender gives up somewhere around 16.5 s: the keepalive then moves the stream
 * but acknowledges nothing, and the sender's socket is gone before the next one. The probe makes
 * libutp emit an ack of its own - growing the receive buffer raises the window, so utp_read_drained()
 * schedules one (utp_internal.cpp:3232-3239) - and that ack leaves through the new mapping.
 *
 * Only the receiving side probes: a stream that has sent data inside the same idle window is the
 * uploader, whose own traffic already keeps its mapping alive. One probe per idle window is enough.
 */
inline bool ShouldProbeIdleStream(const SUtpProbeFacts &facts, std::uint64_t nowMs)
{
	if (!facts.receivedData || nowMs < facts.lastDataReceivedMs) {
		return false;
	}
	const std::uint64_t sinceData = nowMs - facts.lastDataReceivedMs;
	if (sinceData < kUtpProbeIdleMs || sinceData >= kUtpProbeFreshMs) {
		return false;
	}
	if (facts.sentData && nowMs >= facts.lastDataSentMs &&
		nowMs - facts.lastDataSentMs < kUtpProbeIdleMs) {
		return false;
	}
	if (facts.probed && nowMs >= facts.lastProbeMs && nowMs - facts.lastProbeMs < kUtpProbeIdleMs) {
		return false;
	}
	return true;
}

//! Whether @p candidate is strictly ahead of @p known, counting modulo 2^16. A resend carries an
//! older number, so a high-water mark must never move backwards on one - and an equal number changes
//! nothing.
inline bool IsUtpSeqAhead(std::uint16_t candidate, std::uint16_t known)
{
	const std::uint16_t distance = static_cast<std::uint16_t>(candidate - known);
	return distance != 0 && distance < 32'768;
}

//! Whether @p observedPort is the port this stream has just moved away from, still inside the window
//! that bounds moves. A port-preserving NAT can hand the old port back, so the refusal expires with
//! the same ten seconds as the limiter rather than blocking the stream forever.
inline bool IsUtpAbandonedPort(std::uint16_t observedPort,
	std::uint16_t abandonedPort,
	std::uint64_t abandonedAtMs,
	std::uint64_t nowMs)
{
	if (abandonedPort == 0 || observedPort != abandonedPort) {
		return false;
	}
	return nowMs < abandonedAtMs || nowMs - abandonedAtMs < UtpPortRepointLimiter::WindowMs;
}

//! The base uTP header fields a move decision needs, read off the wire by ReadUtpWireHeader()
//! (src/UtpContext.h); the layout is PacketFormatV1 (utp_internal.cpp:113-136).
struct SUtpMoveHeader
{
	std::uint8_t type = 0;
	std::uint32_t connectionId = 0;
	std::uint16_t sequenceNumber = 0;
	std::uint16_t acknowledgeNumber = 0;
};

//! What the adapter has observed of one stream: its peer's identity and sequence numbers, and the
//! last sequence number we sent. All of it comes from frames, since libutp exposes none of it.
struct SUtpPeerStreamFacts
{
	bool known = false;
	std::uint32_t connectionId = 0;
	std::uint16_t peerSequenceNumber = 0;
	std::uint16_t peerAcknowledgeNumber = 0;
	std::uint16_t lastSentSequenceNumber = 0;
};

enum class EUtpMoveVerdict
{
	Move,
	Refuse
};

//! How far behind our last sent number a reset's ack may be: more packets than any send window holds.
constexpr std::uint16_t kUtpResetAckSpan = 1024;

/**
 * Whether a reset answers a packet we sent. libutp sends one only for a non-SYN frame it cannot
 * match, echoing that frame's seq_nr as its ack_nr (utp_internal.cpp:2941), and acts on any reset
 * from its connection id alone (:2866-2887). Checking the ack makes a blind forgery guess our send
 * position as well.
 */
inline bool IsPlausibleUtpReset(const SUtpMoveHeader &header, bool hasSent, std::uint16_t lastSent)
{
	return hasSent && static_cast<std::uint16_t>(lastSent - header.acknowledgeNumber) < kUtpResetAckSpan;
}

/**
 * How long an accepted stream may wait for its peer's first data. An honest dialer sends its hello
 * one round trip after our SYN-ACK. libutp arms no timer for a socket in CS_SYN_RECV
 * (utp_internal.cpp:2593, :1181) and never resends the SYN-ACK, since a repeated SYN is dropped
 * (:2951-2957). Waiting longer only lets forged SYNs hold the half-open budget.
 */
constexpr std::uint64_t kUtpHalfOpenLimitMs = 5'000;

inline bool IsUtpHalfOpenExpired(std::uint64_t acceptedAtMs, std::uint64_t nowMs)
{
	return nowMs >= acceptedAtMs && nowMs - acceptedAtMs >= kUtpHalfOpenLimitMs;
}

/**
 * Whether a frame from a new source port can be the same stream. utp_process_udp() returns 1 for
 * every non-SYN frame it is given -- processed, stale or unmatched alike (utp_internal.cpp:2909,
 * :2925, :2933, :2940) -- so its answer is no evidence and the decision has to come from the
 * header, before the frame is handed over.
 *
 * Deliberately stricter than libutp's own admission, which window-checks the ack against our last
 * send (utp_internal.cpp:1799-1815) but never against what the peer already acknowledged, and which
 * accepts a far wider sequence range. It is not stricter than ordinary traffic, though: an ack-only
 * ST_STATE carries the peer's next sequence number, the same one its next data frame will use, and a
 * keepalive repeats both numbers with the ack one lower (:782, :1085-1086, :833-843); and a peer with
 * a full send window resends its OLDEST unacked packet, whose number sits BEHIND the highest one we
 * have seen. Refusing either would refuse a download-only stream, every idle one, and the frame that
 * recovers a bulk transfer after its port changes. What keeps a replay from dragging a stream back to
 * a port it left is the abandoned-port refusal and the ten-second move limit, not this window.
 */
inline EUtpMoveVerdict DecideUtpMove(const SUtpMoveHeader &header, const SUtpPeerStreamFacts &facts)
{
	// A reset is acted on from its connection id alone (utp_internal.cpp:2866-2887 through
	// LookupAdjacent at :2809-2823) and a SYN asks for a new connection: neither may ever be
	// mapped onto a live stream.
	if (header.type == kUtpTypeReset || header.type == kUtpTypeSyn) {
		return EUtpMoveVerdict::Refuse;
	}
	if (!facts.known || header.connectionId == 0 || header.connectionId != facts.connectionId) {
		return EUtpMoveVerdict::Refuse;
	}
	// From one below the peer's last ack (a keepalive sends exactly that: :835) up to the last number
	// we sent, which is libutp's own upper bound (:1799-1815). Modular at both ends: only a span that
	// would mean the peer acknowledged more than we sent is refused, so a wide send window with slow
	// acks still moves.
	const std::uint16_t lowestAck = static_cast<std::uint16_t>(facts.peerAcknowledgeNumber - 1);
	const std::uint16_t allowedAcks =
		static_cast<std::uint16_t>(facts.lastSentSequenceNumber - lowestAck);
	if (allowedAcks > kUtpMoveAckSpanLimit ||
		!IsUtpSeqWithin(header.acknowledgeNumber, lowestAck, allowedAcks)) {
		return EUtpMoveVerdict::Refuse;
	}
	// Both directions, exactly as libutp measures it against the packet it expects next (:1894-1897):
	// the resend of a full window's oldest unacked packet is behind the highest number we have seen and
	// must still be admitted, or a bulk transfer could never recover after a port change.
	if (UtpSeqDistance(header.sequenceNumber, facts.peerSequenceNumber) > kUtpMoveSeqWindow) {
		return EUtpMoveVerdict::Refuse;
	}
	return EUtpMoveVerdict::Move;
}

#endif // UTP_DIAL_POLICY_H
