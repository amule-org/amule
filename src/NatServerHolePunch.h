// Experimental server-coordinated NAT-T payload codecs. No socket integration.
#ifndef AMULE_NAT_SERVER_HOLE_PUNCH_H
#define AMULE_NAT_SERVER_HOLE_PUNCH_H

#ifdef ENABLE_NATT_SERVER_COORDINATION
#include "include/protocol/Protocols.h"

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
	for (std::size_t i = 0; i < count; ++i) {
		out[i] = static_cast<std::uint8_t>(value >> (8 * i));
	}
}
inline std::uint32_t ReadLE(const std::uint8_t *data, std::size_t count)
{
	std::uint32_t value = 0;
	for (std::size_t i = 0; i < count; ++i) {
		value |= static_cast<std::uint32_t>(data[i]) << (8 * i);
	}
	return value;
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
	if (!data || size != 6) {
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
	if (!data || size != 25) {
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
	if (!data || size != 5) {
		return std::nullopt;
	}
	return Failure{ Detail::ReadLE(data, 4), data[4] };
}
inline Hash BuildKeepalive(const Hash &ownUserHash)
{
	return ownUserHash;
}
inline std::optional<Hash> ParseKeepalive(const std::uint8_t *data, std::size_t size)
{
	if (!data || size != 16) {
		return std::nullopt;
	}
	Hash hash{};
	std::copy_n(data, 16, hash.begin());
	return hash;
}
} // namespace NatServerHolePunch
#endif // ENABLE_NATT_SERVER_COORDINATION
#endif // AMULE_NAT_SERVER_HOLE_PUNCH_H
