//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// Any parts of this program contributed by third-party developers are copyrighted
// by their respective authors.
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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
//

#ifndef QUICSOCKETTRANSPORT_H
#define QUICSOCKETTRANSPORT_H

#include "StreamTransport.h"
#include <cstdint>
#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

class IQuicStreamOperations
{
public:
	using Handle = void *;
	virtual ~IQuicStreamOperations() = default;
	virtual std::ptrdiff_t WriteStream(Handle, const uint8_t *, size_t) = 0;
	virtual void CloseStream(Handle) = 0;
	//! The application consumed @a bytes; reopen that much stream and connection flow-control
	//! credit (ngtcp2_conn_extend_max_stream_offset + ngtcp2_conn_extend_max_offset).
	virtual void ExtendReadWindow(Handle, size_t bytes) = 0;
};

class CQuicSocketTransport final : public IStreamTransport
{
public:
	CQuicSocketTransport(IQuicStreamOperations &,
		IQuicStreamOperations::Handle,
		const CNetworkAddress &,
		uint16_t,
		IStreamTransportEvents *,
		bool);
	~CQuicSocketTransport() override;
	CQuicSocketTransport(const CQuicSocketTransport &) = delete;
	CQuicSocketTransport &operator=(const CQuicSocketTransport &) = delete;
	bool IsConnected() const override;
	bool IsInbound() const override { return m_inbound; }
	bool ObfuscatesStream() const override { return false; }
	void SetEvents(IStreamTransportEvents *) override;
	bool IsOk() const override;
	uint32_t Read(void *, uint32_t) override;
	uint32_t Write(const void *, uint32_t) override;
	void Close() override;
	void Flush() override;
	bool BlocksRead() const override;
	bool BlocksWrite() const override;
	int LastError() const override;
	CNetworkAddress GetPeerAddress() const override { return m_peer; }
	uint16_t GetPeerPort() const override { return m_port; }
	void MarkConnected();
	size_t OnPayload(const uint8_t *, size_t);
	void OnWritable();
	void OnEnded(int error = 0);
	size_t PendingWriteBytes() const;

	/**
	 * Initial stream receive window the engine must advertise. Not a cap that drops bytes:
	 * QUIC has acknowledged every byte OnPayload() sees, so discarding one would corrupt the
	 * eD2k stream. The bound is applied by the peer instead: credit is only reopened through
	 * ExtendReadWindow() as Read() drains, so buffered bytes never exceed this window.
	 */
	static constexpr size_t kReadWindow = 256 * 1024;

private:
	void RequestFlushLocked(IStreamTransportEvents *&);
	void ClearWriteLocked();
	IQuicStreamOperations &m_operations;
	mutable std::mutex m_mutex;
	IQuicStreamOperations::Handle m_handle;
	const CNetworkAddress m_peer;
	const uint16_t m_port;
	const bool m_inbound;
	IStreamTransportEvents *m_events;
	static constexpr size_t kWriteBound = 256 * 1024;
	std::deque<uint8_t> m_read;
	// Chunked so Flush() can hand WriteStream() the front chunk in place. While a flush is
	// in flight, Write() never touches the front chunk, so its buffer stays valid unlocked.
	std::deque<std::vector<uint8_t>> m_writeChunks;
	size_t m_writeFrontOffset = 0;
	size_t m_writeQueued = 0;
	bool m_connected = false;
	bool m_closed = false;
	bool m_flushPending = false;
	bool m_flushInProgress = false;
	bool m_blocksWrite = false;
	int m_error = 0;
};

#endif
