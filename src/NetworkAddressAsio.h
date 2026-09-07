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

#ifndef NETWORKADDRESSASIO_H
#define NETWORKADDRESSASIO_H

#include "NetworkAddress.h"

// Boost.Asio's socket headers redeclare constexpr static members out of line
// and give several classes a user-provided destructor alongside an implicit
// copy constructor. Both are deprecated in C++17 and both are diagnosed by
// Clang, so with the -Werror=deprecated gate in src/CMakeLists.txt they fail
// the build -- in third-party code this tree does not own. Same pragma-wrap
// convention as CryptoPP_Inc.h and the wx wraps from #341, and the same
// reason: the headers are discovered without -isystem, so the gate reaches
// them. Kept to exactly these two diagnostics rather than a blanket
// -Wno-deprecated, so a genuine deprecation in aMule's own code still fails.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-copy-with-user-provided-dtor"
#pragma clang diagnostic ignored "-Wdeprecated-redundant-constexpr-static-def"
#endif
#include <boost/asio/ip/address.hpp>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

/**
 * The one bridge between CNetworkAddress and Boost.Asio.
 *
 * CNetworkAddress deliberately stores its own sixteen octets rather than an
 * asio address, so that the 155 translation units that merely pass an address
 * around stop compiling asio's ~1200-header closure and stop needing @c ws2_32
 * at link time (see the class comment in NetworkAddress.h). The socket backend
 * still has to hand a real @c asio::ip::address to a real socket, so the
 * conversion lives here -- in a header that says asio in its name, and that
 * only the TUs actually opening sockets include.
 *
 * Include this from a TU that talks to asio. Do @b not include it from a public
 * header: doing so re-establishes exactly the coupling this file exists to
 * confine, and nothing will warn you about it until macOS and mingw-w64 CI do.
 */

/**
 * Both conversions are namespaced rather than free at global scope. They take a
 * boost::asio::ip::address, which makes an unqualified pair argument-dependent
 * lookup candidates for every call in a translation unit that mentions an asio
 * address -- and the call sites this type is for will include this header
 * widely. Nothing collides today; the namespace is here because it is cheap
 * now and awkward once those call sites exist.
 */
namespace NetworkAddressAsio
{

/**
 * Widens a CNetworkAddress into the asio value a socket call needs.
 *
 * @pre @a address.IsPresent(). Absence has no asio equivalent -- asio's own
 *      default-constructed address is @c 0.0.0.0, which is exactly the
 *      conflation CNetworkAddress exists to prevent, so an absent address must
 *      be handled by the caller rather than silently becoming the wildcard.
 */
boost::asio::ip::address ToAsioAddress(const CNetworkAddress &address);

/**
 * Narrows an asio address into a CNetworkAddress, preserving the family, the
 * octets and the IPv6 scope id exactly.
 *
 * Every asio address is a present address: there is no value of the argument
 * that yields absence, @c 0.0.0.0 included. A caller that wants asio's
 * all-zero address treated as "no address" must test for that itself, at the
 * edge where it knows the overload applies.
 */
CNetworkAddress FromAsioAddress(const boost::asio::ip::address &address);

} // namespace NetworkAddressAsio

#endif // NETWORKADDRESSASIO_H
// File_checked_for_headers
