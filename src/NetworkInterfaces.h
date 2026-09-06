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

#ifndef NETWORKINTERFACES_H
#define NETWORKINTERFACES_H

#include <array>
#include <cstdint>
#include <vector>

#include <wx/arrstr.h>
#include <wx/string.h>

/**
 * One of this machine's network interfaces, with the addresses assigned to it.
 *
 * Name and addresses arrive together from the platform APIs, so they are
 * reported together: splitting them would mean walking the interface list
 * twice and risking two different answers.
 */
struct NetworkInterface
{
	//! Exactly what goes into the "bind to interface" preference and is later
	//! resolved back to an index in LibSocketAsio.cpp: a POSIX interface name
	//! (en0, eth0, tun0) or, on Windows, an adapter friendly name (Ethernet,
	//! Wi-Fi).
	wxString name;

	//! The OS interface index, or 0 when the platform did not report one.
	//! Windows adapter friendly names cannot be fed to if_nametoindex(), which
	//! wants the GUID-style name, so the index has to come out of the same
	//! enumeration that produced the friendly name.
	unsigned int index = 0;

	//! Assigned IPv4 addresses, printable, for the "bind to IP" drop-downs.
	wxArrayString ipv4;

	//! Assigned IPv6 addresses, 16 bytes each, big-endian as they travel on
	//! the wire. Addresses still running duplicate address detection are
	//! excluded: until DAD finishes the address may belong to somebody else on
	//! the link, so treating it as ours is exactly the case DAD exists to
	//! catch.
	std::vector<std::array<std::uint8_t, 16>> ipv6;
};

/**
 * Enumerate this machine's usable network interfaces.
 *
 * Loopback is skipped; callers that want 127.0.0.1 offer it unconditionally
 * rather than only when the loopback interface happens to enumerate. An
 * interface that is down right now -- a VPN tunnel, a laptop on another
 * network -- simply does not appear, which is why the preference controls stay
 * editable rather than being restricted to this list.
 *
 * Walks the interface list on every call. Callers on a path a remote peer can
 * drive must cache the result instead of calling it per event.
 */
std::vector<NetworkInterface> DetectNetworkInterfaces();

#endif // NETWORKINTERFACES_H
