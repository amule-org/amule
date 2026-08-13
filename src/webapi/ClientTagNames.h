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

#include <cstdint>
#include <string>

namespace webapi
{

/**
 * Stable lowercase tokens for the numeric client tags EC ships.
 *
 * Shared rather than file-local because two paths decode the same tags: the
 * refresher, for the live peers behind /clients, and the on-demand handler for
 * /known_clients, whose records carry the same software, origin and
 * obfuscation codes. Two copies would be free to drift, and the tokens are
 * part of the API surface -- a consumer switching on "kad" must get the same
 * answer whichever endpoint produced it.
 *
 * Deliberately not the GUI's rendering: these are protocol tokens, not
 * display text, so they stay untranslated and stable.
 */

//! EC_TAG_CLIENT_SOFTWARE (ESoftwareType) to a token, e.g. "emule".
const char *ClientSoftwareName(std::uint32_t code);

//! EC_TAG_CLIENT_OBFUSCATION_STATUS (EObfuscationState) to a token.
const char *ClientObfuscationName(std::uint8_t code);

//! EC_TAG_CLIENT_FROM (ESourceFrom) to a token. Local and remote server both
//! collapse to "server".
std::string SourceOriginName(std::uint32_t from);

} // namespace webapi

#endif // WEBAPI_CLIENTTAGNAMES_H
// File_checked_for_headers
