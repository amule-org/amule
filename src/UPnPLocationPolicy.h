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

#ifndef UPNP_LOCATION_POLICY_H
#define UPNP_LOCATION_POLICY_H

#include <string>

namespace UPnP
{

/**
 * Whether an SSDP location names an IPv6 literal host. A gateway that answers the search over IPv4
 * and IPv6 sends one location for each. The port mapping actions map an IPv4 address, and
 * miniupnpd in secure mode refuses them over IPv6, so they must use the IPv4 location. A host name
 * is not resolved here.
 */
inline bool IsIPv6Location(const std::string &location)
{
	const std::string::size_type scheme = location.find("://");
	return scheme != std::string::npos && scheme + 3 < location.size() && location[scheme + 3] == '[';
}

} // namespace UPnP

#endif // UPNP_LOCATION_POLICY_H
