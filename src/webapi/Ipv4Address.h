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

#ifndef WEBAPI_IPV4ADDRESS_H
#define WEBAPI_IPV4ADDRESS_H

#include <cstdint>
#include <string>

namespace webapi
{

/**
 * Parses exactly "a.b.c.d" into a | b << 8 | c << 16 | d << 24: the order EC carries client and
 * server addresses in, and the one Uint32toStringIP() reads back. Anything else is refused,
 * including a trailing ":port" or a fifth octet, which would otherwise address another host.
 */
inline bool ParseIpv4Dotted(const std::string &text, std::uint32_t &out)
{
	std::uint32_t ip = 0;
	unsigned octet = 0;
	unsigned digits = 0;
	unsigned dots = 0;
	for (const char ch : text) {
		if (ch >= '0' && ch <= '9') {
			octet = octet * 10 + static_cast<unsigned>(ch - '0');
			if (++digits > 3 || octet > 255) {
				return false;
			}
		} else if (ch == '.' && digits != 0 && dots < 3) {
			ip |= octet << (8 * dots++);
			octet = 0;
			digits = 0;
		} else {
			return false;
		}
	}
	if (digits == 0 || dots != 3) {
		return false;
	}
	out = ip | (octet << 24);
	return true;
}

/**
 * The same address in Kad order, a << 24 | b << 16 | c << 8 | d, which EC_TAG_BOOTSTRAP_IP carries:
 * amuled hands it straight to CKademlia::Bootstrap().
 */
inline std::uint32_t ToKadIpOrder(std::uint32_t ip)
{
	return (ip >> 24) | ((ip >> 8) & 0x0000FF00u) | ((ip << 8) & 0x00FF0000u) | (ip << 24);
}

} // namespace webapi

#endif // WEBAPI_IPV4ADDRESS_H
