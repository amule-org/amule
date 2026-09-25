//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//

#include <muleunit/test.h>

#include "NatRendezvousProtocol.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

using namespace muleunit;

DECLARE_SIMPLE(NatRendezvousProtocol)

namespace
{
using Bytes = std::vector<std::uint8_t>;
using Hash = NatRendezvous::UserHash;

PeerAddressing::UdpEndpoint Endpoint(const char *address, std::uint16_t port = 4672)
{
	return { CNetworkAddress::FromString(address), port };
}

Hash HashValue(std::uint8_t first)
{
	Hash hash{};
	hash[0] = first;
	return hash;
}

Bytes Rendezvous(std::uint8_t options = 0, bool withFileHash = false)
{
	Bytes bytes;
	bytes.reserve(withFileHash ? NatRendezvous::kRendezvousWithFileHashSize
				   : NatRendezvous::kRendezvousWithoutFileHashSize);
	bytes.push_back(NatRendezvous::OP_RENDEZVOUS);
	const auto userHash = HashValue(1);
	bytes.insert(bytes.end(), userHash.begin(), userHash.end());
	bytes.push_back(options);
	if (withFileHash) {
		const auto fileHash = HashValue(2);
		bytes.insert(bytes.end(), fileHash.begin(), fileHash.end());
	}
	return bytes;
}

bool Relay(const Bytes &bytes,
	const PeerAddressing::UdpEndpoint &observed,
	NatRendezvous::CRequesterLimiter &limiter,
	std::uint64_t now = 1000)
{
	return NatRendezvous::DecideRelay(bytes.data(), bytes.size(), observed, limiter, now).has_value();
}
} // namespace

TEST(NatRendezvousProtocol, ParsesEMuleAIRendezvousWithoutFileContext)
{
	const auto bytes = Rendezvous(0x82);
	const auto parsed = NatRendezvous::ParseRendezvousRequest(bytes.data(), bytes.size());

	ASSERT_TRUE(parsed.has_value());
	ASSERT_EQUALS(std::uint8_t(1), parsed->userHash[0]);
	ASSERT_EQUALS(std::uint8_t(0x82), parsed->connectOptions);
	ASSERT_FALSE(parsed->fileHash.has_value());
}

TEST(NatRendezvousProtocol, ParsesOptionalFileContext)
{
	const auto bytes = Rendezvous(0x80, true);
	const auto parsed = NatRendezvous::ParseRendezvousRequest(bytes.data(), bytes.size());

	ASSERT_TRUE(parsed.has_value());
	ASSERT_TRUE(parsed->fileHash.has_value());
	ASSERT_EQUALS(std::uint8_t(2), parsed->fileHash->at(0));
}

TEST(NatRendezvousProtocol, RejectsTruncationTrailingBytesWrongOpcodeAndInvalidIdentity)
{
	const auto valid = Rendezvous();
	ASSERT_FALSE(NatRendezvous::ParseRendezvousRequest(nullptr, 0).has_value());
	for (std::size_t size = 0; size < valid.size(); ++size) {
		ASSERT_FALSE(NatRendezvous::ParseRendezvousRequest(valid.data(), size).has_value());
	}

	auto trailing = valid;
	trailing.push_back(0);
	ASSERT_FALSE(NatRendezvous::ParseRendezvousRequest(trailing.data(), trailing.size()).has_value());

	auto wrongOpcode = valid;
	wrongOpcode[0] = 0xA1;
	ASSERT_FALSE(
		NatRendezvous::ParseRendezvousRequest(wrongOpcode.data(), wrongOpcode.size()).has_value());

	auto nullUser = valid;
	std::fill(nullUser.begin() + 1, nullUser.begin() + 1 + NatRendezvous::kUserHashSize, 0);
	ASSERT_FALSE(NatRendezvous::ParseRendezvousRequest(nullUser.data(), nullUser.size()).has_value());

	auto nullFile = Rendezvous(0, true);
	std::fill(nullFile.begin() + NatRendezvous::kRendezvousWithoutFileHashSize, nullFile.end(), 0);
	ASSERT_FALSE(NatRendezvous::ParseRendezvousRequest(nullFile.data(), nullFile.size()).has_value());
}

TEST(NatRendezvousProtocol, UsesOnlyObservedEndpointAndPreservesOptions)
{
	NatRendezvous::CRequesterLimiter limiter;
	const auto observed = Endpoint("192.0.2.8", 4673);
	const auto bytes = Rendezvous(0x80);
	const auto action = NatRendezvous::DecideRelay(bytes.data(), bytes.size(), observed, limiter, 1000);

	ASSERT_TRUE(action.has_value());
	ASSERT_TRUE(action->observed.address == observed.address);
	ASSERT_EQUALS(observed.port, action->observed.port);
	ASSERT_EQUALS(std::uint8_t(0x80), action->connectOptions);
	ASSERT_EQUALS(std::uint8_t(1), action->userHash[0]);
}

TEST(NatRendezvousProtocol, RejectsUnusableObservedEndpoint)
{
	NatRendezvous::CRequesterLimiter limiter;
	ASSERT_FALSE(Relay(Rendezvous(), Endpoint("0.0.0.0"), limiter));
	ASSERT_FALSE(Relay(Rendezvous(), Endpoint("::", 4672), limiter));
	ASSERT_FALSE(Relay(Rendezvous(), Endpoint("192.0.2.7", 0), limiter));
}

TEST(NatRendezvousProtocol, ObservedMappedIPv4SharesExistingIPv4Policy)
{
	NatRendezvous::CRequesterLimiter limiter;
	const auto plain = Endpoint("192.0.2.7");
	const auto mapped = Endpoint("::ffff:192.0.2.7");

	ASSERT_TRUE(Relay(Rendezvous(), plain, limiter));
	ASSERT_FALSE(Relay(Rendezvous(), mapped, limiter, 1001));
}

TEST(NatRendezvousProtocol, DoesNotParseAnAddressFromThePayload)
{
	NatRendezvous::CRequesterLimiter limiter;
	const auto payload = Rendezvous();
	Bytes withAddress = payload;
	withAddress.insert(withAddress.end(), 16, 0);
	ASSERT_FALSE(
		NatRendezvous::ParseRendezvousRequest(withAddress.data(), withAddress.size()).has_value());
	const auto observed = Endpoint("2001:db8:1:2::1");
	const auto bytes = Rendezvous();
	const auto action = NatRendezvous::DecideRelay(bytes.data(), bytes.size(), observed, limiter, 1000);

	ASSERT_TRUE(action.has_value());
	ASSERT_TRUE(action->observed.address == observed.address);
}

TEST(NatRendezvousProtocol, AppliesRateLimitBeforeProducingRelayAction)
{
	NatRendezvous::CRequesterLimiter limiter;
	const auto observed = Endpoint("192.0.2.7");

	ASSERT_TRUE(Relay(Rendezvous(), observed, limiter));
	ASSERT_FALSE(Relay(Rendezvous(), observed, limiter, 1001));
	ASSERT_TRUE(Relay(Rendezvous(), observed, limiter, 1000 + NatRendezvous::kRequestThrottleMs));
}

// The observed source is a routing input, not proof of origin. Return-routability belongs to a
// future established-session or cookie handler; this pure decision API intentionally has no such
// authority and does not send packets.
TEST(NatRendezvousProtocol, RelayDecisionDoesNotClaimAuthentication)
{
	NatRendezvous::CRequesterLimiter limiter;
	const auto bytes = Rendezvous();
	const auto action =
		NatRendezvous::DecideRelay(bytes.data(), bytes.size(), Endpoint("192.0.2.7"), limiter, 1000);
	ASSERT_TRUE(action.has_value());
}
