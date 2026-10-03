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

#include "QuicStreamAcceptor.h"

#include "ClientTCPSocket.h"
#include "ClientList.h"
#include "IPFilter.h"
#include "ListenSocket.h"
#include "Logger.h"
#include "NetworkAddress.h"
#include "ServerConnect.h"
#include "Statistics.h"
#include "StreamTransport.h"
#include "amule.h"

#ifdef AMULE_QUIC_TRANSPORT

bool CQuicStreamAcceptor::AcceptStream(
	std::unique_ptr<IStreamTransport> &transport, const CNetworkAddress &address, uint16_t port)
{
	// The same policy as CUtpStreamAcceptor::AcceptStream(), by design: one admitted stream
	// arrives alone, with no accept burst to fold, for either transport. Not shared via a
	// common function: six lines of identical, already-reviewed logic is not worth a
	// cross-module dependency between the two transports' acceptors.
	if (!theApp->IsRunning()) {
		return false;
	}
	if (!theApp->serverconnect->IsConnecting() && theApp->listensocket->TooManySockets()) {
		theStats::AddMaxConnectionLimitReached();
		return false;
	}
	if (!address.IsPresent()) {
		return false;
	}
	if (theApp->ipfilter->IsFiltered(address)) {
		AddDebugLogLineN(logClient,
			CFormat("Denied QUIC stream from %s:%u (Filtered IP)") % address.ToWxString() % port);
		return false;
	}
	if (theApp->clientlist->IsBannedClient(address)) {
		AddDebugLogLineN(logClient,
			CFormat("Denied QUIC stream from %s:%u (Banned IP)") % address.ToWxString() % port);
		return false;
	}

	auto *socket = new CClientTCPSocket();
	// AttachTransport() installs the event sink before it takes ownership, so an early
	// callback always has somewhere to deliver.
	socket->AttachTransport(std::move(transport));
	// Records m_remoteip; its checks were made above. Refusal still has to delete the socket,
	// as CListenSocket::OnAccept does, or it stays with no address.
	if (!socket->InitNetworkData(CClientTCPSocket::AdmissionTransport::QUIC)) {
		socket->Safe_Delete();
		return false;
	}
	AddDebugLogLineN(logClient, CFormat("Accepted QUIC stream from %s:%u") % address.ToWxString() % port);
	return true;
}

#endif // AMULE_QUIC_TRANSPORT
// File_checked_for_headers
