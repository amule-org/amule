//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
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
// along with this program; if not, write to the Free Software Foundation,
// Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA.
// If not, see <https://www.gnu.org/licenses/>.
//

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/udp.hpp>

namespace pcp
{

struct Route
{
	std::array<std::uint8_t, 16> destination{};
	std::uint8_t prefixLength = 0;
	std::array<std::uint8_t, 16> gateway{};
	unsigned int interfaceIndex = 0;
	std::uint32_t metric = 0;
	bool isDefault = false;
};

struct LocalAddress
{
	std::array<std::uint8_t, 16> address{};
	unsigned int interfaceIndex = 0;
};

struct Gateway
{
	boost::asio::ip::address_v6 localAddress;
	boost::asio::ip::udp::endpoint endpoint;
};

// Select the lowest-metric default route whose interface owns the selected address bytes.
std::optional<Gateway> SelectGateway(const std::vector<Route> &routes,
	const std::vector<LocalAddress> &addresses,
	const std::array<std::uint8_t, 16> &selectedAddress);

// Discover the native default IPv6 router for an explicitly selected local address.
std::optional<Gateway> DiscoverGateway(const boost::asio::ip::address_v6 &selectedAddress);

namespace detail
{
// Parse native-endian Linux netlink route-dump datagrams; invalid/incomplete dumps do not mutate routes.
bool ParseLinuxRouteDump(
	const std::vector<std::uint8_t> &datagram, std::uint32_t sequence, std::vector<Route> &routes);
// Parse a native macOS PF_ROUTE sysctl route dump; invalid dumps do not mutate routes.
// Declared unconditionally but defined and exercised on macOS only.
bool ParseMacRouteDump(const std::vector<unsigned char> &dump, std::vector<Route> &routes);
} // namespace detail

} // namespace pcp
