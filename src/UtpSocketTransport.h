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

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

/**
 * The per-socket libutp calls a transport makes, behind a seam.
 *
 * Only four, and they are the ones that must not be made from inside a libutp
 * callback: each can re-enter, and utp_close() in particular can produce
 * UTP_STATE_DESTROYING before it returns. Naming them here keeps the transport
 * testable without the library, and keeps the re-entrancy rule in one place
 * rather than repeated at every call.
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
 * What the layer above a stream is told, in the order it is told.
 *
 * Deliberately not the CoreNotify_LibSocket* macros: those take a CLibSocket*,
 * which does not exist until something accepts a connection, and a transport
 * that reaches for theApp is a transport that cannot be tested. The acceptor
 * implements this by forwarding to those macros.
 */
class IStreamTransportEvents
{
public:
	virtual ~IStreamTransportEvents() = default;

	//! Bytes are readable.
	virtual void OnStreamReadable() = 0;

	//! The send window opened; a blocked writer may continue.
	virtual void OnStreamWritable() = 0;

	//! The stream ended, cleanly or otherwise. Ask the transport which.
	virtual void OnStreamLost() = 0;

	/**
	 * Asks for Flush() to be called on the main thread.
	 *
	 * Raised from the upload bandwidth thread, so the implementation must
	 * marshal -- MuleNotify::DoNotify clones a functor and delivers it to
	 * wxTheApp, which is how the asio layer already crosses the same boundary.
	 * Only the first queue-up since the last flush raises it, so the cost is
	 * one event per idle-to-busy transition rather than one per write.
	 */
	virtual void OnFlushRequested() = 0;
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
		if (events != nullptr) {
			events->OnFlushRequested();
		}
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
			m_stream.OnFailure(EUtpTransportFailure::Eof);
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
	void Flush()
	{
		IUtpSocketOperations::Handle socket = nullptr;
		std::vector<uint8_t> pending;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_flushInProgress) {
				return;
			}
			m_flushPending = false;
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
		}

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
			m_flushInProgress = false;
			// Only a fully accepted offer needs a local continuation. Partial
			// writes wait for UTP_STATE_WRITABLE; nonpositive results must not spin.
			if (accepted == pending.size()) {
				flushEvents = RequestFlushLocked();
			}
		}
		if (flushEvents != nullptr) {
			flushEvents->OnFlushRequested();
		}
	}

	// -- libutp callbacks, translated ---------------------------------

	// Adapter transition, not a libutp callback: the future acceptor MUST call
	// this from UTP_ON_ACCEPT. Only outgoing sockets get UTP_STATE_CONNECT.
	void MarkConnected()
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_connected || m_socket == nullptr || !m_stream.IsOk()) {
				return;
			}
			m_connected = true;
		}
		// utp_write() takes nothing before the handshake completes, so
		// anything queued until now was refused and is still waiting.
		Flush();
		NotifyEvents(&IStreamTransportEvents::OnStreamWritable);
	}

	void OnPayload(const uint8_t *data, size_t length)
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_stream.OnPayload(data, length);
		}
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
};

#endif // UTPSOCKETTRANSPORT_H
// File_checked_for_headers
