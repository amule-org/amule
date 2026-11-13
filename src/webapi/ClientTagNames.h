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

#ifndef WEBAPI_CLIENTTAGNAMES_H
#define WEBAPI_CLIENTTAGNAMES_H

#include "../Constants.h" // Needed for ESourceFrom and SourceFromBit

#include <cstdint>
#include <string>
#include <vector>

namespace webapi
{

/**
 * Stable lowercase tokens for the numeric client tags EC ships.
 *
 * Shared rather than file-local because two paths decode the same tags: the refresher, for the live
 * peers behind /clients, and the on-demand handler for /known_clients, whose records carry the same
 * software, origin and obfuscation codes. Two copies would be free to drift, and the tokens are
 * part of the API surface -- a consumer switching on "kad" must get the same answer whichever
 * endpoint produced it.
 *
 * Deliberately not the GUI's rendering: these are protocol tokens, not display text, so they stay
 * untranslated and stable.
 */

//! EC_TAG_CLIENT_SOFTWARE (ESoftwareType) to a token, e.g. "emule".
const char *ClientSoftwareName(std::uint32_t code);

//! EC_TAG_CLIENT_OBFUSCATION_STATUS (EObfuscationState) to a token.
const char *ClientObfuscationName(std::uint8_t code);

/**
 * EC_TAG_CLIENT_FROM (ESourceFrom) to a token, e.g. "local_server", mirroring the GUI's Origin
 * column without leaking the daemon locale. Inline because the SSE writer needs it in a
 * translation unit that does not link the refresher.
 */
inline std::string SourceOriginName(std::uint32_t from)
{
	switch (from) {
	case SF_LOCAL_SERVER:
		return "local_server";
	case SF_REMOTE_SERVER:
		return "remote_server";
	case SF_KADEMLIA:
		return "kad";
	case SF_SOURCE_EXCHANGE:
		return "source_exchange";
	case SF_PASSIVE:
		return "passive";
	case SF_LINK:
		return "link";
	case SF_SOURCE_SEEDS:
		return "source_seeds";
	case SF_SEARCH_RESULT:
		return "search_result";
	default:
		return "unknown";
	}
}

/**
 * EC_TAG_CLIENT_FROM_MASK to its tokens, in ESourceFrom order. A source merged from several
 * copies carries more than one; SF_NONE is not a channel and is skipped.
 */
inline std::vector<std::string> SourceOriginNames(std::uint32_t mask)
{
	std::vector<std::string> out;
	for (int from = SF_LOCAL_SERVER; from <= SF_SEARCH_RESULT; ++from) {
		if ((mask & SourceFromBit(static_cast<ESourceFrom>(from))) != 0) {
			out.push_back(SourceOriginName(static_cast<std::uint32_t>(from)));
		}
	}
	return out;
}

//! The route nibble of EC_TAG_CLIENT_CONTACT_ROUTE (ESourceRoute) to a token, empty for none.
std::string ContactRouteName(std::uint8_t packed);

} // namespace webapi

#endif // WEBAPI_CLIENTTAGNAMES_H
// File_checked_for_headers
