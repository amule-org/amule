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
#include "amule.h"
#include "updownclient.h"
#include "kademlia/kademlia/Kademlia.h"
#include "kademlia/kademlia/Prefs.h"
#include "kademlia/kademlia/UDPFirewallTester.h"

#include <algorithm>

#ifdef AMULE_UTP_TRANSPORT

SUtpIncomingSynDecision CUtpStreamAcceptor::DecideIncomingSyn(
	uint32_t ip, uint16_t port, const SUtpSynCandidateInfo &candidate)
{
	if (candidate.status == EUtpSynCandidateStatus::None) {
		return {};
	}
	if (candidate.status != EUtpSynCandidateStatus::Unique || candidate.clientToken == 0 ||
		candidate.ip == 0 || candidate.port == 0 || !theApp->clientlist) {
		return { EUtpIncomingSynDisposition::RejectKeepOutgoing, 0 };
	}

	CUpDownClient *peer = theApp->clientlist->FindClientByECID(candidate.clientToken);
	if (peer == nullptr || peer->IsConnected()) {
		return { EUtpIncomingSynDisposition::RejectKeepOutgoing, 0 };
	}
	CClientTCPSocket *outgoing = peer->GetSocket();
	if (outgoing == nullptr || !outgoing->HasTransport() || outgoing->IsUtpInbound()) {
		return { EUtpIncomingSynDisposition::RejectKeepOutgoing, 0 };
	}

	std::optional<SUtpEndpoint> localEndpoint;
	const uint32_t publicIp = theApp->GetPublicIP(true);
	if (publicIp != 0 && Kademlia::CKademlia::IsConnected() &&
		Kademlia::CKademlia::GetPrefs()->GetUseExternKadPort() &&
		Kademlia::CUDPFirewallTester::IsVerified()) {
		localEndpoint = MakeUtpEndpointFromNetworkOrder(
			publicIp, Kademlia::CKademlia::GetPrefs()->GetExternalKadPort());
	}
	const std::optional<SUtpEndpoint> remoteEndpoint = MakeUtpEndpointFromNetworkOrder(ip, port);

	const uint8_t *localHash = thePrefs::GetUserHash().GetHash();
	const bool localHashValid =
		localHash != nullptr &&
		std::any_of(localHash, localHash + 16, [](uint8_t byte) { return byte != 0; });
	const bool peerHashValid = peer->HasValidHash();
	const uint8_t *peerHash = peerHashValid ? peer->GetUserHash().GetHash() : nullptr;
	if (DecideUtpSimultaneousOpen(
		    localEndpoint, remoteEndpoint, localHash, localHashValid, peerHash, peerHashValid) ==
		EUtpSimultaneousOpenDecision::KeepIncoming) {
		return { EUtpIncomingSynDisposition::AcceptKeepIncoming, candidate.clientToken };
	}
	return { EUtpIncomingSynDisposition::RejectKeepOutgoing, 0 };
}

bool CUtpStreamAcceptor::AcceptStream(
	std::unique_ptr<IStreamTransport> &transport, uint32_t ip, uint16_t port)
{
	return AcceptStream(transport, ip, port, {});
}

bool CUtpStreamAcceptor::AcceptStream(std::unique_ptr<IStreamTransport> &transport,
	uint32_t ip,
	uint16_t port,
	const SUtpIncomingSynDecision &synDecision)
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
	if (synDecision.disposition == EUtpIncomingSynDisposition::AcceptKeepIncoming) {
		socket->SetUtpIncomingSynWinner(synDecision.candidateToken);
	}
	// AttachTransport() installs the event sink before it takes ownership, so
	// an early callback always has somewhere to deliver.
	socket->AttachTransport(std::move(transport));
	// Records m_remoteip; its checks were made above. Refusal still has to delete
	// the socket, as CListenSocket::OnAccept does, or it stays with no address.
	if (!socket->InitNetworkData(CClientTCPSocket::AdmissionTransport::UTP)) {
		socket->Safe_Delete();
		return false;
	}
	AddDebugLogLineN(logClient, CFormat("Accepted uTP stream from %s:%u") % Uint32toStringIP(ip) % port);
	return true;
}

#endif // AMULE_UTP_TRANSPORT
// File_checked_for_headers
