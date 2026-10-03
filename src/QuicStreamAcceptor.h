//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// Any parts of this program contributed by third-party developers are copyrighted
// by their respective authors.
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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
//

#ifndef QUICSTREAMACCEPTOR_H
#define QUICSTREAMACCEPTOR_H

#include "StreamTransport.h"

#include <memory>

class CNetworkAddress;

//! Where a QUIC connection whose stream has just opened is offered for admission. The twin of
//! IUtpStreamAcceptor, offered at the same point in a connection's life: the first byte the peer
//! actually sends, not the handshake alone.
class IQuicStreamAcceptor
{
public:
	virtual ~IQuicStreamAcceptor() = default;

	/**
	 * Offers one admitted stream.
	 *
	 * By reference on purpose, matching IUtpStreamAcceptor::AcceptStream(): a refusal leaves
	 * the transport in the caller's hands rather than destroying it as a side effect of a
	 * by-value parameter going out of scope.
	 */
	virtual bool AcceptStream(std::unique_ptr<IStreamTransport> &transport,
		const CNetworkAddress &address,
		uint16_t port) = 0;
};

//! Gathers the same facts CUtpStreamAcceptor gathers, from the same application state, and acts
//! on the same decision -- the two transports share one admission policy by design.
class CQuicStreamAcceptor : public IQuicStreamAcceptor
{
public:
	bool AcceptStream(std::unique_ptr<IStreamTransport> &transport,
		const CNetworkAddress &address,
		uint16_t port) override;
};

#endif // QUICSTREAMACCEPTOR_H
// File_checked_for_headers
