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
	// The listener's own exception: refusing while connecting to a server is
	// what produces a LowID on every server.
	if (!theApp->serverconnect->IsConnecting() && theApp->listensocket->TooManySockets()) {
		theStats::AddMaxConnectionLimitReached();
		return false;
	}

	// Refusals come first: destroying a transport closes its socket, so a
	// refusal after ownership moved would close what the adapter closes too.
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
	// Wired before ownership moves, or a callback arriving first has nowhere
	// to deliver.
	auto *utp = static_cast<CUtpSocketTransport *>(transport.get());
	utp->SetEvents(socket);
	socket->AttachTransport(std::move(transport));
	// Records m_remoteip from the stream's peer; its two checks were made above.
	socket->InitNetworkData();
	AddDebugLogLineN(logClient, CFormat("Accepted uTP stream from %s:%u") % Uint32toStringIP(ip) % port);
	return true;
}

#endif // AMULE_UTP_TRANSPORT
// File_checked_for_headers
