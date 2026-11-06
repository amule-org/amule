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

#include "ProxyAuth.h"

#include <boost/asio/ip/address.hpp>

#include <algorithm>
#include <cctype>

namespace webapi
{

namespace
{

std::string Lower(const std::string &s)
{
	std::string out = s;
	std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return out;
}

} // namespace

bool CanonicalIpAddress(const std::string &text, std::string &out)
{
	boost::system::error_code ec;
	const boost::asio::ip::address addr = boost::asio::ip::make_address(text, ec);
	if (ec) {
		return false;
	}
	if (addr.is_v6() && addr.to_v6().is_v4_mapped()) {
		out = boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, addr.to_v6()).to_string();
	} else {
		out = addr.to_string();
	}
	return true;
}

std::string ProxyAuthUser(const ProxyAuthPolicy &policy,
	const std::string &remote_addr,
	const std::map<std::string, std::string> &headers,
	const std::set<std::string> &repeated_headers)
{
	if (!policy.Enabled()) {
		return std::string();
	}
	std::string peer;
	if (!CanonicalIpAddress(remote_addr, peer) ||
		std::find(policy.trusted_addresses.begin(), policy.trusted_addresses.end(), peer) ==
			policy.trusted_addresses.end()) {
		return std::string();
	}
	// A second copy, in any letter case, means the proxy appended to a header the client sent
	// instead of replacing it, and the client's copy may be the one we see.
	const std::string name = Lower(policy.header);
	if (repeated_headers.count(name) != 0) {
		return std::string();
	}
	const std::string *value = nullptr;
	for (const auto &h : headers) {
		if (Lower(h.first) == name) {
			if (value) {
				return std::string();
			}
			value = &h.second;
		}
	}
	if (!value) {
		return std::string();
	}
	const std::size_t first = value->find_first_not_of(" \t");
	if (first == std::string::npos) {
		return std::string();
	}
	const std::size_t last = value->find_last_not_of(" \t");
	std::string user = value->substr(first, last - first + 1);
	// The name goes into the token, and the token has a size cap.
	if (user.size() > 256) {
		return std::string();
	}
	for (const char c : user) {
		const unsigned char u = static_cast<unsigned char>(c);
		if (u < 0x20 || u == 0x7f) {
			return std::string();
		}
	}
	return user;
}

} // namespace webapi
