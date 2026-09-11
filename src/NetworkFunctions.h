//
// This file is part of the aMule Project.
//
// Copyright (c) 2004-2011 Angel Vidal ( kry@amule.org )
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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

#ifndef NETWORK_FUNCTIONS_H
#define NETWORK_FUNCTIONS_H

#include "Types.h"         // Needed for uint16 and uint32
#include <common/Format.h> // Needed for CFormat

// Network ip/host handling functions
// These functions take IPs in anti-host order

inline wxString Uint32toStringIP(uint32 ip)
{
	return CFormat("%u.%u.%u.%u") % (uint8)ip % (uint8)(ip >> 8) % (uint8)(ip >> 16) % (uint8)(ip >> 24);
}

inline wxString Uint32_16toStringIP_Port(uint32 ip, uint16 port)
{
	return CFormat("%u.%u.%u.%u:%u") % (uint8)ip % (uint8)(ip >> 8) % (uint8)(ip >> 16) %
	       (uint8)(ip >> 24) % port;
}

// These functions take IPs in host-order
inline wxString KadIPToString(uint32_t ip)
{
	return CFormat("%u.%u.%u.%u") % (uint8_t)(ip >> 24) % (uint8_t)(ip >> 16) % (uint8_t)(ip >> 8) %
	       (uint8_t)ip;
}

inline wxString KadIPPortToString(uint32_t ip, uint16_t port)
{
	return CFormat("%u.%u.%u.%u:%u") % (uint8_t)(ip >> 24) % (uint8_t)(ip >> 16) % (uint8_t)(ip >> 8) %
	       (uint8_t)ip % port;
}

/**
 * Parses a string IP of the form "a.b.c.d" into @a Ip, returning whether it parsed. Whitespace
 * around the address is ignored and the result is saved in anti-host order.
 *
 * This exists because the standard inet_aton treats a 0-prefixed number as octal. @a Ip is left
 * unchanged unless the string holds a valid IP address.
 */
bool StringIPtoUint32(const wxString &strIP, uint32 &Ip);

/**
 * Parses a string IP of the form "a.b.c.d" and returns it in anti-host order, or zero if it was
 * invalid (or 0.0.0.0).
 */
inline uint32 StringIPtoUint32(const wxString &strIP)
{
	uint32 ip = 0;
	StringIPtoUint32(strIP, ip);

	return ip;
}

/**
 * Parses a host string and returns its IP in anti-host order, or zero if it was invalid (or
 * 0.0.0.0).
 */
uint32 StringHosttoUint32(const wxString &Host);

/**
 * True if @a IP is a valid address. @a filterLAN also rejects the LAN ranges. @a IP must be in
 * anti-host order (BE on an LE platform, LE on a BE one).
 */
bool IsGoodIP(uint32 IP, bool filterLAN) noexcept;

inline bool IsGoodIPPort(uint32 nIP, uint16 nPort) noexcept
{
	return IsGoodIP(nIP, true) && nPort != 0;
}

#define HIGHEST_LOWID_ED2K_KAD 16777216

inline bool IsLowID(uint32 id)
{
	return (id < HIGHEST_LOWID_ED2K_KAD);
}

/**
 * True if @a ip is a LAN address. Anti-host order.
 */
bool IsLanIP(uint32_t ip) noexcept;

/**
 * True if @a ip is in the IPv4 loopback range 127.0.0.0/8. Anti-host order, the same convention as
 * StringIPtoUint32 and IsLanIP.
 */
bool IsLoopbackIP(uint32_t ip) noexcept;

/**
 * True if @a ip is in the IPv4 link-local range 169.254.0.0/16 (RFC3927 / zeroconf). Anti-host
 * order.
 */
bool IsLinkLocalIP(uint32_t ip) noexcept;

#endif // NETWORK_FUNCTIONS_H
// File_checked_for_headers
