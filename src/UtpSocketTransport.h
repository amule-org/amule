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

#ifndef UTPSOCKETTRANSPORT_H
#define UTPSOCKETTRANSPORT_H

#include "StreamTransport.h"
#include "UtpStream.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

/**
 * The per-socket libutp calls a transport makes, behind a seam.
 *
 * Only four, and the re-entrancy rule differs per call rather than being one
 * blanket ban, because a blanket ban is a rule the code would have to break:
 *
 * - CloseSocket() must never be made from inside a callback: utp_close() can
 *   produce UTP_STATE_DESTROYING before it returns.
 * - WriteToSocket() is what UTP_STATE_WRITABLE exists to invite, so it is
 *   correct from there. From UTP_ON_ACCEPT it achieves nothing, since the
 *   socket is still CS_SYN_RECV, and from UTP_ON_READ it re-enters
 *   utp_process_incoming. Both of those request a flush instead.
 * - NotifyReadDrained() is safe from anywhere, including inside UTP_ON_READ:
 *   utp_read_drained() only recomputes the receive window and sends or
 *   schedules an ACK, and never re-enters the incoming path.
 * - SetReceiveBuffer() is configuration, made once by the acceptor.
 *
 * Naming them here keeps the transport testable without the library.
 *
 * The handle is opaque on purpose: a utp_socket* never appears outside the
 * adapter's translation unit, which is what stops <libutp/utp.h> reaching the
 * rest of src/.
 */
class IUtpSocketOperations
{
public:
	using Handle = void *;

	virtual ~IUtpSocketOperations() = default;

	//! Offers bytes. Nonpositive results (including libutp's -1) accept nothing.
	virtual std::ptrdiff_t WriteToSocket(Handle socket, const uint8_t *data, size_t length) = 0;

	//! Tells libutp the application has caught up, so delivery may resume.
	virtual void NotifyReadDrained(Handle socket) = 0;

	//! utp_close(). Exactly one caller may ever make this call per socket.
	virtual void CloseSocket(Handle socket) = 0;

	//! Sets the receive-buffer size libutp advertises window against.
	virtual void SetReceiveBuffer(Handle socket, size_t bytes) = 0;
};

/**
 * An IStreamTransport over one libutp socket.
 *
 * Main thread only for everything that touches libutp -- Flush(), Read()'s
 * drained notification and Close(). Write() is the exception by necessity: it
 * is reached from the upload bandwidth thread through CEMSocket, so it only
 * queues into CUtpStream and the main thread hands those bytes to libutp on the
 * next Flush(). That split is the whole reason the queue exists.
 *
 * Nothing constructs one of these yet.
 */
class CUtpSocketTransport final : public IStreamTransport
{
public:
	CUtpSocketTransport(IUtpSocketOperations &operations,
		IUtpSocketOperations::Handle socket,
		const CNetworkAddress &peer,
		uint16_t peerPort,
		IStreamTransportEvents *events = nullptr)
	: m_operations(operations)
	, m_socket(socket)
	, m_peer(peer)
	, m_peerPort(peerPort)
	, m_events(events)
	{
	}

	~CUtpSocketTransport() override
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_events = nullptr;
		}
		Close();
	}

	CUtpSocketTransport(const CUtpSocketTransport &) = delete;
	CUtpSocketTransport &operator=(const CUtpSocketTransport &) = delete;

	/**
	 * Sets the receive-window ceiling, not backpressure on its own.
	 *
	 * The adapter must register UTP_GET_READ_BUFFER_SIZE and synchronously
	 * pull ReadBufferSize(): libutp advertises opt_rcvbuf minus those bytes.
	 * Without that callback, occupancy is treated as zero.
	 */
	void ApplyReceiveBound()
	{
		IUtpSocketOperations::Handle socket = nullptr;
		size_t bound = 0;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			socket = m_socket;
			bound = m_stream.ReadBound();
		}
		if (socket != nullptr) {
			m_operations.SetReceiveBuffer(socket, bound);
		}
	}

	// -- IStreamTransport ---------------------------------------------

	// These are read from the upload bandwidth thread inside CEMSocket's send
	// loop while the main thread's callbacks write them, so they take the lock
	// like everything else that touches the stream.
	/**
	 * True once accepted, which is not libutp being ready to send: the acceptor
	 * marks it at CS_SYN_RECV, and CS_CONNECTED arrives silently on the peer's
	 * first ST_DATA. Answers "is there a stream", not "will a write leave now".
	 */
	bool IsConnected() const override
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_connected && m_stream.IsOk();
	}
	bool IsOk() const override
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_stream.IsOk();
	}
	bool BlocksRead() const override
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_stream.BlocksRead();
	}
	bool BlocksWrite() const override
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_stream.BlocksWrite();
	}
	int LastError() const override
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_stream.LastError();
	}
	//! Fixed for the transport's lifetime, so it needs no lock.
	CNetworkAddress GetPeerAddress() const override { return m_peer; }
	uint16_t GetPeerPort() const override { return m_peerPort; }

	uint32_t Read(void *buffer, uint32_t length) override
	{
		IUtpSocketOperations::Handle socket = nullptr;
		uint32_t taken = 0;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			taken = m_stream.Read(buffer, length);
			// Owed only once the reader has emptied the buffer, and only to
			// a socket that still exists: libutp stopped delivering while we
			// were behind, and this is what resumes it.
			if (m_stream.ConsumeReadDrainedEdge()) {
				socket = m_socket;
			}
		}
		if (socket != nullptr) {
			// Outside the lock: this can re-enter.
			m_operations.NotifyReadDrained(socket);
		}
		return taken;
	}

	/**
	 * Queues bytes. Reached from the upload bandwidth thread.
	 *
	 * Deliberately does not call libutp: that would be a library call from a
	 * thread libutp knows nothing about, and utp_write() can produce callbacks.
	 * The bytes leave on the next Flush().
	 */
	uint32_t Write(const void *buffer, uint32_t length) override
	{
		uint32_t taken = 0;
		IStreamTransportEvents *events = nullptr;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			taken = m_stream.Write(buffer, length);
			if (taken != 0 && !m_flushInProgress) {
				events = RequestFlushLocked();
			}
		}
		RaiseFlushRequest(events);
		return taken;
	}

	/**
	 * Closes once, and never touches the handle again.
	 *
	 * The handle is the token: it is cleared before the call, so the
	 * destructor, a second Close() and a DESTROYING callback all find nothing
	 * to close. That one rule also covers every other call -- Flush() and the
	 * drained notification check the same pointer -- which is why it is the
	 * mechanism here rather than a separate flag that would have to be
	 * consulted in each of them.
	 */
	void Close() override
	{
		IUtpSocketOperations::Handle socket = nullptr;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			socket = m_socket;
			m_socket = nullptr;
			m_connected = false;
			// Our own close, not the peer's FIN. Both are clean ends with no
			// error, but Failure() must not claim we saw a FIN we never saw.
			m_stream.OnFailure(EUtpTransportFailure::Closed);
			m_flushPending = false;
		}
		if (socket != nullptr) {
			// Unlocked: utp_close() can produce UTP_STATE_DESTROYING before
			// it returns, and that callback comes back through OnEnded().
			m_operations.CloseSocket(socket);
		}
	}

	// -- main-thread pump ---------------------------------------------

	/**
	 * Offers queued bytes to libutp and keeps whatever it refused.
	 *
	 * utp_write() takes less than it is offered as soon as the congestion
	 * window is full, and nothing while the socket is not yet connected, so
	 * the accepted count is what may be dropped from the queue -- never the
	 * whole of it.
	 */
	void Flush() override
	{
		IUtpSocketOperations::Handle socket = nullptr;
		std::vector<uint8_t> pending;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			// Cleared before the re-entrancy check, not after it. Any entry here
			// absorbs the outstanding request, and the early return below is an
			// entry: leaving the flag set there meant the re-entry was recorded,
			// consumed by the tail, and then dropped anyway, because
			// RequestFlushLocked() short-circuits on this very flag. Write()
			// short-circuits on it too, so the queue stranded for good with
			// IsOk() still reporting true.
			m_flushPending = false;
			if (m_flushInProgress) {
				// Record it rather than drop it: the outer flush offers bytes
				// this caller has not seen, so returning silently would lose
				// whatever edge asked for this one.
				m_flushAgain = true;
				return;
			}
			if (m_socket == nullptr || !m_stream.IsOk()) {
				return;
			}
			socket = m_socket;
			// Bounded: a blocked socket would otherwise have its whole
			// backlog copied on every attempt, and libutp takes at most a
			// window anyway.
			pending = m_stream.PeekQueuedBytes(kFlushChunk);
			if (pending.empty()) {
				return;
			}
			m_flushInProgress = true;
			m_flushAgain = false;
		}
		// From here the in-progress flag must be cleared on every exit: the
		// library call and the writable notification both reach code that can
		// throw, and a flag left set stops Write() from ever requesting a
		// flush again -- silently, with IsOk() still true.
		CFlushGuard guard(*this);

		// Offered with the lock released: IUtpSocketOperations' calls can
		// re-enter, and a callback that reaches Flush() again would deadlock
		// against a non-recursive mutex held across the call.
		const std::ptrdiff_t result =
			m_operations.WriteToSocket(socket, pending.data(), pending.size());
		const size_t accepted = result > 0 ? static_cast<size_t>(result) : 0;
		IStreamTransportEvents *writableEvents = nullptr;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			const bool wasBlocked = m_stream.BlocksWrite();
			if (m_socket == socket && m_stream.IsOk()) {
				m_stream.ConsumeQueuedBytes(std::min(accepted, pending.size()));
				if (wasBlocked && !m_stream.BlocksWrite()) {
					writableEvents = m_events;
				}
			}
		}
		if (writableEvents != nullptr) {
			writableEvents->OnStreamWritable();
		}
		IStreamTransportEvents *flushEvents = nullptr;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			// Released first and unconditionally: short-circuiting past it
			// would leave the re-entry recorded but never consumed, and the
			// in-progress flag set while the sink below runs.
			const bool reentered = guard.Release();
			// A fully accepted offer needs a local continuation: utp_writev
			// returns as soon as it has sent everything it was given, without
			// arming CS_CONNECTED_FULL, so no writable edge is coming for the
			// rest of the queue. A partial or window-full result did arm it.
			if (accepted == pending.size() || reentered) {
				flushEvents = RequestFlushLocked();
			}
		}
		RaiseFlushRequest(flushEvents);
	}

	// -- libutp callbacks, translated ---------------------------------

	// Adapter transition, not a libutp callback: the future acceptor MUST call
	// this from UTP_ON_ACCEPT. Only outgoing sockets get UTP_STATE_CONNECT.
	void MarkConnected()
	{
		IStreamTransportEvents *flushEvents = nullptr;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_connected || m_socket == nullptr || !m_stream.IsOk()) {
				return;
			}
			m_connected = true;
			flushEvents = RequestFlushLocked();
		}
		// Requested, not flushed: this runs inside UTP_ON_ACCEPT, where the
		// socket is still CS_SYN_RECV, so utp_writev would refuse every byte
		// at its state guard and the peek would be copied for nothing.
		RaiseFlushRequest(flushEvents);
		NotifyEvents(&IStreamTransportEvents::OnStreamWritable);
	}

	void OnPayload(const uint8_t *data, size_t length)
	{
		IStreamTransportEvents *flushEvents = nullptr;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_stream.OnPayload(data, length);
			// This ST_DATA is what completes an inbound handshake, and
			// libutp makes that transition silently: CS_SYN_RECV becomes
			// CS_CONNECTED with no callback at all. Until it happens
			// utp_writev refuses everything without arming a writable edge,
			// so a reply queued at accept has been waiting for this moment.
			// Requested rather than flushed here, because this runs inside
			// UTP_ON_READ, which would re-enter utp_process_incoming.
			//
			// A peer that connects and then says nothing never reaches this,
			// and so never gets its reply -- unreachable for eD2k, where the
			// side that opened the connection always speaks first.
			flushEvents = RequestFlushLocked();
		}
		RaiseFlushRequest(flushEvents);
		NotifyEvents(&IStreamTransportEvents::OnStreamReadable);
	}

	void OnWritable() { Flush(); }

	void OnEnded(EUtpTransportFailure failure)
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_stream.OnFailure(failure);
			m_connected = false;
			if (failure == EUtpTransportFailure::Destroying) {
				// The handle dies with this callback, so it must not be
				// closed and must not be touched again. Clearing it is
				// what stops the destructor, and everything else, from
				// reaching for it.
				m_socket = nullptr;
			}
		}
		NotifyEvents(&IStreamTransportEvents::OnStreamLost);
	}

	//! For the acceptor and for tests; never leaves the adapter otherwise.
	IUtpSocketOperations::Handle SocketHandle() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_socket;
	}

	//! After construction: the receiving socket does not exist until admission
	//! has decided, and admission needs the transport first.
	void SetEvents(IStreamTransportEvents *events)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_events = events;
	}

	//! How the stream ended, for a caller that needs more than IsOk().
	EUtpTransportFailure Failure() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_stream.Failure();
	}

	/**
	 * Per socket, not looked up from the destination: one address can host
	 * several clients, and the wrong hash leaves the recipient unable to
	 * decrypt. Copied because AttachToAlreadyKnown() can replace the client.
	 */
	void SetCryptParameters(bool encrypt, const uint8_t *userHash)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_encrypt = encrypt && userHash != nullptr;
		if (m_encrypt) {
			std::copy(userHash, userHash + kUserHashBytes, m_userHash);
		}
	}

	//! True when SendUtpDatagram() should obfuscate, with the hash to key on.
	bool CryptParameters(const uint8_t **userHash) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (!m_encrypt) {
			return false;
		}
		*userHash = m_userHash;
		return true;
	}

	//! Current buffered bytes, pulled synchronously by UTP_GET_READ_BUFFER_SIZE.
	size_t ReadBufferSize() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_stream.ReadBufferSize();
	}

	//! Bytes queued and not yet accepted by libutp.
	size_t PendingWriteBytes() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_stream.WriteBufferSize();
	}

private:
	//! Clears the in-progress flag on every exit and reports any re-entry.
	class CFlushGuard
	{
	public:
		explicit CFlushGuard(CUtpSocketTransport &owner)
		: m_owner(owner)
		{
		}

		~CFlushGuard()
		{
			std::lock_guard<std::mutex> lock(m_owner.m_mutex);
			m_owner.m_flushInProgress = false;
		}

		//! Caller holds the mutex. True when a flush was requested meanwhile.
		bool Release()
		{
			m_owner.m_flushInProgress = false;
			const bool again = m_owner.m_flushAgain;
			m_owner.m_flushAgain = false;
			return again;
		}

		CFlushGuard(const CFlushGuard &) = delete;
		CFlushGuard &operator=(const CFlushGuard &) = delete;

	private:
		CUtpSocketTransport &m_owner;
	};

	// Caller holds m_mutex. The returned sink must be called unlocked.
	IStreamTransportEvents *RequestFlushLocked()
	{
		if (m_flushPending || m_socket == nullptr || !m_stream.IsOk() ||
			m_stream.WriteBufferSize() == 0 || m_events == nullptr) {
			return nullptr;
		}
		m_flushPending = true;
		return m_events;
	}

	/**
	 * Delivers a flush request, and does not leave the flag set if it throws.
	 *
	 * m_flushPending gates every later request, so a sink that throws with it
	 * still set stops the socket sending for good, silently, with IsOk() still
	 * reporting true. Same wedge CFlushGuard prevents, one flag over.
	 */
	void RaiseFlushRequest(IStreamTransportEvents *events)
	{
		if (events == nullptr) {
			return;
		}
		try {
			events->OnFlushRequested();
		} catch (...) {
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_flushPending = false;
			}
			throw;
		}
	}

	void NotifyEvents(void (IStreamTransportEvents::*callback)())
	{
		IStreamTransportEvents *events = nullptr;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			events = m_events;
		}
		if (events != nullptr) {
			(events->*callback)();
		}
	}

	//! One window's worth, so an offer costs a packet or two, not the backlog.
	static constexpr size_t kFlushChunk = 64 * 1024;

	//! An ed2k user hash. Copied, so the owning client may be replaced.
	static constexpr size_t kUserHashBytes = 16;

	IUtpSocketOperations &m_operations;
	mutable std::mutex m_mutex;
	IUtpSocketOperations::Handle m_socket;
	const CNetworkAddress m_peer;
	const uint16_t m_peerPort;
	// Non-owning: the owner must quiesce Write()/queued pumps before destruction
	// and keep the sink alive through all calls. Callbacks must not delete this
	// transport synchronously. Destruction detaches before library re-entry.
	IStreamTransportEvents *m_events;
	CUtpStream m_stream;
	bool m_connected = false;
	bool m_flushPending = false;
	bool m_flushInProgress = false;
	bool m_flushAgain = false;
	bool m_encrypt = false;
	uint8_t m_userHash[kUserHashBytes] = { 0 };
};

#endif // UTPSOCKETTRANSPORT_H
// File_checked_for_headers
