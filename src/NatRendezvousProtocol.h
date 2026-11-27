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
// These are nested control-payload opcodes, not eD2k opcodes. eMuleAI reserves 0xA1 for
// OP_HOLEPUNCH and 0xAA for OP_NATT_ENDPOINT_HINT; those handlers belong to later slices.
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
// Announces a 0xAA (endpoint hint), which no code reads and our buddy does not send.
constexpr std::uint8_t kEndpointHintBit = 0x20;

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

// Our connect options plus the NAT-T transports we serve, so the target arms one we can dial.
inline std::uint8_t RequesterConnectOptions(std::uint8_t myConnectOptions, std::uint8_t localNatT) noexcept
{
	return std::uint8_t((myConnectOptions | (localNatT & kNatTTransports)) & ~kEndpointHintBit);
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
