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

#ifndef UTPSTREAMACCEPTOR_H
#define UTPSTREAMACCEPTOR_H

#include "UtpContext.h"

/**
 * Decides whether an accepted uTP stream becomes a client connection.
 *
 * The same four questions the TCP listener asks, in the same order and for the
 * same reasons: are we running, are we already at the connection limit, is the
 * address filtered, is the peer banned. A stream that gets past libutp is not
 * yet a peer -- it has only proved it can complete a handshake, which any
 * address can.
 *
 * Deliberately not merged into CListenSocket::OnAccept(). That loop is driven
 * by an asio acceptor with a socket queue it polls; this arrives as a single
 * callback with the stream already built, so sharing the loop would mean
 * inventing a queue for one element.
 */
class CUtpStreamAcceptor : public IUtpStreamAcceptor
{
public:
	bool AcceptStream(std::unique_ptr<IStreamTransport> transport, uint32_t ip, uint16_t port) override;
};

#endif // UTPSTREAMACCEPTOR_H
// File_checked_for_headers
