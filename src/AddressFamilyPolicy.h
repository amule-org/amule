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
 * IPv4-only is the default. Dual stack requires an explicit SetConfigured()
 * call; providing this policy does not enable IPv6 or change existing sockets.
 *
 * Kademlia remains IPv4: its wire format carries 32-bit addresses and its
 * routing table keys on them. A socket-family policy cannot widen that format.
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
 * Atomic to allow reads from socket-opening and name-resolution threads while
 * configuration is set from the main thread. Relaxed
 * ordering is enough: nothing else is published with it, and a socket opened in
 * the same instant as a reconfiguration is allowed to see either value.
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
 * Socket protocols, resolver-family selection and wildcard addresses live in
 * AddressFamilyPolicyAsio.h, because their return types are Boost.Asio values
 * and naming those here would pull asio's executor machinery into every TU
 * that only wants to ask which family is permitted. What stays here needs no
 * library: Configured(), the two Permits predicates and Families are the
 * decision itself.
 */

} // namespace AddressFamilyPolicy

#endif // ADDRESSFAMILYPOLICY_H
// File_checked_for_headers
