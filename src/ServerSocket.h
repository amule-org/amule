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

#ifndef SERVERSOCKET_H
#define SERVERSOCKET_H

#include "EMSocket.h" // Needed for CEMSocket
#include "ServerConnect.h"
#ifdef ENABLE_IPV6
#include "NetworkAddress.h" // Needed for CNetworkAddress
#endif

// CServerSocket

class CServer;

class CServerSocket : public CEMSocket
{
	friend class CServerConnect;

public:
	CServerSocket(CServerConnect *in_serverconnect, const CProxyData *ProxyData = NULL);
	virtual ~CServerSocket();

	void ConnectToServer(CServer *server, bool bNoCrypt = false);
	sint8 GetConnectionState() const { return connectionstate; }
	uint64 GetLastTransmission() const { return m_dwLastTransmission; }
	wxString info;

	void OnClose(int nErrorCode) override;
	void OnConnect(int nErrorCode) override;
	void OnReceive(int nErrorCode) override;
	void OnError(int nErrorCode) override;
	bool PacketReceived(CPacket *packet) override;

	// Server control traffic is not subject to the global download bandwidth budget -- search,
	// source and MOTD packets are tiny and latency-sensitive.
	bool IsDownloadThrottled() const override { return false; }
	void SendPacket(CPacket *packet,
		bool delpacket = true,
		bool controlpacket = true,
		uint32 actualPayloadSize = 0) override;
	bool IsSolving() const { return m_IsSolving; };
	void OnHostnameResolved(uint32 ip);
	CServer *GetServerConnected() const { return serverconnect->GetCurrentServer(); }

	uint32 GetServerIP() const;
	// The port ConnectToServer() dialed: the obfuscation port, or GetConnPort(), which can be an
	// aux port rather than the advertised one.
	uint16 GetConnectPort() const { return m_connectPort; }

#ifdef ENABLE_IPV6
	// The IPv6 this connection last sent, by login or a later OP_IPV6_UPDATE; Absent after a
	// withdrawal. This is the own-record check address in CPartFile::AddSources().
	void SetLoginIPv6(const CNetworkAddress &address)
	{
		m_loginIPv6 = address;
		if (address.IsPresent()) {
			m_ipv6SentinelParsingOn = true;
		}
	}
	const CNetworkAddress &GetLoginIPv6() const { return m_loginIPv6; }

	// Sticky for the rest of this server session once any IPv6 was advertised on it: the server
	// sends no reply to OP_IPV6_UPDATE, so an OP_FOUNDSOURCES built before a withdrawal can still
	// arrive after it (0xFFFFFFFF is never a real source id).
	bool IsIPv6SentinelParsingOn() const { return m_ipv6SentinelParsingOn; }
#endif

private:
	bool ProcessPacket(const uint8_t *packet, uint32 size, int8 opcode);
	void SetConnectionState(sint8 newstate);
	CServerConnect *serverconnect;
	sint8 connectionstate;
	CServer *cur_server;
	bool m_bNoCrypt;
	bool headercomplete;
	int32 sizetoget;
	int32 sizereceived;
	char *rbuffer;
	bool m_bIsDeleting; // true: socket is already in deletion phase, don't destroy it in
			    // ::StopConnectionTry
	uint64 m_dwLastTransmission;

	bool m_IsSolving;
	uint16 m_connectPort = 0;
#ifdef ENABLE_IPV6
	CNetworkAddress m_loginIPv6;
	bool m_ipv6SentinelParsingOn = false;
#endif
};

#endif // SERVERSOCKET_H
// File_checked_for_headers
