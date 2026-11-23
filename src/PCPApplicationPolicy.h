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

#include <boost/asio.hpp>
#include <optional>

namespace pcp
{
inline std::optional<boost::asio::ip::address_v6> ApplicationTarget(bool enabled,
	bool listenerAcceptsIPv6,
	const std::optional<boost::asio::ip::address_v6> &address,
	unsigned short port)
{
	if (!enabled || !listenerAcceptsIPv6 || !address || address->is_unspecified() ||
		address->is_multicast() || address->is_v4_mapped() || port == 0) {
		return std::nullopt;
	}
	return address;
}

// Who opens the node's one inbound IPv6 TCP pinhole. Gateways such as miniupnpd keep one entry per
// address and port for both protocols and refuse to let the other protocol update it, so exactly
// one of them may hold it.
enum class PinholeRoute
{
	Close,        // No pinhole is wanted: both protocols delete what they hold.
	PCP,          // PCP asks or holds; IGD2 forgets its pinhole without deleting it.
	IGD2,         // IGD2 holds; PCP stands by without deleting.
	IGD2Fallback, // PCP failed: IGD2 gets the target while PCP keeps asking.
};

struct PinholeOwners
{
	bool pcpLease = false;
	// No gateway, no answer to the first requests, or an error result.
	bool pcpFailed = false;
	bool igd2Available = false;
	bool igd2Holds = false;
};

inline PinholeRoute DecidePinholeRoute(bool wanted, const PinholeOwners &owners)
{
	if (!wanted) {
		return PinholeRoute::Close;
	}
	if (owners.pcpLease) {
		return PinholeRoute::PCP;
	}
	if (owners.igd2Available && owners.igd2Holds) {
		return PinholeRoute::IGD2;
	}
	if (owners.igd2Available && owners.pcpFailed) {
		return PinholeRoute::IGD2Fallback;
	}
	return PinholeRoute::PCP;
}
} // namespace pcp
