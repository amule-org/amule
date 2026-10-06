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
#include "ReservedProtocolFrames.h" // Needed for CFrameLogThrottle

#include <memory>

#ifdef AMULE_UTP_TRANSPORT
#include "UtpContext.h"
#include "UtpStreamAcceptor.h"
#endif
#ifdef AMULE_QUIC_TRANSPORT
#include "QuicContext.h"
#include "QuicNgtcp2Adapter.h"
#include "QuicStreamAcceptor.h"
#endif

class CClientUDPSocket : public CMuleUDPSocket
#ifdef AMULE_UTP_TRANSPORT
,
			 private IUtpDatagramSink
#endif
{
public:
	CClientUDPSocket(const amuleIPV4Address &address, const CProxyData *ProxyData = NULL);
	bool Rebind(const amuleIPV4Address &address);
#ifdef AMULE_UTP_TRANSPORT
	~CClientUDPSocket() override;
	void Close() override;
	void TickUtp();
	IUtpContext *GetUtpContext() { return &m_utp; }
#endif
#ifdef AMULE_QUIC_TRANSPORT
	//! Services every live QUIC connection's RFC 9002 timers, independently of any inbound
	//! datagram. Without this a connection that stops receiving ACKs would never retransmit
	//! and never time out.
	void TickQuic();
#endif

protected:
	void OnReceive(int errorCode) override;

private:
#ifdef AMULE_UTP_TRANSPORT
	void SendUtpDatagram(const uint8_t *payload,
		size_t length,
		uint32_t ip,
		uint16_t port,
		bool encrypt,
		const uint8_t *userHash) override;
	CUtpContext m_utp;
	CUtpStreamAcceptor m_utpAcceptor;
#endif
#ifdef AMULE_QUIC_TRANSPORT
	// Declaration order is construction order: each of these is built from the ones before it,
	// so none may move above whichever it depends on.
	std::shared_ptr<IQuicTlsCredentials> m_quicCredentials; // the real, shared server certificate
	std::shared_ptr<IQuicDatagramSink> m_quicSink;
	std::shared_ptr<IQuicNgtcp2Engine> m_quicEngine;
	std::unique_ptr<CQuicNgtcp2Factory> m_quicFactory;
	CQuicContext m_quic;
	CQuicStreamAcceptor m_quicAcceptor;
#endif
	void OnPacketReceived(
		const CNetworkAddress &address, uint16 port, uint8_t *buffer, size_t length) override;
	void ProcessPacket(uint8_t *packet, int16 size, int8 opcode, uint32 host, uint16 port);

	/**
	 * OP_UDPRESERVEDPROT2: no opcode, a frame type byte instead.
	 *
	 * @param frame  points at the frame type byte.
	 * @param frameLength  bytes available from there. Zero is a datagram that carried nothing
	 * but the
	 *                     protocol byte.
	 */
	void ProcessReservedProt2Frame(const uint8_t *frame, size_t frameLength, uint32 ip, uint16 port);

	//! One unknown-frame line per minute, with a suppressed count. A peer speaking a frame type
	//! this build does not know retries, so the useful information is that it happened plus how
	//! often.
	// One throttle per reason rather than one for the branch: a peer flooding
	// any single kind of frame must not be able to silence the diagnostics for
	// the others. The uTP case keeps its own because it is the line a developer
	// is usually looking for, meaning libutp saw the datagram and disclaimed it.
	CFrameLogThrottle m_truncatedFrameLog{ 60 * 1000 };
	CFrameLogThrottle m_unknownFrameLog{ 60 * 1000 };
	CFrameLogThrottle m_unservedFrameLog{ 60 * 1000 };
	CFrameLogThrottle m_utpUnmatchedFrameLog{ 60 * 1000 };
	// Separate from the unmatched one: a frame nobody can parse and a frame from
	// a peer we have no socket for are different problems, and sharing a
	// throttle would let a flood of one hide the other entirely.
	CFrameLogThrottle m_utpMalformedFrameLog{ 60 * 1000 };
};

#endif // CLIENTUDPSOCKET_H
// File_checked_for_headers
