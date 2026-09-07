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

#ifndef CLIENTUDPSOCKET_H
#define CLIENTUDPSOCKET_H

#include "MuleUDPSocket.h"
#include "ReservedProtocolFrames.h" // Needed for CUnknownFrameLogThrottle

class CClientUDPSocket : public CMuleUDPSocket
{
public:
	CClientUDPSocket(const amuleIPV4Address &address, const CProxyData *ProxyData = NULL);

protected:
	void OnReceive(int errorCode);

private:
	void OnPacketReceived(uint32 ip, uint16 port, uint8_t *buffer, size_t length);
	void ProcessPacket(uint8_t *packet, int16 size, int8 opcode, uint32 host, uint16 port);

	/**
	 * OP_UDPRESERVEDPROT2: no opcode, a frame type byte instead.
	 *
	 * @param frame  points at the frame type byte.
	 * @param frameLength  bytes available from there. Zero is a datagram
	 *                     that carried nothing but the protocol byte.
	 */
	void ProcessReservedProt2Frame(const uint8_t *frame, size_t frameLength, uint32 ip, uint16 port);

	//! One unknown-frame line per minute, with a suppressed count. A peer
	//! speaking a frame type this build does not know retries, so the useful
	//! information is that it happened plus how often.
	CUnknownFrameLogThrottle m_unknownFrameLog{ 60 * 1000 };
};

#endif // CLIENTUDPSOCKET_H
// File_checked_for_headers
