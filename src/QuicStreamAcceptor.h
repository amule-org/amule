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

//! Why an offered QUIC stream was, or was not, taken.
enum class EQuicAdmission
{
	Admit,
	ShuttingDown,
	TooManySockets,
	NoAddress,
	Filtered,
	Banned
};

/**
 * The admission decision, separated from gathering the facts it needs.
 *
 * DecideUtpAdmission() less its half-open budget, which counts streams before their first data: a
 * QUIC stream is offered at its first data. No exception while connecting to a server either: that
 * one is the TCP listener's, for the server's TCP callback test.
 */
constexpr EQuicAdmission DecideQuicAdmission(
	bool running, bool tooManySockets, bool hasAddress, bool filtered, bool banned)
{
	if (!running) {
		return EQuicAdmission::ShuttingDown;
	}
	if (tooManySockets) {
		return EQuicAdmission::TooManySockets;
	}
	if (!hasAddress) {
		return EQuicAdmission::NoAddress;
	}
	if (filtered) {
		return EQuicAdmission::Filtered;
	}
	if (banned) {
		return EQuicAdmission::Banned;
	}
	return EQuicAdmission::Admit;
}

//! Gathers those facts from the application and acts on the decision.
class CQuicStreamAcceptor : public IQuicStreamAcceptor
{
public:
	bool AcceptStream(std::unique_ptr<IStreamTransport> &transport,
		const CNetworkAddress &address,
		uint16_t port) override;
};

#endif // QUICSTREAMACCEPTOR_H
// File_checked_for_headers
