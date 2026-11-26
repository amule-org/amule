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

#ifndef KAD_FIREWALL_CHECK_POLICY_H
#define KAD_FIREWALL_CHECK_POLICY_H

#include "SourceRoutePolicy.h" // Needed for EConnectionOpener

/**
 * Whether a connection to a node that asked us to check its TCP port shows the port takes
 * connections, so that our acknowledgement may count as one of its confirmations.
 *
 * Only a plain TCP connection we dialled does. One the node opened, accepted after a rendezvous,
 * or dialled through a NAT mapping a punch opened proves nothing about inbound reachability, and
 * neither does a uTP or QUIC stream. A connection we opened to answer a callback would, but a
 * checker also answering the node's callback is rare enough to leave out.
 */
inline bool ProvesTcpPortReachable(EConnectionOpener opener, bool streamTransport) noexcept
{
	return opener == EConnectionOpener::Dial && !streamTransport;
}

/**
 * Whether a connection an acknowledgement of our own TCP firewall check arrived on shows our port
 * takes connections, so that it may count as one of our confirmations.
 *
 * Only a plain TCP connection the checker opened to us does. A checker that runs eMule or an older
 * aMule may send it over a connection we opened, or one a rendezvous set up, and neither shows
 * anything about our port.
 */
inline bool ProvesOwnTcpPortReachable(EConnectionOpener opener, bool streamTransport) noexcept
{
	return opener == EConnectionOpener::Accepted && !streamTransport;
}

#endif // KAD_FIREWALL_CHECK_POLICY_H
