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

#include <set>
#include <vector>

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

namespace
{
struct Fake
{
	std::set<int> alive{ 1 };
	QuicCapabilityState state = QuicCapabilityState::Unknown;
	bool dialStarts = true;
	int capsSent = 0;
	std::vector<int> dialed;
	std::vector<int> failed;
	std::vector<int> utp;
	CQuicCapsHoldoffs<int> holdoffs{ { [this](int &owner) { return alive.count(owner) != 0; },
		[this](const std::array<uint8_t, 16> &, uint64_t) { return state; },
		[this](const CQuicCapsHoldoffs<int>::STarget &) { ++capsSent; },
		[this](const CQuicCapsHoldoffs<int>::STarget &, int &owner, uint64_t) {
			dialed.push_back(owner);
			return dialStarts;
		},
		[this](const CQuicCapsHoldoffs<int>::STarget &, int &owner, uint64_t) {
			failed.push_back(owner);
		},
		[this](int &owner) { utp.push_back(owner); } } };

	Fake() { holdoffs.Add({ {}, 1, 2, {} }, 1000, 1); }
};
} // namespace

DECLARE_SIMPLE(QuicCapsHoldoffs)

TEST(QuicCapsHoldoffs, DropsAnEntryWhoseOwnerIsGone)
{
	Fake f;
	f.alive.clear();
	f.holdoffs.Service(3000);
	ASSERT_TRUE(f.holdoffs.Empty());
	ASSERT_EQUALS(0, f.capsSent);
	ASSERT_TRUE(f.dialed.empty() && f.failed.empty() && f.utp.empty());
}

TEST(QuicCapsHoldoffs, ResendsTwiceThenFallsBackToUtpAtTheDeadline)
{
	Fake f;
	for (uint64_t now = 1000; now < 2500; now += 100) {
		f.holdoffs.Service(now);
	}
	ASSERT_EQUALS(2, f.capsSent);
	ASSERT_TRUE(f.utp.empty());

	f.holdoffs.Service(2500);
	ASSERT_EQUALS(1, static_cast<int>(f.utp.size()));
	ASSERT_TRUE(f.holdoffs.Empty());
	ASSERT_TRUE(f.dialed.empty());
}

TEST(QuicCapsHoldoffs, CompatibleAnswerDialsQuicOnce)
{
	Fake f;
	f.holdoffs.Service(1100);
	f.state = QuicCapabilityState::Compatible;
	f.holdoffs.Service(1200);
	f.holdoffs.Service(1300);
	ASSERT_EQUALS(1, static_cast<int>(f.dialed.size()));
	ASSERT_TRUE(f.failed.empty() && f.utp.empty());
	ASSERT_TRUE(f.holdoffs.Empty());
}

TEST(QuicCapsHoldoffs, QuicDialThatDoesNotStartIsReportedAsFailed)
{
	Fake f;
	f.state = QuicCapabilityState::Compatible;
	f.dialStarts = false;
	f.holdoffs.Service(1100);
	ASSERT_EQUALS(1, static_cast<int>(f.failed.size()));
	ASSERT_TRUE(f.utp.empty());
	ASSERT_TRUE(f.holdoffs.Empty());
}

TEST(QuicCapsHoldoffs, IncompatibleAnswerFallsBackToUtpWithoutDialling)
{
	Fake f;
	f.state = QuicCapabilityState::Incompatible;
	f.holdoffs.Service(1100);
	ASSERT_EQUALS(1, static_cast<int>(f.utp.size()));
	ASSERT_TRUE(f.dialed.empty());
	ASSERT_TRUE(f.holdoffs.Empty());
}

TEST(QuicCapsHoldoffs, AFallbackCanAddAHoldoff)
{
	CQuicCapsHoldoffs<int> *self = nullptr;
	int fallbacks = 0;
	CQuicCapsHoldoffs<int> holdoffs{ { [](int &) { return true; },
		[](const std::array<uint8_t, 16> &, uint64_t) { return QuicCapabilityState::Incompatible; },
		[](const CQuicCapsHoldoffs<int>::STarget &) {},
		[](const CQuicCapsHoldoffs<int>::STarget &, int &, uint64_t) { return true; },
		[](const CQuicCapsHoldoffs<int>::STarget &, int &, uint64_t) {},
		[&](int &) {
			if (fallbacks++ == 0) {
				self->Add({ {}, 1, 2, {} }, 1100, 2);
			}
		} } };
	self = &holdoffs;
	holdoffs.Add({ {}, 1, 2, {} }, 1000, 1);

	holdoffs.Service(1100);
	ASSERT_EQUALS(2, fallbacks);
	ASSERT_TRUE(holdoffs.Empty());
}
