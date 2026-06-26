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

#include "Etag.h"

#include <string>

using namespace muleunit;
using namespace webcommon;

DECLARE_SIMPLE(Etag)

// ----------------------------------------------------------------------
// `Etag()` — SHA-256 truncated to 8 bytes (16 hex chars).
// ----------------------------------------------------------------------

TEST(Etag, BareHexLength)
{
	// 16 hex chars regardless of body length — the truncation is the
	// wire contract that prevents header bloat.
	ASSERT_EQUALS(static_cast<size_t>(16), Etag("").size());
	ASSERT_EQUALS(static_cast<size_t>(16), Etag("x").size());
	ASSERT_EQUALS(static_cast<size_t>(16), Etag(std::string(1024 * 1024, 'A')).size());
}

TEST(Etag, EmptyBodyKnownDigest)
{
	// SHA-256("") truncated to 8 bytes, lowercase hex.
	// Reference: `printf '' | shasum -a 256` →
	// "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
	// Leading 16 hex chars = "e3b0c44298fc1c14".
	ASSERT_EQUALS(std::string("e3b0c44298fc1c14"), Etag(""));
}

TEST(Etag, DistinctBodiesProduceDistinctEtags)
{
	// Sanity: the truncation didn't accidentally collapse common
	// short payloads to the same digest.
	ASSERT_TRUE(Etag("a") != Etag("b"));
	ASSERT_TRUE(Etag("{\"ok\":true}") != Etag("{\"ok\":false}"));
}

// ----------------------------------------------------------------------
// `IfNoneMatchHits()` — fix for the bare-vs-quoted asymmetry.
// ----------------------------------------------------------------------

TEST(Etag, IfNoneMatchEmptyHeaderNoHit)
{
	// Absent header → cannot be a match.
	ASSERT_FALSE(IfNoneMatchHits("", "deadbeefdeadbeef"));
}

TEST(Etag, IfNoneMatchBareHexHits)
{
	// Bare-vs-bare compare must hit — backward compatibility for
	// clients that send unquoted validators.
	ASSERT_TRUE(IfNoneMatchHits("deadbeefdeadbeef", "deadbeefdeadbeef"));
}

TEST(Etag, IfNoneMatchQuotedHexHits)
{
	// RFC 7232 §2.3-canonical form: `"<hex>"`. This was the latent
	// bug — strictly-RFC clients sending the quoted form never got
	// 304 from the prior implementation.
	ASSERT_TRUE(IfNoneMatchHits("\"deadbeefdeadbeef\"", "deadbeefdeadbeef"));
}

TEST(Etag, IfNoneMatchWeakValidatorHits)
{
	// `W/"<hex>"`: weak validator. For conditional GETs we treat
	// weak and strong as equivalent (Section 2.3.2 — opaque payload
	// equality is what matters for 304 semantics).
	ASSERT_TRUE(IfNoneMatchHits("W/\"deadbeefdeadbeef\"", "deadbeefdeadbeef"));
}

TEST(Etag, IfNoneMatchWildcardHits)
{
	// `*` matches any existing representation per RFC §3.2.
	ASSERT_TRUE(IfNoneMatchHits("*", "deadbeefdeadbeef"));
}

TEST(Etag, IfNoneMatchListAnyMatchWins)
{
	// Comma-separated list — any matching entry returns true.
	ASSERT_TRUE(IfNoneMatchHits(
		"\"someotheretag\", \"deadbeefdeadbeef\", \"yetanother\"", "deadbeefdeadbeef"));
	// Even with mixed strong/weak/bare.
	ASSERT_TRUE(IfNoneMatchHits("W/\"first\", deadbeefdeadbeef", "deadbeefdeadbeef"));
}

TEST(Etag, IfNoneMatchListNoMatchMisses)
{
	// None of the entries match → no hit.
	ASSERT_FALSE(IfNoneMatchHits("\"someotheretag\", \"yetanother\"", "deadbeefdeadbeef"));
}

TEST(Etag, IfNoneMatchWhitespaceTolerated)
{
	// Surrounding whitespace within list entries is stripped.
	ASSERT_TRUE(IfNoneMatchHits("   \"deadbeefdeadbeef\"   ", "deadbeefdeadbeef"));
}

TEST(Etag, IfNoneMatchHexMismatchMisses)
{
	// Different hex payload → no hit even with right shape.
	ASSERT_FALSE(IfNoneMatchHits("\"feedfacefeedface\"", "deadbeefdeadbeef"));
}

TEST(Etag, IfNoneMatchHexCaseSensitive)
{
	// RFC §2.3.2: opaque-string equality. We emit lowercase hex on
	// the response side; clients echoing the value back must also
	// send lowercase. Uppercase variant → no hit.
	ASSERT_FALSE(IfNoneMatchHits("DEADBEEFDEADBEEF", "deadbeefdeadbeef"));
}
