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

//! Why an accepted uTP stream was, or was not, taken.
enum class EUtpAdmission
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
 * What CListenSocket::OnAccept and CClientTCPSocket::InitNetworkData ask for
 * TCP. @a connectingToServer is the listener's exception: refusing then is what
 * produces a LowID on every server.
 */
constexpr EUtpAdmission DecideUtpAdmission(
	bool running, bool connectingToServer, bool tooManySockets, uint32_t ip, bool filtered, bool banned)
{
	if (!running) {
		return EUtpAdmission::ShuttingDown;
	}
	if (!connectingToServer && tooManySockets) {
		return EUtpAdmission::TooManySockets;
	}
	if (ip == 0) {
		return EUtpAdmission::NoAddress;
	}
	if (filtered) {
		return EUtpAdmission::Filtered;
	}
	if (banned) {
		return EUtpAdmission::Banned;
	}
	return EUtpAdmission::Admit;
}

//! Gathers those facts from the application and acts on the decision.
class CUtpStreamAcceptor : public IUtpStreamAcceptor
{
public:
	bool AcceptStream(std::unique_ptr<IStreamTransport> &transport, uint32_t ip, uint16_t port) override;
};

#endif // UTPSTREAMACCEPTOR_H
// File_checked_for_headers
