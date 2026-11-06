//								-*- C++ -*-
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

#ifndef NATSERVERKEEPALIVEPROOF_H
#define NATSERVERKEEPALIVEPROOF_H

#include "NatServerHolePunch.h"

#ifdef ENABLE_NATT_SERVER_COORDINATION
#include <cstddef>
#include <cstdint>
#include <vector>

namespace NatServerHolePunch
{

constexpr std::size_t kKeepaliveProofMacSize = 16;
constexpr std::size_t kKeepaliveProofSize = 16 + 4 + kKeepaliveProofMacSize;

// The proof form of 0x9F: <user hash 16><counter u32 LE><mac 16>, the mac being the first 16 bytes
// of HMAC-SHA256 keyed with the session token over the hash and counter. The server takes it from
// any address while the counter rises within a login, so the token itself never goes on UDP.
std::vector<std::uint8_t> BuildKeepaliveProof(
	const Hash &ownUserHash, const Hash &sessionToken, std::uint32_t counter);

} // namespace NatServerHolePunch

#endif // ENABLE_NATT_SERVER_COORDINATION
#endif // NATSERVERKEEPALIVEPROOF_H
// File_checked_for_headers
