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

#ifndef NATRENDEZVOUSPROTOCOL_H
#define NATRENDEZVOUSPROTOCOL_H

#include "NatRendezvousPolicy.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace NatRendezvous
{
// These are nested control-payload opcodes, not eD2k opcodes. eMuleAI also uses 0xA1 for
// OP_HOLEPUNCH and 0xAA for OP_NATT_ENDPOINT_HINT, below.
constexpr std::uint8_t OP_RENDEZVOUS = 0xA0;
constexpr std::size_t kHashSize = 16;
constexpr std::size_t kRendezvousPayloadSize = 1 + kHashSize + 1 + kHashSize;
constexpr std::size_t kRendezvousPrefixSize = kHashSize + kHashSize;
constexpr std::size_t kRendezvousEnvelopeSize = kRendezvousPrefixSize + kRendezvousPayloadSize;
constexpr std::size_t kRendezvousTailSize = 4 + 2 + 1;
constexpr std::size_t kRendezvousEnvelopeWithHintSize = kRendezvousEnvelopeSize + kRendezvousTailSize;
// The extension block after the tail (ModderMule/emule-qt#265): <type 1><len 1><value len> elements.
constexpr std::uint8_t kExtRequesterUdp6 = 0x01; // <ipv6 16><udp port 2 LE>
constexpr std::size_t kExtRequesterUdp6Size = 16 + 2;

using ServingBuddyId = std::array<std::uint8_t, kHashSize>;
using UserHash = std::array<std::uint8_t, kHashSize>;
using FileHash = std::array<std::uint8_t, kHashSize>;

struct RequesterEndpointHint
{
	CNetworkAddress address;
	std::uint16_t port;
};

// The tail's last byte, eMuleAI's ENatTraversalTransport.
constexpr std::uint8_t kTransportHintUtp = 1;
constexpr std::uint8_t kTransportHintQuic = 2;

struct RendezvousRequest
{
	UserHash requesterHash;
	std::uint8_t connectOptions;
	std::optional<FileHash> fileHash;
	std::optional<RequesterEndpointHint> requesterHint;
	// Kept when the address before it is unusable, as eMuleAI reads it.
	std::uint8_t transportHint = 0;
	// From the extension block. We have no IPv6 UDP to use it; as a buddy we forward it.
	std::optional<RequesterEndpointHint> requesterUdp6;
};

inline bool IsZeroHash(const std::array<std::uint8_t, kHashSize> &hash) noexcept
{
	return std::all_of(hash.begin(), hash.end(), [](std::uint8_t byte) { return byte == 0; });
}

inline std::uint32_t ReadNetworkIPv4(const std::uint8_t *data) noexcept
{
	return std::uint32_t(data[0]) | (std::uint32_t(data[1]) << 8) | (std::uint32_t(data[2]) << 16) |
	       (std::uint32_t(data[3]) << 24);
}

inline std::optional<RendezvousRequest> ParseRendezvousRequest(
	const std::uint8_t *data, std::size_t size) noexcept
{
	if (!data || size != kRendezvousPayloadSize || data[0] != OP_RENDEZVOUS) {
		return std::nullopt;
	}

	RendezvousRequest request{};
	std::copy_n(data + 1, kHashSize, request.requesterHash.begin());
	request.connectOptions = data[1 + kHashSize];
	if (IsZeroHash(request.requesterHash)) {
		return std::nullopt;
	}

	FileHash fileHash{};
	std::copy_n(data + 1 + kHashSize + 1, kHashSize, fileHash.begin());
	if (!IsZeroHash(fileHash)) {
		request.fileHash = fileHash;
	}
	return request;
}

// The first usable 0x01 element, as eMuleQt's target reads the block. An element that runs past
// the end, or a lone byte left at the end, voids the whole block. A longer value is read for its
// first 18 bytes.
inline std::optional<RequesterEndpointHint> ParseExtensionUdp6(
	const std::uint8_t *data, std::size_t size) noexcept
{
	std::optional<RequesterEndpointHint> found;
	std::size_t pos = 0;
	while (pos < size) {
		if (size - pos < 2) {
			return std::nullopt;
		}
		const std::uint8_t type = data[pos];
		const std::size_t len = data[pos + 1];
		pos += 2;
		if (len > size - pos) {
			return std::nullopt;
		}
		if (type == kExtRequesterUdp6 && len >= kExtRequesterUdp6Size && !found) {
			const CNetworkAddress address = CNetworkAddress::FromIPv6Bytes(data + pos);
			const std::uint16_t port =
				std::uint16_t(data[pos + 16]) | (std::uint16_t(data[pos + 17]) << 8);
			if (port != 0 && address.IsGloballyRoutableIPv6()) {
				found = RequesterEndpointHint{ address, port };
			}
		}
		pos += len;
	}
	return found;
}

inline std::optional<RendezvousRequest> ParseRendezvousRequestWithHint(
	const std::uint8_t *data, std::size_t size) noexcept
{
	if (size < kRendezvousPayloadSize) {
		return std::nullopt;
	}
	const auto request = ParseRendezvousRequest(data, kRendezvousPayloadSize);
	if (!request) {
		return std::nullopt;
	}
	if (size < kRendezvousPayloadSize + 6) {
		return request;
	}

	RendezvousRequest result = *request;
	const auto *hint = data + kRendezvousPayloadSize;
	const std::uint32_t address = ReadNetworkIPv4(hint);
	const std::uint16_t port = std::uint16_t(hint[4]) | (std::uint16_t(hint[5]) << 8);
	const CNetworkAddress endpoint = CNetworkAddress::FromIPv4NetworkOrder(address);
	if (address != 0 && port != 0 && endpoint.IsGloballyRoutableIPv4()) {
		result.requesterHint = RequesterEndpointHint{ endpoint, port };
	}
	result.transportHint = size >= kRendezvousPayloadSize + kRendezvousTailSize ? hint[6] : 0;
	if (size > kRendezvousPayloadSize + kRendezvousTailSize) {
		result.requesterUdp6 = ParseExtensionUdp6(
			hint + kRendezvousTailSize, size - kRendezvousPayloadSize - kRendezvousTailSize);
	}
	return result;
}

struct RendezvousEnvelope
{
	ServingBuddyId servingBuddyId;
	RendezvousRequest request;
};

// The envelope is OP_REASKCALLBACKUDP's payload. eMuleAI uses the serving-buddy ID to select the
// target, a zero hash marker to identify the extended payload, then OP_RENDEZVOUS and its request.
inline std::optional<RendezvousEnvelope> ParseRendezvousEnvelope(
	const std::uint8_t *data, std::size_t size) noexcept
{
	if (!data || size < kRendezvousEnvelopeSize) {
		return std::nullopt;
	}

	RendezvousEnvelope envelope{};
	std::copy_n(data, kHashSize, envelope.servingBuddyId.begin());
	if (IsZeroHash(envelope.servingBuddyId) || !std::all_of(data + kHashSize,
							   data + kRendezvousPrefixSize,
							   [](std::uint8_t byte) { return byte == 0; })) {
		return std::nullopt;
	}

	const auto request =
		ParseRendezvousRequestWithHint(data + kRendezvousPrefixSize, size - kRendezvousPrefixSize);
	if (!request) {
		return std::nullopt;
	}
	envelope.request = *request;
	return envelope;
}

// What a buddy forwards after the requester's observed endpoint: every field is written from the
// parsed request, so nothing the parser did not accept is carried to the served node. Of the
// extension block only a checked 0x01 element is carried.
inline std::vector<std::uint8_t> BuildForwardedRendezvous(const RendezvousRequest &request)
{
	std::vector<std::uint8_t> body(kHashSize, 0); // Extended-payload marker.
	body.push_back(OP_RENDEZVOUS);
	body.insert(body.end(), request.requesterHash.begin(), request.requesterHash.end());
	body.push_back(request.connectOptions);
	const FileHash noFile{};
	const FileHash &file = request.fileHash ? *request.fileHash : noFile;
	body.insert(body.end(), file.begin(), file.end());
	if (request.requesterHint || request.transportHint != 0 || request.requesterUdp6) {
		// An unusable address goes out as zeros, so the transport byte still reaches the served node.
		const std::uint32_t address =
			request.requesterHint ? request.requesterHint->address.ToIPv4NetworkOrderOrZero() : 0;
		const std::uint16_t port = request.requesterHint ? request.requesterHint->port : 0;
		for (int shift = 0; shift < 32; shift += 8) {
			body.push_back(std::uint8_t(address >> shift));
		}
		body.push_back(std::uint8_t(port));
		body.push_back(std::uint8_t(port >> 8));
		body.push_back(request.transportHint);
	}
	std::array<std::uint8_t, 16> ipv6{};
	if (request.requesterUdp6 && request.requesterUdp6->address.ToIPv6Bytes(ipv6.data())) {
		body.push_back(kExtRequesterUdp6);
		body.push_back(std::uint8_t(kExtRequesterUdp6Size));
		body.insert(body.end(), ipv6.begin(), ipv6.end());
		body.push_back(std::uint8_t(request.requesterUdp6->port));
		body.push_back(std::uint8_t(request.requesterUdp6->port >> 8));
	}
	return body;
}

// The NAT-T bits of a rendezvous' connect options: 0x80 for uTP, 0x40 for QUIC (eMuleAI).
constexpr std::uint8_t kNatTQuic = 0x40;
constexpr std::uint8_t kNatTTransports = 0x80 | kNatTQuic;
// A requester's 0xA0 asks the buddy for a 0xAA (endpoint hint) with this bit.
constexpr std::uint8_t kEndpointHintBit = 0x20;

// OP_NATT_ENDPOINT_HINT (0xAA), eMuleAI's, sent plain over UDP from the buddy to the requester:
// <version 1><target hash 16><buddy ID 16><file hash 16><IPv4 4><UDP port 2 LE><connect options 1>.
// eMuleAI drops any other size or version, and an address that is not public.
constexpr std::uint8_t kEndpointHintVersion = 1;
constexpr std::size_t kEndpointHintSize = 1 + kHashSize * 3 + 4 + 2 + 1;

struct EndpointHint
{
	UserHash target;
	ServingBuddyId buddyId; // As the requester sent it, in its layout.
	FileHash fileHash;
	std::uint32_t ipv4; // aMule's byte order, first octet lowest.
	std::uint16_t udpPort;
	std::uint8_t connectOptions;
};

inline std::array<std::uint8_t, kEndpointHintSize> BuildEndpointHint(const EndpointHint &hint) noexcept
{
	std::array<std::uint8_t, kEndpointHintSize> out{};
	auto it = out.begin();
	*it++ = kEndpointHintVersion;
	it = std::copy(hint.target.begin(), hint.target.end(), it);
	it = std::copy(hint.buddyId.begin(), hint.buddyId.end(), it);
	it = std::copy(hint.fileHash.begin(), hint.fileHash.end(), it);
	for (int shift = 0; shift < 32; shift += 8) {
		*it++ = std::uint8_t(hint.ipv4 >> shift);
	}
	*it++ = std::uint8_t(hint.udpPort);
	*it++ = std::uint8_t(hint.udpPort >> 8);
	*it = hint.connectOptions;
	return out;
}

// A hint a requester received. The caller checks the address; a zero one is refused here.
inline std::optional<EndpointHint> ParseEndpointHint(const std::uint8_t *data, std::size_t size) noexcept
{
	if (!data || size != kEndpointHintSize || data[0] != kEndpointHintVersion) {
		return std::nullopt;
	}
	EndpointHint hint{};
	const std::uint8_t *at = data + 1;
	std::copy_n(at, kHashSize, hint.target.begin());
	std::copy_n(at + kHashSize, kHashSize, hint.buddyId.begin());
	std::copy_n(at + 2 * kHashSize, kHashSize, hint.fileHash.begin());
	at += 3 * kHashSize;
	hint.ipv4 = ReadNetworkIPv4(at);
	hint.udpPort = std::uint16_t(at[4] | (at[5] << 8));
	hint.connectOptions = at[6];
	if (hint.ipv4 == 0 || hint.udpPort == 0) {
		return std::nullopt;
	}
	return hint;
}

/**
 * The hint a buddy sends for the node it serves (ModderMule/emule-qt#281): the node's IPv4 with
 * the UDP port its buddy request or last refresh came from. A NAT keeps that outside port for
 * every destination; the hello's Kad port is the inside one, so it is only the fallback. The
 * options are the node's crypt bits and the NAT-T transports its buddy request declared.
 * @a addressUsable is the caller's: a public address, or a LAN one where LAN IPs are not filtered.
 */
inline std::optional<EndpointHint> MakeServedEndpointHint(const UserHash &target,
	const ServingBuddyId &buddyId,
	const FileHash &fileHash,
	std::uint32_t servedIPv4,
	bool addressUsable,
	std::uint16_t observedUdpPort,
	std::uint16_t kadPort,
	std::uint8_t cryptOptions,
	std::uint8_t servedOptions) noexcept
{
	const std::uint16_t port = observedUdpPort != 0 ? observedUdpPort : kadPort;
	if (port == 0 || !addressUsable) {
		return std::nullopt;
	}
	return EndpointHint{ target,
		buddyId,
		fileHash,
		servedIPv4,
		port,
		std::uint8_t((cryptOptions & 0x07) | (servedOptions & kNatTTransports)) };
}

// How long after a hint a refresh that moves the served node's port sends the requester another.
constexpr std::uint64_t kEndpointRehintMs = 3000;
constexpr std::size_t kMaxSentEndpointHints = 64;

//! The hints a buddy sent in the last kEndpointRehintMs.
class CSentEndpointHints
{
public:
	struct Sent
	{
		std::uint32_t requesterIP; // Network order, as the UDP socket reports it.
		std::uint16_t requesterPort;
		EndpointHint hint;
		std::uint64_t at;
	};

	void Record(std::uint32_t requesterIP,
		std::uint16_t requesterPort,
		const EndpointHint &hint,
		std::uint64_t now)
	{
		Prune(now);
		m_sent.erase(std::remove_if(m_sent.begin(),
				     m_sent.end(),
				     [&](const Sent &sent) {
					     return sent.requesterIP == requesterIP &&
						    sent.requesterPort == requesterPort &&
						    sent.hint.target == hint.target;
				     }),
			m_sent.end());
		if (m_sent.size() >= kMaxSentEndpointHints) {
			m_sent.erase(m_sent.begin());
		}
		m_sent.push_back({ requesterIP, requesterPort, hint, now });
	}

	//! The hints for @a target still in the window whose port was not @a udpPort, now at it.
	std::vector<Sent> TakeMoved(const UserHash &target, std::uint16_t udpPort, std::uint64_t now)
	{
		Prune(now);
		std::vector<Sent> moved;
		for (Sent &sent : m_sent) {
			if (sent.hint.target == target && sent.hint.udpPort != udpPort) {
				sent.hint.udpPort = udpPort;
				moved.push_back(sent);
			}
		}
		return moved;
	}

private:
	void Prune(std::uint64_t now)
	{
		m_sent.erase(std::remove_if(m_sent.begin(),
				     m_sent.end(),
				     [now](const Sent &sent) { return now - sent.at > kEndpointRehintMs; }),
			m_sent.end());
	}

	std::vector<Sent> m_sent;
};

// What a requester sends its target's buddy as OP_REASKCALLBACKUDP. The tail is always written,
// zeros for what we do not know, so the extension block of ModderMule/emule-qt#265 can follow it.
// @a ourIPv4 is in aMule's byte order, first octet lowest, as BuildForwardedRendezvous writes it.
inline std::vector<std::uint8_t> BuildRendezvousEnvelope(const ServingBuddyId &buddyId,
	const UserHash &ourHash,
	std::uint8_t connectOptions,
	const FileHash &fileHash,
	std::uint32_t ourIPv4,
	std::uint16_t ourUdpPort,
	std::uint8_t transportHint)
{
	std::vector<std::uint8_t> envelope(buddyId.begin(), buddyId.end());
	envelope.resize(kRendezvousPrefixSize, 0);
	envelope.push_back(OP_RENDEZVOUS);
	envelope.insert(envelope.end(), ourHash.begin(), ourHash.end());
	envelope.push_back(connectOptions);
	envelope.insert(envelope.end(), fileHash.begin(), fileHash.end());
	for (int shift = 0; shift < 32; shift += 8) {
		envelope.push_back(std::uint8_t(ourIPv4 >> shift));
	}
	envelope.push_back(std::uint8_t(ourUdpPort));
	envelope.push_back(std::uint8_t(ourUdpPort >> 8));
	envelope.push_back(transportHint);
	return envelope;
}

// Our connect options plus the NAT-T transports we serve, so the target arms one we can dial, and
// the bit that asks the target's buddy for a hint.
inline std::uint8_t RequesterConnectOptions(std::uint8_t myConnectOptions, std::uint8_t localNatT) noexcept
{
	return std::uint8_t(myConnectOptions | (localNatT & kNatTTransports) | kEndpointHintBit);
}

// The transport we will dial first: QUIC when we serve it, else uTP.
inline std::uint8_t RequesterTransportHint(std::uint8_t localNatT) noexcept
{
	return (localNatT & kNatTQuic) != 0 ? kTransportHintQuic : kTransportHintUtp;
}

// The transports a requester declared, as NAT-T connect-options bits. eMuleAI sets the bits; eMuleQt
// sets none and names its one transport in the tail.
inline std::uint8_t RequesterTransports(const RendezvousRequest &request) noexcept
{
	const std::uint8_t hinted = request.transportHint == kTransportHintUtp    ? 0x80
				    : request.transportHint == kTransportHintQuic ? 0x40
										  : 0;
	return std::uint8_t((request.connectOptions & kNatTTransports) | hinted);
}

// What follows the requester's ip and port in an OP_REASKCALLBACKTCP from our buddy: a plain reask
// starts with its file hash, a forwarded control payload with an all-zero one.
inline bool IsForwardedControl(const std::uint8_t *body, std::size_t size) noexcept
{
	return body && size >= kHashSize &&
	       std::all_of(body, body + kHashSize, [](std::uint8_t byte) { return byte == 0; });
}

enum class EServedRendezvous
{
	Punch,
	Malformed,
	NoSharedTransport
};

struct ServedRendezvousDecision
{
	EServedRendezvous outcome;
	RendezvousRequest request;
	// The endpoint our buddy saw the request come from. The requester's hint is never punched:
	// it only helps a requester on the buddy's own network.
	PeerAddressing::UdpEndpoint target;
	std::uint8_t sharedTransports;
};

// The served node's side of a forwarded 0xA0 (rendezvous). The requester dials and we wait, so we
// arm every transport both sides have and let it choose; with none in common there is nothing to
// arm. Our buddy checks this too, but an eMuleAI buddy forwards without the check.
inline ServedRendezvousDecision DecideServedRendezvous(const PeerAddressing::UdpEndpoint &observed,
	const std::uint8_t *body,
	std::size_t size,
	std::uint8_t localTransports) noexcept
{
	ServedRendezvousDecision decision{ EServedRendezvous::Malformed, {}, observed, 0 };
	if (!IsForwardedControl(body, size) || size <= kHashSize || body[kHashSize] != OP_RENDEZVOUS ||
		PeerAddressing::ClassifyUdpPeer(observed) == PeerAddressing::EUdpRoute::Reject) {
		return decision;
	}
	const auto request = ParseRendezvousRequestWithHint(body + kHashSize, size - kHashSize);
	if (!request) {
		return decision;
	}
	decision.request = *request;
	decision.sharedTransports = RequesterTransports(*request) & localTransports;
	decision.outcome = decision.sharedTransports != 0 ? EServedRendezvous::Punch
							  : EServedRendezvous::NoSharedTransport;
	return decision;
}

// The endpoint is deliberately not read as an identity from the payload. eMuleAI receives this
// request through a buddy, whose observed source endpoint is the routing input. The optional
// requester hint is unverified metadata only. An observed source is not authentication: a later
// handler must require an established session or cookie-based return-routability before sending
// relay traffic. This slice only decides.
struct RelayAction
{
	ServingBuddyId servingBuddyId;
	UserHash requesterHash;
	std::uint8_t connectOptions;
	std::optional<FileHash> fileHash;
	std::optional<RequesterEndpointHint> requesterHint;
	PeerAddressing::UdpEndpoint observed;
};

inline std::optional<RelayAction> DecideRelay(const std::uint8_t *data,
	std::size_t size,
	const PeerAddressing::UdpEndpoint &observed,
	CRequesterLimiter &limiter,
	std::uint64_t now) noexcept
{
	const auto envelope = ParseRendezvousEnvelope(data, size);
	if (!envelope || PeerAddressing::ClassifyUdpPeer(observed) == PeerAddressing::EUdpRoute::Reject ||
		!limiter.Admit(observed.address, now)) {
		return std::nullopt;
	}

	return RelayAction{ envelope->servingBuddyId,
		envelope->request.requesterHash,
		envelope->request.connectOptions,
		envelope->request.fileHash,
		envelope->request.requesterHint,
		observed };
}
} // namespace NatRendezvous

#endif // NATRENDEZVOUSPROTOCOL_H
