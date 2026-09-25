//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//

#include <muleunit/test.h>

#include "NatRendezvousProtocol.h"

#include <algorithm>
#include <vector>

using namespace muleunit;

DECLARE_SIMPLE(NatRendezvousProtocol)

namespace
{
using Bytes = std::vector<std::uint8_t>;

PeerAddressing::UdpEndpoint Endpoint(const char *address, std::uint16_t port = 4672)
{
	return { CNetworkAddress::FromString(address), port };
}

// Literal fixtures pin the local layout, independently of a production encoder.
// 0x1240 is UDP port 4672 in big endian; these are not interoperability fixtures.
Bytes IPv4()
{
	return { 0x01, 0x00, 0x04, 192, 0, 2, 7, 0x12, 0x40 };
}

Bytes IPv6(std::uint8_t subnet = 2, std::uint8_t host = 1)
{
	return { 0x01,
		0x00,
		0x06,
		0x20,
		0x01,
		0x0d,
		0xb8,
		0,
		1,
		0,
		subnet,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		host,
		0x12,
		0x40 };
}

bool Relay(const Bytes &bytes,
	const PeerAddressing::UdpEndpoint &observed,
	NatRendezvous::CRequesterLimiter &limiter,
	std::uint64_t now = 1000)
{
	return NatRendezvous::DecideRelay(bytes.data(), bytes.size(), observed, limiter, now).has_value();
}
} // namespace

TEST(NatRendezvousProtocol, ParsesExactLocalLayouts)
{
	for (const auto &bytes : { IPv4(), IPv6() }) {
		const auto parsed = NatRendezvous::ParseControlPayload(bytes.data(), bytes.size());
		ASSERT_TRUE(parsed.has_value());
		ASSERT_EQUALS(std::uint16_t(4672), parsed->claimed.port);
		ASSERT_TRUE(parsed->claimed.address ==
			    Endpoint(bytes.size() == 9 ? "192.0.2.7" : "2001:db8:1:2::1").address);
	}
}

TEST(NatRendezvousProtocol, RejectsEveryTruncationAndTrailingBytes)
{
	ASSERT_FALSE(NatRendezvous::ParseControlPayload(nullptr, 0).has_value());
	ASSERT_FALSE(NatRendezvous::ParseControlPayload(nullptr, 21).has_value());
	for (auto bytes : { IPv4(), IPv6() }) {
		for (std::size_t size = 0; size < bytes.size(); ++size) {
			ASSERT_FALSE(NatRendezvous::ParseControlPayload(bytes.data(), size).has_value());
		}
		bytes.push_back(0);
		ASSERT_FALSE(NatRendezvous::ParseControlPayload(bytes.data(), bytes.size()).has_value());
	}
}

TEST(NatRendezvousProtocol, RejectsUnknownOpcodeOptionsFamilyAndUnusableEndpoints)
{
	for (std::size_t field = 0; field < 3; ++field) {
		auto bytes = IPv4();
		bytes[field] = 0xff;
		ASSERT_FALSE(NatRendezvous::ParseControlPayload(bytes.data(), bytes.size()).has_value());
	}
	for (auto bytes : { IPv4(), IPv6() }) {
		bytes[bytes.size() - 2] = bytes.back() = 0;
		ASSERT_FALSE(NatRendezvous::ParseControlPayload(bytes.data(), bytes.size()).has_value());
	}
	for (auto bytes : { IPv4(), IPv6() }) {
		std::fill(bytes.begin() + 3, bytes.end() - 2, 0);
		ASSERT_FALSE(NatRendezvous::ParseControlPayload(bytes.data(), bytes.size()).has_value());
	}
}

TEST(NatRendezvousProtocol, RejectsThirdPartyHintsWithoutConsumingObservedBudget)
{
	NatRendezvous::CRequesterLimiter limiter;
	ASSERT_FALSE(Relay(IPv4(), Endpoint("192.0.2.8"), limiter));
	ASSERT_FALSE(Relay(IPv4(), Endpoint("192.0.2.7", 4673), limiter));
	ASSERT_FALSE(Relay(IPv4(), Endpoint("192.0.2.7", 0), limiter));
	ASSERT_FALSE(Relay(IPv4(), Endpoint("0.0.0.0"), limiter));
	ASSERT_TRUE(Relay(IPv4(), Endpoint("192.0.2.7"), limiter));
}

TEST(NatRendezvousProtocol, ForwardsObservedRepresentationAndMarksRelayed)
{
	NatRendezvous::CRequesterLimiter limiter;
	const auto observed = Endpoint("::ffff:192.0.2.7");
	const auto bytes = IPv4();
	const auto action = NatRendezvous::DecideRelay(bytes.data(), bytes.size(), observed, limiter, 1000);
	ASSERT_TRUE(action.has_value());
	ASSERT_TRUE(action->observed.address == observed.address);
	ASSERT_EQUALS(observed.port, action->observed.port);
	ASSERT_EQUALS(NatRendezvous::CONNECT_OPT_NATT_RELAYED, action->options);
}

TEST(NatRendezvousProtocol, RejectsAlreadyRelayedWithoutConsumingBudget)
{
	NatRendezvous::CRequesterLimiter limiter;
	auto bytes = IPv4();
	bytes[1] = NatRendezvous::CONNECT_OPT_NATT_RELAYED;
	ASSERT_TRUE(NatRendezvous::ParseControlPayload(bytes.data(), bytes.size()).has_value());
	ASSERT_FALSE(Relay(bytes, Endpoint("192.0.2.7"), limiter));
	ASSERT_TRUE(Relay(IPv4(), Endpoint("192.0.2.7"), limiter));
}

TEST(NatRendezvousProtocol, ThrottlesBeforeRelayAndSharesMappedIPv4Budget)
{
	NatRendezvous::CRequesterLimiter limiter;
	ASSERT_TRUE(Relay(IPv4(), Endpoint("192.0.2.7"), limiter));
	ASSERT_FALSE(Relay(IPv4(), Endpoint("192.0.2.7"), limiter, 1001));
	ASSERT_FALSE(Relay(IPv4(), Endpoint("::ffff:192.0.2.7"), limiter, 1001));
	ASSERT_TRUE(Relay(
		IPv4(), Endpoint("::ffff:192.0.2.7"), limiter, 1000 + NatRendezvous::kRequestThrottleMs));
	ASSERT_FALSE(Relay(IPv4(), Endpoint("192.0.2.7"), limiter, 1001 + NatRendezvous::kRequestThrottleMs));
}

TEST(NatRendezvousProtocol, SharesIPv6Slash64BudgetButNotIdentityOrAdjacentPrefix)
{
	NatRendezvous::CRequesterLimiter limiter;
	ASSERT_FALSE(Relay(IPv6(), Endpoint("2001:db8:1:2::2"), limiter));
	ASSERT_TRUE(Relay(IPv6(), Endpoint("2001:db8:1:2::1"), limiter));
	ASSERT_FALSE(Relay(IPv6(2, 2), Endpoint("2001:db8:1:2::2"), limiter, 1001));
	ASSERT_TRUE(Relay(IPv6(3), Endpoint("2001:db8:1:3::1"), limiter, 1001));
}
