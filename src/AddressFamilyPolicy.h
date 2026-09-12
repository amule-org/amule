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
 * Address-family decisions derived from configuration and target addresses.
 *
 * IPv4-only is the default. Dual stack requires an explicit SetConfigured() call; providing this
 * policy does not enable IPv6 or change existing sockets.
 *
 * Kademlia remains IPv4: its wire format carries 32-bit addresses and its routing table keys on
 * them. A socket-family policy cannot widen that format.
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
 * Atomic so socket-opening and name-resolution threads can read it while configuration is set from
 * the main thread. Relaxed ordering is enough: nothing else is published with it, and a socket
 * opened in the same instant as a reconfiguration may see either value.
 */
inline std::atomic<Families> &ConfiguredStorage() noexcept
{
	static std::atomic<Families> families{ Families::IPv4Only };
	return families;
}

inline Families Configured() noexcept
{
	return ConfiguredStorage().load(std::memory_order_relaxed);
}

/**
 * Sets the configured family set explicitly, including opting into dual stack.
 *
 * Sockets already open are unaffected -- this decides what the @b next socket does, exactly like
 * the bind-interface setting next to it.
 */
inline void SetConfigured(Families families) noexcept
{
	ConfiguredStorage().store(families, std::memory_order_relaxed);
}

// Both predicates switch rather than test for inequality, so an out-of-range value falls back the
// same way ResolverFamilyForLookup() does. Comparing against one enumerator made a value outside
// the enum permit *both* families while the resolver fell back to IPv4Only, and piece 4 will build
// this from a configuration integer, which is exactly where such a value comes from.
inline bool PermitsIPv4() noexcept
{
	switch (Configured()) {
	case Families::IPv4Only:
	case Families::DualStack:
		return true;
	case Families::IPv6Only:
		return false;
	}
	return true; // Same IPv4-only fallback as the resolver.
}

inline bool PermitsIPv6() noexcept
{
	switch (Configured()) {
	case Families::IPv6Only:
	case Families::DualStack:
		return true;
	case Families::IPv4Only:
		return false;
	}
	return false; // Same IPv4-only fallback as the resolver.
}

/**
 * Whether @a target is reachable over IPv4, mapped form included.
 *
 * One definition, because the asio half asks the same question when it picks a protocol. Two
 * copies would let a later change to what counts as IPv4, for NAT64 or 6to4, be applied to one
 * of them and reintroduce a v4 socket opened towards a v6 endpoint.
 */
inline bool IsIPv4Reachable(const CNetworkAddress &target) noexcept
{
	return target.IsIPv4() || target.IsIPv4Mapped();
}

/**
 * Whether a socket may be opened towards @a target at all.
 *
 * An IPv4-mapped IPv6 target counts as IPv4: it narrows losslessly, so an IPv4-only configuration
 * can reach it.
 *
 * Absence is refused, and so is the unspecified address in either spelling. It arrives as a
 * present IPv4 value, so the family test alone would permit it under every configuration, and a
 * call site replacing an old @c if(ip) guard would dial 0.0.0.0 -- which on Linux connects to
 * this machine. A value that names nobody is not a target.
 */
inline bool Permits(const CNetworkAddress &target) noexcept
{
	// Absence names no peer, and neither does the unspecified address. It reaches here as a
	// present IPv4 value, so the family test would permit it under any configuration, and a
	// call site replacing an old `if (ip)` guard would dial 0.0.0.0 -- which on Linux connects
	// to this machine. "May a socket be opened towards this" has to answer no.
	if (target.IsAbsent() || target.Unmapped().IsUnspecified()) {
		return false;
	}
	if (IsIPv4Reachable(target)) {
		return PermitsIPv4();
	}
	return PermitsIPv6();
}

/*
 * Socket protocols, resolver-family selection and wildcard addresses live in
 * AddressFamilyPolicyAsio.h, because their return types are Boost.Asio values and naming those
 * here would pull asio's executor machinery into every TU that only wants to ask which family is
 * permitted. What stays here needs no library: Configured(), the two Permits predicates and
 * Families are the decision itself.
 */

} // namespace AddressFamilyPolicy

#endif // ADDRESSFAMILYPOLICY_H
// File_checked_for_headers
