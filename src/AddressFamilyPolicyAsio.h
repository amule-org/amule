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

#include <boost/optional.hpp>

// Reviewer measurement with AppleClang 21 and Boost 1.92: without both
// suppressions, ip/tcp.hpp produces 29 errors under -Werror=deprecated:
// 27 redundant constexpr static definitions and 2 deprecated copies with a
// user-provided destructor. Each suppression is independently necessary;
// restrict both to this Boost include.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-copy-with-user-provided-dtor"
#pragma clang diagnostic ignored "-Wdeprecated-redundant-constexpr-static-def"
#endif
#include <boost/asio/ip/tcp.hpp>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

/**
 * Socket-specific address-family policy and Boost.Asio values.
 *
 * Keeping this separate avoids compiling Asio socket headers in consumers that
 * only need family predicates. Include it only where socket protocols or name
 * resolution are needed.
 */
namespace AddressFamilyPolicy
{

/**
 * The TCP protocol a socket towards @a target must be opened in.
 *
 * @return The protocol, or no value when @a target is absent or its family is
 *         not permitted by the configuration. There is no fallback: opening a
 *         v4 socket for a v6 target is how a truncated address turns into a
 *         connection to the wrong host.
 */
inline boost::optional<boost::asio::ip::tcp> TcpProtocolForTarget(const CNetworkAddress &target) noexcept
{
	if (!Permits(target)) {
		return boost::none;
	}
	if (target.IsIPv4() || target.IsIPv4Mapped()) {
		return boost::asio::ip::tcp::v4();
	}
	return boost::asio::ip::tcp::v6();
}

/** An explicit lookup-family restriction; Any means unrestricted, not refusal. */
enum class ResolverFamily
{
	Any,
	IPv4Only,
	IPv6Only
};

/**
 * The family restriction for a name lookup.
 *
 * Dual stack requests an unrestricted lookup. A single-family configuration
 * restricts results to that family; DNS results do not establish connectivity.
 */
inline ResolverFamily TcpResolverProtocol() noexcept
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
 * The IPv6 wildcard, @c ::. With @c IPV6_V6ONLY off it also accepts IPv4 peers,
 * which arrive in IPv4-mapped form.
 */
inline boost::asio::ip::address AnyIPv6Address() noexcept
{
	return boost::asio::ip::address(boost::asio::ip::address_v6::any());
}

/**
 * The wildcard "any address of this machine" for a caller that has not said
 * which family it wants.
 *
 * Prefer the IPv4 wildcard whenever IPv4 is permitted, including dual stack,
 * to avoid implicitly moving a single-socket service to another family.
 * Accepting both families on an IPv6 socket requires explicitly choosing
 * AnyIPv6Address() and clearing @c IPV6_V6ONLY.
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
