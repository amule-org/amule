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

#include "ServerKeyRecovery.h"

using namespace muleunit;
using namespace ServerKeyRecovery;

DECLARE_SIMPLE(ServerKeyRecovery)

namespace
{
constexpr time_t kNow = 1'800'000'000;

// A server that answered a crypt ping long ago and is next due hours from now, without a key.
Facts MissingKey()
{
	Facts facts;
	facts.cryptLayerUdpEnabled = true;
	facts.publicIpKnown = true;
	facts.serverSupportsObfuscationUdp = true;
	facts.serverKeyUdp = 0;
	facts.cryptPingReplyPending = false;
	facts.lastPingedTime = kNow - HR2S(1);
	facts.realLastPingedTime = kNow - HR2S(1);
	return facts;
}
} // namespace

TEST(ServerKeyRecovery, AMissingKeyBringsTheStatusPingForwardToNow)
{
	const std::optional<time_t> lastPinged = RecoveryLastPingedTime(MissingKey(), kNow);

	ASSERT_TRUE(lastPinged.has_value());
	ASSERT_EQUALS(kNow, *lastPinged + UDPSERVSTATREASKTIME);
}

TEST(ServerKeyRecovery, ARecentPingDelaysRecoveryToTheMinimumReaskTime)
{
	Facts facts = MissingKey();
	facts.realLastPingedTime = kNow - MIN2S(5);

	const std::optional<time_t> lastPinged = RecoveryLastPingedTime(facts, kNow);

	ASSERT_TRUE(lastPinged.has_value());
	ASSERT_EQUALS(kNow + MIN2S(15), *lastPinged + UDPSERVSTATREASKTIME);
}

TEST(ServerKeyRecovery, NeverPushesAnEarlierPingLater)
{
	Facts dueNow = MissingKey();
	dueNow.lastPingedTime = 0;
	ASSERT_FALSE(RecoveryLastPingedTime(dueNow, kNow).has_value());

	Facts dueSooner = MissingKey();
	dueSooner.realLastPingedTime = kNow - MIN2S(5);
	dueSooner.lastPingedTime = kNow - UDPSERVSTATREASKTIME + MIN2S(10);
	ASSERT_FALSE(RecoveryLastPingedTime(dueSooner, kNow).has_value());

	Facts dueAtTheSameTime = MissingKey();
	dueAtTheSameTime.lastPingedTime = kNow - UDPSERVSTATREASKTIME;
	ASSERT_FALSE(RecoveryLastPingedTime(dueAtTheSameTime, kNow).has_value());
}

TEST(ServerKeyRecovery, LeavesTheScheduleAloneWhenNoKeyCanBeRecovered)
{
	Facts haveKey = MissingKey();
	haveKey.serverKeyUdp = 0x12345678;
	ASSERT_FALSE(RecoveryLastPingedTime(haveKey, kNow).has_value());

	Facts pending = MissingKey();
	pending.cryptPingReplyPending = true;
	ASSERT_FALSE(RecoveryLastPingedTime(pending, kNow).has_value());

	Facts cryptLayerOff = MissingKey();
	cryptLayerOff.cryptLayerUdpEnabled = false;
	ASSERT_FALSE(RecoveryLastPingedTime(cryptLayerOff, kNow).has_value());

	Facts noPublicIp = MissingKey();
	noPublicIp.publicIpKnown = false;
	ASSERT_FALSE(RecoveryLastPingedTime(noPublicIp, kNow).has_value());

	Facts noObfuscation = MissingKey();
	noObfuscation.serverSupportsObfuscationUdp = false;
	ASSERT_FALSE(RecoveryLastPingedTime(noObfuscation, kNow).has_value());
}

TEST(ServerKeyRecovery, ANeverPingedServerIsAlreadyDue)
{
	Facts facts = MissingKey();
	facts.lastPingedTime = 0;
	facts.realLastPingedTime = 0;

	ASSERT_FALSE(RecoveryLastPingedTime(facts, kNow).has_value());
}
