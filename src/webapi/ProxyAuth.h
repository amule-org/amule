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

#ifndef WEBAPI_PROXYAUTH_H
#define WEBAPI_PROXYAUTH_H

#include <map>
#include <set>
#include <string>
#include <vector>

#include "Role.h"

namespace webapi
{

// A reverse proxy that signs users in itself and names the user in a request header. Off while
// `header` is empty.
struct ProxyAuthPolicy
{
	std::string header;
	// Canonical form, from CanonicalIpAddress(). Matched against the TCP peer, never against
	// X-Forwarded-For, which the client controls.
	std::vector<std::string> trusted_addresses;
	Role role = Role::ADMIN;
	std::string logout_url;
	bool Enabled() const { return !header.empty(); }
};

// Text form of an IPv4 or IPv6 address, with an IPv4-mapped IPv6 address reduced to IPv4: a
// dual-stack listener reports "::ffff:127.0.0.1" for a client that connected to 127.0.0.1.
bool CanonicalIpAddress(const std::string &text, std::string &out);

// The user name the proxy sent with this request, or empty when proxy auth is off, the peer is not
// trusted, or the header is missing, empty, repeated or holds a control character.
// `repeated_headers` holds the lowercase names the request carried more than once.
std::string ProxyAuthUser(const ProxyAuthPolicy &policy,
	const std::string &remote_addr,
	const std::map<std::string, std::string> &headers,
	const std::set<std::string> &repeated_headers);

} // namespace webapi

#endif // WEBAPI_PROXYAUTH_H
