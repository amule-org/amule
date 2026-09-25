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
constexpr std::size_t kUserHashSize = 16;
constexpr std::size_t kRendezvousWithoutFileHashSize = 1 + kUserHashSize + 1;
constexpr std::size_t kRendezvousWithFileHashSize = kRendezvousWithoutFileHashSize + kUserHashSize;

using UserHash = std::array<std::uint8_t, kUserHashSize>;
using FileHash = std::array<std::uint8_t, kUserHashSize>;

struct RendezvousRequest
{
	UserHash userHash;
	std::uint8_t connectOptions;
	std::optional<FileHash> fileHash;
};

inline bool IsZeroHash(const UserHash &hash) noexcept
{
	return std::all_of(hash.begin(), hash.end(), [](std::uint8_t byte) { return byte == 0; });
}

inline std::optional<RendezvousRequest> ParseRendezvousRequest(
	const std::uint8_t *data, std::size_t size) noexcept
{
	if (!data || (size != kRendezvousWithoutFileHashSize && size != kRendezvousWithFileHashSize) ||
		data[0] != OP_RENDEZVOUS) {
		return std::nullopt;
	}

	RendezvousRequest request{};
	std::copy_n(data + 1, kUserHashSize, request.userHash.begin());
	request.connectOptions = data[1 + kUserHashSize];
	if (IsZeroHash(request.userHash)) {
		return std::nullopt;
	}

	if (size == kRendezvousWithFileHashSize) {
		FileHash fileHash{};
		std::copy_n(data + kRendezvousWithoutFileHashSize, kUserHashSize, fileHash.begin());
		if (IsZeroHash(fileHash)) {
			return std::nullopt;
		}
		request.fileHash = fileHash;
	}

	return request;
}

// The endpoint is deliberately not read from the payload. eMuleAI receives this request through a
// buddy, whose observed source endpoint is the only endpoint the relay may carry forward. An
// observed source is not authentication: a later handler must require an established session or a
// cookie-based return-routability check before sending relay traffic. This slice only decides.
struct RelayAction
{
	UserHash userHash;
	std::uint8_t connectOptions;
	std::optional<FileHash> fileHash;
	PeerAddressing::UdpEndpoint observed;
};

inline std::optional<RelayAction> DecideRelay(const std::uint8_t *data,
	std::size_t size,
	const PeerAddressing::UdpEndpoint &observed,
	CRequesterLimiter &limiter,
	std::uint64_t now) noexcept
{
	const auto request = ParseRendezvousRequest(data, size);
	if (!request || PeerAddressing::ClassifyUdpPeer(observed) == PeerAddressing::EUdpRoute::Reject ||
		!limiter.Admit(observed.address, now)) {
		return std::nullopt;
	}

	return RelayAction{ request->userHash, request->connectOptions, request->fileHash, observed };
}
} // namespace NatRendezvous

#endif // NATRENDEZVOUSPROTOCOL_H
