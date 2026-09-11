//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2004-2011 Angel Vidal Veiga ( kry@users.sourceforge.net )
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

#ifndef ECSOCKET_H
#define ECSOCKET_H

#include <deque>  // Needed for std::deque
#include <memory> // Needed for std::shared_ptr
#include <string>
#include <vector>

#include <zlib.h>           // Needed for packet (de)compression
#include "../../../Types.h" // Needed for uint32_t

#include "ECCrypt.h"

#include <wx/defs.h>  // Needed for wx/debug.h
#include <wx/debug.h> // Needed for wxASSERT

#include <common/SmartPtr.h> // Needed for CSmartPtr

enum ECSocketErrors
{
	EC_ERROR_NOERROR,
	EC_ERROR_INVOP,
	EC_ERROR_IOERR,
	EC_ERROR_INVADDR,
	EC_ERROR_INVSOCK,
	EC_ERROR_NOHOST,
	EC_ERROR_INVPORT,
	EC_ERROR_WOULDBLOCK,
	EC_ERROR_TIMEDOUT,
	EC_ERROR_MEMERR,
	EC_ERROR_UNKNOWN
};

class CECPacket;
class CQueuedData;

/*! \class CECSocket
 * \brief Socket handler for External Communications (EC), i.e. the transmission
 * of EC packets.
 */

class CECSocket
{
	friend class CECPacket;
	friend class CECTag;
	// CECMemSocket captures all I/O into an in-memory vector. To finish a
	// serialization round it needs the FlushBuffers / m_output_queue drain machinery
	// the real-socket path reaches via the friend declarations above.
	friend class CECMemSocket;

private:
	static const unsigned int EC_SOCKET_BUFFER_SIZE = 2048;
	// Cap on one queued output block. The tx path used to reuse
	// EC_SOCKET_BUFFER_SIZE, so every packet left in 2 KB writes and each one's
	// sub-MSS remainder became a segment of its own. A cap and not a size:
	// TxChunkSize() asks for what the packet needs and no more.
	static const unsigned int EC_SOCKET_TX_CHUNK_MAX = 64 * 1024;
	static const unsigned int EC_HEADER_SIZE = 8;
	const bool m_use_events;

	// Output related data
	std::list<CQueuedData *> m_output_queue;

	// zlib (deflation) buffers
	std::vector<unsigned char> m_in_ptr;
	std::vector<unsigned char> m_out_ptr;
	CSmartPtr<CQueuedData> m_curr_rx_data;
	CSmartPtr<CQueuedData> m_curr_tx_data;

	// This transfer only
	uint32_t m_rx_flags;
	uint32_t m_tx_flags;
	size_t m_bytes_needed;
	bool m_in_header;

	uint32_t m_curr_packet_len;
	z_stream m_z;

	// --- transport encryption -------------------------------------------
	//
	// Keys are derived once the handshake has both nonces and the chosen cipher.
	// m_crypt_ready says the keys exist; m_crypt_enabled says to seal outgoing
	// packets. Separate because the packet that completes the handshake must still go
	// out in clear -- the client sends EC_OP_AUTH_PASSWD unencrypted and only then
	// switches on, which m_crypt_enable_after_write expresses. Incoming packets are
	// decided per packet by EC_FLAG_ENCRYPTED.
	ECCrypt::Session m_crypt;
	bool m_crypt_ready;
	bool m_crypt_enabled;
	bool m_crypt_enable_after_write;

	// Whether the packet ReadPacket last returned arrived sealed, so the app dispatch
	// can reject cleartext injected into a session that negotiated encryption.
	bool m_last_rx_encrypted;

protected:
	// Pure arithmetic, protected rather than private so a test can pin the floor and
	// the cap without standing up a socket (see CECMemSocket).
	static size_t TxChunkSize(uint32 bodyLen);

	// IsCryptReady() is true once keys exist, i.e. the session negotiated AEAD;
	// WasLastPacketEncrypted() reports how the last packet arrived.
	bool IsCryptReady() const { return m_crypt_ready; }
	bool WasLastPacketEncrypted() const { return m_last_rx_encrypted; }

	uint32_t m_my_flags;
	bool m_haveNotificationSupport;

	// Daemon-internal subclasses (e.g. CECMemSocket) must set the per-packet wire
	// flags before calling serialization primitives like CECTag::Serialize, which read
	// m_tx_flags directly. Promoting just the setter keeps the rest of m_tx_flags'
	// lifecycle private to WritePacket / WriteBuffer.
	void SetTxFlags(uint32_t flags) { m_tx_flags = flags; }

	// True when the peer is on loopback / RFC1918 LAN / RFC3927 link-local, i.e. the
	// wire cost of an uncompressed response is irrelevant. WritePacket then skips ZLIB
	// up to a size threshold; large packets still compress so we never blow the
	// receiver's 256 MB packet budget (ReadHeader gate).
	bool m_isLocalPeer;

public:
	CECSocket(bool use_events);
	virtual ~CECSocket();

	bool ConnectSocket(uint32_t ip, uint16_t port);

	// Reset the EC packet layer to its just-constructed state so the SAME socket
	// object can be reused for a fresh connection (amulegui reconnect, issue #444).
	// Drops queued output and rewinds the RX/TX reassembly to "expecting a header";
	// without this a mid-packet read left over from the drop misparses the reconnected
	// session's first bytes and the login handshake fails.
	void ResetProtocolState();

	/**
	 * Drop the capability bits agreed with the previous peer, keeping the ones this
	 * end chose locally. Only for a caller reusing this object against a different
	 * peer; the definition says why it is separate from ResetProtocolState.
	 */
	void ClearPeerNegotiatedFlags();

	void CloseSocket() { InternalClose(); }

	/// Seal the queued body chunks in place and append the tag chunk.
	bool SealOutputQueue(std::list<CQueuedData *>::iterator outputStart);

	/// Open the received body in place and drop the verified tag.
	bool OpenReceivedBody();

	/**
	 * Derive the session keys for this connection. Does not start sealing on its
	 * own -- see EnableAEADAfterNextWrite() and EnableAEADNow(). Incoming packets
	 * can be opened as soon as this succeeds, which lets the two ends switch over
	 * one packet apart.
	 *
	 * @return false if the cipher is unsupported or the nonces are malformed.
	 */
	bool SetupAEAD(uint8_t cipher,
		const std::vector<uint8_t> &ikm,
		const std::vector<uint8_t> &serverNonce,
		const std::vector<uint8_t> &clientNonce,
		const std::vector<uint8_t> &transcript,
		bool isServer);

	/// Seal everything after the packet currently being sent. The client uses
	/// this so its EC_OP_AUTH_PASSWD leaves in clear but the reply is sealed.
	void EnableAEADAfterNextWrite() { m_crypt_enable_after_write = m_crypt_ready; }

	/// Seal from the next packet on. The daemon uses this once the password checks
	/// out, so EC_OP_AUTH_OK is itself sealed -- proof that it holds the password.
	void EnableAEADNow() { m_crypt_enabled = m_crypt_ready; }

	bool IsAEADReady() const { return m_crypt_ready; }
	bool IsAEADEnabled() const { return m_crypt_enabled; }
	uint8_t GetAEADCipher() const { return m_crypt.GetCipher(); }

	// Locally-initiated abort: CloseSocket + OnLost. Use from the protocol-error paths
	// in ReadHeader / ReadPacket where we close the socket ourselves.
	// CAsioSocketImpl::Close sets m_closed = true before the asio close, which
	// suppresses the operation_aborted path through HandleRead -> PostLostEvent
	// (LibSocketAsio.cpp:695), so the wrapper's OnLost never fires from the asio side.
	// Without an explicit dispatch the EC client never finds out the connection is
	// gone -- the same #757 "wedge" as the kernel-FIN miss, on the self-close leg.
	// Sites with their own UI-facing notification (CRemoteConnect::ProcessAuthPacket
	// fires wxEVT_EC_CONNECTION) keep using plain CloseSocket.
	void CloseAndDispatchLost()
	{
		InternalClose();
		OnLost();
	}

	bool HaveNotificationSupport() const { return m_haveNotificationSupport; }

	// Set by CECServerSocket::Authenticate once the peer IP is known.
	// Affects the ZLIB-skip decision in WritePacket only.
	void SetLocalPeer(bool isLocal) { m_isLocalPeer = isLocal; }

	/**
	 * Sends an EC packet and returns immediately; it goes out on idle time.
	 *
	 * @param packet The CECPacket packet to be sent. The caller must \c delete it.
	 */
	void SendPacket(const CECPacket *packet);

	/**
	 * Send a response whose body was pre-serialized outside the normal CECPacket ->
	 * WritePacket pipeline. Each blob is the byte-form of one top-level child tag, as
	 * produced by CECMemSocket::SerializeTag.
	 *
	 * The flag-byte / length-header / per-connection compression / length-patch dance
	 * is the same one SendPacket -> WritePacket does, concentrated here so callers
	 * need not reach into CECSocket's private buffer machinery.
	 *
	 * The cached blobs must be UTF-8 numbers + LARGE_TAG_COUNT and uncompressed;
	 * compression is layered on per connection at this layer. Both capability bits are
	 * forced on in the wire flag byte regardless of negotiation, so the caller must
	 * not serve the cache to a client that did not advertise them.
	 *
	 * @param opcode The packet opcode the receiver should see.
	 * @param blobs One pre-serialized child tag per element.
	 */
	void SendCachedBodyResponse(
		uint8_t opcode, const std::vector<std::shared_ptr<const std::vector<unsigned char>>> &blobs);

	/**
	 * Sends an EC packet and blocks until the reply arrives or the request times out.
	 * OnPacketReceived() is not called for packets received this way.
	 *
	 * @param request The CECPacket packet to be sent.
	 * @return The reply, heap-allocated with \c new, or \c NULL on timeout.
	 *
	 * @note It's the caller's responsibility to \c delete both request and reply.
	 */
	const CECPacket *SendRecvPacket(const CECPacket *request);

	/**
	 * Event handler called when a new packet is received. Not called for packets
	 * received via SendRecvPacket().
	 *
	 * The application processes the packet here and returns a reply allocated on the
	 * heap with \c new, or \c NULL if none is needed. The library \c deletes both
	 * packets.
	 *
	 * @param packet The packet that has been received.
	 * @return The reply packet or \c NULL if no reply needed.
	 */
	virtual const CECPacket *OnPacketReceived(const CECPacket *packet, uint32 trueSize);

	/**
	 * @return Text describing the last error.
	 */
	virtual std::string GetLastErrorMsg();

	/**
	 * Error handler, called when an error occurs. Use GetLastError() and
	 * GetErrorMsg() to find out its nature. The default prints a message in debug
	 * builds and destroys the socket.
	 */
	virtual void OnError();

	/**
	 * Socket lost event handler: the network failed or the remote end closed the
	 * socket gracefully. The default handler destroys the socket.
	 */
	virtual void OnLost();

	/**
	 * Event handler called when a connection attempt succeeds.
	 */
	virtual void OnConnect();

	void OnInput();
	void OnOutput();

	bool WouldBlock() { return InternalGetLastError() == EC_ERROR_WOULDBLOCK; }
	bool GotError() { return InternalGetLastError() != EC_ERROR_NOERROR; }

	uint32 SocketRead(void *ptr, size_t len) { return InternalRead(ptr, len); }
	uint32 SocketWrite(const void *ptr, size_t len) { return InternalWrite(ptr, len); }
	bool SocketError() { return InternalError() && GotError(); }
	bool SocketRealError();

	bool WaitSocketConnect(long secs = -1, long msecs = 0) { return InternalWaitOnConnect(secs, msecs); }
	bool WaitSocketWrite(long secs = -1, long msecs = 0) { return InternalWaitForWrite(secs, msecs); }
	bool WaitSocketRead(long secs = -1, long msecs = 0) { return InternalWaitForRead(secs, msecs); }

	bool IsSocketConnected() { return InternalIsConnected(); }

	void DestroySocket() { return InternalDestroy(); }

	bool DataPending();

private:
	const CECPacket *ReadPacket();
	uint32 WritePacket(const CECPacket *packet);

	// These 4 methods are to be used by CECPacket & CECTag
	bool ReadNumber(void *buffer, size_t len);
	bool ReadBuffer(void *buffer, size_t len);
	bool ReadHeader();

	bool WriteNumber(const void *buffer, size_t len);
	bool WriteBuffer(const void *buffer, size_t len);

	// Internal stuff
	bool FlushBuffers();
	void SizeTxChunks(uint32 bodyLen);

	size_t ReadBufferFromSocket(void *buffer, size_t len);
	void WriteBufferToSocket(const void *buffer, size_t len);

	/* virtuals */
	virtual void WriteDoneAndQueueEmpty() = 0;

	virtual bool InternalConnect(uint32_t ip, uint16_t port, bool wait) = 0;

	virtual bool InternalWaitOnConnect(long secs = -1, long msecs = 0) = 0;
	virtual bool InternalWaitForWrite(long secs = -1, long msecs = 0) = 0;
	virtual bool InternalWaitForRead(long secs = -1, long msecs = 0) = 0;

	virtual int InternalGetLastError() = 0;

	virtual void InternalClose() = 0;
	virtual bool InternalError() = 0;
	virtual uint32 InternalRead(void *ptr, uint32 len) = 0;
	virtual uint32 InternalWrite(const void *ptr, uint32 len) = 0;

	virtual bool InternalIsConnected() = 0;
	virtual void InternalDestroy() = 0;

	// Was login successful ?
	virtual bool IsAuthorized() { return true; }
};

class CQueuedData
{
	std::vector<unsigned char> m_data;
	unsigned char *m_rd_ptr, *m_wr_ptr;

public:
	CQueuedData(size_t len)
	: m_data(len)
	{
		m_rd_ptr = m_wr_ptr = &m_data[0];
	}

	~CQueuedData() {}

	void Rewind() { m_rd_ptr = m_wr_ptr = &m_data[0]; }

	void Write(const void *data, size_t len);
	void WriteAt(const void *data, size_t len, size_t off);
	void Read(void *data, size_t len);

	/*
	 * Start of the buffered bytes, for transforms that rewrite the payload in place.
	 * Used by the AEAD path in WritePacket / ReadPacket; flattening the queue into a
	 * scratch buffer would double peak memory on the largest packets.
	 */
	unsigned char *GetDataPtr() { return &m_data[0]; }

	/* Drop @a len bytes from the end (the AEAD tag, once verified). */
	void TruncateBy(size_t len);

	// Pass pointers to zlib. From now on, no Read() calls are allowed
	void ToZlib(z_stream &m_z)
	{
		m_z.avail_in = (uInt)GetUnreadDataLength();
		m_z.next_in = m_rd_ptr;
	}

	uint32 WriteToSocket(CECSocket *sock);
	uint32 ReadFromSocket(CECSocket *sock, size_t len);

	size_t ReadFromSocketAll(CECSocket *sock, size_t len);

	size_t GetLength() const;
	size_t GetDataLength() const;
	size_t GetRemLength() const;
	size_t GetUnreadDataLength() const;
};

#endif // ECSOCKET_H
