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
	// Refusals come before ownership moves: destroying a transport closes its
	// socket, and the adapter closes a refused one too.
	const EUtpAdmission decision = DecideUtpAdmission(theApp->IsRunning(),
		theApp->serverconnect->IsConnecting(),
		theApp->listensocket->TooManySockets(),
		ip,
		ip != 0 && theApp->ipfilter->IsFiltered(ip),
		ip != 0 && theApp->clientlist->IsBannedClient(ip));

	switch (decision) {
	case EUtpAdmission::Admit:
		break;
	case EUtpAdmission::TooManySockets:
		// Per stream: a uTP SYN arrives alone, with no accept burst to fold.
		theStats::AddMaxConnectionLimitReached();
		return false;
	case EUtpAdmission::Filtered:
		AddDebugLogLineN(logClient,
			CFormat("Denied uTP stream from %s:%u (Filtered IP)") % Uint32toStringIP(ip) % port);
		return false;
	case EUtpAdmission::Banned:
		AddDebugLogLineN(logClient,
			CFormat("Denied uTP stream from %s:%u (Banned IP)") % Uint32toStringIP(ip) % port);
		return false;
	case EUtpAdmission::ShuttingDown:
	case EUtpAdmission::NoAddress:
		return false;
	}

	auto *socket = new CClientTCPSocket();
	// Before ownership moves, or an early callback has nowhere to deliver.
	auto *utp = static_cast<CUtpSocketTransport *>(transport.get());
	utp->SetEvents(socket);
	socket->AttachTransport(std::move(transport));
	// Records m_remoteip; its checks were made above. Refusal still has to delete
	// the socket, as CListenSocket::OnAccept does, or it stays with no address.
	if (!socket->InitNetworkData()) {
		socket->Safe_Delete();
		return false;
	}
	AddDebugLogLineN(logClient, CFormat("Accepted uTP stream from %s:%u") % Uint32toStringIP(ip) % port);
	return true;
}

#endif // AMULE_UTP_TRANSPORT
// File_checked_for_headers
