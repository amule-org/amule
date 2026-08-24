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

// --- Per-coding validators ------------------------------------------
//
// The body hash is taken before compression, so both codings of a resource
// derive from one hash. A strong validator names ONE representation, so the
// selected coding is appended to the wire value and the conditional-GET
// comparison runs against THAT value. An earlier cut matched either coding,
// which defeats the suffix entirely: a client holding gzip bytes and asking
// for identity was told its copy was current.
TEST(Etag, CodingSuffixDistinguishesTheTwoRepresentations)
{
	const std::string bare = "a1b2c3d4";
	const std::string coded = bare + webcommon::kGzipEtagSuffix;
	ASSERT_TRUE(bare != coded);
	// Each validator matches its own representation...
	ASSERT_TRUE(webcommon::IfNoneMatchHits("\"a1b2c3d4\"", bare));
	ASSERT_TRUE(webcommon::IfNoneMatchHits("\"a1b2c3d4-gzip\"", coded));
	// ...and NOT the other one. This is the whole purpose of the suffix.
	ASSERT_TRUE(!webcommon::IfNoneMatchHits("\"a1b2c3d4-gzip\"", bare));
	ASSERT_TRUE(!webcommon::IfNoneMatchHits("\"a1b2c3d4\"", coded));
}

// The grammar still applies to whichever representation was selected: `*`,
// weak validators and comma-separated lists all work against the coded form.
TEST(Etag, CodedValidatorKeepsTheFullGrammar)
{
	const std::string coded = std::string("a1b2c3d4") + webcommon::kGzipEtagSuffix;
	ASSERT_TRUE(webcommon::IfNoneMatchHits("*", coded));
	ASSERT_TRUE(webcommon::IfNoneMatchHits("W/\"a1b2c3d4-gzip\"", coded));
	ASSERT_TRUE(webcommon::IfNoneMatchHits("\"nope\", \"a1b2c3d4-gzip\"", coded));
	ASSERT_TRUE(!webcommon::IfNoneMatchHits("\"deadbeef-gzip\"", coded));
}

// The suffix is a marker on one body's validator, not a wildcard that joins
// two different bodies.
TEST(Etag, CodingSuffixIsNotAWildcard)
{
	ASSERT_TRUE(!webcommon::IfNoneMatchHits("\"-gzip\"", "a1b2c3d4"));
	ASSERT_TRUE(!webcommon::IfNoneMatchHits("", "a1b2c3d4"));
}

// --- Naming a representation ----------------------------------------
//
// WithCodingSuffix is asked which coding the value must NAME, not whether to
// add or remove a suffix. Three call sites used to spell that edit out by
// hand -- two that could only add and one that could only add-or-strip -- and
// the difference is what let a failed deflate ship a gzip validator on
// identity bytes.
TEST(Etag, CodingSuffixNamesTheSelectedRepresentation)
{
	ASSERT_TRUE(webcommon::WithCodingSuffix("a1b2c3d4", true) == "a1b2c3d4-gzip");
	ASSERT_TRUE(webcommon::WithCodingSuffix("a1b2c3d4", false) == "a1b2c3d4");
	// The quoted form the static path carries: the suffix belongs on the
	// opaque payload, INSIDE the quotes, or the value stops being a valid
	// entity-tag and no client matches it again.
	ASSERT_TRUE(webcommon::WithCodingSuffix("\"1f-2a3b\"", true) == "\"1f-2a3b-gzip\"");
	ASSERT_TRUE(webcommon::WithCodingSuffix("\"1f-2a3b-gzip\"", false) == "\"1f-2a3b\"");
}

// The transport calls this on a value the dispatcher may already have
// stamped, so asking for the coding that is already named must not double it.
TEST(Etag, CodingSuffixIsIdempotent)
{
	ASSERT_TRUE(webcommon::WithCodingSuffix("a1b2c3d4-gzip", true) == "a1b2c3d4-gzip");
	ASSERT_TRUE(webcommon::WithCodingSuffix("\"a1b2c3d4-gzip\"", true) == "\"a1b2c3d4-gzip\"");
	ASSERT_TRUE(webcommon::WithCodingSuffix("a1b2c3d4", false) == "a1b2c3d4");
}

// The reason the helper exists. GzipOnce can fail -- deflateInit2 or deflate
// returning short -- after the dispatcher has already predicted compression
// and stamped the suffix. The body then ships as identity, and the validator
// has to come back down with it, or a cache stores identity bytes under the
// gzip validator and serves them to a client that asked for gzip.
TEST(Etag, AFailedCompressionTakesTheSuffixBackOff)
{
	const std::string predicted = webcommon::WithCodingSuffix("a1b2c3d4", true);
	ASSERT_TRUE(predicted == "a1b2c3d4-gzip");
	// ...deflate fails, so the coding that actually shipped is identity.
	const std::string shipped = webcommon::WithCodingSuffix(predicted, false);
	ASSERT_TRUE(shipped == "a1b2c3d4");
	// And the identity validator is what an identity client will send back.
	ASSERT_TRUE(webcommon::IfNoneMatchHits("\"a1b2c3d4\"", shipped));
	ASSERT_TRUE(!webcommon::IfNoneMatchHits("\"a1b2c3d4-gzip\"", shipped));
}

// A body whose hash happens to end in the suffix text is not "already coded".
// Guarded because the check is a suffix compare on the payload: if the digest
// alphabet ever widened past hex, `...-gzip` could occur naturally and a
// wrongly-detected prediction would strip a byte off a real validator.
TEST(Etag, CodingSuffixLooksOnlyAtTheEndOfThePayload)
{
	ASSERT_TRUE(webcommon::WithCodingSuffix("-gzipa1b2", true) == "-gzipa1b2-gzip");
	ASSERT_TRUE(webcommon::WithCodingSuffix("-gzipa1b2", false) == "-gzipa1b2");
	// Degenerate inputs must not underflow the erase.
	ASSERT_TRUE(webcommon::WithCodingSuffix("", false).empty());
	ASSERT_TRUE(webcommon::WithCodingSuffix("", true) == "-gzip");
}
