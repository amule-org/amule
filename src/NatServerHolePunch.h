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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
//
// Experimental server-coordinated NAT-T payload codecs. No socket integration.
#ifndef AMULE_NAT_SERVER_HOLE_PUNCH_H
#define AMULE_NAT_SERVER_HOLE_PUNCH_H

#ifdef ENABLE_NATT_SERVER_COORDINATION
#include "ArchSpecific.h"
#include "NetworkFunctions.h" // Needed for IsLowID
#include "include/protocol/Protocols.h"
#include "include/protocol/ed2k/Client2Server/TCP.h" // Needed for SRV_TCPFLG_LOWID_HOLEPUNCH

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace NatServerHolePunch
{
using Hash = std::array<std::uint8_t, 16>;
using IPv4 = std::array<std::uint8_t, 4>;

struct Request
{
	std::uint32_t targetId;
	std::uint16_t udpPort;
};
struct Info
{
	// Octets in wire order, e.g. {192, 0, 2, 1}, not a host-order integer.
	IPv4 peerIP;
	std::uint16_t tcpPort;
	std::uint16_t udpPort;
	Hash userHash;
	std::uint8_t role; // Opaque: decoding does not authorize a peer action.
};
struct Failure
{
	std::uint32_t targetId;
	std::uint8_t reason; // Preserve unknown reasons; never treat one as success.
};

// Send OP_LOWID_HOLEPUNCH_REQUEST only to a server whose OP_IDCHANGE flags announce it.
inline bool ServerAcceptsHolePunchRequest(std::uint32_t tcpFlags)
{
	return (tcpFlags & SRV_TCPFLG_LOWID_HOLEPUNCH) != 0;
}

inline bool IsServerTcpOpcode(std::uint8_t opcode)
{
	return opcode == NatServerTcp::OP_LOWID_HOLEPUNCH_REQUEST ||
	       opcode == NatServerTcp::OP_LOWID_HOLEPUNCH_INFO ||
	       opcode == NatServerTcp::OP_LOWID_HOLEPUNCH_FAIL;
}
inline bool IsServerUdpOpcode(std::uint8_t opcode)
{
	return opcode == NatServerUdp::OP_NATT_KEEPALIVE;
}
inline bool IsPeerUdpOpcode(std::uint8_t opcode)
{
	return opcode == NatPeerUdp::OP_NATT_HOLEPUNCH;
}

namespace Detail
{
inline void WriteLE(std::uint8_t *out, std::uint32_t value, std::size_t count)
{
	if (count == 2) {
		PokeUInt16(out, static_cast<uint16>(value));
	} else {
		PokeUInt32(out, static_cast<uint32>(value));
	}
}
inline std::uint32_t ReadLE(const std::uint8_t *data, std::size_t count)
{
	return count == 2 ? PeekUInt16(data) : PeekUInt32(data);
}
} // namespace Detail

// Payloads only: callers must supply transport/protocol/opcode framing separately.
inline std::array<std::uint8_t, 6> BuildRequest(const Request &request)
{
	std::array<std::uint8_t, 6> bytes{};
	Detail::WriteLE(bytes.data(), request.targetId, 4);
	Detail::WriteLE(bytes.data() + 4, request.udpPort, 2);
	return bytes;
}
inline std::optional<Request> ParseRequest(const std::uint8_t *data, std::size_t size)
{
	if (!data || size < 6) {
		return std::nullopt;
	}
	return Request{ Detail::ReadLE(data, 4), static_cast<std::uint16_t>(Detail::ReadLE(data + 4, 2)) };
}
inline std::array<std::uint8_t, 25> BuildInfo(const Info &info)
{
	std::array<std::uint8_t, 25> bytes{};
	std::copy(info.peerIP.begin(), info.peerIP.end(), bytes.begin());
	Detail::WriteLE(bytes.data() + 4, info.tcpPort, 2);
	Detail::WriteLE(bytes.data() + 6, info.udpPort, 2);
	std::copy(info.userHash.begin(), info.userHash.end(), bytes.begin() + 8);
	bytes[24] = info.role;
	return bytes;
}
inline std::optional<Info> ParseInfo(const std::uint8_t *data, std::size_t size)
{
	if (!data || size < 25) {
		return std::nullopt;
	}
	Info info{};
	std::copy_n(data, 4, info.peerIP.begin());
	info.tcpPort = static_cast<std::uint16_t>(Detail::ReadLE(data + 4, 2));
	info.udpPort = static_cast<std::uint16_t>(Detail::ReadLE(data + 6, 2));
	std::copy_n(data + 8, 16, info.userHash.begin());
	info.role = data[24];
	return info;
}
inline std::array<std::uint8_t, 5> BuildFailure(const Failure &failure)
{
	std::array<std::uint8_t, 5> bytes{};
	Detail::WriteLE(bytes.data(), failure.targetId, 4);
	bytes[4] = failure.reason;
	return bytes;
}
inline std::optional<Failure> ParseFailure(const std::uint8_t *data, std::size_t size)
{
	if (!data || size < 5) {
		return std::nullopt;
	}
	return Failure{ Detail::ReadLE(data, 4), data[4] };
}
// Keepalive framing: protocol byte 0xE3, NAT-T payload opcode 0x9F, sent via
// the client's UDP socket to the server TCP port plus 4.
inline Hash BuildKeepalive(const Hash &ownUserHash)
{
	return ownUserHash;
}
inline std::optional<Hash> ParseKeepalive(const std::uint8_t *data, std::size_t size)
{
	if (!data || size < 16) {
		return std::nullopt;
	}
	Hash hash{};
	std::copy_n(data, 16, hash.begin());
	return hash;
}

// CT_EMULE_UDPPORTS (0xF9) login tag value: high 16 bits Kad UDP port, low 16 bits client UDP
// port. aMule shares one UDP socket for both, so the two arguments are usually equal.
inline std::uint32_t BuildUdpPortsLoginValue(std::uint16_t kadUdpPort, std::uint16_t clientUdpPort)
{
	return (static_cast<std::uint32_t>(kadUdpPort) << 16) | clientUdpPort;
}

struct KeepaliveFacts
{
	bool settingEnabled = false;
	bool connected = false;
	bool serverAnnouncesHolePunch = false;
};

// Whether to send OP_SERVER_NATT_KEEPALIVE now: connected to a server that announced
// SRV_TCPFLG_LOWID_HOLEPUNCH, with the runtime setting on.
inline bool ShouldSendKeepalive(const KeepaliveFacts &facts)
{
	return facts.settingEnabled && facts.connected && facts.serverAnnouncesHolePunch;
}

struct RequestFacts
{
	std::uint32_t ourId = 0;
	std::uint32_t sourceId = 0;
	bool sameServer = false;
	bool serverAnnouncesHolePunch = false;
	// A server is either Lugdunum or ed2k-server: one announcing Lugdunum's NAT
	// callback is handled by TrySendLugdunumNatCallbackRequest instead, never both.
	bool serverIsLugdunumActive = false;
	bool settingEnabled = false;
};

// Whether to send OP_LOWID_HOLEPUNCH_REQUEST for a LowID source: both ends must be LowID on the
// same, hole-punch-capable, non-Lugdunum server, and the runtime setting must be on.
inline bool ShouldRequestLowIdHolePunch(const RequestFacts &facts)
{
	if (!facts.settingEnabled || !facts.sameServer || facts.serverIsLugdunumActive ||
		!facts.serverAnnouncesHolePunch) {
		return false;
	}
	return IsLowID(facts.ourId) && IsLowID(facts.sourceId);
}

struct LowIdServerSourceFacts
{
	bool originIsLocalServer = false;
	bool sourceServerIsOurCurrentServer = false;
	bool weAreLowId = false;
	bool serverAnnouncesHolePunch = false;
	bool settingEnabled = false;
};

// Whether a LowID server source survives CanAddSource's LowID-while-firewalled drop. ed2k-server
// sends LowID sources to any LowID requester unconditionally (no opt-in flag needed, confirmed
// against get_sources.rs), so this only has to confirm the source came from our own current,
// hole-punch-capable server and that the runtime setting is on.
inline bool ShouldKeepLowIdServerSource(const LowIdServerSourceFacts &facts)
{
	return facts.settingEnabled && facts.originIsLocalServer && facts.sourceServerIsOurCurrentServer &&
	       facts.weAreLowId && facts.serverAnnouncesHolePunch;
}
} // namespace NatServerHolePunch
#endif // ENABLE_NATT_SERVER_COORDINATION
#endif // AMULE_NAT_SERVER_HOLE_PUNCH_H
