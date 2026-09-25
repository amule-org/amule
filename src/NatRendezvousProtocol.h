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
	std::uint8_t transportHint;
};

struct RendezvousRequest
{
	UserHash requesterHash;
	std::uint8_t connectOptions;
	std::optional<FileHash> fileHash;
	std::optional<RequesterEndpointHint> requesterHint;
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
		const std::uint8_t transportHint = size >= kRendezvousPayloadSize + 7 ? hint[6] : 0;
		result.requesterHint = RequesterEndpointHint{ endpoint, port, transportHint };
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
