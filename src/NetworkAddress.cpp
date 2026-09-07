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

// The only translation unit in the ed2k core that compiles Boost.Asio for the
// sake of an address.
//
// CNetworkAddress stores its own octets so that the 155 TUs which merely pass
// an address around no longer pull asio's include closure (and, on Windows, no
// longer need ws2_32 to link) -- the full argument is in NetworkAddress.h. But
// two of its operations are text handling, and text handling is where a
// hand-rolled implementation goes wrong quietly: RFC 4291 zero compression has
// a canonical form with real rules (longest run, leftmost on a tie, never a
// single group), IPv4-mapped addresses print with a dotted-quad tail, and a
// literal may carry a %scope suffix. Restating that here would be a parser and
// a formatter to maintain, so both stay asio's job and pay for themselves in
// this one file.

#include "NetworkAddressAsio.h"

#include <boost/system/error_code.hpp>

CNetworkAddress CNetworkAddress::FromString(const std::string &text)
{
	if (text.empty()) {
		return Absent();
	}
	boost::system::error_code ec;
	const boost::asio::ip::address address = boost::asio::ip::make_address(text, ec);
	if (ec) {
		return Absent();
	}
	return NetworkAddressAsio::FromAsioAddress(address);
}

std::string CNetworkAddress::ToString() const
{
	if (IsAbsent()) {
		return "<absent>";
	}
	return NetworkAddressAsio::ToAsioAddress(*this).to_string();
}

namespace NetworkAddressAsio
{

boost::asio::ip::address ToAsioAddress(const CNetworkAddress &address)
{
	if (address.IsIPv4()) {
		const CNetworkAddress::Octets &octets = address.GetOctets();
		// asio's address_v4 takes its four octets in the same wire order
		// CNetworkAddress stores them in, so this is a copy and not a swap --
		// neither of the two 32-bit conventions is involved.
		const boost::asio::ip::address_v4::bytes_type v4Bytes = {
			{ octets[0], octets[1], octets[2], octets[3] }
		};
		return boost::asio::ip::address(boost::asio::ip::address_v4(v4Bytes));
	}
	// An absent address reaches here only as a broken precondition, and asio
	// has no value for it: the caller was told to test IsPresent() first. Give
	// it :: rather than 0.0.0.0 so that a caller which ignored that contract
	// gets an address of the family it asked about instead of silently binding
	// the IPv4 wildcard.
	boost::asio::ip::address_v6::bytes_type v6Bytes;
	const CNetworkAddress::Octets &octets = address.GetOctets();
	for (std::size_t i = 0; i < v6Bytes.size(); ++i) {
		v6Bytes[i] = octets[i];
	}
	return boost::asio::ip::address(boost::asio::ip::address_v6(v6Bytes, address.GetScopeId()));
}

CNetworkAddress FromAsioAddress(const boost::asio::ip::address &address)
{
	if (address.is_v4()) {
		// to_uint() is asio's host-order (numeric) accessor, which is the same
		// convention FromIPv4HostOrder() names -- so no swap here either.
		return CNetworkAddress::FromIPv4HostOrder(address.to_v4().to_uint());
	}
	const boost::asio::ip::address_v6 v6 = address.to_v6();
	const boost::asio::ip::address_v6::bytes_type v6Bytes = v6.to_bytes();
	CNetworkAddress::Octets octets;
	for (std::size_t i = 0; i < octets.size(); ++i) {
		octets[i] = v6Bytes[i];
	}
	// IPv6FromOctets() rather than FromIPv6Bytes(): :: is a legitimate asio
	// address (it is the wildcard every dual-stack listener binds), and the
	// all-zero-means-absent rule belongs to the wire-tag edge, not here.
	return CNetworkAddress::IPv6FromOctets(octets, v6.scope_id());
}

} // namespace NetworkAddressAsio
// File_checked_for_headers
