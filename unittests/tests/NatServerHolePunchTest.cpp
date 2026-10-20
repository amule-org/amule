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
	ASSERT_TRUE(ShouldSendKeepalive(facts));

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
	facts.serverIsLugdunumActive = false;
	facts.settingEnabled = true;
	facts.ourUdpPort = 4672;
	ASSERT_TRUE(ShouldRequestLowIdHolePunch(facts));

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

	// A server is either Lugdunum or ed2k-server: a Lugdunum-active server is left to
	// TrySendLugdunumNatCallbackRequest, even if it also happened to carry this flag.
	RequestFacts lugdunumActive = facts;
	lugdunumActive.serverIsLugdunumActive = true;
	ASSERT_FALSE(ShouldRequestLowIdHolePunch(lugdunumActive));

	RequestFacts highIdSource = facts;
	highIdSource.sourceId = 20000000;
	ASSERT_FALSE(ShouldRequestLowIdHolePunch(highIdSource));

	RequestFacts highIdUs = facts;
	highIdUs.ourId = 20000000;
	ASSERT_FALSE(ShouldRequestLowIdHolePunch(highIdUs));
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
