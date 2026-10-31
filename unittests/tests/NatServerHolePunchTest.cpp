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

#include <muleunit/test.h>

#include "NatServerHolePunch.h"

#include <algorithm>

using namespace muleunit;
using namespace NatServerHolePunch;

DECLARE_SIMPLE(NatServerHolePunch)

TEST(NatServerHolePunch, BuildsExactPayloadsAndParsesFields)
{
	const Request request{ 0x12345678, 0xABCD };
	const std::array<std::uint8_t, 6> requestBytes{ { 0x78, 0x56, 0x34, 0x12, 0xCD, 0xAB } };
	ASSERT_TRUE(BuildRequest(request) == requestBytes);
	const auto parsedRequest = ParseRequest(requestBytes.data(), requestBytes.size());
	ASSERT_TRUE(parsedRequest.has_value());
	ASSERT_EQUALS(request.targetId, parsedRequest->targetId);
	ASSERT_EQUALS(request.udpPort, parsedRequest->udpPort);

	const Hash hash{ { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 } };
	const Info info{ { { 192, 0, 2, 1 } }, 0x1236, 0x5678, hash, 0xFE };
	const std::array<std::uint8_t, 25> infoBytes{ { 192,
		0,
		2,
		1,
		0x36,
		0x12,
		0x78,
		0x56,
		0,
		1,
		2,
		3,
		4,
		5,
		6,
		7,
		8,
		9,
		10,
		11,
		12,
		13,
		14,
		15,
		0xFE } };
	ASSERT_TRUE(BuildInfo(info) == infoBytes);
	const auto parsedInfo = ParseInfo(infoBytes.data(), infoBytes.size());
	ASSERT_TRUE(parsedInfo.has_value());
	ASSERT_TRUE(parsedInfo->peerIP == info.peerIP && parsedInfo->userHash == hash);
	ASSERT_EQUALS(info.tcpPort, parsedInfo->tcpPort);
	ASSERT_EQUALS(info.udpPort, parsedInfo->udpPort);
	ASSERT_EQUALS(info.role, parsedInfo->role);

	const Failure failure{ 0x89ABCDEF, 0xFF };
	const std::array<std::uint8_t, 5> failureBytes{ { 0xEF, 0xCD, 0xAB, 0x89, 0xFF } };
	ASSERT_TRUE(BuildFailure(failure) == failureBytes);
	const auto parsedFailure = ParseFailure(failureBytes.data(), failureBytes.size());
	ASSERT_TRUE(parsedFailure.has_value());
	ASSERT_EQUALS(failure.targetId, parsedFailure->targetId);
	ASSERT_EQUALS(failure.reason, parsedFailure->reason);
	ASSERT_TRUE(BuildKeepalive(hash) == hash);
	ASSERT_TRUE(ParseKeepalive(hash.data(), hash.size()).value() == hash);
}

TEST(NatServerHolePunch, AcceptsMinimumPayloadAndIgnoresTrailingBytes)
{
	const Request request{ 1, 2 };
	const auto requestBytes = BuildRequest(request);
	std::array<std::uint8_t, 7> requestExtended{};
	std::copy(requestBytes.begin(), requestBytes.end(), requestExtended.begin());
	ASSERT_TRUE(ParseRequest(requestExtended.data(), requestExtended.size()).has_value());
	ASSERT_FALSE(ParseRequest(requestBytes.data(), requestBytes.size() - 1).has_value());
	ASSERT_FALSE(ParseRequest(nullptr, requestBytes.size()).has_value());

	const Info info{ { { 192, 0, 2, 1 } }, 3, 4, {}, 0 };
	const auto infoBytes = BuildInfo(info);
	std::array<std::uint8_t, 26> infoExtended{};
	std::copy(infoBytes.begin(), infoBytes.end(), infoExtended.begin());
	ASSERT_TRUE(ParseInfo(infoExtended.data(), infoExtended.size()).has_value());
	ASSERT_FALSE(ParseInfo(infoBytes.data(), infoBytes.size() - 1).has_value());
	ASSERT_FALSE(ParseInfo(nullptr, infoBytes.size()).has_value());

	const auto failureBytes = BuildFailure({ 5, 0xA7 });
	std::array<std::uint8_t, 6> failureExtended{};
	std::copy(failureBytes.begin(), failureBytes.end(), failureExtended.begin());
	ASSERT_TRUE(ParseFailure(failureExtended.data(), failureExtended.size()).has_value());
	ASSERT_FALSE(ParseFailure(failureBytes.data(), failureBytes.size() - 1).has_value());
	ASSERT_FALSE(ParseFailure(nullptr, failureBytes.size()).has_value());

	const Hash hash{ { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 } };
	std::array<std::uint8_t, 17> keepaliveExtended{};
	std::copy(hash.begin(), hash.end(), keepaliveExtended.begin());
	ASSERT_TRUE(ParseKeepalive(keepaliveExtended.data(), keepaliveExtended.size()).has_value());
	ASSERT_FALSE(ParseKeepalive(hash.data(), hash.size() - 1).has_value());
	ASSERT_FALSE(ParseKeepalive(nullptr, hash.size()).has_value());
}

TEST(NatServerHolePunch, PreservesUnknownReasonsAndOpcodeNamespaces)
{
	for (unsigned reason = 0; reason < 256; ++reason) {
		const auto bytes = BuildFailure({ 0, static_cast<std::uint8_t>(reason) });
		const auto parsed = ParseFailure(bytes.data(), bytes.size());
		ASSERT_TRUE(parsed.has_value());
		ASSERT_EQUALS(reason, parsed->reason);
	}
	ASSERT_TRUE(IsServerTcpOpcode(0x60));
	ASSERT_TRUE(IsServerUdpOpcode(0x9F));
	ASSERT_TRUE(IsPeerUdpOpcode(0xB3));
	ASSERT_FALSE(IsServerUdpOpcode(0xE3));
	ASSERT_FALSE(IsServerTcpOpcode(0xB3));
}

// ed2k-server 0.9.84 announces OP_LOWID_HOLEPUNCH_REQUEST with 0x20000 and always sets it
// (flags 0x205DD). Lugdunum's 0x1000 and the IPv6 bits must not be read as that support.
TEST(NatServerHolePunch, OnlyAServerWithTheHolePunchFlagAcceptsRequests)
{
	ASSERT_TRUE(ServerAcceptsHolePunchRequest(0x000205DDu));
	ASSERT_TRUE(ServerAcceptsHolePunchRequest(0x00020000u));
	ASSERT_FALSE(ServerAcceptsHolePunchRequest(0x000005DDu));
	ASSERT_FALSE(ServerAcceptsHolePunchRequest(0x000017F9u));
	ASSERT_FALSE(ServerAcceptsHolePunchRequest(0x0000C5DDu));
	ASSERT_FALSE(ServerAcceptsHolePunchRequest(0));
}

TEST(NatServerHolePunch, BuildsTheUdpPortsLoginValueFromBothPorts)
{
	ASSERT_EQUALS((std::uint32_t)0x04D20539, BuildUdpPortsLoginValue(0x04D2, 0x0539));
	ASSERT_EQUALS((std::uint32_t)0, BuildUdpPortsLoginValue(0, 0));
	// aMule shares one UDP socket for Kad and the client protocol: both arguments are usually
	// equal in practice, which must not collapse the two fields into one.
	ASSERT_EQUALS((std::uint32_t)0x12340000, BuildUdpPortsLoginValue(0x1234, 0));
	ASSERT_EQUALS((std::uint32_t)0x00005678, BuildUdpPortsLoginValue(0, 0x5678));
}

TEST(NatServerHolePunch, SendsKeepaliveOnlyWhileConnectedToACapableServerWithTheSettingOn)
{
	KeepaliveFacts facts;
	facts.settingEnabled = true;
	facts.connected = true;
	facts.serverAnnouncesHolePunch = true;
	facts.weAreLowId = true;
	facts.udpPort = 4672;
	facts.utpAvailable = true;
	ASSERT_TRUE(ShouldSendKeepalive(facts));

	KeepaliveFacts withoutUtp = facts;
	withoutUtp.utpAvailable = false;
	ASSERT_FALSE(ShouldSendKeepalive(withoutUtp));

	KeepaliveFacts settingOff = facts;
	settingOff.settingEnabled = false;
	ASSERT_FALSE(ShouldSendKeepalive(settingOff));

	KeepaliveFacts notConnected = facts;
	notConnected.connected = false;
	ASSERT_FALSE(ShouldSendKeepalive(notConnected));

	KeepaliveFacts notCapable = facts;
	notCapable.serverAnnouncesHolePunch = false;
	ASSERT_FALSE(ShouldSendKeepalive(notCapable));

	// ed2k-server refuses a HighID target (FAIL_TARGET_IS_HIGHID): a HighID client gains
	// nothing from sending the keepalive.
	KeepaliveFacts highId = facts;
	highId.weAreLowId = false;
	ASSERT_FALSE(ShouldSendKeepalive(highId));

	// GetEffectiveUDPPort() returns 0 while UDP is disabled: the keepalive would go out from
	// a disabled socket.
	KeepaliveFacts udpDisabled = facts;
	udpDisabled.udpPort = 0;
	ASSERT_FALSE(ShouldSendKeepalive(udpDisabled));
}

TEST(NatServerHolePunch, RequestsHolePunchOnlyWhenEveryPreconditionHolds)
{
	RequestFacts facts;
	facts.ourId = 100;    // LowID
	facts.sourceId = 200; // LowID
	facts.sameServer = true;
	facts.serverAnnouncesHolePunch = true;
	facts.settingEnabled = true;
	facts.ourUdpPort = 4672;
	facts.utpAvailable = true;
	ASSERT_TRUE(ShouldRequestLowIdHolePunch(facts));

	RequestFacts withoutUtp = facts;
	withoutUtp.utpAvailable = false;
	ASSERT_FALSE(ShouldRequestLowIdHolePunch(withoutUtp));

	// GetEffectiveUDPPort() returns 0 while UDP is disabled: ed2k-server would refuse a
	// target with no known UDP port.
	RequestFacts udpDisabled = facts;
	udpDisabled.ourUdpPort = 0;
	ASSERT_FALSE(ShouldRequestLowIdHolePunch(udpDisabled));

	RequestFacts settingOff = facts;
	settingOff.settingEnabled = false;
	ASSERT_FALSE(ShouldRequestLowIdHolePunch(settingOff));

	RequestFacts differentServer = facts;
	differentServer.sameServer = false;
	ASSERT_FALSE(ShouldRequestLowIdHolePunch(differentServer));

	RequestFacts notCapable = facts;
	notCapable.serverAnnouncesHolePunch = false;
	ASSERT_FALSE(ShouldRequestLowIdHolePunch(notCapable));

	RequestFacts highIdSource = facts;
	highIdSource.sourceId = 20000000;
	ASSERT_FALSE(ShouldRequestLowIdHolePunch(highIdSource));

	RequestFacts highIdUs = facts;
	highIdUs.ourId = 20000000;
	ASSERT_FALSE(ShouldRequestLowIdHolePunch(highIdUs));
}

TEST(NatServerHolePunch, OnlyAdvertisesUdpPortsWhenUtpCanServeThePunch)
{
	ASSERT_TRUE(ShouldAdvertiseUdpPorts(true, true));
	ASSERT_FALSE(ShouldAdvertiseUdpPorts(true, false));
	ASSERT_FALSE(ShouldAdvertiseUdpPorts(false, true));
}

TEST(NatServerHolePunch, ReadsThePeerIdOnlyFromAnInfoOf29BytesOrMore)
{
	std::array<std::uint8_t, 29> bytes{};
	const Info info{ { { 192, 0, 2, 1 } }, 3, 4, {}, 0 };
	const auto base = BuildInfo(info);
	std::copy(base.begin(), base.end(), bytes.begin());
	bytes[25] = 0x78;
	bytes[26] = 0x56;
	bytes[27] = 0x34;
	bytes[28] = 0x12;
	const auto withId = ParseInfo(bytes.data(), bytes.size());
	ASSERT_TRUE(withId.has_value());
	ASSERT_TRUE(withId->peerId.has_value());
	ASSERT_EQUALS((std::uint32_t)0x12345678, *withId->peerId);

	std::array<std::uint8_t, 30> longer{};
	std::copy(bytes.begin(), bytes.end(), longer.begin());
	ASSERT_EQUALS((std::uint32_t)0x12345678, *ParseInfo(longer.data(), longer.size())->peerId);

	// A 25-byte frame is a valid INFO that simply has no peer ID.
	const auto withoutId = ParseInfo(bytes.data(), 25);
	ASSERT_TRUE(withoutId.has_value());
	ASSERT_FALSE(withoutId->peerId.has_value());
	ASSERT_FALSE(ParseInfo(bytes.data(), 28)->peerId.has_value());
}

TEST(NatServerHolePunch, ReadsThePeerCryptOptionsOnlyFromAnInfoOf30BytesOrMore)
{
	std::array<std::uint8_t, 31> bytes{};
	const Info info{ { { 192, 0, 2, 1 } }, 3, 4, {}, 0 };
	const auto base = BuildInfo(info);
	std::copy(base.begin(), base.end(), bytes.begin());
	bytes[29] = 0x05; // support | require

	const auto withCrypt = ParseInfo(bytes.data(), 30);
	ASSERT_TRUE(withCrypt.has_value());
	ASSERT_TRUE(withCrypt->peerCrypt.has_value());
	ASSERT_EQUALS(0x05, static_cast<int>(*withCrypt->peerCrypt));
	ASSERT_EQUALS(0x05, static_cast<int>(*ParseInfo(bytes.data(), bytes.size())->peerCrypt));

	// Today's 29-byte INFO, and the legacy 25-byte one, carry no crypt options.
	ASSERT_FALSE(ParseInfo(bytes.data(), 29)->peerCrypt.has_value());
	ASSERT_FALSE(ParseInfo(bytes.data(), 25)->peerCrypt.has_value());
}

TEST(NatServerHolePunch, SeveralRequestsCanBePendingAndEachIsTakenOnce)
{
	PendingRequestSet requests;
	ASSERT_TRUE(requests.CanSend(42, 100));
	requests.OnSent(42, 100);
	ASSERT_TRUE(requests.CanSend(43, 100));
	requests.OnSent(43, 100);
	// The same target is not asked twice while its request is pending.
	ASSERT_FALSE(requests.CanSend(42, 101));

	ASSERT_FALSE(requests.Take(44, 101));
	ASSERT_TRUE(requests.Take(42, 101));
	ASSERT_FALSE(requests.Take(42, 101));
	ASSERT_TRUE(requests.CanSend(42, 101));
	ASSERT_TRUE(requests.Take(43, 101));

	requests.OnSent(50, 200);
	requests.Release(51);
	ASSERT_FALSE(requests.CanSend(50, 200));
	requests.Release(50);
	ASSERT_TRUE(requests.CanSend(50, 200));
}

TEST(NatServerHolePunch, PendingRequestsAreBoundedAndExpire)
{
	PendingRequestSet requests;
	for (std::uint32_t id = 1; id <= kMaxPendingRequests; ++id) {
		ASSERT_TRUE(requests.CanSend(id, 0));
		requests.OnSent(id, 0);
	}
	ASSERT_FALSE(requests.CanSend(1000, kPendingRequestTimeoutMs - 1));
	ASSERT_FALSE(requests.Take(1, kPendingRequestTimeoutMs));
	ASSERT_TRUE(requests.CanSend(1000, kPendingRequestTimeoutMs));
}

TEST(NatServerHolePunch, AnExpectedEndpointHandsItsVouchedPeerToTheDial)
{
	ExpectedEndpointRegistry endpoints;
	const IPv4 peer{ { 192, 0, 2, 1 } };
	VouchedPeer vouched;
	vouched.userHash.fill(0x11);
	vouched.cryptOptions = 0x03;

	endpoints.Register(peer, 4662, 1, vouched);
	const PunchAction later = endpoints.Take(peer, 5000, 2);
	ASSERT_TRUE(later.kind == PunchAction::DialLater);
	ASSERT_TRUE(later.peer.userHash == vouched.userHash);
	ASSERT_TRUE(later.peer.cryptOptions == vouched.cryptOptions);

	endpoints.Register(peer, 4662, 3, vouched);
	endpoints.NoteDialled(peer, 5000);
	const PunchAction now = endpoints.Take(peer, 5001, 4);
	ASSERT_TRUE(now.kind == PunchAction::DialNow);
	ASSERT_TRUE(now.peer.userHash == vouched.userHash);
	ASSERT_TRUE(now.peer.cryptOptions == vouched.cryptOptions);

	// Without a vouched peer the dial stays as before: no hash, no crypt options.
	endpoints.Register(peer, 4662, 5);
	const PunchAction bare = endpoints.Take(peer, 5000, 6);
	ASSERT_TRUE(bare.peer.userHash == Hash{});
	ASSERT_FALSE(bare.peer.cryptOptions.has_value());
}

TEST(NatServerHolePunch, ADialBackKeepsItsVouchedPeer)
{
	DialBackSchedule dialBacks;
	const IPv4 peer{ { 192, 0, 2, 1 } };
	VouchedPeer vouched;
	vouched.userHash.fill(0x22);
	vouched.cryptOptions = 0x01;
	dialBacks.Schedule(peer, 4662, 5000, 10, vouched);
	const auto due = dialBacks.TakeDue(10 + kDialBackDelayMs);
	ASSERT_EQUALS((std::size_t)1, due.size());
	ASSERT_TRUE(due[0].peer.userHash == vouched.userHash);
	ASSERT_TRUE(due[0].peer.cryptOptions == vouched.cryptOptions);
}

TEST(NatServerHolePunch, DuplicateInfoAndActionCapBoundServerDirectedTraffic)
{
	DuplicateInfoSuppressor duplicates;
	const Hash hash{ { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 } };
	ASSERT_TRUE(duplicates.ShouldProcess(hash, 0));
	ASSERT_FALSE(duplicates.ShouldProcess(hash, kDuplicateInfoWindowMs - 1));
	ASSERT_TRUE(duplicates.ShouldProcess(hash, kDuplicateInfoWindowMs));
	ActionRateLimiter actions;
	for (std::size_t i = 0; i < kMaxActionsPerWindow; ++i) {
		ASSERT_TRUE(actions.Admit(0));
	}
	ASSERT_FALSE(actions.Admit(kActionWindowMs - 1));
	ASSERT_TRUE(actions.Admit(kActionWindowMs));
}

TEST(NatServerHolePunch, ExpectsOnlyRegisteredPunchAddressesWithinTtl)
{
	ExpectedEndpointRegistry endpoints;
	const IPv4 peer{ { 192, 0, 2, 1 } };
	const IPv4 other{ { 192, 0, 2, 2 } };
	ASSERT_TRUE(endpoints.Take(peer, 5000, 0).kind == PunchAction::None);
	endpoints.Register(peer, 4662, 1);
	ASSERT_TRUE(endpoints.Take(other, 5000, 1).kind == PunchAction::None);
	const PunchAction first = endpoints.Take(peer, 5000, 1);
	ASSERT_TRUE(first.kind == PunchAction::DialLater);
	ASSERT_EQUALS((std::uint16_t)4662, first.tcpPort);
	ASSERT_TRUE(endpoints.Take(peer, 5000, 1).kind == PunchAction::None);
	endpoints.Register(peer, 4662, 2);
	ASSERT_TRUE(endpoints.Take(peer, 5000, 2 + kExpectedEndpointTtlMs).kind == PunchAction::None);
	endpoints.Register(peer, 0, 3 + kExpectedEndpointTtlMs);
	ASSERT_TRUE(endpoints.Take(peer, 5000, 3 + kExpectedEndpointTtlMs).kind == PunchAction::None);
}

TEST(NatServerHolePunch, ADialInFlightToThePunchSourceIsNotRepeated)
{
	ExpectedEndpointRegistry endpoints;
	const IPv4 peer{ { 192, 0, 2, 1 } };
	endpoints.Register(peer, 4662, 1);
	endpoints.NoteDialled(peer, 5000);
	ASSERT_TRUE(endpoints.Take(peer, 5000, 2).kind == PunchAction::None);
	// The punch consumed the entry: a later one from the same address is unexpected.
	ASSERT_TRUE(endpoints.Take(peer, 5001, 2).kind == PunchAction::None);

	// A punch from another port is a different NAT mapping, so it earns its own dial now.
	endpoints.Register(peer, 4662, 3);
	endpoints.NoteDialled(peer, 5000);
	const PunchAction other = endpoints.Take(peer, 5001, 4);
	ASSERT_TRUE(other.kind == PunchAction::DialNow);
	ASSERT_EQUALS((std::uint16_t)4662, other.tcpPort);

	// An endpoint nobody dialled waits for a stream from the peer before dialling back.
	endpoints.Register(peer, 4662, 5);
	ASSERT_TRUE(endpoints.Take(peer, 5000, 6).kind == PunchAction::DialLater);

	// Noting a dial for an address that was never registered changes nothing.
	endpoints.NoteDialled(peer, 5000);
	ASSERT_TRUE(endpoints.Take(peer, 5000, 7).kind == PunchAction::None);
}

TEST(NatServerHolePunch, DialBacksWaitForTheirDelayAndAreTakenOnce)
{
	DialBackSchedule dialBacks;
	const IPv4 peer{ { 192, 0, 2, 1 } };
	const IPv4 other{ { 192, 0, 2, 2 } };
	ASSERT_TRUE(dialBacks.TakeDue(0).empty());
	dialBacks.Schedule(peer, 4662, 5000, 10);
	dialBacks.Schedule(other, 4663, 5001, 11);
	ASSERT_TRUE(dialBacks.TakeDue(10 + kDialBackDelayMs - 1).empty());
	auto due = dialBacks.TakeDue(10 + kDialBackDelayMs);
	ASSERT_EQUALS((std::size_t)1, due.size());
	ASSERT_TRUE(due[0].peerIP == peer);
	ASSERT_EQUALS((std::uint16_t)4662, due[0].tcpPort);
	ASSERT_EQUALS((std::uint16_t)5000, due[0].udpPort);
	due = dialBacks.TakeDue(11 + kDialBackDelayMs);
	ASSERT_EQUALS((std::size_t)1, due.size());
	ASSERT_TRUE(due[0].peerIP == other);
	ASSERT_TRUE(dialBacks.TakeDue(100000).empty());

	// A newer punch from the same address replaces the one still waiting.
	dialBacks.Schedule(peer, 4662, 5000, 200);
	dialBacks.Schedule(peer, 4662, 5002, 201);
	due = dialBacks.TakeDue(201 + kDialBackDelayMs);
	ASSERT_EQUALS((std::size_t)1, due.size());
	ASSERT_EQUALS((std::uint16_t)5002, due[0].udpPort);

	for (std::size_t i = 0; i <= kMaxDialBacks; ++i) {
		dialBacks.Schedule({ { 198, 51, 100, static_cast<std::uint8_t>(i) } }, 4662, 5000, 300);
	}
	ASSERT_EQUALS(kMaxDialBacks, dialBacks.TakeDue(300 + kDialBackDelayMs).size());
}

TEST(NatServerHolePunch, KeepsLowIdServerSourcesOnlyFromOurOwnCapableServer)
{
	LowIdServerSourceFacts facts;
	facts.originIsLocalServer = true;
	facts.sourceServerIsOurCurrentServer = true;
	facts.weAreLowId = true;
	facts.serverAnnouncesHolePunch = true;
	facts.settingEnabled = true;
	ASSERT_TRUE(ShouldKeepLowIdServerSource(facts));

	LowIdServerSourceFacts settingOff = facts;
	settingOff.settingEnabled = false;
	ASSERT_FALSE(ShouldKeepLowIdServerSource(settingOff));

	LowIdServerSourceFacts notLocalServer = facts;
	notLocalServer.originIsLocalServer = false;
	ASSERT_FALSE(ShouldKeepLowIdServerSource(notLocalServer));

	LowIdServerSourceFacts notOurServer = facts;
	notOurServer.sourceServerIsOurCurrentServer = false;
	ASSERT_FALSE(ShouldKeepLowIdServerSource(notOurServer));

	LowIdServerSourceFacts notLowId = facts;
	notLowId.weAreLowId = false;
	ASSERT_FALSE(ShouldKeepLowIdServerSource(notLowId));

	LowIdServerSourceFacts notCapable = facts;
	notCapable.serverAnnouncesHolePunch = false;
	ASSERT_FALSE(ShouldKeepLowIdServerSource(notCapable));
}

TEST(NatServerHolePunch, KeepsALowIdSourceOfAnotherServerThatTakesCrossServerRequests)
{
	LowIdServerSourceFacts facts;
	facts.weAreLowId = true;
	facts.extensionAnnounced = true;
	facts.sourceServerTakesForeignCallback = true;
	// From source exchange or another server's answer: neither origin nor server is ours.
	ASSERT_TRUE(ShouldKeepLowIdServerSource(facts));

	LowIdServerSourceFacts settingOff = facts;
	settingOff.extensionAnnounced = false;
	ASSERT_FALSE(ShouldKeepLowIdServerSource(settingOff));

	LowIdServerSourceFacts serverDoesNotTakeThem = facts;
	serverDoesNotTakeThem.sourceServerTakesForeignCallback = false;
	ASSERT_FALSE(ShouldKeepLowIdServerSource(serverDoesNotTakeThem));

	LowIdServerSourceFacts weAreHighId = facts;
	weAreHighId.weAreLowId = false;
	ASSERT_FALSE(ShouldKeepLowIdServerSource(weAreHighId));

	// A source on our own server is the same-server rule's alone.
	LowIdServerSourceFacts ourServer = facts;
	ourServer.sourceServerIsOurCurrentServer = true;
	ASSERT_FALSE(ShouldKeepLowIdServerSource(ourServer));
}

TEST(NatServerHolePunch, AServerTakesCrossServerRequestsOnlyWithItsFlagAndAKey)
{
	ForeignServerFacts facts;
	facts.inServerList = true;
	facts.udpFlags = 0x100000 | 0x200;
	facts.serverKeyUdp = 42;
	facts.supportsObfuscationUdp = true;
	facts.cryptLayerUdpEnabled = true;
	ASSERT_TRUE(ServerTakesForeignCallback(facts));

	// A source's server address is learnt from a peer: only one we list ourselves is asked.
	ForeignServerFacts notListed = facts;
	notListed.inServerList = false;
	ASSERT_FALSE(ServerTakesForeignCallback(notListed));

	ForeignServerFacts noFlag = facts;
	noFlag.udpFlags = 0x200;
	ASSERT_FALSE(ServerTakesForeignCallback(noFlag));

	ForeignServerFacts noKey = facts;
	noKey.serverKeyUdp = 0;
	ASSERT_FALSE(ServerTakesForeignCallback(noKey));

	ForeignServerFacts noObfuscation = facts;
	noObfuscation.supportsObfuscationUdp = false;
	ASSERT_FALSE(ServerTakesForeignCallback(noObfuscation));

	ForeignServerFacts cryptOff = facts;
	cryptOff.cryptLayerUdpEnabled = false;
	ASSERT_FALSE(ServerTakesForeignCallback(cryptOff));
}

TEST(NatServerHolePunch, OnlyAServerWithTheExtensionFlagOffersTheExtension)
{
	ASSERT_TRUE(ServerOffersHolePunchExtension(0x100000));
	ASSERT_TRUE(ServerOffersHolePunchExtension(0x100000 | 0x20000 | 0x1000));
	ASSERT_FALSE(ServerOffersHolePunchExtension(0x20000 | 0x1000));
	ASSERT_FALSE(ServerOffersHolePunchExtension(0));
}

TEST(NatServerHolePunch, AnnouncesTheExtensionOnlyWithBothServerPathsAndUtp)
{
	ASSERT_TRUE(ShouldAnnounceHolePunchExtension(true, true, true));
	// The extension wakes the target with Lugdunum's 0x37, which only that path serves.
	ASSERT_FALSE(ShouldAnnounceHolePunchExtension(true, false, true));
	ASSERT_FALSE(ShouldAnnounceHolePunchExtension(false, true, true));
	ASSERT_FALSE(ShouldAnnounceHolePunchExtension(true, true, false));
}

TEST(NatServerHolePunch, ObfuscatesTheKeepaliveOnlyOnAnExtensionServerWithItsUdpKey)
{
	KeepaliveCryptFacts facts;
	facts.extensionActive = true;
	facts.cryptLayerUdpEnabled = true;
	facts.serverKeyUdp = 42;
	facts.serverSupportsObfuscationUdp = true;
	ASSERT_TRUE(ShouldObfuscateKeepalive(facts));

	// An older ed2k-server reads only a plain 0x9F.
	KeepaliveCryptFacts noExtension = facts;
	noExtension.extensionActive = false;
	ASSERT_FALSE(ShouldObfuscateKeepalive(noExtension));

	// Without the key it stays plain, which the server still accepts until a first obfuscated one.
	KeepaliveCryptFacts noKey = facts;
	noKey.serverKeyUdp = 0;
	ASSERT_FALSE(ShouldObfuscateKeepalive(noKey));
	KeepaliveCryptFacts noCryptLayer = facts;
	noCryptLayer.cryptLayerUdpEnabled = false;
	ASSERT_FALSE(ShouldObfuscateKeepalive(noCryptLayer));
	KeepaliveCryptFacts noServerObfuscation = facts;
	noServerObfuscation.serverSupportsObfuscationUdp = false;
	ASSERT_FALSE(ShouldObfuscateKeepalive(noServerObfuscation));
}

TEST(NatServerHolePunch, AWakeSendsThreeKeepalivesHalfASecondApart)
{
	KeepaliveBurst burst;
	ASSERT_EQUALS((std::size_t)0, burst.TakeDue(0));
	burst.Start(100);
	ASSERT_EQUALS((std::size_t)1, burst.TakeDue(100));
	ASSERT_EQUALS((std::size_t)0, burst.TakeDue(599));
	ASSERT_EQUALS((std::size_t)1, burst.TakeDue(600));
	ASSERT_EQUALS((std::size_t)1, burst.TakeDue(1100));
	ASSERT_EQUALS((std::size_t)0, burst.TakeDue(5000));

	// A late tick sends whatever is due at once, never more than was scheduled.
	burst.Start(10000);
	ASSERT_EQUALS((std::size_t)3, burst.TakeDue(20000));
}

TEST(NatServerHolePunch, KeepaliveBurstsStayBounded)
{
	KeepaliveBurst burst;
	for (int i = 0; i < 100; ++i) {
		burst.Start(0);
	}
	ASSERT_TRUE(burst.TakeDue(10000) <= kMaxPendingBurstKeepalives);
}

TEST(NatServerHolePunch, AForeignWakeWaitsToBeDialledAndIsNeverHandedBack)
{
	// Requester ID 0 on an extension server is a cross-server request: no 0x61 follows, and the
	// target waits for the requester's dial instead of falling back to its own.
	PendingWakeSet<int> wakes;
	wakes.Add(0, 1, 0);
	ASSERT_TRUE(wakes.TakeExpired(kWakeInfoTimeoutMs).empty());
	ASSERT_FALSE(wakes.Resolve(0));
}

TEST(NatServerHolePunch, AWakeIsResolvedByTheInfoThatNamesItsRequester)
{
	PendingWakeSet<int> wakes;
	wakes.Add(41, 1, 0);
	wakes.Add(43, 3, 0);
	ASSERT_TRUE(wakes.Resolve(41));
	ASSERT_FALSE(wakes.Resolve(41));
	ASSERT_FALSE(wakes.Resolve(45));
	const std::vector<int> expired = wakes.TakeExpired(kWakeInfoTimeoutMs);
	ASSERT_EQUALS((std::size_t)1, expired.size());
	ASSERT_EQUALS(3, expired[0]);
}

TEST(NatServerHolePunch, AnUnansweredWakeExpiresOnceAfterItsTimeout)
{
	PendingWakeSet<int> wakes;
	wakes.Add(41, 1, 10);
	ASSERT_TRUE(wakes.TakeExpired(10 + kWakeInfoTimeoutMs - 1).empty());
	ASSERT_EQUALS((std::size_t)1, wakes.TakeExpired(10 + kWakeInfoTimeoutMs).size());
	ASSERT_TRUE(wakes.TakeExpired(10 + 2 * kWakeInfoTimeoutMs).empty());

	// A repeated wake for the same requester replaces the earlier one and restarts its wait.
	wakes.Add(41, 1, 100);
	wakes.Add(41, 2, 200);
	ASSERT_TRUE(wakes.TakeExpired(100 + kWakeInfoTimeoutMs).empty());
	const std::vector<int> latest = wakes.TakeExpired(200 + kWakeInfoTimeoutMs);
	ASSERT_EQUALS((std::size_t)1, latest.size());
	ASSERT_EQUALS(2, latest[0]);
}

TEST(NatServerHolePunch, PendingWakesStayBounded)
{
	PendingWakeSet<int> wakes;
	for (std::uint32_t id = 1; id <= 100; ++id) {
		wakes.Add(id, static_cast<int>(id), 0);
	}
	ASSERT_TRUE(wakes.TakeExpired(kWakeInfoTimeoutMs).size() <= kMaxPendingWakes);
}

namespace
{
Hash FilledHash(std::uint8_t value)
{
	Hash hash;
	hash.fill(value);
	return hash;
}
} // namespace

TEST(NatServerHolePunch, EachRendezvousCountsOneAttemptPerPathAndPeer)
{
	RendezvousOutcomes outcomes;
	ASSERT_TRUE(outcomes.NoteAttempt(RendezvousPath::ServerPunch, 1, FilledHash(1), 0));
	// A resend or a second notice for the same peer is the same rendezvous.
	ASSERT_FALSE(outcomes.NoteAttempt(RendezvousPath::ServerPunch, 1, FilledHash(1), 10));
	ASSERT_TRUE(outcomes.NoteAttempt(RendezvousPath::ServerPunch, 2, FilledHash(2), 10));
	ASSERT_TRUE(outcomes.NoteAttempt(RendezvousPath::LugdunumCallback, 1, std::nullopt, 10));
}

TEST(NatServerHolePunch, AHelloFromTheRendezvousPeerIsOneSuccessOnItsPath)
{
	RendezvousOutcomes outcomes;
	outcomes.NoteAttempt(RendezvousPath::ServerPunch, 1, FilledHash(1), 0);
	ASSERT_FALSE(outcomes.TakeSuccess(2, FilledHash(1), 10).has_value());
	const std::optional<RendezvousPath> path = outcomes.TakeSuccess(1, FilledHash(1), 10);
	ASSERT_TRUE(path == RendezvousPath::ServerPunch);
	// A second stream to the same peer is not another success.
	ASSERT_FALSE(outcomes.TakeSuccess(1, FilledHash(1), 20).has_value());
	// And the next rendezvous with it counts again.
	ASSERT_TRUE(outcomes.NoteAttempt(RendezvousPath::ServerPunch, 1, FilledHash(1), 30));
}

TEST(NatServerHolePunch, AHelloWithAnotherHashIsNotTheRendezvousPeer)
{
	RendezvousOutcomes outcomes;
	outcomes.NoteAttempt(RendezvousPath::ServerPunch, 1, FilledHash(1), 0);
	ASSERT_FALSE(outcomes.TakeSuccess(1, FilledHash(9), 10).has_value());
	ASSERT_TRUE(outcomes.TakeSuccess(1, FilledHash(1), 10) == RendezvousPath::ServerPunch);
}

TEST(NatServerHolePunch, ARendezvousWithoutAHashMatchesByAddress)
{
	RendezvousOutcomes outcomes;
	outcomes.NoteAttempt(RendezvousPath::LugdunumCallback, 1, std::nullopt, 0);
	ASSERT_TRUE(outcomes.TakeSuccess(1, FilledHash(7), 10) == RendezvousPath::LugdunumCallback);
}

TEST(NatServerHolePunch, AnUnansweredRendezvousExpires)
{
	RendezvousOutcomes outcomes;
	outcomes.NoteAttempt(RendezvousPath::ServerPunch, 1, FilledHash(1), 0);
	ASSERT_FALSE(outcomes.TakeSuccess(1, FilledHash(1), kRendezvousOutcomeTtlMs).has_value());
	ASSERT_TRUE(
		outcomes.NoteAttempt(RendezvousPath::ServerPunch, 1, FilledHash(1), kRendezvousOutcomeTtlMs));
}

TEST(NatServerHolePunch, PendingRendezvousStayBounded)
{
	RendezvousOutcomes outcomes;
	for (std::uint32_t ip = 1; ip <= 1000; ++ip) {
		outcomes.NoteAttempt(RendezvousPath::ServerPunch, ip, std::nullopt, 0);
	}
	ASSERT_TRUE(outcomes.Size() <= kMaxPendingRendezvous);
	ASSERT_TRUE(outcomes.TakeSuccess(1000, FilledHash(1), 1) == RendezvousPath::ServerPunch);
}

TEST(NatServerHolePunch, TwoPeersBehindOneAddressAreTwoRendezvous)
{
	RendezvousOutcomes outcomes;
	ASSERT_TRUE(outcomes.NoteAttempt(RendezvousPath::ServerPunch, 1, FilledHash(1), 0));
	ASSERT_TRUE(outcomes.NoteAttempt(RendezvousPath::ServerPunch, 1, FilledHash(2), 0));
	ASSERT_TRUE(outcomes.TakeSuccess(1, FilledHash(2), 10) == RendezvousPath::ServerPunch);
	ASSERT_TRUE(outcomes.TakeSuccess(1, FilledHash(1), 10) == RendezvousPath::ServerPunch);
	ASSERT_FALSE(outcomes.TakeSuccess(1, FilledHash(1), 10).has_value());
}

TEST(NatServerHolePunch, AHelloPrefersTheRendezvousThatNamedItsHash)
{
	RendezvousOutcomes outcomes;
	outcomes.NoteAttempt(RendezvousPath::LugdunumCallback, 1, std::nullopt, 0);
	outcomes.NoteAttempt(RendezvousPath::ServerPunch, 1, FilledHash(2), 0);
	ASSERT_TRUE(outcomes.TakeSuccess(1, FilledHash(2), 10) == RendezvousPath::ServerPunch);
	ASSERT_TRUE(outcomes.TakeSuccess(1, FilledHash(3), 10) == RendezvousPath::LugdunumCallback);
}

TEST(NatServerHolePunch, ACallbackRequestIsAnsweredOnceWithinItsWindow)
{
	PendingCallbackRequest request;
	ASSERT_FALSE(request.TakeAnswered(0));
	request.NoteSent(100);
	ASSERT_TRUE(request.TakeAnswered(100 + kRendezvousOutcomeTtlMs - 1));
	ASSERT_FALSE(request.TakeAnswered(100 + kRendezvousOutcomeTtlMs - 1));

	request.NoteSent(1000);
	ASSERT_FALSE(request.TakeAnswered(1000 + kRendezvousOutcomeTtlMs));
}

TEST(NatServerHolePunch, ACallbackFromATargetWeAskedAboutIsTheReverseOfOurRequest)
{
	RequestedCallbackTargets targets;
	targets.Note(7, 0);
	ASSERT_FALSE(targets.Take(8, 10));
	ASSERT_TRUE(targets.Take(7, 10));
	// Taken once: a second callback from it is a new rendezvous.
	ASSERT_FALSE(targets.Take(7, 20));
}

TEST(NatServerHolePunch, ARequestedTargetExpires)
{
	RequestedCallbackTargets targets;
	targets.Note(7, 0);
	ASSERT_FALSE(targets.Take(7, kRendezvousOutcomeTtlMs));
}

TEST(NatServerHolePunch, RequestedTargetsStayBounded)
{
	RequestedCallbackTargets targets;
	for (std::uint32_t id = 1; id <= 1000; ++id) {
		targets.Note(id, 0);
	}
	ASSERT_TRUE(targets.Size() <= kMaxPendingRendezvous);
	ASSERT_TRUE(targets.Take(1000, 1));
}

namespace
{
constexpr std::uint32_t kForeignServer = 0x0A000001;
constexpr std::uint32_t kOtherServer = 0x0A000002;

Hash ForeignHash(std::uint8_t value)
{
	Hash hash;
	hash.fill(value);
	return hash;
}
} // namespace

TEST(NatServerHolePunch, BuildsTheCrossServerRequestWithRequesterIdZero)
{
	const auto bytes = BuildForeignCallbackRequest(0x01020305, 0x03);
	ASSERT_EQUALS(9, static_cast<int>(bytes.size()));
	ASSERT_EQUALS(0x05, static_cast<int>(bytes[0]));
	ASSERT_EQUALS(0x01, static_cast<int>(bytes[3]));
	for (int i = 4; i < 8; ++i) {
		ASSERT_EQUALS(0, static_cast<int>(bytes[i]));
	}
	ASSERT_EQUALS(0x03, static_cast<int>(bytes[8]));
}

TEST(NatServerHolePunch, ParsesOnlyAnAnswerOfExactly27Bytes)
{
	std::array<std::uint8_t, 28> bytes{};
	PokeUInt32(bytes.data(), 0x0B000001);
	PokeUInt16(bytes.data() + 4, 4672);
	PokeUInt32(bytes.data() + 6, 0x01020305);
	bytes[10] = 0x81;
	std::fill(bytes.begin() + 11, bytes.begin() + 27, 0x44);

	const auto answer = ParseForeignCallbackAnswer(bytes.data(), 27);
	ASSERT_TRUE(answer.has_value());
	ASSERT_EQUALS((std::uint32_t)0x0B000001, answer->targetIp);
	ASSERT_EQUALS((std::uint16_t)4672, answer->targetUdpPort);
	ASSERT_EQUALS((std::uint32_t)0x01020305, answer->targetId);
	ASSERT_EQUALS(0x81, static_cast<int>(answer->crypt));
	ASSERT_TRUE(answer->targetHash == ForeignHash(0x44));

	ASSERT_FALSE(ParseForeignCallbackAnswer(bytes.data(), 26).has_value());
	ASSERT_FALSE(ParseForeignCallbackAnswer(bytes.data(), 28).has_value());
	ASSERT_FALSE(ParseForeignCallbackAnswer(nullptr, 27).has_value());
}

TEST(NatServerHolePunch, RequestsACrossServerCallbackOnlyForAnOddLowIdOnAServerThatTakesIt)
{
	ForeignRequestFacts facts;
	facts.ourId = 101;
	facts.sourceId = 201;
	facts.sourceServerIsOurs = false;
	facts.serverTakesForeignCallback = true;
	facts.extensionAnnounced = true;
	facts.ourUdpPort = 4672;
	ASSERT_TRUE(ShouldRequestForeignCallback(facts));

	ForeignRequestFacts settingOff = facts;
	settingOff.extensionAnnounced = false;
	ASSERT_FALSE(ShouldRequestForeignCallback(settingOff));

	ForeignRequestFacts sameServer = facts;
	sameServer.sourceServerIsOurs = true;
	ASSERT_FALSE(ShouldRequestForeignCallback(sameServer));

	ForeignRequestFacts serverDoesNotTakeIt = facts;
	serverDoesNotTakeIt.serverTakesForeignCallback = false;
	ASSERT_FALSE(ShouldRequestForeignCallback(serverDoesNotTakeIt));

	ForeignRequestFacts evenTarget = facts;
	evenTarget.sourceId = 200;
	ASSERT_FALSE(ShouldRequestForeignCallback(evenTarget));

	ForeignRequestFacts weAreHighId = facts;
	weAreHighId.ourId = 20000000;
	ASSERT_FALSE(ShouldRequestForeignCallback(weAreHighId));

	ForeignRequestFacts highIdSource = facts;
	highIdSource.sourceId = 20000001;
	ASSERT_FALSE(ShouldRequestForeignCallback(highIdSource));

	ForeignRequestFacts noUdpPort = facts;
	noUdpPort.ourUdpPort = 0;
	ASSERT_FALSE(ShouldRequestForeignCallback(noUdpPort));
}

TEST(NatServerHolePunch, ACrossServerAnswerIsTakenOnceForItsPendingTarget)
{
	ForeignCallbackRequests requests;
	ASSERT_TRUE(requests.TryStart(kForeignServer, 42, 201, 4662, std::nullopt, 0));
	// The same target is not asked twice while its answer is pending.
	ASSERT_FALSE(requests.TryStart(kForeignServer, 42, 201, 4662, std::nullopt, 10));

	ASSERT_FALSE(requests.TakeAnswer(kOtherServer, 201, ForeignHash(1), 100).has_value());
	ASSERT_FALSE(requests.TakeAnswer(kForeignServer, 203, ForeignHash(1), 100).has_value());
	const auto taken = requests.TakeAnswer(kForeignServer, 201, ForeignHash(1), 100);
	ASSERT_TRUE(taken.has_value());
	ASSERT_EQUALS((std::uint16_t)4662, taken->sourceTcpPort);
	// The server resends its answer; only the first copy is used.
	ASSERT_FALSE(requests.TakeAnswer(kForeignServer, 201, ForeignHash(1), 1300).has_value());
}

TEST(NatServerHolePunch, ACrossServerAnswerMustCarryTheSourceHashWhenWeKnowIt)
{
	ForeignCallbackRequests requests;
	ASSERT_TRUE(requests.TryStart(kForeignServer, 42, 201, 4662, ForeignHash(7), 0));
	ASSERT_FALSE(requests.TakeAnswer(kForeignServer, 201, ForeignHash(8), 100).has_value());
	ASSERT_TRUE(requests.TakeAnswer(kForeignServer, 201, ForeignHash(7), 200).has_value());
}

TEST(NatServerHolePunch, AnUnansweredCrossServerTargetCoolsDown)
{
	ForeignCallbackRequests requests;
	ASSERT_TRUE(requests.TryStart(kForeignServer, 42, 201, 4662, std::nullopt, 0));
	const std::uint64_t timedOut = kForeignRequestTimeoutMs;
	ASSERT_FALSE(requests.TakeAnswer(kForeignServer, 201, ForeignHash(1), timedOut).has_value());
	ASSERT_FALSE(
		requests.TryStart(kForeignServer, 42, 201, 4662, std::nullopt, kForeignRequestTimeoutMs + 1));
	ASSERT_FALSE(requests.TryStart(kForeignServer,
		42,
		201,
		4662,
		std::nullopt,
		kForeignRequestTimeoutMs + kForeignTargetCooldownMs - 1));
	ASSERT_TRUE(requests.TryStart(kForeignServer,
		42,
		201,
		4662,
		std::nullopt,
		kForeignRequestTimeoutMs + kForeignTargetCooldownMs));
}

TEST(NatServerHolePunch, CrossServerRequestsStayBelowTheServerRateLimit)
{
	ForeignCallbackRequests requests;
	for (std::uint32_t target = 1; target <= kForeignRequestBurst; ++target) {
		ASSERT_TRUE(requests.TryStart(kForeignServer, 42, 2 * target + 1, 4662, std::nullopt, 0));
	}
	ASSERT_FALSE(requests.TryStart(kForeignServer, 42, 1001, 4662, std::nullopt, 0));
	// The limit is per server.
	ASSERT_TRUE(requests.TryStart(kOtherServer, 42, 1001, 4662, std::nullopt, 0));
	ASSERT_FALSE(
		requests.TryStart(kForeignServer, 42, 1001, 4662, std::nullopt, kForeignRequestRefillMs - 1));
	ASSERT_TRUE(requests.TryStart(kForeignServer, 42, 1001, 4662, std::nullopt, kForeignRequestRefillMs));
}

TEST(NatServerHolePunch, CrossServerTargetsPerServerStayBelowTheServerWindow)
{
	ForeignCallbackRequests requests;
	std::uint64_t now = 0;
	for (std::uint32_t target = 0; target < kForeignTargetsPerWindow; ++target) {
		ASSERT_TRUE(requests.TryStart(kForeignServer, 42, 2 * target + 1, 4662, std::nullopt, now));
		requests.TakeAnswer(kForeignServer, 2 * target + 1, ForeignHash(1), now);
		now += kForeignRequestRefillMs;
	}
	ASSERT_FALSE(requests.TryStart(kForeignServer, 42, 9001, 4662, std::nullopt, now));
	ASSERT_TRUE(requests.TryStart(kForeignServer,
		42,
		9001,
		4662,
		std::nullopt,
		kForeignTargetWindowMs + kForeignRequestRefillMs));
}

TEST(NatServerHolePunch, ACrossServerAnswerIsDecryptedWithTheKeyItsRequestWasSentWith)
{
	// Two listed servers can share an IP with different ports, and so different UDP keys.
	ForeignCallbackRequests requests;
	ASSERT_TRUE(requests.TryStart(kForeignServer, 42, 201, 4662, std::nullopt, 0));
	ASSERT_TRUE(requests.TryStart(kForeignServer, 43, 203, 4662, std::nullopt, 0));
	const std::vector<std::uint32_t> keys = requests.PendingKeysFrom(kForeignServer, 10);
	ASSERT_EQUALS(2, static_cast<int>(keys.size()));
	ASSERT_TRUE(std::find(keys.begin(), keys.end(), 42u) != keys.end());
	ASSERT_TRUE(std::find(keys.begin(), keys.end(), 43u) != keys.end());
	ASSERT_TRUE(requests.PendingKeysFrom(kOtherServer, 10).empty());

	requests.TakeAnswer(kForeignServer, 201, ForeignHash(1), 20);
	ASSERT_EQUALS(1, static_cast<int>(requests.PendingKeysFrom(kForeignServer, 20).size()));
}

TEST(NatServerHolePunch, NoCrossServerRequestWhenEitherSideRequiresTheCryptLayer)
{
	// The server sends nothing when one side requires it, so the request would only spend its budget.
	ForeignRequestFacts facts;
	facts.ourId = 101;
	facts.sourceId = 201;
	facts.serverTakesForeignCallback = true;
	facts.extensionAnnounced = true;
	facts.ourUdpPort = 4672;
	ASSERT_TRUE(ShouldRequestForeignCallback(facts));

	ForeignRequestFacts weRequire = facts;
	weRequire.weRequireCryptLayer = true;
	ASSERT_FALSE(ShouldRequestForeignCallback(weRequire));

	ForeignRequestFacts sourceRequires = facts;
	sourceRequires.sourceRequiresCryptLayer = true;
	ASSERT_FALSE(ShouldRequestForeignCallback(sourceRequires));
}

TEST(NatServerHolePunch, AnAnsweredCrossServerTargetCoolsDownToo)
{
	// A dial that fails after the answer must not ask again on the next retry.
	ForeignCallbackRequests requests;
	ASSERT_TRUE(requests.TryStart(kForeignServer, 42, 201, 4662, std::nullopt, 0));
	ASSERT_TRUE(requests.TakeAnswer(kForeignServer, 201, ForeignHash(1), 100).has_value());
	ASSERT_FALSE(requests.TryStart(kForeignServer, 42, 201, 4662, std::nullopt, 101));
	ASSERT_FALSE(requests.TryStart(
		kForeignServer, 42, 201, 4662, std::nullopt, 100 + kForeignTargetCooldownMs - 1));
	ASSERT_TRUE(requests.TryStart(
		kForeignServer, 42, 201, 4662, std::nullopt, 100 + kForeignTargetCooldownMs));
}

TEST(NatServerHolePunch, ACrossServerAnswerCanBeCheckedWithoutTakingIt)
{
	// Checked before the action limit, so a refusal there leaves the request for the server's resend.
	ForeignCallbackRequests requests;
	ASSERT_TRUE(requests.TryStart(kForeignServer, 42, 201, 4662, ForeignHash(7), 0));
	ASSERT_FALSE(requests.AnswersPending(kForeignServer, 201, ForeignHash(8), 10));
	ASSERT_FALSE(requests.AnswersPending(kForeignServer, 203, ForeignHash(7), 10));
	ASSERT_TRUE(requests.AnswersPending(kForeignServer, 201, ForeignHash(7), 10));
	ASSERT_TRUE(requests.AnswersPending(kForeignServer, 201, ForeignHash(7), 20));
	ASSERT_TRUE(requests.TakeAnswer(kForeignServer, 201, ForeignHash(7), 30).has_value());
	ASSERT_FALSE(requests.AnswersPending(kForeignServer, 201, ForeignHash(7), 40));
}
