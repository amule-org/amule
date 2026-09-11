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

#ifndef STREAMTRANSPORT_H
#define STREAMTRANSPORT_H

#include "NetworkAddress.h"

#include <cstddef>
#include <cstdint>

/**
 * What a byte stream has to do to stand in for a TCP socket here.
 *
 * The eD2k stack above -- CEMSocket and everything it carries -- is written
 * against CLibSocket, so a second transport is only possible if it answers the
 * same questions. This is that set of questions, named once, so a transport can
 * be written and tested without a socket, a peer, or the app.
 *
 * The contract that matters is the would-block one, because CEMSocket depends
 * on it rather than merely tolerating it:
 *
 *   - a read or write that cannot proceed returns 0, sets the matching Blocks
 *     flag, and leaves LastError() at 0;
 *   - a transport that returns 0 with an error set is reporting a dead
 *     connection, and the stack drops the peer.
 *
 * Returning 0 and an error for a full send window is therefore not a cosmetic
 * mistake: it disconnects a peer whose window will open a millisecond later.
 *
 * Deliberately free of Boost.Asio and of libutp. Both exist below this line;
 * neither belongs in the include closure of the code that only wants bytes.
 */
class IStreamTransport
{
public:
	virtual ~IStreamTransport() = default;

	//! Whether the stream has completed its handshake and not yet ended.
	virtual bool IsConnected() const = 0;

	//! Whether the stream is usable. False once it has failed or been closed.
	virtual bool IsOk() const = 0;

	//! Bytes moved into @a buffer. 0 with BlocksRead() means "not yet", not "never".
	virtual uint32_t Read(void *buffer, uint32_t length) = 0;

	//! Bytes accepted from @a buffer. 0 with BlocksWrite() means "not yet", not "never".
	virtual uint32_t Write(const void *buffer, uint32_t length) = 0;

	//! Ends the stream. Calling it twice must be harmless.
	virtual void Close() = 0;

	virtual bool BlocksRead() const = 0;
	virtual bool BlocksWrite() const = 0;

	/**
	 * Zero unless the stream actually failed. Never set for a would-block.
	 *
	 * Only its truthiness is defined: the value is opaque and carries no
	 * meaning a caller may act on. It shares its name and type with
	 * `CLibSocket::LastError()`, which returns a boost error_code value, so
	 * the trap this warns about is a call site comparing against an errno or
	 * WinSock constant and matching by coincidence. Implementations keep
	 * their values outside those ranges so such a comparison cannot
	 * accidentally succeed; ask the transport for the reason instead.
	 */
	virtual int LastError() const = 0;

	/**
	 * The peer's address, full width.
	 *
	 * Deliberately not the 32-bit form `CLibSocket::GetPeerInt()` returns,
	 * even though mirroring it would make substitution simpler: this series
	 * exists because a peer's identity does not fit in 32 bits, and a new
	 * interface that narrows by construction would put back exactly what
	 * NetworkAddress.h was added to remove. A call site that genuinely needs
	 * the eD2k wire form narrows with ToIPv4NetworkOrder() and handles the
	 * failure, which is the point of that API.
	 */
	virtual CNetworkAddress GetPeerAddress() const = 0;
	virtual uint16_t GetPeerPort() const = 0;
};

#endif // STREAMTRANSPORT_H
// File_checked_for_headers
