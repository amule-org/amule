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
