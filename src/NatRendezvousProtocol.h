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
constexpr std::size_t kRendezvousEnvelopeWithHintSize = kRendezvousEnvelopeSize + 4 + 2 + 1;

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
	result.transportHint = size >= kRendezvousPayloadSize + 7 ? hint[6] : 0;
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
// parsed request, so nothing the parser did not accept is carried to the served node.
inline std::vector<std::uint8_t> BuildForwardedRendezvous(const RendezvousRequest &request)
{
	std::vector<std::uint8_t> body(kHashSize, 0); // Extended-payload marker.
	body.push_back(OP_RENDEZVOUS);
	body.insert(body.end(), request.requesterHash.begin(), request.requesterHash.end());
	body.push_back(request.connectOptions);
	const FileHash noFile{};
	const FileHash &file = request.fileHash ? *request.fileHash : noFile;
	body.insert(body.end(), file.begin(), file.end());
	if (request.requesterHint || request.transportHint != 0) {
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
	return body;
}

// The NAT-T bits of a rendezvous' connect options: 0x80 for uTP, 0x40 for QUIC (eMuleAI).
constexpr std::uint8_t kNatTTransports = 0x80 | 0x40;

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
