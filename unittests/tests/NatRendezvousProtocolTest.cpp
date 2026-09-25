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

void AppendHash(Bytes &bytes, std::uint8_t first)
{
	const auto hash = HashValue(first);
	bytes.insert(bytes.end(), hash.begin(), hash.end());
}

// This is the OP_REASKCALLBACKUDP payload built by eMuleAI's BaseClient.cpp. The outer
// OP_REASKCALLBACKUDP opcode is not part of the payload passed to the handler.
Bytes Rendezvous(bool withEndpointHint, bool withFileContext)
{
	Bytes bytes;
	bytes.reserve(withEndpointHint ? NatRendezvous::kRendezvousEnvelopeWithHintSize
				       : NatRendezvous::kRendezvousEnvelopeSize);
	AppendHash(bytes, 0x10); // ServingBuddyID selects the target on the relay.
	bytes.insert(bytes.end(), NatRendezvous::kHashSize, 0); // Extended-payload marker.
	bytes.push_back(NatRendezvous::OP_RENDEZVOUS);
	AppendHash(bytes, 0x01); // Requester's own user hash.
	bytes.push_back(0x82);   // Connect options, including the endpoint-hint capability.
	if (withFileContext) {
		AppendHash(bytes, 0x02);
	} else {
		bytes.insert(bytes.end(), NatRendezvous::kHashSize, 0);
	}
	if (withEndpointHint) {
		// 8.8.8.8, UDP port 4672, transport hint 2; the port is native little-endian.
		bytes.insert(bytes.end(), { 8, 8, 8, 8, 0x40, 0x12, 2 });
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

TEST(NatRendezvousProtocol, ParsesEMuleAIRendezvousWithZeroFilePlaceholder)
{
	const auto bytes = Rendezvous(false, false);
	const auto parsed = NatRendezvous::ParseRendezvousEnvelope(bytes.data(), bytes.size());

	ASSERT_TRUE(parsed.has_value());
	ASSERT_EQUALS(std::uint8_t(0x10), parsed->servingBuddyId[0]);
	ASSERT_EQUALS(std::uint8_t(1), parsed->request.requesterHash[0]);
	ASSERT_EQUALS(std::uint8_t(0x82), parsed->request.connectOptions);
	ASSERT_FALSE(parsed->request.fileHash.has_value());
	ASSERT_FALSE(parsed->request.requesterHint.has_value());
}

TEST(NatRendezvousProtocol, ParsesFileContextAndUnverifiedEndpointHint)
{
	const auto bytes = Rendezvous(true, true);
	const auto parsed = NatRendezvous::ParseRendezvousEnvelope(bytes.data(), bytes.size());

	ASSERT_TRUE(parsed.has_value());
	ASSERT_TRUE(parsed->request.fileHash.has_value());
	ASSERT_EQUALS(std::uint8_t(2), parsed->request.fileHash->at(0));
	ASSERT_TRUE(parsed->request.requesterHint.has_value());
	ASSERT_TRUE(parsed->request.requesterHint->address == CNetworkAddress::FromString("8.8.8.8"));
	ASSERT_EQUALS(std::uint16_t(4672), parsed->request.requesterHint->port);
	ASSERT_EQUALS(std::uint8_t(2), parsed->request.requesterHint->transportHint);
}

TEST(NatRendezvousProtocol, RejectsTruncationTrailingBytesWrongOpcodeAndInvalidMarkers)
{
	const auto valid = Rendezvous(false, false);
	for (std::size_t size = 0; size < valid.size(); ++size) {
		ASSERT_FALSE(NatRendezvous::ParseRendezvousEnvelope(valid.data(), size).has_value());
	}

	auto trailing = valid;
	trailing.push_back(0);
	trailing.push_back(1);
	ASSERT_TRUE(NatRendezvous::ParseRendezvousEnvelope(trailing.data(), trailing.size()).has_value());

	auto wrongOpcode = valid;
	wrongOpcode[NatRendezvous::kRendezvousPrefixSize] = 0xA1;
	ASSERT_FALSE(
		NatRendezvous::ParseRendezvousEnvelope(wrongOpcode.data(), wrongOpcode.size()).has_value());

	auto nullBuddy = valid;
	std::fill(nullBuddy.begin(), nullBuddy.begin() + NatRendezvous::kHashSize, 0);
	ASSERT_FALSE(NatRendezvous::ParseRendezvousEnvelope(nullBuddy.data(), nullBuddy.size()).has_value());

	auto nonzeroMarker = valid;
	nonzeroMarker[NatRendezvous::kHashSize] = 1;
	const bool nonzeroMarkerAccepted =
		NatRendezvous::ParseRendezvousEnvelope(nonzeroMarker.data(), nonzeroMarker.size())
			.has_value();
	ASSERT_FALSE(nonzeroMarkerAccepted);

	auto nullRequester = valid;
	std::fill(nullRequester.begin() + NatRendezvous::kRendezvousPrefixSize + 1,
		nullRequester.begin() + NatRendezvous::kRendezvousPrefixSize + 1 + NatRendezvous::kHashSize,
		0);
	const bool nullRequesterAccepted =
		NatRendezvous::ParseRendezvousEnvelope(nullRequester.data(), nullRequester.size())
			.has_value();
	ASSERT_FALSE(nullRequesterAccepted);
}

TEST(NatRendezvousProtocol, DropsInvalidOptionalEndpointHintButKeepsRequest)
{
	for (const auto invalid : { std::array<std::uint8_t, 7>{ 0, 0, 0, 0, 0x40, 0x12, 2 },
		     std::array<std::uint8_t, 7>{ 192, 0, 2, 7, 0, 0, 2 } }) {
		auto bytes = Rendezvous(false, false);
		bytes.insert(bytes.end(), invalid.begin(), invalid.end());
		const auto parsed = NatRendezvous::ParseRendezvousEnvelope(bytes.data(), bytes.size());
		ASSERT_TRUE(parsed.has_value());
		ASSERT_FALSE(parsed->request.requesterHint.has_value());
	}
}

TEST(NatRendezvousProtocol, RelayCarriesBuddyRequesterOptionsAndObservedEndpoint)
{
	NatRendezvous::CRequesterLimiter limiter;
	const auto observed = Endpoint("192.0.2.8", 4673);
	const auto bytes = Rendezvous(true, true);
	const auto action = NatRendezvous::DecideRelay(bytes.data(), bytes.size(), observed, limiter, 1000);

	ASSERT_TRUE(action.has_value());
	ASSERT_EQUALS(std::uint8_t(0x10), action->servingBuddyId[0]);
	ASSERT_EQUALS(std::uint8_t(1), action->requesterHash[0]);
	ASSERT_EQUALS(std::uint8_t(0x82), action->connectOptions);
	ASSERT_TRUE(action->fileHash.has_value());
	ASSERT_TRUE(action->requesterHint.has_value());
	ASSERT_TRUE(action->observed.address == observed.address);
	ASSERT_EQUALS(observed.port, action->observed.port);
}

TEST(NatRendezvousProtocol, RejectsUnusableObservedEndpoint)
{
	NatRendezvous::CRequesterLimiter limiter;
	ASSERT_FALSE(Relay(Rendezvous(false, false), Endpoint("0.0.0.0"), limiter));
	ASSERT_FALSE(Relay(Rendezvous(false, false), Endpoint("::", 4672), limiter));
	ASSERT_FALSE(Relay(Rendezvous(false, false), Endpoint("192.0.2.7", 0), limiter));
}

TEST(NatRendezvousProtocol, ObservedMappedIPv4SharesExistingIPv4Policy)
{
	NatRendezvous::CRequesterLimiter limiter;
	const auto plain = Endpoint("192.0.2.7");
	const auto mapped = Endpoint("::ffff:192.0.2.7");

	ASSERT_TRUE(Relay(Rendezvous(false, false), plain, limiter));
	ASSERT_FALSE(Relay(Rendezvous(false, false), mapped, limiter, 1001));
}

TEST(NatRendezvousProtocol, AppliesRateLimitBeforeProducingRelayAction)
{
	NatRendezvous::CRequesterLimiter limiter;
	const auto observed = Endpoint("192.0.2.7");

	ASSERT_TRUE(Relay(Rendezvous(false, false), observed, limiter));
	ASSERT_FALSE(Relay(Rendezvous(false, false), observed, limiter, 1001));
	ASSERT_TRUE(
		Relay(Rendezvous(false, false), observed, limiter, 1000 + NatRendezvous::kRequestThrottleMs));
}

// The observed source is a routing input, not proof of origin. Return-routability belongs to a
// future established-session or cookie handler; this pure decision API intentionally has no such
// authority and does not send packets.
TEST(NatRendezvousProtocol, RelayDecisionDoesNotClaimAuthentication)
{
	NatRendezvous::CRequesterLimiter limiter;
	const auto bytes = Rendezvous(false, false);
	const auto action =
		NatRendezvous::DecideRelay(bytes.data(), bytes.size(), Endpoint("192.0.2.7"), limiter, 1000);
	ASSERT_TRUE(action.has_value());
}
