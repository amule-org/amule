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

class CUpDownClient;

#include "MuleUDPSocket.h"
#include "ReservedProtocolFrames.h" // Needed for CFrameLogThrottle

#include <array>
#include <memory>
#include <cstdint>

#if defined(ENABLE_NATT_SERVER_COORDINATION) && defined(AMULE_UTP_TRANSPORT)
#include "LugdunumNatTraversal.h"
#endif

#ifdef AMULE_UTP_TRANSPORT
#include "UtpContext.h"
#include "UtpStreamAcceptor.h"
#endif
#ifdef AMULE_QUIC_TRANSPORT
#include "QuicContext.h"
#include "QuicNgtcp2Adapter.h"
#include "NatRendezvousPolicy.h"
#include "QuicStreamAcceptor.h"
#include "QuicCapsRequestLedger.h"
#include "QuicCapabilityCache.h"
#include "NatKeyFrame.h"
#endif
#if defined(ENABLE_NATT_SERVER_COORDINATION) && defined(AMULE_UTP_TRANSPORT) && defined(AMULE_QUIC_TRANSPORT)
#include "ClientRef.h" // Needed for a safe CAPS-holdoff owner reference
#include <vector>
#endif
#ifdef ENABLE_NATT_SERVER_COORDINATION
namespace LugdunumNatTraversal
{
struct ForwardedCallback;
}
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
#if defined(ENABLE_NATT_SERVER_COORDINATION)
	bool DialLowIdHolePunchPeer(
		uint32 ip, uint16 tcpPort, uint16 udpPort, const std::array<std::uint8_t, 16> &userHash);
	bool DialLugdunumNatCallbackPeer(const LugdunumNatTraversal::ForwardedCallback &callback);
	bool IsLugdunumNatCallbackCoolingDown(const LugdunumNatTraversal::CallbackFailureKey &key);
	void RecordLugdunumNatCallbackFailure(const LugdunumNatTraversal::CallbackFailureKey &key);
	void ClearLugdunumNatCallbackFailure(const LugdunumNatTraversal::CallbackFailureKey &key);
	bool IsLugdunumCallbackRendezvousActive(const LugdunumNatTraversal::CallbackFailureKey &key) const;
	void CompleteLugdunumCallbackRendezvous(const LugdunumNatTraversal::CallbackFailureKey &key);
#endif
#endif
#ifdef AMULE_QUIC_TRANSPORT
	//! Services every live QUIC connection's RFC 9002 timers, independently of any inbound
	//! datagram. Without this a connection that stops receiving ACKs would never retransmit
	//! and never time out.
	void TickQuic();
	//! Sends one QUIC datagram, obfuscated with the peer's hash when it is known. A long header
	//! is preceded by our key frame, so the peer can obfuscate its replies the same way.
	bool SendQuicFrame(const uint8_t *payload, size_t length, uint32_t ip, uint16_t port);
	//! A rendezvous says this peer is about to contact us from this UDP endpoint; only such an
	//! endpoint may announce a key frame. When supplied, its server-vouched hash is authoritative.
	void ExpectNatTPeer(
		uint32_t ip, uint16_t udpPort, const std::array<uint8_t, 16> *vouchedHash = nullptr);
	bool SendQuicCapsRequest(const std::array<uint8_t, 16> &peer,
		uint32_t ipv4,
		uint16_t udpPort,
		const std::array<uint8_t, 16> &fileHash);
	//! Sends CAPS for an unknown peer, but only dials QUIC for a cached compatible peer.
	//! For a server-vouched LowID NAT-T dial (owner non-null) an unknown peer instead holds
	//! the transport choice open -- see the CAPS holdoff serviced by TickQuic() -- and this
	//! returns true for that case too, the same as a successful synchronous dial.
	bool TryQuicDial(const std::array<uint8_t, 16> &peer,
		uint32_t ipv4,
		uint16_t udpPort,
		const std::array<uint8_t, 16> &fileHash,
		CUpDownClient *owner = nullptr);
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
#ifdef ENABLE_NATT_SERVER_COORDINATION
	LugdunumNatTraversal::CallbackFailureMemory m_lugdunumCallbackFailures;
	LugdunumNatTraversal::ForwardedCallbackActionGuard m_lugdunumCallbackActions;
	LugdunumNatTraversal::CCallbackRendezvous m_lugdunumCallbackRendezvous;
#endif
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
	NatRendezvous::CRequesterLimiter m_capsAckLimiter;
	CQuicCapsRequestLedger m_capsRequestLedger{ 256 };
	CQuicCapabilityCache m_capabilityCache{ 256 };
	NatKeyFrame::CKeyFrameService m_keyFrames{ KeyFrameCallbacks() };
#if defined(ENABLE_NATT_SERVER_COORDINATION) && defined(AMULE_UTP_TRANSPORT)
	// Punch-dial temporary clients drop their last CClientRef once the dial starts and have no
	// socket until the handoff; the holdoff's reference is the only thing keeping them alive.
	CQuicCapsHoldoffs<CClientRef> m_quicCapsHoldoffs;
	CQuicCapsHoldoffs<CClientRef>::SCallbacks QuicCapsHoldoffCallbacks();
#endif
#endif
#ifdef AMULE_QUIC_TRANSPORT
	NatKeyFrame::Callbacks KeyFrameCallbacks();
	void HandleNatKeyFrame(const uint8_t *payload, size_t length, uint32_t ip, uint16_t port);
#endif
	void OnPacketReceived(
		const CNetworkAddress &address, uint16 port, uint8_t *buffer, size_t length) override;
	void ProcessPacket(uint8_t *packet, int16 size, int8 opcode, uint32 host, uint16 port);
#ifdef ENABLE_NATT_SERVER_COORDINATION
	//! Lugdunum's forwarded NAT callback (0xA7), server-way encrypted on this client socket
	//! (S6.4). Returns true -- and has already logged -- only once the datagram actually
	//! decrypted server-way and carried OP_NAT_CALLBACKREQUESTED_UDP; otherwise \p buffer is left
	//! untouched for the normal client-way path below.
	bool TryProcessServerWayNatCallback(uint32 ip, uint8_t *buffer, size_t length);
#ifdef AMULE_UTP_TRANSPORT
	void HandleLowIdHolePunch(uint32 host, uint16 port);
	void SendLugdunumNatCallbackRequest(const LugdunumNatTraversal::CallbackFailureKey &key);
	void TickLugdunumCallbackRendezvous();
#endif
#endif

	/**
	 * OP_UDPRESERVEDPROT2: no opcode, a frame type byte instead.
	 *
	 * @param frame  points at the frame type byte.
	 * @param frameLength  bytes available from there. Zero is a datagram that carried nothing
	 * but the
	 *                     protocol byte.
	 * @param obfuscated  whether the datagram arrived under the UDP obfuscation layer.
	 */
	void ProcessReservedProt2Frame(
		const uint8_t *frame, size_t frameLength, uint32 ip, uint16 port, bool obfuscated);

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
