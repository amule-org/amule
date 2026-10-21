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
// codecs plus the pure request/routing decisions; the opt-in login bit, the 0xA6 send and the
// 0xA7/0x37 receive live in ServerConnect.cpp, BaseClient.cpp, ClientUDPSocket.cpp and
// ServerSocket.cpp, all under ENABLE_NATT_SERVER_COORDINATION. See docs/NAT_T_Protocol.md.
#ifndef AMULE_LUGDUNUM_NAT_TRAVERSAL_H
#define AMULE_LUGDUNUM_NAT_TRAVERSAL_H

#ifdef ENABLE_NATT_SERVER_COORDINATION

#include "ArchSpecific.h"
#include "NetworkFunctions.h"                // Needed for IsLowID
#include <protocol/ed2k/Client2Server/TCP.h> // Needed for SRV_TCPFLG_NATTRAVERSAL

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

// The NAT callback counts as active for this session only when both the 24-byte OP_IDCHANGE
// answering our opt-in and the server's 0x1000 TCP flag are present.
inline bool IsNatCallbackActive(const IdChangeFields &fields)
{
	return fields.hasYourPort && fields.hasTcpFlags && (fields.tcpFlags & SRV_TCPFLG_NATTRAVERSAL) != 0;
}

struct CallbackRequest
{
	std::uint32_t targetId;
	std::uint32_t requesterId;
};

// OP_NAT_CALLBACKREQUEST (0xA6) payload: Target ID, Requester ID, each a 4-byte little-endian
// eD2k uint32. Sent from BaseClient.cpp under ENABLE_NATT_SERVER_COORDINATION (S6.3).
inline std::array<std::uint8_t, 8> BuildCallbackRequest(const CallbackRequest &request)
{
	std::array<std::uint8_t, 8> bytes{};
	PokeUInt32(bytes.data(), request.targetId);
	PokeUInt32(bytes.data() + 4, request.requesterId);
	return bytes;
}

struct CallbackRequestFacts
{
	std::uint32_t ourId = 0;
	std::uint32_t sourceId = 0;
	bool sameServer = false;
	bool serverCallbackCapable = false;
	// Mirrors CServerUDPSocket::SendPacket's own gate (ServerUDPSocket.cpp): without all three the
	// server will not accept an obfuscated UDP datagram at all, so sending one would be wasted.
	bool serverCryptLayerUdpEnabled = false;
	std::uint32_t serverKeyUdp = 0;
	bool serverSupportsObfuscationUdp = false;
	// thePrefs::IsLugdunumNatCallbackEnabled() (amule.conf's LugdunumNatCallback, off by default):
	// a build-time switch is not an opt-in to live traffic on its own.
	bool settingEnabled = false;
};

// Whether to send OP_NAT_CALLBACKREQUEST for a LowID source: both ends must be LowID on the same,
// callback-capable server, the server-way crypto preconditions must hold (S6.3), and the runtime
// setting must be on.
inline bool ShouldRequestLugdunumNatCallback(const CallbackRequestFacts &facts)
{
	if (!facts.settingEnabled || !facts.sameServer || !facts.serverCallbackCapable) {
		return false;
	}
	if (!IsLowID(facts.ourId) || !IsLowID(facts.sourceId)) {
		return false;
	}
	return facts.serverCryptLayerUdpEnabled && facts.serverKeyUdp != 0 &&
	       facts.serverSupportsObfuscationUdp;
}

struct ServerWayDatagramFacts
{
	std::uint32_t sourceIp = 0;
	std::uint32_t serverIp = 0;
	bool serverCallbackCapable = false;
};

// Whether an inbound client-UDP-socket datagram is worth a DecryptReceivedServer attempt at all:
// source IP alone never authenticates it (S5), but it does gate whether trying is worthwhile.
inline bool IsServerWayDatagramFromOurServer(const ServerWayDatagramFacts &facts)
{
	return facts.serverCallbackCapable && facts.sourceIp == facts.serverIp;
}

// Picks the server-way UDP base key exactly as CServerUDPSocket::OnPacketReceived does
// (ServerUDPSocket.cpp): a pending crypt-ping reply's challenge takes priority over the
// established server key, because that specific exchange is what the challenge authenticates.
// Confirmed against NeoMule's ClientUDPSocket.cpp, which makes the same choice on this same
// opcode.
inline std::uint32_t SelectServerWayKey(
	bool cryptPingReplyPending, std::uint32_t challenge, std::uint32_t serverKeyUdp)
{
	if (cryptPingReplyPending && challenge != 0) {
		return challenge;
	}
	return serverKeyUdp;
}

struct LowIdServerSourceFacts
{
	bool originIsLocalServer = false;
	bool sourceServerIsOurCurrentServer = false;
	bool weAreLowId = false;
	bool serverCallbackCapable = false;
	// thePrefs::IsLugdunumNatCallbackEnabled() (amule.conf's LugdunumNatCallback, off by default):
	// with the setting off, a kept source could never be answered.
	bool settingEnabled = false;
};

// Whether a LowID server source survives CanAddSource's LowID-while-firewalled drop. Such a server
// only sends them to a client whose opt-in it answered (NeoMule's CanLow2Low, made per server), and
// only when the runtime setting is on.
inline bool ShouldKeepLowIdServerSource(const LowIdServerSourceFacts &facts)
{
	return facts.settingEnabled && facts.originIsLocalServer && facts.sourceServerIsOurCurrentServer &&
	       facts.weAreLowId && facts.serverCallbackCapable;
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

struct CallbackDialTarget
{
	std::uint32_t provisionalUserId = 0;
	std::uint32_t ip = 0;
	std::uint16_t udpPort = 0;
};

// The forwarded IP/UDP tuple selects a route only; a peer ID is established by OP_HELLO.
inline CallbackDialTarget BuildCallbackDialTarget(const ForwardedCallback &callback)
{
	return { 0, callback.seenIp, callback.seenUdpPort };
}

inline bool ShouldDialForwardedCallback(
	const ForwardedCallback &callback, bool serverCallbackActive, bool settingEnabled)
{
	return callback.ok && serverCallbackActive && settingEnabled && callback.seenIp != 0 &&
	       callback.seenUdpPort != 0;
}

// A zero requester ID is a reverse callback: act on it, but never request a callback back on failure.
inline bool ShouldSendCallbackRequestAfterFailure(std::uint32_t requesterId)
{
	return requesterId != 0;
}

} // namespace LugdunumNatTraversal

#endif // ENABLE_NATT_SERVER_COORDINATION

#endif // AMULE_LUGDUNUM_NAT_TRAVERSAL_H
