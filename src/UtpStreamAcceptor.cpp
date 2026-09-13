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

#include "UtpStreamAcceptor.h"

#include "ClientTCPSocket.h"
#include "Logger.h"
#include "ClientList.h"
#include "IPFilter.h"
#include "ListenSocket.h"
#include "ServerConnect.h"
#include "Statistics.h"
#include "StreamTransport.h"
#include "UtpSocketTransport.h"
#include "amule.h"

#ifdef AMULE_UTP_TRANSPORT

bool CUtpStreamAcceptor::AcceptStream(
	std::unique_ptr<IStreamTransport> &transport, uint32_t ip, uint16_t port)
{
	if (!theApp->IsRunning()) {
		return false;
	}
	// The exception the TCP listener makes for itself: refusing while
	// connecting to a server is what produces a LowID on every server, so the
	// limit is allowed to be exceeded in exactly that window.
	if (!theApp->serverconnect->IsConnecting() && theApp->listensocket->TooManySockets()) {
		theStats::AddMaxConnectionLimitReached();
		return false;
	}

	// Every refusal happens before ownership moves, and that ordering is not a
	// style choice. Once the socket holds the transport, deleting it closes the
	// libutp socket through the transport's destructor -- and returning false
	// then has the adapter close the same socket again. Refusing first means
	// exactly one close on every path.
	//
	// The same questions InitNetworkData() asks, against the address the stream
	// arrived from. It runs below anyway once the socket exists, which is where
	// m_remoteip gets set; here it only decides.
	if (ip == 0) {
		return false;
	}
	if (theApp->ipfilter->IsFiltered(ip)) {
		AddDebugLogLineN(logClient,
			CFormat("Denied uTP stream from %s:%u (Filtered IP)") % Uint32toStringIP(ip) % port);
		return false;
	}
	if (theApp->clientlist->IsBannedClient(ip)) {
		AddDebugLogLineN(logClient,
			CFormat("Denied uTP stream from %s:%u (Banned IP)") % Uint32toStringIP(ip) % port);
		return false;
	}

	auto *socket = new CClientTCPSocket();
	// Events wired before ownership moves: once the socket holds the transport,
	// a libutp callback can reach it, and a stream event with nowhere to go is
	// a connection nothing ever services.
	auto *utp = static_cast<CUtpSocketTransport *>(transport.get());
	utp->SetEvents(socket);
	socket->AttachTransport(std::move(transport));
	// Reached through the socket, so it sees the stream's peer rather than the
	// asio socket's absent one, and sets m_remoteip from it. It cannot refuse
	// here: the two checks it makes were made above, and the address is known.
	socket->InitNetworkData();
	AddDebugLogLineN(logClient, CFormat("Accepted uTP stream from %s:%u") % Uint32toStringIP(ip) % port);
	return true;
}

#endif // AMULE_UTP_TRANSPORT
// File_checked_for_headers
