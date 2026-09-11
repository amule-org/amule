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

	//! Offers bytes. Returns how many were taken, which is routinely fewer.
	virtual size_t WriteToSocket(Handle socket, const uint8_t *data, size_t length) = 0;

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
		uint16_t peerPort)
	: m_operations(operations)
	, m_socket(socket)
	, m_peer(peer)
	, m_peerPort(peerPort)
	{
	}

	~CUtpSocketTransport() override { Close(); }

	CUtpSocketTransport(const CUtpSocketTransport &) = delete;
	CUtpSocketTransport &operator=(const CUtpSocketTransport &) = delete;

	//! Applies the receive bound, which is what makes ReadBound() take effect.
	void ApplyReceiveBound()
	{
		if (m_socket != nullptr) {
			m_operations.SetReceiveBuffer(m_socket, m_stream.ReadBound());
		}
	}

	// -- IStreamTransport ---------------------------------------------

	bool IsConnected() const override { return m_connected && m_stream.IsOk(); }
	bool IsOk() const override { return m_stream.IsOk(); }
	bool BlocksRead() const override { return m_stream.BlocksRead(); }
	bool BlocksWrite() const override { return m_stream.BlocksWrite(); }
	int LastError() const override { return m_stream.LastError(); }
	CNetworkAddress GetPeerAddress() const override { return m_peer; }
	uint16_t GetPeerPort() const override { return m_peerPort; }

	uint32_t Read(void *buffer, uint32_t length) override
	{
		const uint32_t taken = m_stream.Read(buffer, length);
		// Owed only once the reader has emptied the buffer, and only to a
		// socket that still exists: libutp stopped delivering while we were
		// behind, and this is what resumes it.
		if (m_stream.ConsumeReadDrainedEdge() && m_socket != nullptr) {
			m_operations.NotifyReadDrained(m_socket);
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
		return m_stream.Write(buffer, length);
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
		if (m_socket == nullptr) {
			return;
		}
		IUtpSocketOperations::Handle socket = m_socket;
		m_socket = nullptr;
		m_operations.CloseSocket(socket);
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
		if (m_socket == nullptr || !m_stream.IsOk()) {
			return;
		}
		const std::vector<uint8_t> pending = m_stream.PeekQueuedBytes();
		if (pending.empty()) {
			return;
		}
		const size_t accepted = m_operations.WriteToSocket(m_socket, pending.data(), pending.size());
		m_stream.ConsumeQueuedBytes(accepted);
	}

	// -- libutp callbacks, translated ---------------------------------

	void OnConnected(IStreamTransportEvents *events)
	{
		m_connected = true;
		if (events != nullptr) {
			events->OnStreamWritable();
		}
	}

	void OnPayload(const uint8_t *data, size_t length, IStreamTransportEvents *events)
	{
		m_stream.OnPayload(data, length);
		if (events != nullptr) {
			events->OnStreamReadable();
		}
	}

	void OnWritable(IStreamTransportEvents *events)
	{
		const bool wasBlocked = m_stream.BlocksWrite();
		m_stream.OnWritable();
		Flush();
		if (wasBlocked && !m_stream.BlocksWrite() && events != nullptr) {
			events->OnStreamWritable();
		}
	}

	void OnEnded(EUtpTransportFailure failure, IStreamTransportEvents *events)
	{
		m_stream.OnFailure(failure);
		m_connected = false;
		if (failure == EUtpTransportFailure::Destroying) {
			// The handle dies with this callback, so it must not be closed
			// and must not be touched again. Clearing it is what stops the
			// destructor, and everything else, from reaching for it.
			m_socket = nullptr;
		}
		if (events != nullptr) {
			events->OnStreamLost();
		}
	}

	//! For the acceptor and for tests; never leaves the adapter otherwise.
	IUtpSocketOperations::Handle SocketHandle() const { return m_socket; }

	const CUtpStream &Stream() const { return m_stream; }

private:
	IUtpSocketOperations &m_operations;
	IUtpSocketOperations::Handle m_socket;
	CNetworkAddress m_peer;
	uint16_t m_peerPort;
	CUtpStream m_stream;
	bool m_connected = false;
};

#endif // UTPSOCKETTRANSPORT_H
// File_checked_for_headers
