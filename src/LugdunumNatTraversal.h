//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// Any parts of this program contributed by third-party developers are copyrighted
// by their respective authors.
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
// Lugdunum's NAT callback (eMule wiki, "NatT protocol", section "eServer (Lugdunum)"). Wire
// codecs only: aMule neither sets SRVCAP_NATTRAVERSAL at login nor sends OP_NAT_CALLBACKREQUEST.
// See docs/NAT_T_Protocol.md.
#ifndef AMULE_LUGDUNUM_NAT_TRAVERSAL_H
#define AMULE_LUGDUNUM_NAT_TRAVERSAL_H

#ifdef ENABLE_NATT_SERVER_COORDINATION

#include "ArchSpecific.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace LugdunumNatTraversal
{
using Hash = std::array<std::uint8_t, 16>;

struct IdChangeFields
{
	bool ok = false;
	std::uint32_t id = 0;
	bool hasTcpFlags = false;
	std::uint32_t tcpFlags = 0;
	bool hasAuxPort = false;
	std::uint32_t auxPort = 0;
	bool hasReportedIp = false;
	std::uint32_t reportedIp = 0;
	std::uint32_t obfuscationPortTcp = 0;
	bool hasYourPort = false;
	std::uint32_t yourPort = 0;
};

// OP_IDCHANGE field layout: [ID][flags][aux port][reported IP][obfuscation TCP port][your port],
// each a 4-byte little-endian eD2k uint32, every field after ID optional and gated on packet
// size. Reported IP and the obfuscation port are read as a pair, matching ServerSocket.cpp's
// historical behaviour: a packet that carries a reported IP without an obfuscation port (fewer
// than 20 bytes) yields neither field, not just the one that is missing.
inline IdChangeFields ParseIdChangeFields(const std::uint8_t *data, std::size_t size)
{
	IdChangeFields fields;
	if (data == nullptr || size < 4) {
		return fields;
	}
	fields.ok = true;
	fields.id = PeekUInt32(data);
	if (size >= 8) {
		fields.hasTcpFlags = true;
		fields.tcpFlags = PeekUInt32(data + 4);
		if (size >= 12) {
			fields.hasAuxPort = true;
			fields.auxPort = PeekUInt32(data + 8);
		}
	}
	if (size >= 20) {
		fields.hasReportedIp = true;
		fields.reportedIp = PeekUInt32(data + 12);
		fields.obfuscationPortTcp = PeekUInt32(data + 16);
		if (size >= 24) {
			fields.hasYourPort = true;
			fields.yourPort = PeekUInt32(data + 20);
		}
	}
	return fields;
}

struct CallbackRequest
{
	std::uint32_t targetId;
	std::uint32_t requesterId;
};

// OP_NAT_CALLBACKREQUEST (0xA6) payload: Target ID, Requester ID, each a 4-byte little-endian
// eD2k uint32. Built for tests only -- aMule never sends this opcode yet.
inline std::array<std::uint8_t, 8> BuildCallbackRequest(const CallbackRequest &request)
{
	std::array<std::uint8_t, 8> bytes{};
	PokeUInt32(bytes.data(), request.targetId);
	PokeUInt32(bytes.data() + 4, request.requesterId);
	return bytes;
}

struct ForwardedCallback
{
	bool ok = false;
	std::uint32_t seenIp = 0;
	std::uint16_t seenUdpPort = 0;
	std::uint32_t requesterId = 0;
	bool hasObfuscation = false;
	std::uint8_t obfuscationSettings = 0;
	Hash userHash{};
};

// OP_NAT_CALLBACKREQUESTED{,_UDP} (0x37 / 0xA7) forwarded payload: Seen IP (4), Seen Client UDP
// Port (2), Requester ID (4, may be 0), then an optional 17-byte obfuscation part (1-byte settings
// + 16-byte user hash). The wiki writes the port as "[uint16 4]"; the type is taken over the
// width, which no captured packet has confirmed yet. Trailing bytes are ignored.
inline ForwardedCallback ParseForwardedCallback(const std::uint8_t *data, std::size_t size)
{
	ForwardedCallback result;
	if (data == nullptr || size < 10) {
		return result;
	}
	result.ok = true;
	result.seenIp = PeekUInt32(data);
	result.seenUdpPort = PeekUInt16(data + 4);
	result.requesterId = PeekUInt32(data + 6);
	if (size >= 27) {
		result.hasObfuscation = true;
		result.obfuscationSettings = data[10];
		std::copy_n(data + 11, 16, result.userHash.begin());
	}
	return result;
}

} // namespace LugdunumNatTraversal

#endif // ENABLE_NATT_SERVER_COORDINATION

#endif // AMULE_LUGDUNUM_NAT_TRAVERSAL_H
