// This file is part of the aMule Project.
// Copyright (c) 2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#include <muleunit/test.h>
#include <Proxy.h>
#include <NetworkAddressAsio.h>

#include <algorithm>
#include <vector>

using namespace muleunit;

DECLARE_SIMPLE(Proxy)

namespace
{
// Literal wire bytes: neither headers nor port expectations use the encoder.
const std::vector<unsigned char> ipv4 = { 0, 0, 0, 1, 192, 0, 2, 7, 0x12, 0x80, 0xde, 0, 0xad };
const std::vector<unsigned char> ipv6 = {
	0, 0, 0, 4, 0x20, 1, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0x12, 0x80, 0xde, 0, 0xad
};
const std::vector<unsigned char> mapped = {
	0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 0, 2, 7, 0x12, 0x80, 0xde, 0, 0xad
};

uint32 Parse(const std::vector<unsigned char> &packet,
	CNetworkAddress &addr,
	uint16 &port,
	void *output,
	uint32 capacity)
{
	return ParseSocks5UDPDatagram(
		reinterpret_cast<const char *>(packet.data()), packet.size(), addr, port, output, capacity);
}

void CheckRejected(const std::vector<unsigned char> &packet)
{
	auto addr = CNetworkAddress::FromString("192.0.2.99");
	uint16 port = 1234;
	unsigned char output[5];
	std::fill(std::begin(output), std::end(output), 0xcc);
	ASSERT_EQUALS(0u, Parse(packet, addr, port, output, sizeof(output)));
	ASSERT_TRUE(addr.IsAbsent());
	ASSERT_EQUALS(0u, port);
	for (auto byte : output) {
		ASSERT_EQUALS(0xcc, byte);
	}
}
} // namespace

TEST(Proxy, IPv4IPv6AndMappedSourcesMatchSocketIngress)
{
	const std::vector<unsigned char> packets[] = { ipv4, ipv6, mapped };
	const char *sources[] = { "192.0.2.7", "2001:db8::1", "::ffff:192.0.2.7" };
	for (unsigned i = 0; i < 3; ++i) {
		CNetworkAddress addr;
		uint16 port = 0;
		unsigned char output[5] = { 0xcc, 0xcc, 0xcc, 0xcc, 0xcc };
		ASSERT_EQUALS(3u, Parse(packets[i], addr, port, output + 1, 3));
		const auto expected = NetworkAddressAsio::FromIngressAddress(
			NetworkAddressAsio::ToAsioAddress(CNetworkAddress::FromString(sources[i])));
		ASSERT_TRUE(addr == expected);
		ASSERT_EQUALS(4736u, port); // 0x1280, not 0x8012; low byte has its high bit set.
		ASSERT_EQUALS(0xcc, output[0]);
		ASSERT_EQUALS(0xde, output[1]);
		ASSERT_EQUALS(0, output[2]);
		ASSERT_EQUALS(0xad, output[3]);
		ASSERT_EQUALS(0xcc, output[4]);
		ASSERT_TRUE(i == 1 ? addr.IsIPv6() : addr.IsIPv4());
		ASSERT_FALSE(addr.IsIPv4Mapped());
	}
}

TEST(Proxy, EveryTruncatedHeaderIsRejected)
{
	for (const auto &packet : { ipv4, ipv6, mapped }) {
		const auto headerSize = packet.size() - 3;
		for (size_t size = 0; size < headerSize; ++size) {
			CheckRejected(std::vector<unsigned char>(packet.begin(), packet.begin() + size));
		}
	}
}

TEST(Proxy, ReservedBytesAndFragmentsAreRejected)
{
	for (const auto &packet : { ipv4, ipv6 }) {
		for (unsigned index = 0; index < 3; ++index) {
			for (unsigned char value : { 1, 0x80, 0xff }) {
				auto invalid = packet;
				invalid[index] = value;
				CheckRejected(invalid);
			}
		}
	}
}

TEST(Proxy, DomainAndUnknownAddressTypesAreRejected)
{
	// SOCKS5_ATYP_DOMAINNAME is refused deliberately, not left unimplemented. The name is
	// length-prefixed, so its header has no fixed size; the code this replaced assumed a flat 262
	// bytes and mis-offset every such datagram. If support is ever added, this row changes to an
	// expectation rather than being deleted as an obsolete restriction.
	CheckRejected({ 0, 0, 0, 3, 3, 'a', 'b', 'c', 0x12, 0x80, 0xde });
	for (unsigned char type : { 0, 2, 5, 0xff }) {
		auto invalid = ipv6;
		invalid[3] = type;
		CheckRejected(invalid);
	}
}

TEST(Proxy, PayloadIsBoundedByAvailableBytesAndCapacity)
{
	for (const auto &packet : { ipv4, ipv6, mapped }) {
		const auto headerSize = packet.size() - 3;
		for (unsigned availablePayload = 0; availablePayload <= 3; ++availablePayload) {
			const std::vector<unsigned char> shortened(
				packet.begin(), packet.begin() + headerSize + availablePayload);
			for (unsigned capacity = 0; capacity <= 5; ++capacity) {
				CNetworkAddress addr;
				uint16 port = 0;
				unsigned char output[7];
				std::fill(std::begin(output), std::end(output), 0xcc);
				const unsigned expected = std::min(availablePayload, capacity);
				ASSERT_EQUALS(expected, Parse(shortened, addr, port, output + 1, capacity));
				ASSERT_TRUE(addr.IsPresent());
				ASSERT_EQUALS(4736u, port);
				ASSERT_EQUALS(0xcc, output[0]);
				for (unsigned i = 0; i < 6; ++i) {
					ASSERT_EQUALS(
						i < expected ? packet[headerSize + i] : 0xcc, output[i + 1]);
				}
			}
		}
	}
}

TEST(Proxy, PortDecodesBothOctetsInNetworkOrder)
{
	for (const auto &packet : { ipv4, ipv6 }) {
		const auto headerSize = packet.size() - 3;
		for (unsigned expected : { 0u, 1u, 255u, 256u, 0x80ffu, 65535u }) {
			auto input = packet;
			input[headerSize - 2] = expected >> 8;
			input[headerSize - 1] = expected & 0xff;
			CNetworkAddress addr;
			uint16 port = 0;
			unsigned char output[3];
			ASSERT_EQUALS(3u, Parse(input, addr, port, output, sizeof(output)));
			ASSERT_EQUALS(expected, port);
		}
	}
}

namespace
{
unsigned int PortOffset(const std::vector<unsigned char> &reply)
{
	return Socks5ReplyPortOffset(reinterpret_cast<const char *>(reply.data()), reply.size());
}
} // namespace

TEST(Proxy, CommandReplyPortOffsetFollowsTheAddressType)
{
	const std::vector<unsigned char> v4 = { 5, 0, 0, 1, 192, 0, 2, 7, 0x12, 0x80 };
	ASSERT_EQUALS(8u, PortOffset(v4));
	// RFC 1928: VER REP RSV ATYP, one length octet, the name, then the port.
	const std::vector<unsigned char> domain = { 5, 0, 0, 3, 3, 'a', '.', 'b', 0x12, 0x80 };
	ASSERT_EQUALS(8u, PortOffset(domain));
	for (const auto &reply : { v4, domain }) {
		for (std::size_t size = 0; size < reply.size(); ++size) {
			ASSERT_EQUALS(0u,
				PortOffset(std::vector<unsigned char>(reply.begin(), reply.begin() + size)));
		}
	}
	// The bound address is kept as IPv4, so an IPv6 or unknown one cannot be used.
	std::vector<unsigned char> v6(22, 0);
	v6[0] = 5;
	v6[3] = 4;
	ASSERT_EQUALS(0u, PortOffset(v6));
	auto unknown = v4;
	unknown[3] = 2;
	ASSERT_EQUALS(0u, PortOffset(unknown));
}

TEST(Proxy, AnUnspecifiedBoundAddressFallsBackToTheProxyItself)
{
	const auto proxy = CNetworkAddress::FromString("198.51.100.9");
	const auto bound = CNetworkAddress::FromString("192.0.2.1");
	ASSERT_TRUE(Socks5ExpectedRelay(bound, proxy) == bound);
	ASSERT_TRUE(Socks5ExpectedRelay(CNetworkAddress::FromString("0.0.0.0"), proxy) == proxy);
	// Whatever the reply named, a datagram from anywhere else is refused.
	const auto expected = Socks5ExpectedRelay(CNetworkAddress::FromString("0.0.0.0"), proxy);
	ASSERT_TRUE(IsFromSocks5Relay(proxy, 1080, expected, 1080));
	ASSERT_FALSE(IsFromSocks5Relay(bound, 1080, expected, 1080));
}

TEST(Proxy, RelayDatagramsMustComeFromTheBoundRelay)
{
	const auto relay = CNetworkAddress::FromString("192.0.2.1");
	ASSERT_TRUE(IsFromSocks5Relay(relay, 1080, relay, 1080));
	ASSERT_TRUE(IsFromSocks5Relay(CNetworkAddress::FromString("::ffff:192.0.2.1"), 1080, relay, 1080));
	ASSERT_FALSE(IsFromSocks5Relay(relay, 1081, relay, 1080));
	ASSERT_FALSE(IsFromSocks5Relay(CNetworkAddress::FromString("192.0.2.2"), 1080, relay, 1080));
	ASSERT_FALSE(IsFromSocks5Relay(CNetworkAddress::Absent(), 1080, relay, 1080));
	// A relay reported as 0.0.0.0 pins only the port.
	const auto any = CNetworkAddress::FromString("0.0.0.0");
	ASSERT_TRUE(IsFromSocks5Relay(CNetworkAddress::FromString("198.51.100.9"), 1080, any, 1080));
	ASSERT_FALSE(IsFromSocks5Relay(CNetworkAddress::FromString("198.51.100.9"), 1081, any, 1080));
	ASSERT_FALSE(IsFromSocks5Relay(CNetworkAddress::Absent(), 1080, any, 1080));
}

// File_checked_for_headers
