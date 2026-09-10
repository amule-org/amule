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

// See NetworkAddressAsio.h for why this wrap is here and why it is scoped to
// exactly these two diagnostics. ip/tcp.hpp is the heavier of the two asio
// headers this tree includes: it reaches boost/asio/execution/*.hpp, which is
// where the redundant constexpr static definitions live.
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
 * The half of AddressFamilyPolicy whose answers are Boost.Asio values.
 *
 * Split out of AddressFamilyPolicy.h so that the 93 translation units which
 * only need to know *whether* a family is permitted stop compiling asio's
 * socket headers to find out; the reasoning is spelled out at the bottom of
 * that file. Include this only from a TU that actually opens a socket or
 * resolves a name -- today that is LibSocketAsio.cpp and the tests that pin
 * these decisions. Including it from a public header puts asio back into the
 * closure of most of src/, which is what this split undoes.
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

/**
 * The protocol to restrict a name lookup to.
 *
 * getaddrinfo() with an unrestricted family answers with AAAA records on any
 * host, whether or not it has IPv6 connectivity, so the query has to state the
 * family it wants. Under a dual-stack configuration there is nothing to state
 * and the caller should query unrestricted, hence the empty result.
 */
inline boost::optional<boost::asio::ip::tcp> TcpResolverProtocol() noexcept
{
	switch (Configured()) {
	case Families::IPv4Only:
		return boost::asio::ip::tcp::v4();
	case Families::IPv6Only:
		return boost::asio::ip::tcp::v6();
	case Families::DualStack:
		break;
	}
	return boost::none;
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
 * This stays the IPv4 wildcard whenever IPv4 is permitted, dual stack included,
 * and that is deliberate. The callers are the ones that bind a single socket
 * and are not part of the ed2k dual-stack work: the external-connection
 * listener and the web server. Handing them @c :: because the ed2k listener now
 * wants both families would silently move the daemon's control channel onto
 * another family -- a change to what an EC client must dial, made as a side
 * effect. A caller that genuinely wants both families says so by asking for
 * AnyIPv6Address() and clearing @c IPV6_V6ONLY, which is what the ed2k
 * listeners do; see DualStackListeners.h.
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
