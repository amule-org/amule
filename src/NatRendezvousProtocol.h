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

namespace NatRendezvous
{
// Local control-payload abstraction, NOT eD2k opcodes or an eMuleAI wire contract.
// Layout: opcode (1), options (1), family (1: 4 or 6), address (4 or 16
// network-order octets), UDP port (2, big endian). Exact size: 9 or 21 bytes.
// No scope-id, target lookup, uTP envelope or extension bytes are encoded here.
// Future socket integration must remain behind the experimental uTP gate.
constexpr std::uint8_t OP_RENDEZVOUS = 0x01;
constexpr std::uint8_t CONNECT_OPT_NATT_RELAYED = 0x01;

struct ControlPayload
{
	PeerAddressing::UdpEndpoint claimed;
	std::uint8_t options;
};

inline std::optional<ControlPayload> ParseControlPayload(const std::uint8_t *data, std::size_t size) noexcept
{
	if (!data || size < 3 || data[0] != OP_RENDEZVOUS || (data[1] & ~CONNECT_OPT_NATT_RELAYED) != 0) {
		return std::nullopt;
	}
	const std::size_t addressSize = data[2] == 4 ? 4 : data[2] == 6 ? 16 : 0;
	if (addressSize == 0 || size != addressSize + 5) {
		return std::nullopt;
	}
	CNetworkAddress address;
	if (addressSize == 4) {
		const auto ip = (std::uint32_t(data[3]) << 24) | (std::uint32_t(data[4]) << 16) |
				(std::uint32_t(data[5]) << 8) | std::uint32_t(data[6]);
		address = CNetworkAddress::FromIPv4HostOrder(ip);
	} else {
		address = CNetworkAddress::FromIPv6Bytes(data + 3);
	}
	const auto port = static_cast<std::uint16_t>((std::uint16_t(data[size - 2]) << 8) | data[size - 1]);
	const PeerAddressing::UdpEndpoint claimed{ address, port };
	if (PeerAddressing::ClassifyUdpPeer(claimed) == PeerAddressing::EUdpRoute::Reject) {
		return std::nullopt;
	}
	return ControlPayload{ claimed, data[1] };
}

// A decision only, not a destination or permission to send on a socket. The caller
// owns peer selection. Carry the observed representation, even for mapped IPv4.
struct RelayAction
{
	PeerAddressing::UdpEndpoint observed;
	std::uint8_t options;
};

inline std::optional<RelayAction> DecideRelay(const std::uint8_t *data,
	std::size_t size,
	const PeerAddressing::UdpEndpoint &observed,
	CRequesterLimiter &limiter,
	std::uint64_t now)
{
	const auto payload = ParseControlPayload(data, size);
	if (!payload || (payload->options & CONNECT_OPT_NATT_RELAYED) != 0) {
		return std::nullopt;
	}
	const auto accepted = AcceptObservedEndpoint(payload->claimed, observed);
	if (!accepted || !limiter.Admit(observed.address, now)) {
		return std::nullopt;
	}
	return RelayAction{ *accepted, CONNECT_OPT_NATT_RELAYED };
}
} // namespace NatRendezvous

#endif // NATRENDEZVOUSPROTOCOL_H
