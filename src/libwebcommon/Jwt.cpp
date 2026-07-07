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

#include "Jwt.h"

#include "ConstantTime.h"

#define PICOJSON_USE_INT64
#include "picojson.h"

// cryptopp headers pull in deprecated implicit copy ctors + throw()
// specs (P0806 + C++17). See CryptoPP_Inc.h for the full rationale.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-copy-with-user-provided-dtor"
#pragma clang diagnostic ignored "-Wdeprecated-copy-with-user-provided-copy"
#pragma clang diagnostic ignored "-Wdeprecated-dynamic-exception-spec"
#endif
#include <cryptopp/hmac.h>
#include <cryptopp/osrng.h>
#include <cryptopp/sha.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <cstdio>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace
{

const char kB64UrlChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
			    "abcdefghijklmnopqrstuvwxyz"
			    "0123456789-_";

std::string Base64UrlEncode(const unsigned char *data, size_t len)
{
	std::string out;
	out.reserve(((len + 2) / 3) * 4);
	for (size_t i = 0; i < len; i += 3) {
		uint32_t triple = static_cast<uint32_t>(data[i]) << 16;
		size_t avail = 1;
		if (i + 1 < len) {
			triple |= static_cast<uint32_t>(data[i + 1]) << 8;
			avail = 2;
		}
		if (i + 2 < len) {
			triple |= static_cast<uint32_t>(data[i + 2]);
			avail = 3;
		}
		out.push_back(kB64UrlChars[(triple >> 18) & 0x3F]);
		out.push_back(kB64UrlChars[(triple >> 12) & 0x3F]);
		if (avail >= 2)
			out.push_back(kB64UrlChars[(triple >> 6) & 0x3F]);
		if (avail >= 3)
			out.push_back(kB64UrlChars[triple & 0x3F]);
	}
	return out;
}

bool Base64UrlDecodeChar(char c, unsigned int &out)
{
	if (c >= 'A' && c <= 'Z') {
		out = static_cast<unsigned>(c - 'A');
		return true;
	}
	if (c >= 'a' && c <= 'z') {
		out = static_cast<unsigned>(c - 'a' + 26);
		return true;
	}
	if (c >= '0' && c <= '9') {
		out = static_cast<unsigned>(c - '0' + 52);
		return true;
	}
	if (c == '-') {
		out = 62;
		return true;
	}
	if (c == '_') {
		out = 63;
		return true;
	}
	return false;
}

bool Base64UrlDecode(const std::string &in, std::vector<unsigned char> &out)
{
	out.clear();
	out.reserve(in.size() * 3 / 4 + 3);
	uint32_t acc = 0;
	int bits = 0;
	for (size_t i = 0; i < in.size(); ++i) {
		unsigned int v;
		if (!Base64UrlDecodeChar(in[i], v))
			return false;
		acc = (acc << 6) | v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out.push_back(static_cast<unsigned char>((acc >> bits) & 0xFF));
		}
	}
	// A well-formed base64url string of length len(input)%4 == 0/2/3
	// has 0/4/2 trailing bits respectively, all expected to be zero.
	// Reject inputs that left non-zero residue — they're malformed even
	// if every char is in the b64url alphabet.
	if (bits > 0 && (acc & ((1u << bits) - 1)) != 0)
		return false;
	// Length-mod-4 of 1 is impossible for a valid base64url encoding.
	if ((in.size() & 3) == 1)
		return false;
	return true;
}

// HMAC-SHA-256(secret, signing_input) → 32-byte MAC. CryptoPP's HMAC
// handles keys of any length per RFC 2104.
void HmacSha256(const CryptoPP::SecByteBlock &secret,
	const std::string &signing_input,
	unsigned char out_mac[CryptoPP::SHA256::DIGESTSIZE])
{
	CryptoPP::HMAC<CryptoPP::SHA256> hmac(secret.empty() ? nullptr : secret.data(), secret.size());
	hmac.Update(reinterpret_cast<const unsigned char *>(signing_input.data()), signing_input.size());
	hmac.Final(out_mac);
}

// 24 h JWT expiry. Documented in the v0 API spec.
const std::time_t TOKEN_LIFETIME_SECONDS = 24 * 60 * 60;

// 128-bit `jti`. Wide enough that collisions are infeasible across the
// revocation-list lifetime, narrow enough that 22 b64url chars fit
// comfortably in cookie/header payloads.
const size_t JTI_BYTES = 16;

// Hard cap on JSON nesting in a JWT header or payload.
//
// picojson's _parse_array / _parse_object is unbounded recursive
// descent and both parse sites in Verify() run BEFORE the MAC
// compare returns its verdict — an unauthenticated peer can blow
// the worker stack with `{"a":{"a":...}}` nested deep enough.
// musl's 128 KiB pthread stack limits the attack to ~300-600
// frames; glibc is higher but still finite.
//
// Real JWT payloads here are flat (scalar claims: role / exp /
// iat / jti / typ / alg) so 8 would suffice. 32 leaves headroom
// for a third-party producer accepted later.
//
// The check sums `{` + `[` in the decoded JSON. Openers inside
// string literals are counted too — false positives — but our
// payloads don't legitimately contain unbalanced braces in
// strings, so the conservative direction is fine.
const std::size_t MAX_JSON_OPENERS = 32;

bool DepthWithinLimit(const std::string &json)
{
	std::size_t count = 0;
	for (char c : json) {
		if (c == '{' || c == '[') {
			if (++count > MAX_JSON_OPENERS)
				return false;
		}
	}
	return true;
}

} // namespace

CJwt::CJwt(std::vector<unsigned char> secret)
: m_secret(secret.empty() ? nullptr : secret.data(), secret.size())
{
	// An empty signing key is always a config bug (truncated
	// amuleapi-jwt-secret read, missing file write, etc.). Refusing
	// it at construction is the cheapest way to avoid the failure
	// mode where the daemon happily issues and verifies tokens
	// signed with a zero-length key. CryptoPP's HMAC accepts a null
	// key + len=0 without complaint, which is why this slipped
	// through MAC checking.
	if (m_secret.empty()) {
		throw std::invalid_argument("CJwt: signing secret must not be empty");
	}
	// Wipe the caller's copy now that we've taken our own. The
	// SecByteBlock owns the live copy and will scrub itself on
	// destruction; the std::vector the caller passed in is
	// moved-from, leaving any residual bytes outside our control.
	// Best-effort: explicitly overwrite if any bytes remain.
	if (!secret.empty()) {
		std::fill(secret.begin(), secret.end(), 0);
	}
}

CJwt::IssuedToken CJwt::Issue(Role role)
{
	IssuedToken out;
	const std::time_t now = std::time(nullptr);
	out.expires_at = now + TOKEN_LIFETIME_SECONDS;

	unsigned char jti_bytes[JTI_BYTES];
	CryptoPP::AutoSeededRandomPool rng;
	rng.GenerateBlock(jti_bytes, sizeof(jti_bytes));
	out.jti = Base64UrlEncode(jti_bytes, sizeof(jti_bytes));

	const char *role_str = (role == Role::ADMIN) ? "admin" : "guest";

	const std::string header_json = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";

	char payload_buf[256];
	std::snprintf(payload_buf,
		sizeof(payload_buf),
		"{\"role\":\"%s\",\"iat\":%lld,\"exp\":%lld,\"jti\":\"%s\"}",
		role_str,
		static_cast<long long>(now),
		static_cast<long long>(out.expires_at),
		out.jti.c_str());
	const std::string payload_json = payload_buf;

	const std::string header_b64 = Base64UrlEncode(
		reinterpret_cast<const unsigned char *>(header_json.data()), header_json.size());
	const std::string payload_b64 = Base64UrlEncode(
		reinterpret_cast<const unsigned char *>(payload_json.data()), payload_json.size());
	const std::string signing_input = header_b64 + "." + payload_b64;

	unsigned char mac[CryptoPP::SHA256::DIGESTSIZE];
	HmacSha256(m_secret, signing_input, mac);
	const std::string sig_b64 = Base64UrlEncode(mac, sizeof(mac));

	out.token = signing_input + "." + sig_b64;
	return out;
}

bool CJwt::Verify(const std::string &token, VerifyResult &out) const
{
	// Reject before any Base64UrlDecode walk on absurd-length tokens.
	// A legitimate amuleapi token is ~280 bytes (header 36 + payload
	// ~120 + signature 43, each base64url-encoded); 4 KiB leaves
	// ~10x headroom. Without this cap an unauthenticated peer can
	// burn three full-token walks per request before the MAC
	// compare rejects, which is a cheap CPU-amplification surface
	// against the listener (1 MiB body cap × N concurrent peers).
	if (token.size() > 4096)
		return false;
	// Two dots split the token into three sections.
	const size_t first_dot = token.find('.');
	if (first_dot == std::string::npos)
		return false;
	const size_t second_dot = token.find('.', first_dot + 1);
	if (second_dot == std::string::npos)
		return false;
	if (token.find('.', second_dot + 1) != std::string::npos)
		return false;

	const std::string header_b64 = token.substr(0, first_dot);
	const std::string payload_b64 = token.substr(first_dot + 1, second_dot - first_dot - 1);
	const std::string sig_b64 = token.substr(second_dot + 1);
	const std::string signing_input = header_b64 + "." + payload_b64;

	// Recompute MAC and compare in constant time before validating the
	// header, so timing of a malformed header is indistinguishable from
	// a wrong MAC. Not exploitable today (32-byte secret makes collision
	// infeasible) but keeps the channel closed against future shifts.
	unsigned char mac[CryptoPP::SHA256::DIGESTSIZE];
	HmacSha256(m_secret, signing_input, mac);
	const std::string expected_sig = Base64UrlEncode(mac, sizeof(mac));
	if (!webcommon::ConstantTimeEquals(expected_sig, sig_b64)) {
		return false;
	}

	// Defence in depth: validate the header announces HS256.
	// The MAC already matches our secret so only we could have signed
	// the token; this closes the door against future key-confusion if
	// asymmetric algorithms are ever added.
	{
		std::vector<unsigned char> header_bytes;
		if (!Base64UrlDecode(header_b64, header_bytes))
			return false;
		const std::string header_json(header_bytes.begin(), header_bytes.end());
		if (!DepthWithinLimit(header_json))
			return false;
		picojson::value hv;
		std::string herr;
		picojson::parse(hv, header_json.begin(), header_json.end(), &herr);
		if (!herr.empty() || !hv.is<picojson::object>())
			return false;
		const picojson::object &hobj = hv.get<picojson::object>();
		const auto alg_it = hobj.find("alg");
		if (alg_it == hobj.end() || !alg_it->second.is<std::string>() ||
			alg_it->second.get<std::string>() != "HS256") {
			return false;
		}
		// `typ` is optional in RFC 7519, but if present it must say JWT.
		const auto typ_it = hobj.find("typ");
		if (typ_it != hobj.end() &&
			(!typ_it->second.is<std::string>() || typ_it->second.get<std::string>() != "JWT")) {
			return false;
		}
	}

	// Decode and parse the payload.
	std::vector<unsigned char> payload_bytes;
	if (!Base64UrlDecode(payload_b64, payload_bytes))
		return false;
	const std::string payload_json(payload_bytes.begin(), payload_bytes.end());
	if (!DepthWithinLimit(payload_json))
		return false;

	picojson::value v;
	std::string err;
	picojson::parse(v, payload_json.begin(), payload_json.end(), &err);
	if (!err.empty() || !v.is<picojson::object>())
		return false;

	const picojson::object &obj = v.get<picojson::object>();
	const auto role_it = obj.find("role");
	const auto exp_it = obj.find("exp");
	const auto jti_it = obj.find("jti");
	if (role_it == obj.end() || !role_it->second.is<std::string>())
		return false;
	if (exp_it == obj.end() || !exp_it->second.is<int64_t>())
		return false;
	if (jti_it == obj.end() || !jti_it->second.is<std::string>())
		return false;

	const std::string role_str = role_it->second.get<std::string>();
	if (role_str == "admin")
		out.role = Role::ADMIN;
	else if (role_str == "guest")
		out.role = Role::GUEST;
	else
		return false;

	out.exp = static_cast<std::time_t>(exp_it->second.get<int64_t>());
	{
		// Five-second clock-skew tolerance on the exp check. Issuer
		// and verifier today run in the same process so the skew is
		// always zero; tomorrow they may not (federated tokens,
		// reverse-proxy auth handoff, etc.) and a token landing on
		// the verifier microseconds after exp shouldn't 401 the
		// caller's last request. A few seconds of leeway is the
		// standard RFC 7519 §4.1.4 implementation note.
		constexpr std::time_t skew = 5;
		const std::time_t now = std::time(nullptr);
		if (out.exp + skew <= now)
			return false; // expired

		// `iat` (issued-at, §4.1.6) is mandatory. Without an iat
		// claim a token has unbounded lifetime — an attacker who
		// somehow gained mint capability (compromised secret,
		// stolen --jwt-secret file, …) could otherwise issue a
		// token with exp = year-2100 and bypass the lifetime cap
		// entirely. With iat mandatory we additionally cap
		// (exp - iat) ≤ TOKEN_LIFETIME_SECONDS + skew so the
		// cap survives a future Issue() change too.
		const auto iat_it = obj.find("iat");
		if (iat_it == obj.end() || !iat_it->second.is<int64_t>()) {
			return false;
		}
		const std::time_t iat = static_cast<std::time_t>(iat_it->second.get<int64_t>());
		if (iat > now + 60)
			return false; // iat in the future
		if (out.exp <= iat)
			return false; // exp must follow iat
		if (out.exp - iat > TOKEN_LIFETIME_SECONDS + skew) {
			return false; // lifetime cap
		}
	}

	out.jti = jti_it->second.get<std::string>();
	if (out.jti.empty())
		return false;

	// nbf (RFC 7519 §4.1.5, "not before") is intentionally not
	// enforced: Issue() never emits the claim and we don't accept
	// externally-issued tokens. If federated tokens are ever added,
	// the check belongs immediately above the `exp` check.

	return true;
}
