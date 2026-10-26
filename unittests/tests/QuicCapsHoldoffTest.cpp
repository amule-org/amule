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

#include <QuicCapabilityCache.h>

using namespace muleunit;
DECLARE_SIMPLE(QuicCapsHoldoff)

TEST(QuicCapsHoldoff, CompatibleResolvesQuicRegardlessOfTimingOrProbeCount)
{
	auto decision = DecideQuicCapsHoldoff(QuicCapabilityState::Compatible, 1, 0);
	ASSERT_TRUE(decision.resolveQuic);
	ASSERT_FALSE(decision.resolveUtp);
	ASSERT_FALSE(decision.resend);

	decision = DecideQuicCapsHoldoff(QuicCapabilityState::Compatible, 3, 10000);
	ASSERT_TRUE(decision.resolveQuic);
}

TEST(QuicCapsHoldoff, IncompatibleResolvesUtpImmediately)
{
	const auto decision = DecideQuicCapsHoldoff(QuicCapabilityState::Incompatible, 1, 0);
	ASSERT_TRUE(decision.resolveUtp);
	ASSERT_FALSE(decision.resolveQuic);
	ASSERT_FALSE(decision.resend);
}

TEST(QuicCapsHoldoff, CooldownResolvesUtpImmediately)
{
	const auto decision = DecideQuicCapsHoldoff(QuicCapabilityState::Cooldown, 1, 0);
	ASSERT_TRUE(decision.resolveUtp);
}

TEST(QuicCapsHoldoff, UnknownWaitsBeforeTheFirstResendWindow)
{
	const auto decision = DecideQuicCapsHoldoff(QuicCapabilityState::Unknown, 1, 499);
	ASSERT_FALSE(decision.resend);
	ASSERT_FALSE(decision.resolveQuic);
	ASSERT_FALSE(decision.resolveUtp);
}

TEST(QuicCapsHoldoff, UnknownResendsAtEachFiveHundredMsWindowUpToThreeProbesTotal)
{
	// Probe 1 was already sent when the holdoff was registered; the tick loop only ever
	// sees probesSent >= 1, and must produce exactly two more resends (probes 2 and 3).
	auto decision = DecideQuicCapsHoldoff(QuicCapabilityState::Unknown, 1, 500);
	ASSERT_TRUE(decision.resend);

	decision = DecideQuicCapsHoldoff(QuicCapabilityState::Unknown, 2, 1000);
	ASSERT_TRUE(decision.resend);

	// A third probe is never sent: three total (one initial plus these two) is the eMuleAI
	// schedule being mirrored.
	decision = DecideQuicCapsHoldoff(QuicCapabilityState::Unknown, 3, 1200);
	ASSERT_FALSE(decision.resend);
	ASSERT_FALSE(decision.resolveQuic);
	ASSERT_FALSE(decision.resolveUtp);
}

TEST(QuicCapsHoldoff, UnknownResolvesUtpAtAndAfterTheDeadline)
{
	auto decision = DecideQuicCapsHoldoff(QuicCapabilityState::Unknown, 3, 1500);
	ASSERT_TRUE(decision.resolveUtp);
	ASSERT_FALSE(decision.resend);

	decision = DecideQuicCapsHoldoff(QuicCapabilityState::Unknown, 1, 5000);
	ASSERT_TRUE(decision.resolveUtp);
}

TEST(QuicCapsHoldoff, UnknownJustBeforeTheDeadlineStillWaits)
{
	const auto decision = DecideQuicCapsHoldoff(QuicCapabilityState::Unknown, 3, 1499);
	ASSERT_FALSE(decision.resend);
	ASSERT_FALSE(decision.resolveQuic);
	ASSERT_FALSE(decision.resolveUtp);
}
