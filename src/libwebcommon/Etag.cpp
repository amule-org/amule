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

#include "Etag.h"

// See CryptoPP_Inc.h for pragma rationale.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-copy-with-user-provided-dtor"
#pragma clang diagnostic ignored "-Wdeprecated-copy-with-user-provided-copy"
#pragma clang diagnostic ignored "-Wdeprecated-dynamic-exception-spec"
#endif
#include <cryptopp/sha.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace webcommon
{

std::string Etag(const std::string &body_utf8)
{
	CryptoPP::SHA256 sha;
	unsigned char digest[CryptoPP::SHA256::DIGESTSIZE];
	sha.CalculateDigest(
		digest, reinterpret_cast<const unsigned char *>(body_utf8.data()), body_utf8.size());
	static const char hex[] = "0123456789abcdef";
	std::string out;
	out.reserve(16);
	for (int i = 0; i < 8; ++i) {
		out.push_back(hex[(digest[i] >> 4) & 0x0F]);
		out.push_back(hex[digest[i] & 0x0F]);
	}
	return out;
}

// Strip leading/trailing whitespace + optional `W/` weak-validator
// prefix + optional outer double quotes from one If-None-Match
// entry (RFC 7232 §3.2 + §2.3). Weak and strong validators with the
// same opaque payload compare equal for 304 purposes; we don't
// carry a separate weak/strong dimension on the response side.
static std::string NormalizeOneValidator(const std::string &raw)
{
	std::size_t start = 0;
	std::size_t end = raw.size();
	while (start < end && (raw[start] == ' ' || raw[start] == '\t'))
		++start;
	while (end > start && (raw[end - 1] == ' ' || raw[end - 1] == '\t'))
		--end;
	// Strip weak-validator prefix `W/` (case-sensitive per RFC).
	if (end - start >= 2 && raw[start] == 'W' && raw[start + 1] == '/') {
		start += 2;
	}
	// Strip outer double quotes if both present.
	if (end - start >= 2 && raw[start] == '"' && raw[end - 1] == '"') {
		++start;
		--end;
	}
	return raw.substr(start, end - start);
}

bool IfNoneMatchHits(const std::string &if_none_match, const std::string &etag)
{
	if (if_none_match.empty())
		return false;
	// Header value may be a single validator or a comma-separated
	// list — walk it, normalise each entry, return true on any hit.
	// `*` matches any existing representation; the caller only
	// invokes this on a 200-with-body, so `*` is always a hit.
	std::size_t pos = 0;
	while (pos <= if_none_match.size()) {
		const std::size_t comma = if_none_match.find(',', pos);
		const std::size_t end = (comma == std::string::npos) ? if_none_match.size() : comma;
		const std::string entry = NormalizeOneValidator(if_none_match.substr(pos, end - pos));
		if (entry == "*" || entry == etag)
			return true;
		if (comma == std::string::npos)
			break;
		pos = comma + 1;
	}
	return false;
}

} // namespace webcommon
