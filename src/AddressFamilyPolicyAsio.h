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

#ifndef ADDRESSFAMILYPOLICYASIO_H
#define ADDRESSFAMILYPOLICYASIO_H

#include "AddressFamilyPolicy.h"
#include "NetworkAddressAsio.h" // Needed for ToAsioAddress in AsioTargetFor

#include <boost/optional.hpp>

#include "WarningsPush_Asio.h"
#include <boost/asio/ip/tcp.hpp>
#include "WarningsPop.h"

/**
 * Socket-specific address-family policy and Boost.Asio values.
 *
 * Split from AddressFamilyPolicy.h so that a translation unit which only needs to know @b whether a
 * family is permitted does not compile asio's socket headers to find out. Unlike asio's
 * ip/address.hpp, ip/tcp.hpp reaches the boost/asio/execution headers, which do not survive this
 * tree's -Werror=deprecated gate unmodified -- see the measurement above the include.
 *
 * Include this only from a TU that opens a socket or resolves a name; including it from a public
 * header puts asio back into the closure of most of src/, which is what the split undoes.
 */
namespace AddressFamilyPolicy
{

/**
 * The TCP protocol a socket towards @a target must be opened in.
 *
 * @return The protocol, or no value when @a target is absent or its family is not permitted by the
 *         configuration. There is no fallback: opening a v4 socket for a v6 target is how a
 *         truncated address turns into a connection to the wrong host.
 */
inline boost::optional<boost::asio::ip::tcp> TcpProtocolForTarget(const CNetworkAddress &target) noexcept
{
	if (!Permits(target)) {
		return boost::none;
	}
	if (IsIPv4Reachable(target)) {
		return boost::asio::ip::tcp::v4();
	}
	return boost::asio::ip::tcp::v6();
}

/** A protocol and the endpoint address to use with it, guaranteed to be the same family. */
struct SAsioTarget
{
	boost::asio::ip::tcp protocol;
	boost::asio::ip::address address;
};

/**
 * The protocol and address for one connection attempt, decided together.
 *
 * Asking TcpProtocolForTarget() and NetworkAddressAsio::ToAsioAddress() separately does not
 * work for an IPv4-mapped target: the first says @c tcp::v4() because a mapped address is
 * reachable over IPv4, the second preserves the family and hands back an @c address_v6, and
 * connecting the two gives EAFNOSUPPORT on every mapped peer. Narrowing the address here is
 * what makes the pair agree, and returning them together is what stops them drifting apart
 * again.
 *
 * Returning one optional also removes the other half of that footgun. With two calls a caller
 * writes `sock.open(*TcpProtocolForTarget(t))` after a separate Permits() check, and
 * dereferences an empty optional if anything moved in between; here there is one decision and
 * one thing to test.
 *
 * @return The pair, or no value when @a target is absent, unspecified, or of a family the
 *         configuration does not permit.
 */
inline boost::optional<SAsioTarget> AsioTargetFor(const CNetworkAddress &target) noexcept
{
	if (!Permits(target)) {
		return boost::none;
	}
	const CNetworkAddress narrowed = target.Unmapped();
	SAsioTarget result = { IsIPv4Reachable(narrowed) ? boost::asio::ip::tcp::v4()
							 : boost::asio::ip::tcp::v6(),
		NetworkAddressAsio::ToAsioAddress(narrowed) };
	return result;
}

/** An explicit lookup-family restriction; Any means unrestricted, not refusal. */
enum class ResolverFamily
{
	Any,
	IPv4Only,
	IPv6Only
};

/**
 * The family restriction for a name lookup. Dual stack requests an unrestricted lookup. A single-
 * family configuration restricts results to that family; DNS results do not establish connectivity.
 */
inline ResolverFamily ResolverFamilyForLookup() noexcept
{
	switch (Configured()) {
	case Families::IPv4Only:
		return ResolverFamily::IPv4Only;
	case Families::IPv6Only:
		return ResolverFamily::IPv6Only;
	case Families::DualStack:
		return ResolverFamily::Any;
	}
	return ResolverFamily::IPv4Only;
}

/** The IPv4 wildcard, @c 0.0.0.0. */
inline boost::asio::ip::address AnyIPv4Address() noexcept
{
	return boost::asio::ip::address(boost::asio::ip::address_v4::any());
}

/**
 * The IPv6 wildcard, @c ::. With @c IPV6_V6ONLY off it also accepts IPv4 peers, which arrive in
 * IPv4-mapped form.
 */
inline boost::asio::ip::address AnyIPv6Address() noexcept
{
	return boost::asio::ip::address(boost::asio::ip::address_v6::any());
}

/**
 * The wildcard "any address of this machine" for a caller that has not said which family it wants.
 *
 * Prefer the IPv4 wildcard whenever IPv4 is permitted, including dual stack, so a single-socket
 * service is not implicitly moved to another family. Accepting both families on an IPv6 socket
 * requires explicitly choosing AnyIPv6Address() and clearing @c IPV6_V6ONLY.
 *
 * Under @c IPv6Only this returns the IPv6 wildcard, and the caller must then @b set
 * @c IPV6_V6ONLY rather than leave it at the platform default. Linux defaults it off, so the
 * listener would accept IPv4 peers as mapped addresses while Permits() refuses mapped addresses
 * under that configuration: connections arriving that the policy will not talk to. This header
 * cannot set socket options, so the obligation is the caller's either way.
 */
inline boost::asio::ip::address AnyAddress() noexcept
{
	if (PermitsIPv4()) {
		return AnyIPv4Address();
	}
	return AnyIPv6Address();
}

} // namespace AddressFamilyPolicy

#endif // ADDRESSFAMILYPOLICYASIO_H
// File_checked_for_headers
