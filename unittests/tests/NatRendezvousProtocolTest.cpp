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
#include <string>
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
	ASSERT_EQUALS(NatRendezvous::kTransportHintQuic, parsed->request.transportHint);
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
		// eMuleAI reads the transport byte whatever the address before it.
		ASSERT_EQUALS(NatRendezvous::kTransportHintQuic, parsed->request.transportHint);
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

TEST(NatRendezvousProtocol, ForwardedRendezvousIsRebuiltFromTheParsedRequest)
{
	for (const bool hint : { false, true }) {
		for (const bool file : { false, true }) {
			Bytes in = Rendezvous(hint, file);
			const auto envelope = NatRendezvous::ParseRendezvousEnvelope(in.data(), in.size());
			ASSERT_TRUE(envelope.has_value());
			const auto body = NatRendezvous::BuildForwardedRendezvous(envelope->request);
			// Same bytes as received after the serving-buddy ID, for a well-formed request.
			ASSERT_EQUALS(in.size() - NatRendezvous::kHashSize, body.size());
			const auto received = in.begin() + NatRendezvous::kHashSize;
			ASSERT_TRUE(std::equal(body.begin(), body.end(), received));
		}
	}
}

namespace
{
// What our buddy writes after the requester's ip/port in OP_REASKCALLBACKTCP: the envelope less the
// serving-buddy ID, so the extended-payload marker comes first.
Bytes ForwardedBody(bool withEndpointHint, std::uint8_t connectOptions = 0xC2)
{
	Bytes body = Rendezvous(withEndpointHint, true);
	body.erase(body.begin(), body.begin() + NatRendezvous::kHashSize);
	body[NatRendezvous::kHashSize + 1 + NatRendezvous::kHashSize] = connectOptions;
	return body;
}

NatRendezvous::ServedRendezvousDecision Served(const Bytes &body,
	std::uint8_t localTransports = NatRendezvous::kNatTTransports,
	const PeerAddressing::UdpEndpoint &observed = Endpoint("192.0.2.8", 4673))
{
	return NatRendezvous::DecideServedRendezvous(observed, body.data(), body.size(), localTransports);
}
} // namespace

TEST(NatRendezvousProtocol, ForwardedControlIsMarkedByAZeroFileHash)
{
	const Bytes body = ForwardedBody(false);
	ASSERT_TRUE(NatRendezvous::IsForwardedControl(body.data(), body.size()));

	Bytes reask(NatRendezvous::kHashSize, 0);
	reask[0] = 0x2A; // A file hash: a plain reask.
	ASSERT_FALSE(NatRendezvous::IsForwardedControl(reask.data(), reask.size()));

	const Bytes shortBody(NatRendezvous::kHashSize - 1, 0);
	ASSERT_FALSE(NatRendezvous::IsForwardedControl(shortBody.data(), shortBody.size()));
}

TEST(NatRendezvousProtocol, ServedRendezvousPunchesTheObservedEndpointNotTheHint)
{
	const auto observed = Endpoint("192.0.2.8", 4673);
	const auto decision = Served(ForwardedBody(true), NatRendezvous::kNatTTransports, observed);

	ASSERT_TRUE(decision.outcome == NatRendezvous::EServedRendezvous::Punch);
	ASSERT_TRUE(decision.target.address == observed.address);
	ASSERT_EQUALS(observed.port, decision.target.port);
	ASSERT_TRUE(decision.request.requesterHint.has_value());
	ASSERT_FALSE(decision.target.address == decision.request.requesterHint->address);
	ASSERT_EQUALS(std::uint8_t(1), decision.request.requesterHash[0]);
}

TEST(NatRendezvousProtocol, ServedRendezvousParsesWithAndWithoutTheTail)
{
	Bytes sixByteTail = ForwardedBody(true);
	sixByteTail.pop_back();
	for (const Bytes &body : { ForwardedBody(false), ForwardedBody(true), sixByteTail }) {
		ASSERT_TRUE(Served(body).outcome == NatRendezvous::EServedRendezvous::Punch);
	}
	ASSERT_EQUALS(NatRendezvous::kTransportHintQuic, Served(ForwardedBody(true)).request.transportHint);
	ASSERT_EQUALS(std::uint8_t(0), Served(sixByteTail).request.transportHint);
}

// ModderMule/emule-qt#265 appends typed elements after the full 7-byte tail. A 0x01 shorter than
// 18 bytes is not an IPv6 endpoint, and the rest of the request parses as if the block were absent.
TEST(NatRendezvousProtocol, ServedRendezvousParsesABodyWithTheExtensionBlock)
{
	Bytes body = ForwardedBody(true);
	body.insert(body.end(), { 0x01, 0x04, 0xDE, 0xAD, 0xBE, 0xEF, 0x02, 0x00 });
	const auto decision = Served(body);

	ASSERT_TRUE(decision.outcome == NatRendezvous::EServedRendezvous::Punch);
	ASSERT_TRUE(decision.request.requesterHint->address == CNetworkAddress::FromString("8.8.8.8"));
	ASSERT_FALSE(decision.request.requesterUdp6.has_value());
}

TEST(NatRendezvousProtocol, ServedRendezvousDropsAnythingButAWellFormed0xA0)
{
	Bytes otherOpcode = ForwardedBody(false);
	otherOpcode[NatRendezvous::kHashSize] = 0xA1;
	Bytes nullRequester = ForwardedBody(false);
	std::fill_n(nullRequester.begin() + NatRendezvous::kHashSize + 1, NatRendezvous::kHashSize, 0);
	Bytes truncated = ForwardedBody(false);
	truncated.pop_back();
	const Bytes markerOnly(NatRendezvous::kHashSize, 0);
	for (const Bytes &body : { otherOpcode, nullRequester, truncated, markerOnly }) {
		ASSERT_TRUE(Served(body).outcome == NatRendezvous::EServedRendezvous::Malformed);
	}
	for (const auto &observed : { Endpoint("0.0.0.0"), Endpoint("192.0.2.8", 0) }) {
		const auto decision = Served(ForwardedBody(false), NatRendezvous::kNatTTransports, observed);
		ASSERT_TRUE(decision.outcome == NatRendezvous::EServedRendezvous::Malformed);
	}
}

// We never dial here: we arm what we share with the requester and let it choose.
TEST(NatRendezvousProtocol, ServedRendezvousNeedsATransportBothSidesHave)
{
	const auto quicOnlyRequester = ForwardedBody(false, 0x40 | 0x02);
	ASSERT_TRUE(Served(quicOnlyRequester, 0x80).outcome ==
		    NatRendezvous::EServedRendezvous::NoSharedTransport);
	const auto shared = Served(quicOnlyRequester, 0x80 | 0x40);
	ASSERT_TRUE(shared.outcome == NatRendezvous::EServedRendezvous::Punch);
	ASSERT_EQUALS(std::uint8_t(0x40), shared.sharedTransports);

	const auto noBits = ForwardedBody(false, 0x02);
	ASSERT_TRUE(Served(noBits).outcome == NatRendezvous::EServedRendezvous::NoSharedTransport);
	ASSERT_TRUE(Served(ForwardedBody(false), 0).outcome ==
		    NatRendezvous::EServedRendezvous::NoSharedTransport);
}

// eMuleQt sets no NAT-T bit in the connect options, only its crypt bits and 0x20 (it takes an
// endpoint hint), and names QUIC in the tail.
TEST(NatRendezvousProtocol, ServedRendezvousTakesTheTransportFromTheTail)
{
	const auto emuleQt = ForwardedBody(true, 0x20 | 0x01);
	const auto quic = Served(emuleQt, 0x80 | 0x40);
	ASSERT_TRUE(quic.outcome == NatRendezvous::EServedRendezvous::Punch);
	ASSERT_EQUALS(std::uint8_t(0x40), quic.sharedTransports);
	ASSERT_TRUE(Served(emuleQt, 0x80).outcome == NatRendezvous::EServedRendezvous::NoSharedTransport);

	Bytes utp = emuleQt;
	utp.back() = NatRendezvous::kTransportHintUtp;
	ASSERT_EQUALS(std::uint8_t(0x80), Served(utp, 0x80 | 0x40).sharedTransports);
}

TEST(NatRendezvousProtocol, RequesterTransportsAreTheBitsAndTheTail)
{
	NatRendezvous::RendezvousRequest request{};
	request.connectOptions = 0x80 | 0x02;
	ASSERT_EQUALS(std::uint8_t(0x80), NatRendezvous::RequesterTransports(request));
	request.transportHint = NatRendezvous::kTransportHintQuic;
	ASSERT_EQUALS(std::uint8_t(0xC0), NatRendezvous::RequesterTransports(request));

	// eMuleAI takes only 1 and 2.
	request.connectOptions = 0x02;
	for (const std::uint8_t unknown : std::array<std::uint8_t, 3>{ 0, 3, 0xFF }) {
		request.transportHint = unknown;
		ASSERT_EQUALS(std::uint8_t(0), NatRendezvous::RequesterTransports(request));
	}
}

TEST(NatRendezvousProtocol, ServedRendezvousRepeatJoinsTheAttemptInFlight)
{
	NatRendezvous::CServedRendezvousGate gate;
	const auto requester = Endpoint("192.0.2.8", 4673);
	using EAdmission = NatRendezvous::EServedAdmission;

	ASSERT_TRUE(gate.Admit(requester, 1000) == EAdmission::Start);
	// eMuleQt resends up to three times, 5 s apart.
	for (const std::uint64_t resend : { 6000, 11000, 16000 }) {
		ASSERT_TRUE(gate.Admit(requester, resend) == EAdmission::Join);
	}
	ASSERT_TRUE(gate.Admit(requester, 1000 + NatRendezvous::kServedRendezvousInFlightMs) ==
		    EAdmission::Start);
}

TEST(NatRendezvousProtocol, ServedRendezvousLimitsEachRequesterAddress)
{
	NatRendezvous::CServedRendezvousGate gate;
	using EAdmission = NatRendezvous::EServedAdmission;

	ASSERT_TRUE(gate.Admit(Endpoint("192.0.2.8", 4673), 1000) == EAdmission::Start);
	ASSERT_TRUE(gate.Admit(Endpoint("192.0.2.8", 4674), 1001) == EAdmission::RequesterLimited);
	ASSERT_TRUE(gate.Admit(Endpoint("192.0.2.8", 4674),
			    1000 + NatRendezvous::kServedRendezvousPerRequesterMs) == EAdmission::Start);
}

// A buddy relays a 0xA0 from any UDP source, and the source can be forged: a sender that forges a
// new one each time is stopped only by a limit for all requesters.
TEST(NatRendezvousProtocol, ServedRendezvousHasAGlobalLimitThatJoinsDoNotSpend)
{
	NatRendezvous::CServedRendezvousGate gate;
	using EAdmission = NatRendezvous::EServedAdmission;
	const auto requester = [](std::size_t i) {
		const std::string address = "198.51.100." + std::to_string(i + 1);
		return Endpoint(address.c_str());
	};

	for (std::size_t i = 0; i < NatRendezvous::kMaxServedRendezvousPerWindow; ++i) {
		ASSERT_TRUE(gate.Admit(requester(i), 1000) == EAdmission::Start);
		ASSERT_TRUE(gate.Admit(requester(i), 1001) == EAdmission::Join);
	}
	const auto next = requester(NatRendezvous::kMaxServedRendezvousPerWindow);
	ASSERT_TRUE(gate.Admit(next, 1002) == EAdmission::OverGlobalLimit);
	ASSERT_TRUE(gate.Admit(next, 1000 + NatRendezvous::kServedRendezvousWindowMs) == EAdmission::Start);
}

TEST(NatRendezvousProtocol, ForwardedRendezvousDropsTrailingBytesTheParserDidNotAccept)
{
	Bytes in = Rendezvous(false, true);
	in.push_back(0x55);
	auto envelope = NatRendezvous::ParseRendezvousEnvelope(in.data(), in.size());
	ASSERT_TRUE(envelope.has_value());
	auto body = NatRendezvous::BuildForwardedRendezvous(envelope->request);
	ASSERT_EQUALS(in.size() - NatRendezvous::kHashSize - 1, body.size());

	// An element of a type we do not know.
	in = Rendezvous(true, true);
	in.insert(in.end(), { 0x7F, 0x01, 0x55 });
	envelope = NatRendezvous::ParseRendezvousEnvelope(in.data(), in.size());
	ASSERT_TRUE(envelope.has_value());
	body = NatRendezvous::BuildForwardedRendezvous(envelope->request);
	ASSERT_EQUALS(in.size() - NatRendezvous::kHashSize - 3, body.size());
}

namespace
{
const char *const kRequesterIPv6 = "2606:4700::1111";

// A 0x01 element: kRequesterIPv6, UDP port 4673 (little-endian), padded or cut to @a len.
Bytes Udp6Element(std::uint8_t len = NatRendezvous::kExtRequesterUdp6Size, std::uint8_t firstOctet = 0x26)
{
	Bytes element{ NatRendezvous::kExtRequesterUdp6, len, firstOctet, 0x06, 0x47, 0x00 };
	element.insert(element.end(), 10, 0);
	element.insert(element.end(), { 0x11, 0x11, 0x41, 0x12 });
	element.resize(2 + std::size_t(len), 0xEE);
	return element;
}

std::vector<std::uint8_t> Forward(const Bytes &in)
{
	const auto envelope = NatRendezvous::ParseRendezvousEnvelope(in.data(), in.size());
	return envelope ? NatRendezvous::BuildForwardedRendezvous(envelope->request) : Bytes{};
}

// What the buddy should forward: the envelope less the serving-buddy ID, then @a block.
Bytes Expected(const Bytes &envelope, const Bytes &block)
{
	Bytes body(envelope.begin() + NatRendezvous::kHashSize, envelope.end());
	body.insert(body.end(), block.begin(), block.end());
	return body;
}
} // namespace

TEST(NatRendezvousProtocol, ForwardedRendezvousCarriesTheRequesterIPv6Endpoint)
{
	Bytes in = Rendezvous(true, true);
	const Bytes element = Udp6Element();
	in.insert(in.end(), element.begin(), element.end());
	const auto envelope = NatRendezvous::ParseRendezvousEnvelope(in.data(), in.size());
	ASSERT_TRUE(envelope.has_value());
	ASSERT_TRUE(envelope->request.requesterUdp6.has_value());
	ASSERT_TRUE(envelope->request.requesterUdp6->address == CNetworkAddress::FromString(kRequesterIPv6));
	ASSERT_EQUALS(std::uint16_t(4673), envelope->request.requesterUdp6->port);

	const auto body = NatRendezvous::BuildForwardedRendezvous(envelope->request);
	ASSERT_TRUE(body == Expected(in, {}));
	const auto decision = Served(body);
	ASSERT_TRUE(decision.outcome == NatRendezvous::EServedRendezvous::Punch);
	ASSERT_TRUE(decision.request.requesterUdp6->address == CNetworkAddress::FromString(kRequesterIPv6));
}

// Unknown types are dropped, and a longer 0x01 is read for its first 18 bytes and sent as 18.
TEST(NatRendezvousProtocol, ForwardedRendezvousCarriesOnlyTheIPv6Element)
{
	Bytes in = Rendezvous(true, true);
	in.insert(in.end(), { 0x02, 0x03, 0xAA, 0xBB, 0xCC });
	const Bytes longer = Udp6Element(20);
	in.insert(in.end(), longer.begin(), longer.end());
	in.insert(in.end(), { 0x7F, 0x00 });
	ASSERT_TRUE(Forward(in) == Expected(Rendezvous(true, true), Udp6Element()));
}

// As eMuleQt's target reads it: the first 0x01 with a routable address and a port wins.
TEST(NatRendezvousProtocol, ForwardedRendezvousTakesTheFirstUsableIPv6Element)
{
	Bytes portZero = Udp6Element();
	portZero[portZero.size() - 2] = 0;
	portZero[portZero.size() - 1] = 0;
	for (const Bytes &unusable :
		{ Udp6Element(18, 0xFD), Udp6Element(18, 0xFE), Udp6Element(4), portZero }) {
		Bytes in = Rendezvous(true, true);
		in.insert(in.end(), unusable.begin(), unusable.end());
		ASSERT_TRUE(Forward(in) == Expected(Rendezvous(true, true), {}));

		const Bytes good = Udp6Element();
		in.insert(in.end(), good.begin(), good.end());
		ASSERT_TRUE(Forward(in) == Expected(Rendezvous(true, true), good));
	}

	Bytes twice = Rendezvous(true, true);
	const Bytes first = Udp6Element();
	Bytes second = Udp6Element();
	second[3] = 0x07;
	twice.insert(twice.end(), first.begin(), first.end());
	twice.insert(twice.end(), second.begin(), second.end());
	ASSERT_TRUE(Forward(twice) == Expected(Rendezvous(true, true), first));
}

// An element that runs past the end, or a lone byte left at the end, voids the whole block.
TEST(NatRendezvousProtocol, ForwardedRendezvousDropsABlockThatRunsPastItsEnd)
{
	const Bytes good = Udp6Element();
	for (const Bytes &bad : { Bytes{ 0x02, 0x05, 0x01, 0x02 }, Bytes{ 0x02 } }) {
		Bytes in = Rendezvous(true, true);
		in.insert(in.end(), good.begin(), good.end());
		in.insert(in.end(), bad.begin(), bad.end());
		const auto envelope = NatRendezvous::ParseRendezvousEnvelope(in.data(), in.size());
		ASSERT_TRUE(envelope.has_value());
		ASSERT_FALSE(envelope->request.requesterUdp6.has_value());
		ASSERT_TRUE(Forward(in) == Expected(Rendezvous(true, true), {}));
	}
}

// A requester that knows no public IPv4 sends a zero tail before the block; it is kept as zeros.
TEST(NatRendezvousProtocol, ForwardedRendezvousWritesAZeroTailBeforeTheIPv6Element)
{
	Bytes in = Rendezvous(false, true);
	in.insert(in.end(), NatRendezvous::kRendezvousTailSize, 0);
	const Bytes element = Udp6Element();
	in.insert(in.end(), element.begin(), element.end());
	const auto envelope = NatRendezvous::ParseRendezvousEnvelope(in.data(), in.size());
	ASSERT_TRUE(envelope.has_value());
	ASSERT_FALSE(envelope->request.requesterHint.has_value());
	ASSERT_EQUALS(std::uint8_t(0), envelope->request.transportHint);
	ASSERT_TRUE(NatRendezvous::BuildForwardedRendezvous(envelope->request) == Expected(in, {}));
}

// Our buddy forwards the transport byte of a tail whose address it drops, with a zero address.
TEST(NatRendezvousProtocol, ForwardedRendezvousKeepsTheTransportOfAnUnusableHint)
{
	Bytes in = Rendezvous(false, true);
	in[NatRendezvous::kRendezvousPrefixSize + 1 + NatRendezvous::kHashSize] = 0x20 | 0x01;
	in.insert(in.end(), { 192, 168, 1, 7, 0x40, 0x12, NatRendezvous::kTransportHintQuic });
	const auto envelope = NatRendezvous::ParseRendezvousEnvelope(in.data(), in.size());
	ASSERT_TRUE(envelope.has_value());
	ASSERT_FALSE(envelope->request.requesterHint.has_value());

	const auto body = NatRendezvous::BuildForwardedRendezvous(envelope->request);
	const Bytes tail(body.end() - 7, body.end());
	ASSERT_TRUE(tail == Bytes({ 0, 0, 0, 0, 0, 0, NatRendezvous::kTransportHintQuic }));
	const auto decision = Served(body);
	ASSERT_TRUE(decision.outcome == NatRendezvous::EServedRendezvous::Punch);
	ASSERT_EQUALS(std::uint8_t(0x40), decision.sharedTransports);
	ASSERT_FALSE(decision.request.requesterHint.has_value());
}

namespace
{
NatRendezvous::ServingBuddyId BuddyId(std::uint8_t first)
{
	NatRendezvous::ServingBuddyId id{};
	id.fill(0x33);
	id[0] = first;
	return id;
}

Hash Filled(std::uint8_t value)
{
	Hash hash{};
	hash.fill(value);
	return hash;
}
} // namespace

TEST(NatRendezvousProtocol, RequesterEnvelopeHasTheSketchedLayout)
{
	const Bytes envelope = NatRendezvous::BuildRendezvousEnvelope(
		BuddyId(0x44), Filled(0x11), 0xC2, Filled(0x22), 0x0100A8C0, 0x1235, 2);

	ASSERT_EQUALS(NatRendezvous::kRendezvousEnvelopeWithHintSize, envelope.size());
	ASSERT_EQUALS(std::uint8_t(0x44), envelope[0]);
	ASSERT_EQUALS(std::uint8_t(0x33), envelope[15]);
	for (std::size_t i = 16; i < 32; ++i) {
		ASSERT_EQUALS(std::uint8_t(0), envelope[i]);
	}
	ASSERT_EQUALS(NatRendezvous::OP_RENDEZVOUS, envelope[32]);
	ASSERT_EQUALS(std::uint8_t(0x11), envelope[33]);
	ASSERT_EQUALS(std::uint8_t(0x11), envelope[48]);
	ASSERT_EQUALS(std::uint8_t(0xC2), envelope[49]);
	ASSERT_EQUALS(std::uint8_t(0x22), envelope[50]);
	ASSERT_EQUALS(std::uint8_t(0x22), envelope[65]);
	// Our IPv4 as aMule stores it (192.168.0.1), then the UDP port low byte first, then the hint.
	ASSERT_EQUALS(std::uint8_t(192), envelope[66]);
	ASSERT_EQUALS(std::uint8_t(168), envelope[67]);
	ASSERT_EQUALS(std::uint8_t(0), envelope[68]);
	ASSERT_EQUALS(std::uint8_t(1), envelope[69]);
	ASSERT_EQUALS(std::uint8_t(0x35), envelope[70]);
	ASSERT_EQUALS(std::uint8_t(0x12), envelope[71]);
	ASSERT_EQUALS(std::uint8_t(2), envelope[72]);
}

TEST(NatRendezvousProtocol, RequesterEnvelopeAlwaysWritesItsTail)
{
	const Bytes envelope = NatRendezvous::BuildRendezvousEnvelope(
		BuddyId(0x44), Filled(0x11), 0x80, Filled(0x22), 0, 0, 1);

	ASSERT_EQUALS(NatRendezvous::kRendezvousEnvelopeWithHintSize, envelope.size());
	for (std::size_t i = 66; i < 72; ++i) {
		ASSERT_EQUALS(std::uint8_t(0), envelope[i]);
	}
	ASSERT_EQUALS(std::uint8_t(1), envelope[72]);
}

TEST(NatRendezvousProtocol, RequesterEnvelopeIsWhatOurBuddyParses)
{
	const Bytes envelope = NatRendezvous::BuildRendezvousEnvelope(
		BuddyId(0x44), Filled(0x11), 0xC2, Filled(0x22), 0, 0, 2);

	const auto parsed = NatRendezvous::ParseRendezvousEnvelope(envelope.data(), envelope.size());
	ASSERT_TRUE(parsed.has_value());
	ASSERT_TRUE(parsed->servingBuddyId == BuddyId(0x44));
	ASSERT_TRUE(parsed->request.requesterHash == Filled(0x11));
	ASSERT_EQUALS(std::uint8_t(0xC2), parsed->request.connectOptions);
	ASSERT_TRUE(parsed->request.fileHash.has_value());
	ASSERT_TRUE(*parsed->request.fileHash == Filled(0x22));
	// A zero endpoint is "unknown", so no hint reaches the served node.
	ASSERT_FALSE(parsed->request.requesterHint.has_value());
}

TEST(NatRendezvousProtocol, RequesterOptionsAddOurNatTAndClearTheEndpointHintBit)
{
	ASSERT_EQUALS(std::uint8_t(0xC3), NatRendezvous::RequesterConnectOptions(0x23, 0xC0));
	ASSERT_EQUALS(std::uint8_t(0x83), NatRendezvous::RequesterConnectOptions(0x03, 0x80));
	// Only the NAT-T bits of the local transports are added.
	ASSERT_EQUALS(std::uint8_t(0x01), NatRendezvous::RequesterConnectOptions(0x01, 0x0F));
}

TEST(NatRendezvousProtocol, RequesterHintIsQuicWhenWeServeQuicElseUtp)
{
	ASSERT_EQUALS(std::uint8_t(2), NatRendezvous::RequesterTransportHint(0xC0));
	ASSERT_EQUALS(std::uint8_t(2), NatRendezvous::RequesterTransportHint(0x40));
	ASSERT_EQUALS(std::uint8_t(1), NatRendezvous::RequesterTransportHint(0x80));
}
