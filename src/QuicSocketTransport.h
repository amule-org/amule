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
#include <functional>
#include <mutex>
#include <vector>

class IQuicStreamOperations
{
public:
	using Handle = void *;
	virtual ~IQuicStreamOperations() = default;
	//! @p nowMs is CQuicSocketTransport's own injected clock, read fresh in Flush(), not a cached
	//! value from the last datagram/tick this connection happened to see --
	//! ngtcp2_conn_writev_stream() uses it for pacing and RTT math, so it must be the actual time
	//! of this call.
	virtual std::ptrdiff_t WriteStream(Handle, const uint8_t *, size_t, uint64_t nowMs) = 0;
	//! A voluntary, clean close (CQuicSocketTransport::Close()): whatever the engine has already
	//! accepted and is still waiting to have acknowledged is not discarded over this. @p nowMs:
	//! see WriteStream().
	virtual void CloseStream(Handle, uint64_t nowMs) = 0;
	//! A write that failed outright (Flush()'s result < 0, a genuine fatal error -- as opposed
	//! to flow-control/congestion, which WriteStream() itself reports as 0 bytes, not an error):
	//! unlike CloseStream(), nothing about this stream can be trusted enough to try preserving.
	virtual void AbortStream(Handle) = 0;
	//! The application consumed @a bytes; reopen that much stream and connection flow-control
	//! credit (ngtcp2_conn_extend_max_stream_offset + ngtcp2_conn_extend_max_offset).
	virtual void ExtendReadWindow(Handle, size_t bytes) = 0;
};

class CQuicSocketTransport final : public IStreamTransport
{
public:
	//! @p clock is this transport's own notion of "now", passed down to
	//! IQuicStreamOperations::WriteStream()/CloseStream() for ngtcp2 pacing/RTT math -- defaults
	//! to the real clock; tests substitute a fake so behaviour does not depend on wall time.
	CQuicSocketTransport(IQuicStreamOperations &,
		IQuicStreamOperations::Handle,
		const CNetworkAddress &,
		uint16_t,
		IStreamTransportEvents *,
		bool,
		std::function<uint64_t()> clock = nullptr);
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
	//! Called with m_mutex held, only once m_closed is set and no flush is in flight: decides
	//! whether the close can finish now (nothing left queued) or must keep draining
	//! asynchronously (m_draining), returning the handle to hand to CloseStream() -- outside the
	//! lock -- only in the former case. Shared by Close() and Flush()'s own completion path,
	//! since a Close() arriving while a flush was already in progress must be decided the same
	//! way once that flush returns.
	IQuicStreamOperations::Handle TryFinishClosingLocked();
	//! Only ~CQuicSocketTransport() calls this: forces a still-draining close to finish right
	//! now, discarding whatever is left queued, since nothing will call OnWritable() on this
	//! object again once it is gone. Whatever is discarded here was never acknowledged by the
	//! peer, so the stream is reset (AbortStream()), not closed gracefully -- a clean FIN would
	//! tell the peer this was a complete, truncated-nothing eD2k exchange (got3nks' review on
	//! #1710, finding D).
	void FinishDrainingForDestruction();
	const std::function<uint64_t()> m_clock;
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
	//! Close() was called but something was still queued that congestion/pacing would not let
	//! the bounded flush loop get out: the handle stays attached and Flush() keeps running
	//! (despite m_closed) as OnWritable() keeps calling it from later real ticks, until the
	//! queue empties on its own or ~CQuicSocketTransport() cuts it short.
	bool m_draining = false;
	bool m_blocksWrite = false;
	int m_error = 0;
};

#endif
