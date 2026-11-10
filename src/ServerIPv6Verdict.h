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

#ifndef SERVER_IPV6_VERDICT_H
#define SERVER_IPV6_VERDICT_H

#include <cstdint>
#include <optional>

enum class EServerIPv6Verdict
{
	Unknown,  // Does not license advertising.
	Probing,  // Does not license advertising.
	Trusted,  // Does not license advertising.
	Verified, // Only this licenses When verified advertising.
	Failed    // Does not license advertising.
};

constexpr std::uint8_t SERVER_IPV6_STATUS_HELD = 0x01;
constexpr std::uint8_t SERVER_IPV6_STATUS_REACHABLE = 0x02;
constexpr std::uint8_t SERVER_IPV6_STATUS_PROBED = 0x04;

inline EServerIPv6Verdict ParseServerIPv6Verdict(std::optional<std::uint8_t> status)
{
	if (!status) {
		return EServerIPv6Verdict::Unknown;
	}
	switch (*status) {
	case SERVER_IPV6_STATUS_HELD | SERVER_IPV6_STATUS_REACHABLE | SERVER_IPV6_STATUS_PROBED:
		return EServerIPv6Verdict::Verified;
	case SERVER_IPV6_STATUS_HELD | SERVER_IPV6_STATUS_PROBED:
		return EServerIPv6Verdict::Failed;
	case SERVER_IPV6_STATUS_HELD:
		return EServerIPv6Verdict::Probing;
	case SERVER_IPV6_STATUS_HELD | SERVER_IPV6_STATUS_REACHABLE:
		return EServerIPv6Verdict::Trusted;
	default:
		return EServerIPv6Verdict::Unknown;
	}
}

#endif // SERVER_IPV6_VERDICT_H
