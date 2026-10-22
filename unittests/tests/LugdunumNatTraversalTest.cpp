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

#include <muleunit/test.h>

#include "LugdunumNatTraversal.h"

#include <algorithm>

using namespace muleunit;
using namespace LugdunumNatTraversal;

DECLARE_SIMPLE(LugdunumNatTraversal)

TEST(LugdunumNatTraversal, KeepsCallbackPinholeOpenAndRequestsReverseEarly)
{
	CCallbackRendezvous rendezvous;
	const CallbackFailureKey key{ 1, 2, 3, 0x0100007F, 4672 };

	ASSERT_TRUE(rendezvous.Start(key, 0x0100007F, 4672, 100));
	ASSERT_TRUE(rendezvous.Tick(3099).empty());
	const std::vector<CallbackRendezvousAction> first = rendezvous.Tick(3100);
	ASSERT_EQUALS(1, (int)first.size());
	ASSERT_TRUE(first[0].sendKeepalive);
	ASSERT_TRUE(first[0].requestReverseCallback);
	ASSERT_TRUE(first[0].key == key);

	ASSERT_TRUE(rendezvous.Tick(6099).empty());
	const std::vector<CallbackRendezvousAction> second = rendezvous.Tick(6100);
	ASSERT_EQUALS(1, (int)second.size());
	ASSERT_TRUE(second[0].sendKeepalive);
	ASSERT_FALSE(second[0].requestReverseCallback);

	ASSERT_TRUE(rendezvous.Complete(key));
	ASSERT_TRUE(rendezvous.Tick(9100).empty());
}

TEST(LugdunumNatTraversal, ReverseCallbackNeverRequestsAnotherReverseAndRendezvousExpires)
{
	CCallbackRendezvous rendezvous;
	const CallbackFailureKey reverseKey{ 1, 2, 0, 0x0100007F, 4672 };
	ASSERT_TRUE(rendezvous.Start(reverseKey, 0x0100007F, 4672, 0));

	const std::vector<CallbackRendezvousAction> action = rendezvous.Tick(3000);
	ASSERT_EQUALS(1, (int)action.size());
	ASSERT_TRUE(action[0].sendKeepalive);
	ASSERT_FALSE(action[0].requestReverseCallback);
	ASSERT_TRUE(rendezvous.Tick(60000).empty());
}

TEST(LugdunumNatTraversal, ParsesLiveIdChangeVectors)
{
	// Mazinga, without 0x1000 (S3): id, flags, aux (server) port only.
	const std::array<std::uint8_t, 16> mazingaA{ { 0xa0,
		0x69,
		0xa1,
		0x00,
		0xf9,
		0x17,
		0x00,
		0x00,
		0x88,
		0x10,
		0x00,
		0x00,
		0x4f,
		0x75,
		0x1c,
		0x71 } };
	const auto a = ParseIdChangeFields(mazingaA.data(), mazingaA.size());
	ASSERT_TRUE(a.ok);
	ASSERT_EQUALS((std::uint32_t)0x00A169A0, a.id);
	ASSERT_TRUE(a.hasTcpFlags);
	ASSERT_EQUALS((std::uint32_t)0x17F9, a.tcpFlags);
	ASSERT_TRUE(a.hasAuxPort);
	ASSERT_EQUALS((std::uint32_t)4232, a.auxPort);
	ASSERT_FALSE(a.hasReportedIp);
	ASSERT_FALSE(a.hasYourPort);

	// Mazinga, with 0x1000 (S3): adds reported IP, obfuscation port and "your port" 56197.
	const std::array<std::uint8_t, 24> mazingaB{ { 0x37,
		0x6b,
		0xa1,
		0x00,
		0xf9,
		0x17,
		0x00,
		0x00,
		0x88,
		0x10,
		0x00,
		0x00,
		0x4f,
		0x75,
		0x1c,
		0x71,
		0x88,
		0x10,
		0x00,
		0x00,
		0x85,
		0xdb,
		0x00,
		0x00 } };
	const auto b = ParseIdChangeFields(mazingaB.data(), mazingaB.size());
	ASSERT_TRUE(b.ok);
	ASSERT_EQUALS((std::uint32_t)0x00A16B37, b.id);
	ASSERT_EQUALS((std::uint32_t)0x17F9, b.tcpFlags);
	ASSERT_EQUALS((std::uint32_t)4232, b.auxPort);
	ASSERT_TRUE(b.hasReportedIp);
	ASSERT_EQUALS((std::uint32_t)4232, b.obfuscationPortTcp);
	ASSERT_TRUE(b.hasYourPort);
	ASSERT_EQUALS((std::uint32_t)56197, b.yourPort);

	// Sunrise, with 0x1000 (S3): different id/ports, "your port" 57428.
	const std::array<std::uint8_t, 24> sunriseC{ { 0xe5,
		0xb1,
		0xca,
		0x00,
		0xf9,
		0x17,
		0x00,
		0x00,
		0x75,
		0x12,
		0x00,
		0x00,
		0x4f,
		0x75,
		0x1c,
		0x71,
		0x75,
		0x12,
		0x00,
		0x00,
		0x54,
		0xe0,
		0x00,
		0x00 } };
	const auto c = ParseIdChangeFields(sunriseC.data(), sunriseC.size());
	ASSERT_TRUE(c.ok);
	ASSERT_EQUALS((std::uint32_t)0x00CAB1E5, c.id);
	ASSERT_EQUALS((std::uint32_t)4725, c.auxPort);
	ASSERT_TRUE(c.hasYourPort);
	ASSERT_EQUALS((std::uint32_t)57428, c.yourPort);
}

// Active needs both the 24-byte form and the 0x1000 flag; they arrive in the same packet.
TEST(LugdunumNatTraversal, CallbackIsActiveOnlyWithTheLongFormAndTheFlag)
{
	const std::array<std::uint8_t, 24> mazingaB{ { 0x37,
		0x6b,
		0xa1,
		0x00,
		0xf9,
		0x17,
		0x00,
		0x00,
		0x88,
		0x10,
		0x00,
		0x00,
		0x4f,
		0x75,
		0x1c,
		0x71,
		0x88,
		0x10,
		0x00,
		0x00,
		0x85,
		0xdb,
		0x00,
		0x00 } };
	ASSERT_TRUE(IsNatCallbackActive(ParseIdChangeFields(mazingaB.data(), mazingaB.size())));

	const std::array<std::uint8_t, 16> mazingaA{ { 0xa0,
		0x69,
		0xa1,
		0x00,
		0xf9,
		0x17,
		0x00,
		0x00,
		0x88,
		0x10,
		0x00,
		0x00,
		0x4f,
		0x75,
		0x1c,
		0x71 } };
	ASSERT_FALSE(IsNatCallbackActive(ParseIdChangeFields(mazingaA.data(), mazingaA.size())));

	std::array<std::uint8_t, 24> longFormWithoutFlag = mazingaB;
	longFormWithoutFlag[5] = 0x07; // flags 0x17F9 -> 0x07F9
	ASSERT_FALSE(IsNatCallbackActive(
		ParseIdChangeFields(longFormWithoutFlag.data(), longFormWithoutFlag.size())));
}

TEST(LugdunumNatTraversal, HandlesIdChangeBoundarySizesAndLeavesExistingFieldsUnchanged)
{
	const std::array<std::uint8_t, 24> full{ { 0x37,
		0x6b,
		0xa1,
		0x00,
		0xf9,
		0x17,
		0x00,
		0x00,
		0x88,
		0x10,
		0x00,
		0x00,
		0x4f,
		0x75,
		0x1c,
		0x71,
		0x88,
		0x10,
		0x00,
		0x00,
		0x85,
		0xdb,
		0x00,
		0x00 } };

	// Exactly 20 bytes: reported IP and obfuscation port present, "your port" absent.
	const auto at20 = ParseIdChangeFields(full.data(), 20);
	ASSERT_TRUE(at20.ok);
	ASSERT_TRUE(at20.hasReportedIp);
	ASSERT_EQUALS((std::uint32_t)4232, at20.obfuscationPortTcp);
	ASSERT_FALSE(at20.hasYourPort);

	// Truncated below the mandatory ID: not ok.
	ASSERT_FALSE(ParseIdChangeFields(full.data(), 3).ok);
	ASSERT_FALSE(ParseIdChangeFields(nullptr, full.size()).ok);

	// Only the ID fits: later optional fields are absent, not garbage-filled.
	const auto onlyId = ParseIdChangeFields(full.data(), 4);
	ASSERT_TRUE(onlyId.ok);
	ASSERT_FALSE(onlyId.hasTcpFlags);
	ASSERT_FALSE(onlyId.hasAuxPort);
	ASSERT_FALSE(onlyId.hasReportedIp);

	// 16 bytes (id+flags+aux port only): matches upstream's existing behaviour of not reading a
	// reported IP unless it is bundled with the obfuscation port (20+ bytes).
	const auto at16 = ParseIdChangeFields(full.data(), 16);
	ASSERT_TRUE(at16.hasAuxPort);
	ASSERT_FALSE(at16.hasReportedIp);
}

TEST(LugdunumNatTraversal, BuildsCallbackRequestPayload)
{
	const CallbackRequest request{ 0x12345678, 0x9ABCDEF0 };
	const std::array<std::uint8_t, 8> expected{ { 0x78, 0x56, 0x34, 0x12, 0xF0, 0xDE, 0xBC, 0x9A } };
	ASSERT_TRUE(BuildCallbackRequest(request) == expected);

	// Requester ID 0 is a valid value (LowID requester before the server assigns one), not an
	// error sentinel.
	const CallbackRequest zeroRequester{ 1, 0 };
	const std::array<std::uint8_t, 8> expectedZero{ { 0x01, 0, 0, 0, 0, 0, 0, 0 } };
	ASSERT_TRUE(BuildCallbackRequest(zeroRequester) == expectedZero);
}

TEST(LugdunumNatTraversal, ParsesForwardedCallbackWithAndWithoutObfuscation)
{
	// 10 bytes: no obfuscation part. Requester ID 0 (may legitimately be unset).
	const std::array<std::uint8_t, 10> bare{
		{ 0x01, 0x02, 0x03, 0x04, 0x34, 0x12, 0x00, 0x00, 0x00, 0x00 }
	};
	const auto bareResult = ParseForwardedCallback(bare.data(), bare.size());
	ASSERT_TRUE(bareResult.ok);
	ASSERT_EQUALS((std::uint32_t)0x04030201, bareResult.seenIp);
	ASSERT_EQUALS((std::uint16_t)0x1234, bareResult.seenUdpPort);
	ASSERT_EQUALS((std::uint32_t)0, bareResult.requesterId);
	ASSERT_FALSE(bareResult.hasObfuscation);

	// 27 bytes: obfuscation settings + 16-byte user hash present.
	std::array<std::uint8_t, 27> full{};
	std::copy(bare.begin(), bare.end(), full.begin());
	full[10] = 0x80;
	for (std::size_t i = 0; i < 16; ++i) {
		full[11 + i] = static_cast<std::uint8_t>(i);
	}
	const auto fullResult = ParseForwardedCallback(full.data(), full.size());
	ASSERT_TRUE(fullResult.ok);
	ASSERT_TRUE(fullResult.hasObfuscation);
	ASSERT_EQUALS((std::uint8_t)0x80, fullResult.obfuscationSettings);
	for (std::size_t i = 0; i < 16; ++i) {
		ASSERT_EQUALS(static_cast<std::uint8_t>(i), fullResult.userHash[i]);
	}

	// Truncated below the 10-byte minimum: not ok.
	ASSERT_FALSE(ParseForwardedCallback(bare.data(), 9).ok);
	ASSERT_FALSE(ParseForwardedCallback(nullptr, bare.size()).ok);

	// Trailing bytes short of the obfuscation part (10..26 bytes) are accepted and ignored.
	std::array<std::uint8_t, 15> withTrailing{};
	std::copy(bare.begin(), bare.end(), withTrailing.begin());
	const auto trailingResult = ParseForwardedCallback(withTrailing.data(), withTrailing.size());
	ASSERT_TRUE(trailingResult.ok);
	ASSERT_FALSE(trailingResult.hasObfuscation);

	// Trailing bytes beyond the 27-byte obfuscated form are accepted and ignored too.
	std::array<std::uint8_t, 30> withExtra{};
	std::copy(full.begin(), full.end(), withExtra.begin());
	const auto extraResult = ParseForwardedCallback(withExtra.data(), withExtra.size());
	ASSERT_TRUE(extraResult.ok);
	ASSERT_TRUE(extraResult.hasObfuscation);
}

TEST(LugdunumNatTraversal, RequestsCallbackOnlyWhenEveryPreconditionHolds)
{
	CallbackRequestFacts facts;
	facts.ourId = 100;    // LowID
	facts.sourceId = 200; // LowID
	facts.sameServer = true;
	facts.serverCallbackCapable = true;
	facts.serverCryptLayerUdpEnabled = true;
	facts.serverKeyUdp = 42;
	facts.serverSupportsObfuscationUdp = true;
	facts.settingEnabled = true;
	ASSERT_TRUE(ShouldRequestLugdunumNatCallback(facts));

	// The runtime setting (amule.conf's LugdunumNatCallback) defaults to off and overrides every
	// other precondition.
	CallbackRequestFacts settingOff = facts;
	settingOff.settingEnabled = false;
	ASSERT_FALSE(ShouldRequestLugdunumNatCallback(settingOff));

	// Either side being HighID means no callback is needed/possible this way.
	CallbackRequestFacts highIdSource = facts;
	highIdSource.sourceId = 20000000;
	ASSERT_FALSE(ShouldRequestLugdunumNatCallback(highIdSource));

	CallbackRequestFacts highIdUs = facts;
	highIdUs.ourId = 20000000;
	ASSERT_FALSE(ShouldRequestLugdunumNatCallback(highIdUs));

	// Different server, or a server that never answered the opt-in: no send.
	CallbackRequestFacts differentServer = facts;
	differentServer.sameServer = false;
	ASSERT_FALSE(ShouldRequestLugdunumNatCallback(differentServer));

	CallbackRequestFacts notCapable = facts;
	notCapable.serverCallbackCapable = false;
	ASSERT_FALSE(ShouldRequestLugdunumNatCallback(notCapable));

	// Each server-way crypto precondition is independently required (S5).
	CallbackRequestFacts noCryptLayer = facts;
	noCryptLayer.serverCryptLayerUdpEnabled = false;
	ASSERT_FALSE(ShouldRequestLugdunumNatCallback(noCryptLayer));

	CallbackRequestFacts noServerKey = facts;
	noServerKey.serverKeyUdp = 0;
	ASSERT_FALSE(ShouldRequestLugdunumNatCallback(noServerKey));

	CallbackRequestFacts noObfuscation = facts;
	noObfuscation.serverSupportsObfuscationUdp = false;
	ASSERT_FALSE(ShouldRequestLugdunumNatCallback(noObfuscation));
}

TEST(LugdunumNatTraversal, RecognisesServerWayDatagramsOnlyFromACapableCurrentServer)
{
	ServerWayDatagramFacts facts;
	facts.sourceIp = 0x0100007F; // matches
	facts.serverIp = 0x0100007F;
	facts.serverCallbackCapable = true;
	ASSERT_TRUE(IsServerWayDatagramFromOurServer(facts));

	// Source IP alone never authenticates: a capable server's datagram from a different IP is
	// not even worth a decrypt attempt (S5).
	ServerWayDatagramFacts differentIp = facts;
	differentIp.sourceIp = 0x0200007F;
	ASSERT_FALSE(IsServerWayDatagramFromOurServer(differentIp));

	// A same-IP datagram from a server that never proved itself callback-capable is not routed
	// here either.
	ServerWayDatagramFacts notCapable = facts;
	notCapable.serverCallbackCapable = false;
	ASSERT_FALSE(IsServerWayDatagramFromOurServer(notCapable));
}

TEST(LugdunumNatTraversal, SelectsChallengeOverServerKeyOnlyWhenAPingReplyIsPending)
{
	// Mirrors CServerUDPSocket::OnPacketReceived (ServerUDPSocket.cpp) and NeoMule's
	// ClientUDPSocket.cpp: a pending crypt-ping reply's challenge wins.
	ASSERT_EQUALS((std::uint32_t)777, SelectServerWayKey(true, 777, 42));

	// No reply pending: the established server key is used regardless of a stale challenge.
	ASSERT_EQUALS((std::uint32_t)42, SelectServerWayKey(false, 777, 42));

	// Reply pending but no real challenge (0): falls back to the server key.
	ASSERT_EQUALS((std::uint32_t)42, SelectServerWayKey(true, 0, 42));
}

TEST(LugdunumNatTraversal, KeepsLowIdServerSourcesOnlyFromOurOwnCapableServer)
{
	LowIdServerSourceFacts facts;
	facts.originIsLocalServer = true;
	facts.sourceServerIsOurCurrentServer = true;
	facts.weAreLowId = true;
	facts.serverCallbackCapable = true;
	facts.settingEnabled = true;
	ASSERT_TRUE(ShouldKeepLowIdServerSource(facts));

	// The runtime setting (amule.conf's LugdunumNatCallback) defaults to off: with it off, a kept
	// source could never be answered.
	LowIdServerSourceFacts settingOff = facts;
	settingOff.settingEnabled = false;
	ASSERT_FALSE(ShouldKeepLowIdServerSource(settingOff));

	// A source-exchange or Kad-sourced record is never kept this way, even with identical
	// server/capability facts: the server only forwards LowID sources it is prepared to call
	// back, and that promise only covers what it told us directly.
	LowIdServerSourceFacts notLocalServer = facts;
	notLocalServer.originIsLocalServer = false;
	ASSERT_FALSE(ShouldKeepLowIdServerSource(notLocalServer));

	// A remote server's OP_GLOBFOUNDSOURCES reply is not our current server.
	LowIdServerSourceFacts notOurServer = facts;
	notOurServer.sourceServerIsOurCurrentServer = false;
	ASSERT_FALSE(ShouldKeepLowIdServerSource(notOurServer));

	// A HighID client has no reason to special-case a LowID source this way.
	LowIdServerSourceFacts notLowId = facts;
	notLowId.weAreLowId = false;
	ASSERT_FALSE(ShouldKeepLowIdServerSource(notLowId));

	// The server never answered our opt-in: no promise to forward callbacks exists.
	LowIdServerSourceFacts notCapable = facts;
	notCapable.serverCallbackCapable = false;
	ASSERT_FALSE(ShouldKeepLowIdServerSource(notCapable));
}

TEST(LugdunumNatTraversal, ReverseCallbackStillDialsButCannotStartReciprocalRequest)
{
	ForwardedCallback callback;
	callback.ok = true;
	callback.seenIp = 0x01020304;
	callback.seenUdpPort = 4662;
	callback.requesterId = 0;

	ASSERT_TRUE(ShouldDialForwardedCallback(callback, true, true));
	ASSERT_FALSE(ShouldSendCallbackRequestAfterFailure(callback.requesterId));

	callback.requesterId = 0x12345678;
	ASSERT_TRUE(ShouldDialForwardedCallback(callback, true, true));
	ASSERT_TRUE(ShouldSendCallbackRequestAfterFailure(callback.requesterId));

	ASSERT_FALSE(ShouldDialForwardedCallback(callback, false, true));
	ASSERT_FALSE(ShouldDialForwardedCallback(callback, true, false));
	callback.seenUdpPort = 0;
	ASSERT_FALSE(ShouldDialForwardedCallback(callback, true, true));
}

TEST(LugdunumNatTraversal, CallbackDialKeepsObservedEndpointOutOfPeerId)
{
	ForwardedCallback callback;
	callback.ok = true;
	callback.seenIp = 0x01020304;
	callback.seenUdpPort = 4662;
	callback.requesterId = 0x12345678;
	callback.hasObfuscation = true;
	callback.userHash.fill(0xAB);

	const CallbackDialTarget target = BuildCallbackDialTarget(callback);
	ASSERT_EQUALS(0u, target.provisionalUserId);
	ASSERT_EQUALS(callback.seenIp, target.ip);
	ASSERT_EQUALS(callback.seenUdpPort, target.udpPort);
}

TEST(LugdunumNatTraversal, CallbackFailureCooldownExpiresAfterTenMinutesAndEndpointChangesBypass)
{
	CallbackFailureMemory failures;
	const CallbackFailureKey key{ 0x0A000001, 4661, 0x12345678, 0xC0000201, 4662 };
	const std::uint64_t failedAt = 100;
	failures.RecordFailure(key, failedAt);

	ASSERT_TRUE(failures.IsCoolingDown(key, failedAt + CallbackFailureMemory::CooldownMs - 1));
	ASSERT_FALSE(failures.IsCoolingDown(key, failedAt + CallbackFailureMemory::CooldownMs));

	failures.RecordFailure(key, failedAt);
	CallbackFailureKey changedEndpoint = key;
	changedEndpoint.endpointUdpPort = 4663;
	ASSERT_FALSE(failures.IsCoolingDown(changedEndpoint, failedAt + 1));

	CallbackFailureKey otherServer = key;
	otherServer.serverIp = 0x0A000002;
	ASSERT_FALSE(failures.IsCoolingDown(otherServer, failedAt + 1));
}

TEST(LugdunumNatTraversal, SuccessfulCallbackHelloClearsOnlyTheMatchingPeerFailure)
{
	CallbackFailureMemory failures;
	const CallbackFailureKey key{ 0x0A000001, 4661, 0x12345678, 0xC0000201, 4662 };
	failures.RecordFailure(key, 100);
	ASSERT_TRUE(failures.IsCoolingDown(key, 101));
	bool cleared = false;
	ASSERT_FALSE(ClearCallbackFailureAfterHello(key, 0x87654321, [&](const auto &) {
		failures.Clear(key);
		cleared = true;
	}));
	ASSERT_FALSE(cleared);
	ASSERT_TRUE(failures.IsCoolingDown(key, 102));

	ASSERT_TRUE(ClearCallbackFailureAfterHello(key, key.requesterId, [&](const auto &) {
		failures.Clear(key);
		cleared = true;
	}));
	ASSERT_TRUE(cleared);
	ASSERT_FALSE(failures.IsCoolingDown(key, 103));

	CallbackFailureKey reverseCallback = key;
	reverseCallback.requesterId = 0;
	ASSERT_TRUE(ClearCallbackFailureAfterHello(reverseCallback, 0x87654321, [](const auto &) {}));
}

TEST(LugdunumNatTraversal, ReverseCallbackFailureMemoryFallsBackToObservedEndpoint)
{
	CallbackFailureMemory failures;
	const CallbackFailureKey key{ 0x0A000001, 4661, 0, 0xC0000201, 4662 };
	failures.RecordFailure(key, 100);
	ASSERT_TRUE(failures.IsCoolingDown(key, 101));

	CallbackFailureKey changedEndpoint = key;
	changedEndpoint.endpointIp = 0xC0000202;
	ASSERT_FALSE(failures.IsCoolingDown(changedEndpoint, 101));
}

TEST(LugdunumNatTraversal, DispatchAndFailureActionsUseInjectedOperations)
{
	ForwardedCallback tcpCallback;
	tcpCallback.ok = true;
	tcpCallback.seenIp = 0xC0000201;
	tcpCallback.seenUdpPort = 4662;
	tcpCallback.requesterId = 0x12345678;
	ForwardedCallback udpCallback = tcpCallback;
	udpCallback.hasObfuscation = true;
	udpCallback.userHash.fill(0xAB);

	int dialCount = 0;
	CallbackDialTarget firstTarget{};
	CallbackDialTarget secondTarget{};
	const auto dial = [&](const ForwardedCallback &callback) {
		const CallbackDialTarget target = BuildCallbackDialTarget(callback);
		if (dialCount == 0) {
			firstTarget = target;
		} else {
			secondTarget = target;
		}
		++dialCount;
	};
	ASSERT_TRUE(DispatchForwardedCallback(tcpCallback, dial));
	ASSERT_TRUE(DispatchForwardedCallback(udpCallback, dial));
	ASSERT_EQUALS(2, dialCount);
	ASSERT_EQUALS(firstTarget.provisionalUserId, secondTarget.provisionalUserId);
	ASSERT_EQUALS(firstTarget.ip, secondTarget.ip);
	ASSERT_EQUALS(firstTarget.udpPort, secondTarget.udpPort);
	ForwardedCallback invalid = tcpCallback;
	invalid.ok = false;
	ASSERT_FALSE(DispatchForwardedCallback(invalid, dial));
	ASSERT_EQUALS(2, dialCount);

	const CallbackFailureKey requesterKey{ 0x0A000001, 4661, 0x12345678, 0xC0000201, 4662 };
	const CallbackFailureKey reverseKey{ 0x0A000001, 4661, 0, 0xC0000201, 4662 };
	int recordedFailures = 0;
	int callbackRequests = 0;
	std::uint32_t requestedId = 0;
	const auto recordFailure = [&](const CallbackFailureKey &) { ++recordedFailures; };
	const auto requestCallback = [&](std::uint32_t requesterId) {
		++callbackRequests;
		requestedId = requesterId;
	};
	ASSERT_TRUE(HandleCallbackDialFailure(requesterKey, recordFailure, requestCallback));
	ASSERT_FALSE(HandleCallbackDialFailure(reverseKey, recordFailure, requestCallback));
	ASSERT_EQUALS(2, recordedFailures);
	ASSERT_EQUALS(1, callbackRequests);
	ASSERT_EQUALS(requesterKey.requesterId, requestedId);
}

TEST(LugdunumNatTraversal, FailedCallbackAttemptKeepsSourceRetryableThroughApplicationSeam)
{
	const CallbackFailureKey key{ 0x0A000001, 4661, 0x12345678, 0xC0000201, 4662 };
	CallbackFailureMemory failures;
	struct FakeClientRegistry
	{
		int sourceCount = 1;
		int temporaryClients = 0;
	} clients;
	int dialCalls = 0;
	int recordedFailures = 0;
	int callbackRequests = 0;
	std::uint32_t requestedId = 0;

	const auto attempt = [&](std::uint64_t now) {
		if (failures.IsCoolingDown(key, now)) {
			return false;
		}
		return RunCallbackDialApplication(
			key,
			[&]() {
				++dialCalls;
				++clients.temporaryClients;
				return false;
			},
			[&](const CallbackFailureKey &failedKey) {
				++recordedFailures;
				failures.RecordFailure(failedKey, now);
			},
			[&](std::uint32_t requesterId) {
				++callbackRequests;
				requestedId = requesterId;
			},
			[&]() { --clients.temporaryClients; });
	};

	ASSERT_FALSE(attempt(100));
	ASSERT_EQUALS(1, clients.sourceCount);
	ASSERT_EQUALS(0, clients.temporaryClients);
	ASSERT_EQUALS(1, dialCalls);
	ASSERT_EQUALS(1, recordedFailures);
	ASSERT_EQUALS(1, callbackRequests);
	ASSERT_EQUALS(key.requesterId, requestedId);

	ASSERT_FALSE(attempt(101));
	ASSERT_EQUALS(1, dialCalls);
	ASSERT_EQUALS(1, clients.sourceCount);

	ASSERT_FALSE(attempt(100 + CallbackFailureMemory::CooldownMs));
	ASSERT_EQUALS(2, dialCalls);
	ASSERT_EQUALS(1, clients.sourceCount);
	ASSERT_EQUALS(0, clients.temporaryClients);
	ASSERT_EQUALS(2, recordedFailures);
	ASSERT_EQUALS(2, callbackRequests);
}

TEST(LugdunumNatTraversal, ForwardedCallbackGuardDeduplicatesAndBoundsActions)
{
	ForwardedCallbackActionGuard guard;
	const CallbackFailureKey key{ 0x0A000001, 4661, 0x12345678, 0xC0000201, 4662 };
	ASSERT_TRUE(guard.Admit(key, 100));
	ASSERT_FALSE(guard.Admit(key, 100 + ForwardedCallbackActionGuard::DuplicateWindowMs - 1));
	ASSERT_TRUE(guard.Admit(key, 100 + ForwardedCallbackActionGuard::DuplicateWindowMs));

	CallbackFailureKey distinct = key;
	for (std::size_t index = 0; index < ForwardedCallbackActionGuard::MaxActions - 2; ++index) {
		distinct.requesterId = static_cast<std::uint32_t>(index + 1);
		ASSERT_TRUE(guard.Admit(distinct, 20'000));
	}
	distinct.requesterId = 0xFFFFFFFF;
	ASSERT_FALSE(guard.Admit(distinct, 20'000));
	distinct.serverIp = 0x0A000002;
	ASSERT_TRUE(guard.Admit(distinct, 20'000));
	ASSERT_TRUE(guard.Admit(distinct, 100 + ForwardedCallbackActionGuard::ActionWindowMs));
}
