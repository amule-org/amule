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
#include "Jwt.h"

#include <cryptopp/hmac.h>
#include <cryptopp/sha.h>

#include <ctime>
#include <set>
#include <string>
#include <vector>

using namespace muleunit;

DECLARE(Jwt)
std::vector<unsigned char> MakeSecret(unsigned char fill)
{
	return std::vector<unsigned char>(32, fill);
}
END_DECLARE;

TEST(Jwt, IssueProducesThreeDottedParts)
{
	CJwt auth(MakeSecret(0xAB));
	const std::string token = auth.Issue(Role::ADMIN).token;
	// Header.Payload.Signature — exactly two dots.
	int dots = 0;
	for (size_t i = 0; i < token.size(); ++i) {
		if (token[i] == '.')
			++dots;
	}
	ASSERT_EQUALS(2, dots);
	ASSERT_TRUE(!token.empty());
}

TEST(Jwt, RoundtripAdmin)
{
	CJwt auth(MakeSecret(0x11));
	const CJwt::IssuedToken issued = auth.Issue(Role::ADMIN);

	CJwt::VerifyResult r;
	ASSERT_TRUE(auth.Verify(issued.token, r));
	ASSERT_TRUE(r.role == Role::ADMIN);
	ASSERT_EQUALS(issued.expires_at, r.exp);
}

TEST(Jwt, RoundtripGuest)
{
	CJwt auth(MakeSecret(0x22));
	const CJwt::IssuedToken issued = auth.Issue(Role::GUEST);

	CJwt::VerifyResult r;
	ASSERT_TRUE(auth.Verify(issued.token, r));
	ASSERT_TRUE(r.role == Role::GUEST);
}

TEST(Jwt, ExpiryIs24Hours)
{
	CJwt auth(MakeSecret(0x33));
	const std::time_t before = std::time(nullptr);
	const CJwt::IssuedToken issued = auth.Issue(Role::ADMIN);
	const std::time_t after = std::time(nullptr);

	// expires_at must be between [before+86400, after+86400] — same-
	// second tolerance for the clock tick.
	const std::time_t lifetime = 24 * 60 * 60;
	ASSERT_TRUE(issued.expires_at >= before + lifetime);
	ASSERT_TRUE(issued.expires_at <= after + lifetime);
}

TEST(Jwt, IssueEmitsJti)
{
	CJwt auth(MakeSecret(0x10));
	const CJwt::IssuedToken issued = auth.Issue(Role::ADMIN);
	// 128-bit jti → 22 base64url chars (16 bytes ÷ 3 × 4 = 21.33 → 22).
	ASSERT_FALSE(issued.jti.empty());
	ASSERT_EQUALS(static_cast<size_t>(22), issued.jti.size());
}

TEST(Jwt, IssueProducesUniqueJti)
{
	// 128 random bits gives 1-in-2^64 collision odds across a single
	// instance's lifetime. Across 1000 issues we'd need ~2^77 calls
	// before a collision becomes likely; 1000 is comfortably in the
	// "never sees a dupe" range and catches any RNG-reset bug.
	CJwt auth(MakeSecret(0x12));
	std::set<std::string> seen;
	for (int i = 0; i < 1000; ++i) {
		const std::string jti = auth.Issue(Role::ADMIN).jti;
		ASSERT_TRUE(seen.insert(jti).second);
	}
}

TEST(Jwt, VerifyRecoversJti)
{
	CJwt auth(MakeSecret(0x13));
	const CJwt::IssuedToken issued = auth.Issue(Role::ADMIN);

	CJwt::VerifyResult r;
	ASSERT_TRUE(auth.Verify(issued.token, r));
	ASSERT_EQUALS(issued.jti, r.jti);
}

TEST(Jwt, TamperedSignatureRejected)
{
	CJwt auth(MakeSecret(0x44));
	std::string token = auth.Issue(Role::ADMIN).token;

	// Flip the last character of the signature. base64url alphabet
	// rotation: any non-equal char.
	token.back() = (token.back() == 'A') ? 'B' : 'A';

	CJwt::VerifyResult r;
	ASSERT_FALSE(auth.Verify(token, r));
}

TEST(Jwt, TamperedPayloadRejected)
{
	CJwt auth(MakeSecret(0x55));
	std::string token = auth.Issue(Role::GUEST).token;

	// Flip a payload byte (between the two dots) so the HMAC mismatches.
	const size_t first_dot = token.find('.');
	const size_t second_dot = token.find('.', first_dot + 1);
	const size_t mid = (first_dot + second_dot) / 2;
	token[mid] = (token[mid] == 'X') ? 'Y' : 'X';

	CJwt::VerifyResult r;
	ASSERT_FALSE(auth.Verify(token, r));
}

TEST(Jwt, WrongSecretRejected)
{
	CJwt issuer(MakeSecret(0x66));
	CJwt verifier(MakeSecret(0x67)); // different secret

	const std::string token = issuer.Issue(Role::ADMIN).token;
	CJwt::VerifyResult r;
	ASSERT_FALSE(verifier.Verify(token, r));
}

TEST(Jwt, MalformedNoDotsRejected)
{
	CJwt auth(MakeSecret(0x77));
	CJwt::VerifyResult r;
	ASSERT_FALSE(auth.Verify("nodotshere", r));
	ASSERT_FALSE(auth.Verify("only.one", r));
	ASSERT_FALSE(auth.Verify("too.many.dots.here", r));
	ASSERT_FALSE(auth.Verify("", r));
}

TEST(Jwt, MalformedBase64Rejected)
{
	// Each section has invalid base64url chars (`=` / `+` aren't in
	// the b64url alphabet). The test pins "malformed input gets
	// rejected" without claiming WHICH layer caught it — currently
	// the MAC compare bails on length mismatch, but if you reorder
	// the verify pipeline the test still holds.
	CJwt auth(MakeSecret(0x88));
	CJwt::VerifyResult r;
	ASSERT_FALSE(auth.Verify("!!!.bbb.ccc", r));
	ASSERT_FALSE(auth.Verify("aaa.!!!.ccc", r));
	ASSERT_FALSE(auth.Verify("aaa.bbb.!!!", r));
}

// --- Header-validation tests (alg-confusion defence) ----------------
//
// These tests build a custom JWT byte-for-byte: an arbitrary header
// + payload + the *correct* HMAC-SHA-256 signature using the test
// secret. That means the MAC compare succeeds and we reach the
// header alg/typ check inside Verify(). If the check is ever
// regressed (e.g. someone removes it for "performance"), these
// tests start failing.

namespace
{

const char b64_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string Base64UrlEncodeForTest(const unsigned char *data, size_t len)
{
	std::string out;
	out.reserve(((len + 2) / 3) * 4);
	for (size_t i = 0; i < len; i += 3) {
		const unsigned b0 = data[i];
		const unsigned b1 = (i + 1 < len) ? data[i + 1] : 0;
		const unsigned b2 = (i + 2 < len) ? data[i + 2] : 0;
		out += b64_alphabet[(b0 >> 2) & 0x3f];
		out += b64_alphabet[((b0 << 4) | (b1 >> 4)) & 0x3f];
		if (i + 1 < len)
			out += b64_alphabet[((b1 << 2) | (b2 >> 6)) & 0x3f];
		if (i + 2 < len)
			out += b64_alphabet[b2 & 0x3f];
	}
	return out;
}

std::string CraftToken(const std::vector<unsigned char> &secret,
	const std::string &header_json,
	const std::string &payload_json)
{
	const std::string h_b64 = Base64UrlEncodeForTest(
		reinterpret_cast<const unsigned char *>(header_json.data()), header_json.size());
	const std::string p_b64 = Base64UrlEncodeForTest(
		reinterpret_cast<const unsigned char *>(payload_json.data()), payload_json.size());
	const std::string signing_input = h_b64 + "." + p_b64;
	unsigned char mac[CryptoPP::SHA256::DIGESTSIZE];
	CryptoPP::HMAC<CryptoPP::SHA256> hmac(secret.empty() ? nullptr : secret.data(), secret.size());
	hmac.Update(reinterpret_cast<const unsigned char *>(signing_input.data()), signing_input.size());
	hmac.Final(mac);
	const std::string sig = Base64UrlEncodeForTest(mac, sizeof(mac));
	return signing_input + "." + sig;
}

} // namespace

TEST(Jwt, AlgNoneRejectedEvenWithValidHs256Mac)
{
	const auto secret = MakeSecret(0x99);
	CJwt auth(secret);
	// Header says alg:none, signature is a valid HS256 MAC against the
	// test secret. Verify must still reject because alg != HS256.
	const std::string token = CraftToken(secret,
		"{\"alg\":\"none\",\"typ\":\"JWT\"}",
		"{\"role\":\"admin\",\"exp\":9999999999,\"jti\":\"t\"}");
	CJwt::VerifyResult r;
	ASSERT_FALSE(auth.Verify(token, r));
}

TEST(Jwt, AlgHs512RejectedEvenWithValidHs256Mac)
{
	const auto secret = MakeSecret(0xAA);
	CJwt auth(secret);
	const std::string token = CraftToken(secret,
		"{\"alg\":\"HS512\",\"typ\":\"JWT\"}",
		"{\"role\":\"admin\",\"exp\":9999999999,\"jti\":\"t\"}");
	CJwt::VerifyResult r;
	ASSERT_FALSE(auth.Verify(token, r));
}

TEST(Jwt, AlgRs256RejectedEvenWithValidHs256Mac)
{
	const auto secret = MakeSecret(0xBB);
	CJwt auth(secret);
	const std::string token = CraftToken(secret,
		"{\"alg\":\"RS256\",\"typ\":\"JWT\"}",
		"{\"role\":\"admin\",\"exp\":9999999999,\"jti\":\"t\"}");
	CJwt::VerifyResult r;
	ASSERT_FALSE(auth.Verify(token, r));
}

TEST(Jwt, MissingAlgRejected)
{
	const auto secret = MakeSecret(0xCC);
	CJwt auth(secret);
	const std::string token = CraftToken(
		secret, "{\"typ\":\"JWT\"}", "{\"role\":\"admin\",\"exp\":9999999999,\"jti\":\"t\"}");
	CJwt::VerifyResult r;
	ASSERT_FALSE(auth.Verify(token, r));
}

TEST(Jwt, WrongTypRejected)
{
	const auto secret = MakeSecret(0xDD);
	CJwt auth(secret);
	// typ is optional in RFC 7519, but if present it MUST be "JWT".
	const std::string token = CraftToken(secret,
		"{\"alg\":\"HS256\",\"typ\":\"not-a-jwt\"}",
		"{\"role\":\"admin\",\"exp\":9999999999,\"jti\":\"t\"}");
	CJwt::VerifyResult r;
	ASSERT_FALSE(auth.Verify(token, r));
}

TEST(Jwt, ExpInPastRejected)
{
	const auto secret = MakeSecret(0xEE);
	CJwt auth(secret);
	// Yesterday — Verify must reject expired tokens regardless of MAC.
	const std::time_t yesterday = std::time(nullptr) - 86400;
	const std::string token = CraftToken(secret,
		"{\"alg\":\"HS256\",\"typ\":\"JWT\"}",
		std::string("{\"role\":\"admin\",\"exp\":") + std::to_string(yesterday) + ",\"jti\":\"t\"}");
	CJwt::VerifyResult r;
	ASSERT_FALSE(auth.Verify(token, r));
}

TEST(Jwt, MalformedPayloadJsonRejected)
{
	const auto secret = MakeSecret(0xFF);
	CJwt auth(secret);
	// Valid HS256 MAC but the payload isn't valid JSON.
	const std::string token =
		CraftToken(secret, "{\"alg\":\"HS256\",\"typ\":\"JWT\"}", "not json at all");
	CJwt::VerifyResult r;
	ASSERT_FALSE(auth.Verify(token, r));
}

TEST(Jwt, MissingJtiRejected)
{
	// Crafted token with a valid HS256 MAC, valid header, valid
	// role/exp — but no `jti` claim. amuleapi's revocation list
	// keys off jti, so a token without one would create a hole in
	// the revocation check; Verify() must refuse to admit one.
	const auto secret = MakeSecret(0x01);
	CJwt auth(secret);
	const std::string token = CraftToken(
		secret, "{\"alg\":\"HS256\",\"typ\":\"JWT\"}", "{\"role\":\"admin\",\"exp\":9999999999}");
	CJwt::VerifyResult r;
	ASSERT_FALSE(auth.Verify(token, r));
}

TEST(Jwt, EmptyJtiRejected)
{
	// Crafted token with an empty `jti` string. The revocation list
	// keys off jti; an empty key collides for every issuer of an
	// empty-jti token, so Verify() must refuse.
	const auto secret = MakeSecret(0x02);
	CJwt auth(secret);
	// iat + 1h exp so the mandatory-iat + lifetime-cap checks both
	// pass and the empty-jti check is the only available reject
	// path.
	const std::time_t now = std::time(nullptr);
	const std::string token = CraftToken(secret,
		"{\"alg\":\"HS256\",\"typ\":\"JWT\"}",
		std::string("{\"role\":\"admin\",\"iat\":") + std::to_string(now) +
			",\"exp\":" + std::to_string(now + 3600) + ",\"jti\":\"\"}");
	CJwt::VerifyResult r;
	ASSERT_FALSE(auth.Verify(token, r));
}

TEST(Jwt, MissingIatRejected)
{
	// Without an iat claim a token has unbounded lifetime — an
	// attacker who somehow gained mint capability could otherwise
	// issue a token with exp = year-2100 and bypass the lifetime
	// cap entirely. Mandatory iat closes the door.
	const auto secret = MakeSecret(0x03);
	CJwt auth(secret);
	const std::time_t now = std::time(nullptr);
	const std::string token = CraftToken(secret,
		"{\"alg\":\"HS256\",\"typ\":\"JWT\"}",
		std::string("{\"role\":\"admin\",\"exp\":") + std::to_string(now + 3600) + ",\"jti\":\"t\"}");
	CJwt::VerifyResult r;
	ASSERT_FALSE(auth.Verify(token, r));
}

TEST(Jwt, ExpIatLifetimeCapExceeded)
{
	// iat present + exp within the same future window, but the
	// total (exp - iat) is two days — well past the 24-hour
	// TOKEN_LIFETIME_SECONDS + skew. Verify must refuse: even
	// with the secret compromised, a hostile mint can't outrun
	// the lifetime cap.
	const auto secret = MakeSecret(0x04);
	CJwt auth(secret);
	const std::time_t now = std::time(nullptr);
	const std::time_t two_days = 2 * 24 * 60 * 60;
	const std::string token = CraftToken(secret,
		"{\"alg\":\"HS256\",\"typ\":\"JWT\"}",
		std::string("{\"role\":\"admin\",\"iat\":") + std::to_string(now) +
			",\"exp\":" + std::to_string(now + two_days) + ",\"jti\":\"t\"}");
	CJwt::VerifyResult r;
	ASSERT_FALSE(auth.Verify(token, r));
}

// --- Base64UrlDecode structural-invariant boundary tests ------------
//
// Each test crafts a token whose signing input is malformed in a way
// Base64UrlDecode is supposed to catch, then signs THAT signing input
// with the matching HMAC so the constant-time MAC compare passes and
// Verify() actually reaches Base64UrlDecode. Without the boundary
// guards, Verify() would accept the malformed token; the asserts pin
// the rejection in place.
//
// Two invariants the impl guards (Jwt.cpp:94-100):
//  * len % 4 == 1 — impossible for any valid base64url string
//  * non-zero residue bits — `len % 4 == 2/3` leaves 4/2 trailing
//    bits that a valid encoder always emits as 0

TEST(Jwt, Base64UrlDecodeRejectsLenMod4EqualsOne)
{
	const auto secret = MakeSecret(0xA7);
	CJwt auth(secret);
	// header section has length % 4 == 1 (9 chars). Any 9-char string
	// drawn from the b64url alphabet works; the decoder rejects on
	// size alone before inspecting the bytes.
	const std::string h_b64 = "AAAAAAAAA"; // 9 chars
	const std::string p_b64 = "AAAA";      // 4 chars (mod 4 == 0)
	const std::string signing_input = h_b64 + "." + p_b64;
	unsigned char mac[CryptoPP::SHA256::DIGESTSIZE];
	CryptoPP::HMAC<CryptoPP::SHA256> hmac(secret.data(), secret.size());
	hmac.Update(reinterpret_cast<const unsigned char *>(signing_input.data()), signing_input.size());
	hmac.Final(mac);
	const std::string sig = Base64UrlEncodeForTest(mac, sizeof(mac));
	const std::string token = signing_input + "." + sig;
	CJwt::VerifyResult r;
	ASSERT_FALSE(auth.Verify(token, r));
}

TEST(Jwt, Base64UrlDecodeRejectsNonZeroResidueBits)
{
	const auto secret = MakeSecret(0xA8);
	CJwt auth(secret);
	// 6-char b64url (len % 4 == 2) decodes to 1 byte and leaves 4
	// trailing bits that a valid encoder always emits as 0. "AAAAAB"
	// → 000000 000000 000000 000000 000000 000001 → 1 byte 0x00 +
	// residue 0001. Decoder must reject the non-zero residue.
	const std::string h_b64 = "AAAAAB";
	const std::string p_b64 = "AAAA";
	const std::string signing_input = h_b64 + "." + p_b64;
	unsigned char mac[CryptoPP::SHA256::DIGESTSIZE];
	CryptoPP::HMAC<CryptoPP::SHA256> hmac(secret.data(), secret.size());
	hmac.Update(reinterpret_cast<const unsigned char *>(signing_input.data()), signing_input.size());
	hmac.Final(mac);
	const std::string sig = Base64UrlEncodeForTest(mac, sizeof(mac));
	const std::string token = signing_input + "." + sig;
	CJwt::VerifyResult r;
	ASSERT_FALSE(auth.Verify(token, r));
}

// --- Depth-cap defence against unauthenticated stack-overflow -------
//
// picojson::parse recurses one stack frame per `{`/`[`. On musl (128
// KiB stack) a few hundred levels crash the worker — and both Verify()
// parse sites run BEFORE the MAC verdict, so an unauthenticated peer
// can submit `Authorization: Bearer <crafted>` with deeply-nested
// JSON and crash the daemon. The pre-parse opener-count cap (>32
// rejects) blocks both sides in O(body length) with zero allocations.
// Tests craft tokens past the cap, sign with the matching HMAC so
// the MAC compare passes, then assert Verify() rejects.

namespace
{

std::string DeeplyNested(const std::string &leaf, std::size_t levels)
{
	// {"a":{"a":...{"a":leaf}...}}  → levels openers.
	std::string out;
	out.reserve(levels * 6 + leaf.size() + levels);
	for (std::size_t i = 0; i < levels; ++i)
		out += "{\"a\":";
	out += leaf;
	for (std::size_t i = 0; i < levels; ++i)
		out += "}";
	return out;
}

} // namespace

TEST(Jwt, DeeplyNestedPayloadRejected)
{
	const auto secret = MakeSecret(0xA9);
	CJwt auth(secret);
	// 200 levels — well over the 32-opener cap but small enough that
	// the test stays fast and doesn't itself risk a stack overflow.
	const std::string token =
		CraftToken(secret, "{\"alg\":\"HS256\",\"typ\":\"JWT\"}", DeeplyNested("1", 200));
	CJwt::VerifyResult r;
	ASSERT_FALSE(auth.Verify(token, r));
}

TEST(Jwt, DeeplyNestedHeaderRejected)
{
	const auto secret = MakeSecret(0xAB);
	CJwt auth(secret);
	const std::string token = CraftToken(
		secret, DeeplyNested("\"x\"", 200), "{\"role\":\"admin\",\"exp\":9999999999,\"jti\":\"t\"}");
	CJwt::VerifyResult r;
	ASSERT_FALSE(auth.Verify(token, r));
}
