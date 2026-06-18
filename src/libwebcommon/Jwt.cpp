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

#include <cryptopp/hmac.h>
#include <cryptopp/osrng.h>
#include <cryptopp/sha.h>

#include <cstdio>
#include <cstdint>
#include <utility>


namespace {

const char kB64UrlChars[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	"abcdefghijklmnopqrstuvwxyz"
	"0123456789-_";


std::string Base64UrlEncode(const unsigned char *data, size_t len)
{
	std::string out;
	out.reserve(((len + 2) / 3) * 4);
	for (size_t i = 0; i < len; i += 3) {
		uint32_t triple = static_cast<uint32_t>(data[i]) << 16;
		size_t avail = 1;
		if (i + 1 < len) { triple |= static_cast<uint32_t>(data[i + 1]) << 8; avail = 2; }
		if (i + 2 < len) { triple |= static_cast<uint32_t>(data[i + 2]);      avail = 3; }
		out.push_back(kB64UrlChars[(triple >> 18) & 0x3F]);
		out.push_back(kB64UrlChars[(triple >> 12) & 0x3F]);
		if (avail >= 2) out.push_back(kB64UrlChars[(triple >> 6) & 0x3F]);
		if (avail >= 3) out.push_back(kB64UrlChars[ triple       & 0x3F]);
	}
	return out;
}


bool Base64UrlDecodeChar(char c, unsigned int &out)
{
	if (c >= 'A' && c <= 'Z') { out = static_cast<unsigned>(c - 'A');      return true; }
	if (c >= 'a' && c <= 'z') { out = static_cast<unsigned>(c - 'a' + 26); return true; }
	if (c >= '0' && c <= '9') { out = static_cast<unsigned>(c - '0' + 52); return true; }
	if (c == '-')             { out = 62; return true; }
	if (c == '_')             { out = 63; return true; }
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
		if (!Base64UrlDecodeChar(in[i], v)) return false;
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
	if (bits > 0 && (acc & ((1u << bits) - 1)) != 0) return false;
	// Length-mod-4 of 1 is impossible for a valid base64url encoding.
	if ((in.size() & 3) == 1) return false;
	return true;
}


// HMAC-SHA-256(secret, signing_input) → 32-byte MAC. CryptoPP's HMAC
// handles keys of any length per RFC 2104.
void HmacSha256(const std::vector<unsigned char> &secret,
                const std::string                &signing_input,
                unsigned char out_mac[CryptoPP::SHA256::DIGESTSIZE])
{
	CryptoPP::HMAC<CryptoPP::SHA256> hmac(
		secret.empty() ? nullptr : secret.data(), secret.size());
	hmac.Update(
		reinterpret_cast<const unsigned char *>(signing_input.data()),
		signing_input.size());
	hmac.Final(out_mac);
}


// 24 h JWT expiry. Documented in the v0 API spec.
const std::time_t TOKEN_LIFETIME_SECONDS = 24 * 60 * 60;

// 128-bit `jti`. Wide enough that collisions are infeasible across the
// revocation-list lifetime, narrow enough that 22 b64url chars fit
// comfortably in cookie/header payloads.
const size_t JTI_BYTES = 16;

} // namespace


CJwt::CJwt(std::vector<unsigned char> secret)
	: m_secret(std::move(secret))
{
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
	std::snprintf(payload_buf, sizeof(payload_buf),
		"{\"role\":\"%s\",\"iat\":%lld,\"exp\":%lld,\"jti\":\"%s\"}",
		role_str,
		static_cast<long long>(now),
		static_cast<long long>(out.expires_at),
		out.jti.c_str());
	const std::string payload_json = payload_buf;

	const std::string header_b64 = Base64UrlEncode(
		reinterpret_cast<const unsigned char *>(header_json.data()),
		header_json.size());
	const std::string payload_b64 = Base64UrlEncode(
		reinterpret_cast<const unsigned char *>(payload_json.data()),
		payload_json.size());
	const std::string signing_input = header_b64 + "." + payload_b64;

	unsigned char mac[CryptoPP::SHA256::DIGESTSIZE];
	HmacSha256(m_secret, signing_input, mac);
	const std::string sig_b64 = Base64UrlEncode(mac, sizeof(mac));

	out.token = signing_input + "." + sig_b64;
	return out;
}


bool CJwt::Verify(const std::string &token, VerifyResult &out) const
{
	// Two dots split the token into three sections.
	const size_t first_dot = token.find('.');
	if (first_dot == std::string::npos) return false;
	const size_t second_dot = token.find('.', first_dot + 1);
	if (second_dot == std::string::npos) return false;
	if (token.find('.', second_dot + 1) != std::string::npos) return false;

	const std::string header_b64  = token.substr(0, first_dot);
	const std::string payload_b64 = token.substr(first_dot + 1,
	                                              second_dot - first_dot - 1);
	const std::string sig_b64     = token.substr(second_dot + 1);
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
		if (!Base64UrlDecode(header_b64, header_bytes)) return false;
		const std::string header_json(
			header_bytes.begin(), header_bytes.end());
		picojson::value hv;
		std::string herr;
		picojson::parse(hv, header_json.begin(), header_json.end(), &herr);
		if (!herr.empty() || !hv.is<picojson::object>()) return false;
		const picojson::object &hobj = hv.get<picojson::object>();
		const auto alg_it = hobj.find("alg");
		if (alg_it == hobj.end()
		    || !alg_it->second.is<std::string>()
		    || alg_it->second.get<std::string>() != "HS256") {
			return false;
		}
		// `typ` is optional in RFC 7519, but if present it must say JWT.
		const auto typ_it = hobj.find("typ");
		if (typ_it != hobj.end()
		    && (!typ_it->second.is<std::string>()
		        || typ_it->second.get<std::string>() != "JWT")) {
			return false;
		}
	}

	// Decode and parse the payload.
	std::vector<unsigned char> payload_bytes;
	if (!Base64UrlDecode(payload_b64, payload_bytes)) return false;
	const std::string payload_json(payload_bytes.begin(), payload_bytes.end());

	picojson::value v;
	std::string err;
	picojson::parse(v, payload_json.begin(), payload_json.end(), &err);
	if (!err.empty() || !v.is<picojson::object>()) return false;

	const picojson::object &obj = v.get<picojson::object>();
	const auto role_it = obj.find("role");
	const auto exp_it  = obj.find("exp");
	const auto jti_it  = obj.find("jti");
	if (role_it == obj.end() || !role_it->second.is<std::string>()) return false;
	if (exp_it  == obj.end() || !exp_it->second.is<int64_t>())      return false;
	if (jti_it  == obj.end() || !jti_it->second.is<std::string>())  return false;

	const std::string role_str = role_it->second.get<std::string>();
	if      (role_str == "admin") out.role = Role::ADMIN;
	else if (role_str == "guest") out.role = Role::GUEST;
	else return false;

	out.exp = static_cast<std::time_t>(exp_it->second.get<int64_t>());
	if (out.exp <= std::time(nullptr)) return false;   // expired

	out.jti = jti_it->second.get<std::string>();
	if (out.jti.empty()) return false;

	// nbf (RFC 7519 §4.1.5, "not before") is intentionally not
	// enforced: Issue() never emits the claim and we don't accept
	// externally-issued tokens. If federated tokens are ever added,
	// the check belongs immediately above the `exp` check.

	return true;
}
