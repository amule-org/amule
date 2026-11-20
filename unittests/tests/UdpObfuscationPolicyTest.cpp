//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//

#include <muleunit/test.h>

#include "UdpObfuscationPolicy.h"

#include <vector>

using namespace muleunit;

DECLARE_SIMPLE(UdpObfuscationPolicy)

namespace
{
constexpr std::uint32_t kPublicIP = 0x0100007F;
constexpr std::uint32_t kReceiverKey = 0x12345678;

bool Is(EUdpSendForm actual, EUdpSendForm expected)
{
	return actual == expected;
}

constexpr int kDatagramLen = 40;
constexpr int kHeaderLen = 8;
constexpr std::uint32_t kChallenge = 777;
constexpr std::uint32_t kServerKey = 42;

// As DecryptReceivedServer(): only the key the server used strips the header.
int Receive(const ServerUdpKeyCandidates &keys, std::uint32_t senderKey, std::vector<std::uint32_t> &tried)
{
	return DecryptWithServerUdpKeys(keys, kDatagramLen, [&](std::uint32_t key) {
		tried.push_back(key);
		return key == senderKey ? kDatagramLen - kHeaderLen : kDatagramLen;
	});
}
} // namespace

TEST(UdpObfuscationPolicy, AKadPacketIsObfuscatedWithTheCryptLayerTurnedOff)
{
	// The receiver key that proves our address to a peer travels in the obfuscation header, so the
	// user preference must not reach the Kad path (issue #2541).
	ASSERT_TRUE(ObfuscateQueuedPacket(true, false, true, kReceiverKey, false));
	ASSERT_TRUE(ObfuscateQueuedPacket(true, true, true, 0, false));
}

TEST(UdpObfuscationPolicy, AnEd2kPacketStillFollowsTheCryptLayerPreference)
{
	ASSERT_TRUE(ObfuscateQueuedPacket(true, true, false, 0, true));
	ASSERT_FALSE(ObfuscateQueuedPacket(true, true, false, 0, false));
}

TEST(UdpObfuscationPolicy, APacketWithNoKeyMaterialIsSentPlain)
{
	ASSERT_FALSE(ObfuscateQueuedPacket(true, false, true, 0, true));
	ASSERT_FALSE(ObfuscateQueuedPacket(true, false, false, kReceiverKey, true));
	ASSERT_FALSE(ObfuscateQueuedPacket(false, true, true, kReceiverKey, true));
}

TEST(UdpObfuscationPolicy, AnEncryptedClientPacketIsObfuscatedWhileThePublicIPIsKnown)
{
	ASSERT_TRUE(Is(DecideUdpSendForm(true, false, false, kPublicIP), EUdpSendForm::Obfuscated));
	ASSERT_TRUE(Is(DecideUdpSendForm(true, true, false, kPublicIP), EUdpSendForm::Obfuscated));
}

TEST(UdpObfuscationPolicy, AnOrdinaryClientPacketStillFallsBackToPlainWithoutAPublicIP)
{
	ASSERT_TRUE(Is(DecideUdpSendForm(true, false, false, 0), EUdpSendForm::Plain));
	ASSERT_TRUE(Is(DecideUdpSendForm(false, false, false, kPublicIP), EUdpSendForm::Plain));
}

TEST(UdpObfuscationPolicy, AKadPacketIsObfuscatedWithoutAPublicIP)
{
	ASSERT_TRUE(Is(DecideUdpSendForm(true, false, true, 0), EUdpSendForm::Obfuscated));
}

TEST(UdpObfuscationPolicy, APacketThatMustBeObfuscatedIsDroppedRatherThanSentPlain)
{
	ASSERT_TRUE(Is(DecideUdpSendForm(true, true, false, 0), EUdpSendForm::Drop));
	ASSERT_TRUE(Is(DecideUdpSendForm(false, true, false, kPublicIP), EUdpSendForm::Drop));
}

TEST(UdpObfuscationPolicy, TriesTheServerKeyAfterAPendingCryptPingChallenge)
{
	const ServerUdpKeyCandidates both = ServerUdpReceiveKeys(true, kChallenge, kServerKey);
	ASSERT_EQUALS((std::size_t)2, both.count);
	ASSERT_TRUE((both.keys == std::array<std::uint32_t, 2>{ kChallenge, kServerKey }));

	// Without a pending ping, or without a usable challenge, there is exactly one established key.
	const ServerUdpKeyCandidates noPending = ServerUdpReceiveKeys(false, kChallenge, kServerKey);
	ASSERT_EQUALS((std::size_t)1, noPending.count);
	ASSERT_EQUALS(kServerKey, noPending.keys[0]);
	const ServerUdpKeyCandidates noChallenge = ServerUdpReceiveKeys(true, 0, kServerKey);
	ASSERT_EQUALS((std::size_t)1, noChallenge.count);
	ASSERT_EQUALS(kServerKey, noChallenge.keys[0]);
	const ServerUdpKeyCandidates noServerKey = ServerUdpReceiveKeys(true, kChallenge, 0);
	ASSERT_EQUALS((std::size_t)1, noServerKey.count);
	ASSERT_EQUALS(kChallenge, noServerKey.keys[0]);
	const ServerUdpKeyCandidates sameKey = ServerUdpReceiveKeys(true, kServerKey, kServerKey);
	ASSERT_EQUALS((std::size_t)1, sameKey.count);
	ASSERT_EQUALS(kServerKey, sameKey.keys[0]);
	ASSERT_EQUALS((std::size_t)0, ServerUdpReceiveKeys(false, kChallenge, 0).count);
}

TEST(UdpObfuscationPolicy, ASourceAnswerWithTheServerKeyIsReadWhileTheCryptPingIsPending)
{
	std::vector<std::uint32_t> tried;
	const int len = Receive(ServerUdpReceiveKeys(true, kChallenge, kServerKey), kServerKey, tried);
	ASSERT_EQUALS(kDatagramLen - kHeaderLen, len);
	ASSERT_TRUE((tried == std::vector<std::uint32_t>{ kChallenge, kServerKey }));
}

TEST(UdpObfuscationPolicy, ThePingReplyStopsAtTheChallenge)
{
	std::vector<std::uint32_t> tried;
	const int len = Receive(ServerUdpReceiveKeys(true, kChallenge, kServerKey), kChallenge, tried);
	ASSERT_EQUALS(kDatagramLen - kHeaderLen, len);
	ASSERT_TRUE((tried == std::vector<std::uint32_t>{ kChallenge }));
}

TEST(UdpObfuscationPolicy, ADatagramNoKeyDecryptsKeepsItsLength)
{
	// A plain datagram, or one obfuscated with another key, goes on to the protocol check as it is.
	std::vector<std::uint32_t> tried;
	const int len = Receive(ServerUdpReceiveKeys(true, kChallenge, kServerKey), 0, tried);
	ASSERT_EQUALS(kDatagramLen, len);
	ASSERT_EQUALS((std::size_t)2, tried.size());

	tried.clear();
	ASSERT_EQUALS(kDatagramLen, Receive(ServerUdpKeyCandidates{}, kServerKey, tried));
	ASSERT_TRUE(tried.empty());
}
