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

#ifndef LIBWEBCOMMON_JWT_H
#define LIBWEBCOMMON_JWT_H

#include <ctime>
#include <string>
#include <vector>

// See CryptoPP_Inc.h for pragma rationale.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-copy-with-user-provided-dtor"
#pragma clang diagnostic ignored "-Wdeprecated-copy-with-user-provided-copy"
#pragma clang diagnostic ignored "-Wdeprecated-dynamic-exception-spec"
#endif
#include <cryptopp/secblock.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include "Role.h"

// HS256 JWT machinery for the /api/v0 surface. Token shape per
// RFC 7519: <base64url(header)>.<base64url(payload)>.<base64url(sig)>
//
//   header  = {"alg":"HS256","typ":"JWT"}
//   payload = {"role":"admin"|"guest","iat":<unix>,"exp":<unix>,
//              "jti":"<random base64url>"}
//   sig     = HMAC-SHA-256(secret, header_b64 + "." + payload_b64)
//
// The HMAC secret is supplied at construction by the owning binary
// (amuleapi loads it from `${config_dir}/amuleapi-jwt-secret`); this
// class never touches the filesystem.
//
// `jti` (RFC 7519 §4.1.7) is a 128-bit random identifier emitted
// per Issue() and surfaced through Verify() so the owning binary
// can run a server-side revocation list — `/auth/logout` adds the
// jti, Verify rejects on the next request. The revocation set lives
// in the owner, not in this library.
class CJwt
{
public:
	// `secret` is the HMAC-SHA-256 key. amuleapi loads 32 random bytes
	// (256 bits, matching the digest size) from amuleapi-jwt-secret;
	// the test-only constructor passes a deterministic fill so test
	// vectors are reproducible.
	explicit CJwt(std::vector<unsigned char> secret);

	struct IssuedToken
	{
		std::string token;
		std::string jti;        // emitted in the `jti` claim
		std::time_t expires_at; // matches the `exp` claim
	};

	// Generates a fresh JWT for the given role with a 24 h expiry.
	// Each call returns a fresh `jti`.
	IssuedToken Issue(Role role);

	struct VerifyResult
	{
		Role role;
		std::time_t exp;
		std::string jti; // for revocation-list lookup
	};

	// Verifies a token's signature, header `alg`/`typ`, and payload
	// shape. Returns true and fills `out` on success; false on bad
	// signature, expired `exp`, malformed base64, malformed JSON, or
	// wrong algorithm. Constant-time MAC compare runs before the
	// header `alg` parse so the timing channel doesn't distinguish
	// "wrong MAC" from "malformed header".
	bool Verify(const std::string &token, VerifyResult &out) const;

private:
	// CryptoPP::SecBlock zeros its backing buffer at destruction
	// (and on reallocation via AlignedAllocator), so a coredump or
	// swap from a long-lived amuleapi process won't leak the HMAC
	// signing key the same way a plain std::vector<unsigned char>
	// would. The constructor accepts a vector for caller convenience
	// (config-load doesn't want to drag SecBlock into its surface)
	// and copies into the SecBlock once.
	CryptoPP::SecByteBlock m_secret;
};

#endif // LIBWEBCOMMON_JWT_H
