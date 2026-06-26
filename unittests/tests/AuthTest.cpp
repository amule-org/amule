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
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
//

#include <muleunit/test.h>

#include "Auth.h"

#include "Jwt.h"

#include <chrono>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

using namespace muleunit;
using namespace webapi;

DECLARE_SIMPLE(Auth)

// ---------- CRevocationSet ---------------------------------------

TEST(Auth, RevocationSet_FreshIsNotRevoked)
{
	CRevocationSet rs;
	ASSERT_FALSE(rs.IsRevoked("never-seen-jti"));
	ASSERT_EQUALS(static_cast<std::size_t>(0), rs.Size());
}

TEST(Auth, RevocationSet_RevokedJtiSticks)
{
	CRevocationSet rs;
	const std::time_t hour_from_now = std::time(nullptr) + 3600;
	rs.Revoke("abc-jti", hour_from_now);

	ASSERT_TRUE(rs.IsRevoked("abc-jti"));
	ASSERT_FALSE(rs.IsRevoked("def-jti"));
	ASSERT_EQUALS(static_cast<std::size_t>(1), rs.Size());
}

TEST(Auth, RevocationSet_ExpiredEntryGcsOnNextLookup)
{
	CRevocationSet rs;
	// Revoke with exp in the PAST — simulates a token whose JWT lifetime
	// has already elapsed. IsRevoked must drop the entry rather than
	// keep flagging it (no point — the JWT itself would fail Verify).
	const std::time_t two_hours_ago = std::time(nullptr) - 7200;
	rs.Revoke("stale-jti", two_hours_ago);

	ASSERT_FALSE(rs.IsRevoked("stale-jti"));
	ASSERT_EQUALS(static_cast<std::size_t>(0), rs.Size());
}

// ---------- CRateLimiter -----------------------------------------

TEST(Auth, RateLimiter_NoFailuresMeansNoLockout)
{
	CRateLimiter::Config cfg;
	cfg.window_seconds = 60;
	cfg.threshold = 3;
	cfg.lockout_seconds = 60;
	CRateLimiter rl(cfg);
	const auto d = rl.Check("192.0.2.1");
	ASSERT_FALSE(d.locked_out);
}

TEST(Auth, RateLimiter_ThresholdFailuresLockOut)
{
	CRateLimiter::Config cfg;
	cfg.window_seconds = 60;
	cfg.threshold = 3;
	cfg.lockout_seconds = 120;
	CRateLimiter rl(cfg);
	const std::string ip = "192.0.2.2";

	rl.NoteFailure(ip);
	ASSERT_FALSE(rl.Check(ip).locked_out);
	rl.NoteFailure(ip);
	ASSERT_FALSE(rl.Check(ip).locked_out);
	rl.NoteFailure(ip); // third → lockout armed
	const auto d = rl.Check(ip);
	ASSERT_TRUE(d.locked_out);
	ASSERT_TRUE(d.retry_after_seconds > 0);
	ASSERT_TRUE(d.retry_after_seconds <= 120);
}

TEST(Auth, RateLimiter_SuccessClearsBucket)
{
	CRateLimiter::Config cfg;
	cfg.window_seconds = 60;
	cfg.threshold = 2;
	cfg.lockout_seconds = 60;
	CRateLimiter rl(cfg);
	const std::string ip = "192.0.2.3";

	rl.NoteFailure(ip);
	rl.NoteSuccess(ip);
	rl.NoteFailure(ip); // only one failure since the reset

	ASSERT_FALSE(rl.Check(ip).locked_out);
}

TEST(Auth, RateLimiter_DifferentIpsTrackedSeparately)
{
	CRateLimiter::Config cfg;
	cfg.window_seconds = 60;
	cfg.threshold = 2;
	cfg.lockout_seconds = 60;
	CRateLimiter rl(cfg);

	rl.NoteFailure("198.51.100.1");
	rl.NoteFailure("198.51.100.1"); // locks out .1

	ASSERT_TRUE(rl.Check("198.51.100.1").locked_out);
	ASSERT_FALSE(rl.Check("198.51.100.2").locked_out);
}

TEST(Auth, RateLimiter_LockoutExpiresAfterLockoutSeconds)
{
	// Regression: forgetting the "lockout_until <= now → wipe
	// bucket" path would silently jail the affected IP forever.
	// Clock injection lets us step `now` past lockout_until without
	// burning real time on a sleep.
	CRateLimiter::Config cfg;
	cfg.window_seconds = 60;
	cfg.threshold = 1;
	cfg.lockout_seconds = 1;
	std::time_t fake_now = 1000;
	CRateLimiter rl(cfg, [&] { return fake_now; });
	const std::string ip = "203.0.113.7";

	rl.NoteFailure(ip);
	ASSERT_TRUE(rl.Check(ip).locked_out);

	fake_now += 2;

	const auto d = rl.Check(ip);
	ASSERT_FALSE(d.locked_out);
	ASSERT_EQUALS(static_cast<std::time_t>(0), d.retry_after_seconds);
}

TEST(Auth, RateLimiter_SlidingWindowSplitAttemptsStillLockOut)
{
	// Regression check for the original tumbling-window bug:
	// threshold-1 failures in the tail of window N + threshold-1 in
	// the head of window N+1 never tripped lockout because the old
	// code reset failure_count whenever `now - window_start >
	// window_seconds`. The current per-stamp eviction keeps any
	// failure within `window_seconds` live in the count.
	//
	// Sequence (window=3, threshold=3) — clock-injected:
	//   t=0  NoteFailure  → failures=[0],          count=1
	//   t=3  NoteFailure  → failures=[0, 3],       count=2
	//   t=4  NoteFailure  → failures=[3, 4],       count=2  (evict<1)
	//   t=5  NoteFailure  → failures=[3, 4, 5],    count=3 → LOCKOUT
	CRateLimiter::Config cfg;
	cfg.window_seconds = 3;
	cfg.threshold = 3;
	cfg.lockout_seconds = 60;
	std::time_t fake_now = 0;
	CRateLimiter rl(cfg, [&] { return fake_now; });
	const std::string ip = "203.0.113.9";

	rl.NoteFailure(ip); // t=0
	fake_now = 3;
	rl.NoteFailure(ip); // t=3
	fake_now = 4;
	rl.NoteFailure(ip); // t=4 (boundary crossing)
	fake_now = 5;
	rl.NoteFailure(ip); // t=5 (sliding count reaches 3)
	ASSERT_TRUE(rl.Check(ip).locked_out);
}

// ---------- Revocation × Verify cross-test -----------------------

// Each side is unit-tested separately. This case wires the two
// together: issue a token, mark its `jti` revoked, then verify
// the token's body — Verify itself returns true (the token is
// structurally valid and the MAC matches), but the caller must
// consult CRevocationSet AFTER Verify and refuse if the jti is
// listed. A regression where IsRevoked() short-circuits or where
// Verify silently incorporates the revocation set would slip past
// each component's own tests; this one would catch it.
TEST(Auth, RevocationListBlocksOtherwiseValidToken)
{
	const std::vector<unsigned char> secret(32, 0xC1);
	CJwt jwt(secret);
	const CJwt::IssuedToken issued = jwt.Issue(Role::ADMIN);

	CJwt::VerifyResult vr;
	ASSERT_TRUE(jwt.Verify(issued.token, vr));
	ASSERT_EQUALS(static_cast<int>(Role::ADMIN), static_cast<int>(vr.role));

	CRevocationSet rev;
	ASSERT_FALSE(rev.IsRevoked(vr.jti));

	rev.Revoke(vr.jti, vr.exp);
	ASSERT_TRUE(rev.IsRevoked(vr.jti));

	// A second Verify of the same token still passes (cryptography
	// is independent of the revocation list). The auth gate's
	// contract is: Verify FIRST, then check IsRevoked, and refuse
	// the request if either step rejects.
	CJwt::VerifyResult vr2;
	ASSERT_TRUE(jwt.Verify(issued.token, vr2));
	ASSERT_EQUALS(vr.jti, vr2.jti);
	ASSERT_TRUE(rev.IsRevoked(vr2.jti));
}

// ---------- Token extraction -------------------------------------

TEST(Auth, ExtractBearerToken_HappyPath)
{
	const std::string token = ExtractBearerToken("Bearer abc.def.ghi");
	ASSERT_EQUALS(std::string("abc.def.ghi"), token);
}

TEST(Auth, ExtractBearerToken_CaseInsensitiveScheme)
{
	ASSERT_EQUALS(std::string("xyz"), ExtractBearerToken("bearer xyz"));
	ASSERT_EQUALS(std::string("xyz"), ExtractBearerToken("BEARER xyz"));
}

TEST(Auth, ExtractBearerToken_RejectsMissingValue)
{
	ASSERT_TRUE(ExtractBearerToken("Bearer ").empty());
	ASSERT_TRUE(ExtractBearerToken("Bearer").empty());
	ASSERT_TRUE(ExtractBearerToken("Basic dXNlcjpwYXNz").empty());
	ASSERT_TRUE(ExtractBearerToken("").empty());
}

TEST(Auth, ExtractBearerToken_TolerantOfExtraSpaces)
{
	// Multiple OWS chars between scheme and credentials per RFC 7230 §3.2.3.
	ASSERT_EQUALS(std::string("tok"), ExtractBearerToken("Bearer   tok"));
}

TEST(Auth, ExtractCookieValue_HappyPath)
{
	ASSERT_EQUALS(std::string("xyz"), ExtractCookieValue("amuleapi_token=xyz", "amuleapi_token"));
}

TEST(Auth, ExtractCookieValue_OtherCookiesInWay)
{
	const std::string header = "foo=1; amuleapi_token=abc; bar=2";
	ASSERT_EQUALS(std::string("abc"), ExtractCookieValue(header, "amuleapi_token"));
}

TEST(Auth, ExtractCookieValue_MissingReturnsEmpty)
{
	ASSERT_TRUE(ExtractCookieValue("foo=1; bar=2", "amuleapi_token").empty());
	ASSERT_TRUE(ExtractCookieValue("", "amuleapi_token").empty());
}

// ---------- ISO-8601 ---------------------------------------------

TEST(Auth, FormatIso8601Utc_KnownTimestamp)
{
	// 1768523696 = 2026-01-16T00:34:56Z (verified via
	// `date -r 1768523696 -u`); a single point exercises the standard
	// year/month/day/hour/min/sec formatting path.
	const std::time_t t = 1768523696;
	ASSERT_EQUALS(std::string("2026-01-16T00:34:56Z"), FormatIso8601Utc(t));
}

TEST(Auth, FormatIso8601Utc_FixedWidth)
{
	// 1735734005 = 2025-01-01T12:20:05Z (verified via
	// `date -r 1735734005 -u`). Single-digit month / day / second
	// here all need leading zeros — pinning length=20 + trailing
	// 'Z' catches any %d → %2d regression.
	const std::time_t t = 1735734005;
	const std::string s = FormatIso8601Utc(t);
	ASSERT_EQUALS(static_cast<size_t>(20), s.size());
	ASSERT_EQUALS('Z', s.back());
	ASSERT_EQUALS(std::string("2025-01-01T12:20:05Z"), s);
}
