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

#ifndef ADDRESSFAMILYPOLICY_H
#define ADDRESSFAMILYPOLICY_H

#include "NetworkAddress.h"

#include <atomic>

/**
 * The single place that decides which address families aMule uses.
 *
 * The Asio backend used to answer that question with a literal
 * @c ip::tcp::v4() at every socket-opening site, which meant the family was not
 * a decision at all -- it was five independent constants. This namespace makes
 * it one decision, derived either from configuration or from the target
 * address, so that enabling IPv6 later is a change to Configured() and not a
 * hunt through LibSocketAsio.cpp.
 *
 * The configured answer is dual stack as of amule-dual-stack-reachability, and
 * it is now a runtime value rather than a compile-time constant: a host with no
 * IPv6 stack must keep working exactly as it did, and that is decided when the
 * listeners are bound, not when the tree is compiled.
 *
 * What dual stack does @b not mean here: Kademlia stays IPv4. Its wire format
 * carries 32-bit addresses and its routing table keys on them, so widening the
 * socket layer gives Kad nothing to widen into. That boundary is deliberate and
 * is documented in openspec/specs/network-addressing/spec.md.
 */
namespace AddressFamilyPolicy
{

enum class Families
{
	IPv4Only,
	IPv6Only,
	DualStack
};

/**
 * The configured family set.
 *
 * Atomic because it is read from the Asio thread pool (socket opening, name
 * resolution) and written once from the main thread during startup. Relaxed
 * ordering is enough: nothing else is published with it, and a socket opened in
 * the same instant as a reconfiguration is allowed to see either value.
 */
inline std::atomic<Families> &ConfiguredStorage() noexcept
{
	static std::atomic<Families> families{ Families::DualStack };
	return families;
}

inline Families Configured() noexcept
{
	return ConfiguredStorage().load(std::memory_order_relaxed);
}

/**
 * Sets the configured family set. Called once from startup with what the user
 * asked for; also used by tests to reach the branches a given host cannot.
 *
 * Sockets already open are unaffected -- this decides what the @b next socket
 * does, exactly like the bind-interface setting next to it.
 */
inline void SetConfigured(Families families) noexcept
{
	ConfiguredStorage().store(families, std::memory_order_relaxed);
}

inline bool PermitsIPv4() noexcept
{
	return Configured() != Families::IPv6Only;
}

inline bool PermitsIPv6() noexcept
{
	return Configured() != Families::IPv4Only;
}

/**
 * Whether a socket may be opened towards @a target at all.
 *
 * An IPv4-mapped IPv6 target counts as IPv4: it narrows losslessly, so an
 * IPv4-only configuration can reach it.
 */
inline bool Permits(const CNetworkAddress &target) noexcept
{
	if (target.IsAbsent()) {
		return false;
	}
	if (target.IsIPv4() || target.IsIPv4Mapped()) {
		return PermitsIPv4();
	}
	return PermitsIPv6();
}

/*
 * The rest of this policy -- the TCP protocol for a target, the resolver
 * protocol, and the wildcard addresses -- is in AddressFamilyPolicyAsio.h.
 *
 * Those five functions are the ones whose return type is a Boost.Asio type,
 * and keeping them here meant this header included <boost/asio/ip/tcp.hpp>.
 * That include reached 93 translation units, and unlike asio's ip/address.hpp
 * it drags in asio's executor machinery (the boost/asio/execution headers), which
 * redeclares constexpr static members out of line -- deprecated in C++17, and
 * an error under the -Werror=deprecated gate in src/CMakeLists.txt on every
 * Clang build. So 93 TUs failed on macOS to answer a question that, for all
 * but the socket backend, is just "which family".
 *
 * What stayed here needs no library: Configured(), the two Permits predicates
 * and Families are the decision itself. Include the Asio twin only from a TU
 * that opens sockets; a caller that only needs the IPv6 wildcard as a value
 * should use CNetworkAddress::AnyIPv6() instead and stay asio-free.
 */

} // namespace AddressFamilyPolicy

#endif // ADDRESSFAMILYPOLICY_H
// File_checked_for_headers
