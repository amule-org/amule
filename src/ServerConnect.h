//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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

// Client to Server communication

#ifndef SERVERCONNECT_H
#define SERVERCONNECT_H

// AMULE_UTP_TRANSPORT and AMULE_QUIC_TRANSPORT change this header and are defined only for
// amule and amuled. A shared library unit would see another layout than the binary it links into.
#ifdef AMULE_LIBRARY_UNIT
#error "ServerConnect.h depends on the amule/amuled transport defines; compile this unit in those targets"
#endif

#include "amuleIPV4Address.h" // Needed for amuleIPV4Address
#include "Timer.h"            // Needed for CTimer
#ifdef ENABLE_NATT_SERVER_COORDINATION
#include "NatServerHolePunch.h"   // Needed for the 0x61 policy state
#include "LugdunumNatTraversal.h" // Needed for ForwardedCallback in the extension's wakes
#endif

#include <map> // Needed for std::map
#if defined(ENABLE_IPV6) || defined(ENABLE_NATT_SERVER_COORDINATION)
#include <optional> // Needed for std::optional
#endif

class CServerList;
class CServerSocket;
class CServer;
class CPacket;
class CServerUDPSocket;
#ifdef ENABLE_IPV6
class CNetworkAddress;
#endif

#define CS_FATALERROR -5
#define CS_DISCONNECTED -4
#define CS_SERVERDEAD -3
#define CS_ERROR -2
#define CS_SERVERFULL -1
#define CS_NOTCONNECTED 0
#define CS_CONNECTING 1
#define CS_CONNECTED 2
#define CS_WAITFORLOGIN 3
#define CS_RETRYCONNECTTIME 30 // seconds

typedef std::map<uint64, CServerSocket *> ServerSocketMap;

class CServerConnect
{
public:
	CServerConnect(CServerList *in_serverlist, amuleIPV4Address &address);
	~CServerConnect();

	void ConnectionFailed(CServerSocket *sender);
	void ConnectionEstablished(CServerSocket *sender);

	void ConnectToAnyServer(bool prioSort = true, bool bNoCrypt = false);
	void ConnectToServer(CServer *toconnect, bool multiconnect = false, bool bNoCrypt = false);
	void StopConnectionTry();
	void CheckForTimeout();
	bool RebindServerUDPSocket(const amuleIPV4Address &address);
	void InitServerUDPSocket(amuleIPV4Address &address);

	// safe socket closure and destruction
	void DestroySocket(CServerSocket *pSck);
	bool SendPacket(CPacket *packet, bool delpacket = true, CServerSocket *to = 0);

	// Creteil Begin
	bool IsUDPSocketAvailable() const { return serverudpsocket != NULL; }
	// Creteil End

	bool SendUDPPacket(CPacket *packet,
		CServer *host,
		bool delpacket,
		bool rawpacket = false,
		uint16 port_offset = 4);
	bool Disconnect();
	bool IsConnecting() { return connecting; }
	bool IsConnected() { return connected; }
	uint32 GetClientID() { return clientid; }
	CServer *GetCurrentServer();
	// Whether server, a server-list entry, is the one we are connected to.
	bool IsCurrentServer(const CServer *server);
	// The server being connected to while exactly one attempt is running, else nullptr: a
	// multi-server connect tries several at once.
	CServer *GetConnectingServer() const;
	uint32 clientid;
	bool IsLowID() { return ::IsLowID(clientid); }
	void SetClientID(uint32 newid);
	bool IsLocalServer(uint32 dwIP, uint16 nPort);

	void TryAnotherConnectionrequest();
	bool IsSingleConnect() { return singleconnecting; }
	CServerSocket *GetConnectedSocket() const { return connected ? connectedsocket : nullptr; }
	void KeepConnectionAlive();

	bool AwaitingTestFromIP(uint32 ip);
	bool IsConnectedObfuscated() const;

#ifdef ENABLE_NATT_SERVER_COORDINATION
	//! Sends OP_SERVER_NATT_KEEPALIVE from the client UDP socket when connected to a server that
	//! announced SRV_TCPFLG_LOWID_HOLEPUNCH and the runtime setting is on. Called every ~60 s
	//! from CamuleApp::OnCoreTimer -- no new thread.
	void TrySendLowIdHolePunchKeepalive();

	//! Target IDs are scoped to a server session, so a reconnect resets this state.
	NatServerHolePunch::PendingRequestSet &LowIdHolePunchPendingRequests()
	{
		return m_lowIdHolePunchPendingRequests;
	}
	NatServerHolePunch::DialBackSchedule &LowIdHolePunchDialBacks() { return m_lowIdHolePunchDialBacks; }
	NatServerHolePunch::DuplicateInfoSuppressor &LowIdHolePunchDuplicates()
	{
		return m_lowIdHolePunchDuplicates;
	}
	NatServerHolePunch::ActionRateLimiter &LowIdHolePunchActionLimiter()
	{
		return m_lowIdHolePunchActionLimiter;
	}
	NatServerHolePunch::ExpectedEndpointRegistry &LowIdHolePunchExpectedEndpoints()
	{
		return m_lowIdHolePunchExpectedEndpoints;
	}
	//! Clears the pending requests, expected endpoints and waiting dial backs: called on a new
	//! login, since a previous connection's target IDs mean nothing on a different server.
	void ResetLowIdHolePunchState();
	//! The listed server at that TCP address, when it is not ours and takes a cross-server NAT
	//! callback request from us (NatServerHolePunch::ServerTakesForeignCallback()).
	CServer *ForeignCallbackServer(uint32 serverIp, uint16 serverPort);
	//! We announced the hole-punch extension at login and this server runs it, having also
	//! answered our Lugdunum opt-in, whose 0x37 it wakes a target with.
	bool IsHolePunchExtensionActive();
	//! 0x37 wakes on an extension server, each waiting for the 0x61 that names its requester.
	NatServerHolePunch::PendingWakeSet<LugdunumNatTraversal::ForwardedCallback> &HolePunchExtensionWakes()
	{
		return m_holePunchExtensionWakes;
	}
	//! A woken target sends 0x9F at 0, 0.5 and 1 s so the server learns its current UDP port.
	void StartHolePunchExtensionKeepalives();
#ifdef AMULE_UTP_TRANSPORT
	//! Dials back each punched peer whose delay ran out without a stream from it. Called about
	//! once a second from CamuleApp::OnCoreTimer.
	void ProcessLowIdHolePunchDialBacks();
	//! Sends the due wake keepalives, and dials the Lugdunum way for each wake no 0x61 answered.
	//! Called from every CamuleApp::OnCoreTimer tick, which the 0.5 s burst needs.
	void ProcessHolePunchExtension();
#endif
#endif

	//! Records that a server's hostname resolved during the current sweep.
	void NoteHostnameResolved() { m_hostnameResolvedThisSweep = true; }

	/**
	 * Whether a resolver has answered since the current sweep began.
	 *
	 * A server whose hostname does not resolve is the commonest way an eD2k server dies, and
	 * pruning it is the point of "remove dead servers" -- but with the link down nothing
	 * resolves, and blaming the whole list for that is what emptied it (issue #887). One server
	 * failing to resolve while others answered is the case that says something about that
	 * server, and this is what separates the two.
	 *
	 * Only a lookup that actually ran counts: most of server.met carries an address already,
	 * and going straight to connect proves nothing about the resolver. Cleared by
	 * StopConnectionTry(), which every sweep ends at, so a connect made outside one is never
	 * judged on an earlier sweep's link.
	 */
	bool HostnameResolvedThisSweep() const { return m_hostnameResolvedThisSweep; }

	/**
	 * Called when a socket has been DNS resolved.
	 *
	 * @param socket The socket object requesting DNS resolution. May or may not refer to a
	 * valid
	 *               object, so check before use.
	 * @param ip The found IP, or zero on error.
	 */
	void OnServerHostnameResolved(void *socket, uint32 ip);

#ifdef ENABLE_IPV6
	//! Sends OP_IPV6_UPDATE on the current server connection when DecideServerIPv6Update() says
	//! to, and keeps CServerSocket::GetLoginIPv6() in step with what was actually sent.
	void UpdateAdvertisedIPv6(const std::optional<CNetworkAddress> &advertised);
#endif

private:
	bool connecting;
	bool singleconnecting;
	bool connected;
	int8 max_simcons;
	bool m_bTryObfuscated;
	bool m_recurseTryAnotherConnectionrequest;
	//! See HostnameResolvedThisSweep(); reset as each sweep starts.
	bool m_hostnameResolvedThisSweep;
	CServerSocket *connectedsocket;
	CServerList *used_list;
	CServerUDPSocket *serverudpsocket;

	// list of currently opened sockets
	typedef std::list<CServerSocket *> SocketsList;
	SocketsList m_lstOpenSockets;
	CTimer m_idRetryTimer;

	ServerSocketMap connectionattemps;

#ifdef ENABLE_NATT_SERVER_COORDINATION
	NatServerHolePunch::PendingRequestSet m_lowIdHolePunchPendingRequests;
	NatServerHolePunch::DialBackSchedule m_lowIdHolePunchDialBacks;
	NatServerHolePunch::DuplicateInfoSuppressor m_lowIdHolePunchDuplicates;
	NatServerHolePunch::ActionRateLimiter m_lowIdHolePunchActionLimiter;
	NatServerHolePunch::ExpectedEndpointRegistry m_lowIdHolePunchExpectedEndpoints;
	NatServerHolePunch::PendingWakeSet<LugdunumNatTraversal::ForwardedCallback> m_holePunchExtensionWakes;
	NatServerHolePunch::KeepaliveBurst m_holePunchKeepaliveBurst;
	// Fixed at login, so a setting changed mid-session cannot claim what the server was not told.
	bool m_announcedHolePunchExtension = false;
#endif
};

#endif // SERVERCONNECT_H
// File_checked_for_headers
