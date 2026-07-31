//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// Any parts of this program derived from the xMule, lMule or eMule project,
// or contributed by third-party developers are copyrighted by their
// respective authors.
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

#include <common/RateLimiter.h>
#include <muleunit/test.h>

using namespace muleunit;

// The External Connection listener's shipped defaults, copied here rather than
// read from Preferences: the limiter is a standalone class and pulling in the
// preferences machinery would need a running app.
//
// So this fixture asserts the limiter behaves correctly *at the numbers EC
// ships with* -- it does not verify that Preferences.cpp still registers those
// numbers. Changing a default there and not here leaves this suite green. What
// it does catch is a regression in the limiter itself: AuthTest exercises the
// same class with small synthetic values, and these cases add the ones EC
// depends on, including that the window slides rather than tumbles.
namespace
{
const unsigned EC_WINDOW = 60;
const unsigned EC_THRESHOLD = 10;
const unsigned EC_LOCKOUT = 300;

CRateLimiter::Config EcDefaults()
{
	CRateLimiter::Config cfg;
	cfg.window_seconds = EC_WINDOW;
	cfg.threshold = EC_THRESHOLD;
	cfg.lockout_seconds = EC_LOCKOUT;
	return cfg;
}
} // namespace

DECLARE_SIMPLE(RateLimiterTest)

TEST(RateLimiterTest, AllowsNineFailuresThenLocksOnTheTenth)
{
	std::time_t now = 1000;
	CRateLimiter rl(EcDefaults(), [&] { return now; });

	// The threshold is the count that arms the lockout, so the first nine
	// failures must each leave the next attempt permitted.
	for (unsigned i = 0; i < EC_THRESHOLD - 1; ++i) {
		rl.NoteFailure("10.0.0.1");
		ASSERT_FALSE(rl.Check("10.0.0.1").locked_out);
	}

	rl.NoteFailure("10.0.0.1");
	ASSERT_TRUE(rl.Check("10.0.0.1").locked_out);
}

TEST(RateLimiterTest, ReportsTheFullLockoutOnTheFirstRefusal)
{
	std::time_t now = 1000;
	CRateLimiter rl(EcDefaults(), [&] { return now; });

	for (unsigned i = 0; i < EC_THRESHOLD; ++i) {
		rl.NoteFailure("10.0.0.1");
	}
	// The daemon puts this number in front of the user, so it has to be the
	// real remaining time rather than a constant.
	ASSERT_EQUALS((std::time_t)EC_LOCKOUT, rl.Check("10.0.0.1").retry_after_seconds);

	now += 100;
	ASSERT_EQUALS((std::time_t)(EC_LOCKOUT - 100), rl.Check("10.0.0.1").retry_after_seconds);
}

TEST(RateLimiterTest, LockoutExpiresAndTheAddressIsUsableAgain)
{
	std::time_t now = 1000;
	CRateLimiter rl(EcDefaults(), [&] { return now; });

	for (unsigned i = 0; i < EC_THRESHOLD; ++i) {
		rl.NoteFailure("10.0.0.1");
	}
	ASSERT_TRUE(rl.Check("10.0.0.1").locked_out);

	now += EC_LOCKOUT + 1;
	ASSERT_FALSE(rl.Check("10.0.0.1").locked_out);
}

TEST(RateLimiterTest, SuccessClearsTheStreak)
{
	std::time_t now = 1000;
	CRateLimiter rl(EcDefaults(), [&] { return now; });

	// A user who mistypes, gets in, then mistypes again must not be locked
	// out by the two streaks adding up: nine plus nine is well past the
	// threshold, but the success in between resets the count.
	for (unsigned i = 0; i < EC_THRESHOLD - 1; ++i) {
		rl.NoteFailure("10.0.0.1");
	}
	rl.NoteSuccess("10.0.0.1");
	for (unsigned i = 0; i < EC_THRESHOLD - 1; ++i) {
		rl.NoteFailure("10.0.0.1");
	}
	ASSERT_FALSE(rl.Check("10.0.0.1").locked_out);
}

TEST(RateLimiterTest, BucketsAreIndependentPerAddress)
{
	std::time_t now = 1000;
	CRateLimiter rl(EcDefaults(), [&] { return now; });

	for (unsigned i = 0; i < EC_THRESHOLD; ++i) {
		rl.NoteFailure("10.0.0.1");
	}
	ASSERT_TRUE(rl.Check("10.0.0.1").locked_out);
	// One guesser must not lock everyone else out of the daemon.
	ASSERT_FALSE(rl.Check("10.0.0.2").locked_out);
}

TEST(RateLimiterTest, WindowSlidesRatherThanTumbling)
{
	std::time_t now = 1000;
	CRateLimiter rl(EcDefaults(), [&] { return now; });

	// The failure that matters is the one that ages out. Spend the whole
	// allowance, let a single stamp expire, and only one more failure should
	// fit before the lockout arms again -- a tumbling window would have
	// cleared the entire count at the boundary and allowed the full
	// allowance a second time.
	for (unsigned i = 0; i < EC_THRESHOLD - 1; ++i) {
		rl.NoteFailure("10.0.0.1");
	}
	now += EC_WINDOW + 1;

	for (unsigned i = 0; i < EC_THRESHOLD - 1; ++i) {
		rl.NoteFailure("10.0.0.1");
		ASSERT_FALSE(rl.Check("10.0.0.1").locked_out);
	}
	rl.NoteFailure("10.0.0.1");
	ASSERT_TRUE(rl.Check("10.0.0.1").locked_out);
}
