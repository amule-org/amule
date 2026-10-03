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

#include "QuicSocketTransport.h"
#include "GetTickCount.h" // GetTickCount64(), the default clock when none is injected
#include <algorithm>
#include <cstring>

CQuicSocketTransport::CQuicSocketTransport(IQuicStreamOperations &operations,
	IQuicStreamOperations::Handle handle,
	const CNetworkAddress &peer,
	uint16_t port,
	IStreamTransportEvents *events,
	bool inbound,
	std::function<uint64_t()> clock)
: m_clock(clock ? std::move(clock) : std::function<uint64_t()>(&::GetTickCount64))
, m_operations(operations)
, m_handle(handle)
, m_peer(peer)
, m_port(port)
, m_inbound(inbound)
, m_events(events)
{
}

CQuicSocketTransport::~CQuicSocketTransport()
{
	SetEvents(nullptr);
	Close();
	// Close() itself may have only started an asynchronous drain (m_draining) rather than
	// finished it -- this object is about to stop existing, so nothing else will ever call
	// OnWritable() on it again to keep that going. A no-op if Close() already finished.
	FinishDrainingForDestruction();
}

bool CQuicSocketTransport::IsConnected() const
{
	std::lock_guard<std::mutex> l(m_mutex);
	return m_connected && !m_closed;
}
bool CQuicSocketTransport::IsOk() const
{
	std::lock_guard<std::mutex> l(m_mutex);
	return !m_closed && m_error == 0;
}
bool CQuicSocketTransport::BlocksRead() const
{
	std::lock_guard<std::mutex> l(m_mutex);
	return m_read.empty() && !m_closed;
}
bool CQuicSocketTransport::BlocksWrite() const
{
	std::lock_guard<std::mutex> l(m_mutex);
	return m_blocksWrite;
}
int CQuicSocketTransport::LastError() const
{
	std::lock_guard<std::mutex> l(m_mutex);
	return m_error;
}
size_t CQuicSocketTransport::PendingWriteBytes() const
{
	std::lock_guard<std::mutex> l(m_mutex);
	return m_writeQueued;
}

void CQuicSocketTransport::SetEvents(IStreamTransportEvents *events)
{
	std::lock_guard<std::mutex> l(m_mutex);
	m_events = events;
}

uint32_t CQuicSocketTransport::Read(void *buffer, uint32_t length)
{
	IQuicStreamOperations::Handle handle = nullptr;
	size_t n;
	{
		std::lock_guard<std::mutex> l(m_mutex);
		n = std::min<size_t>(length, m_read.size());
		const auto consumed = static_cast<std::deque<uint8_t>::difference_type>(n);
		std::copy(m_read.begin(), m_read.begin() + consumed, static_cast<uint8_t *>(buffer));
		m_read.erase(m_read.begin(), m_read.begin() + consumed);
		if (n && !m_closed)
			handle = m_handle;
	}
	if (handle)
		m_operations.ExtendReadWindow(handle, n);
	return static_cast<uint32_t>(n);
}

uint32_t CQuicSocketTransport::Write(const void *buffer, uint32_t length)
{
	IStreamTransportEvents *event = nullptr;
	uint32_t taken = 0;
	{
		std::lock_guard<std::mutex> l(m_mutex);
		if (m_closed)
			return 0;
		if (m_writeQueued >= kWriteBound) {
			m_blocksWrite = true;
			return 0;
		}
		const size_t room = kWriteBound - m_writeQueued;
		taken = static_cast<uint32_t>(std::min<size_t>(room, length));
		if (taken == 0)
			return 0;
		const auto *p = static_cast<const uint8_t *>(buffer);
		const bool backIsInFlight = m_flushInProgress && m_writeChunks.size() == 1;
		if (m_writeChunks.empty() || backIsInFlight)
			m_writeChunks.emplace_back(p, p + taken);
		else
			m_writeChunks.back().insert(m_writeChunks.back().end(), p, p + taken);
		m_writeQueued += taken;
		m_blocksWrite = m_writeQueued >= kWriteBound;
		RequestFlushLocked(event);
	}
	if (event)
		event->OnFlushRequested();
	return taken;
}

void CQuicSocketTransport::RequestFlushLocked(IStreamTransportEvents *&event)
{
	if (!m_flushPending && m_writeQueued && !m_closed && m_events) {
		m_flushPending = true;
		event = m_events;
	}
}

void CQuicSocketTransport::ClearWriteLocked()
{
	m_writeChunks.clear();
	m_writeFrontOffset = 0;
	m_writeQueued = 0;
}

void CQuicSocketTransport::Flush()
{
	IQuicStreamOperations::Handle handle;
	const uint8_t *front;
	size_t frontLength;
	{
		std::lock_guard<std::mutex> l(m_mutex);
		m_flushPending = false;
		// m_draining is the one case Flush() still runs despite m_closed: Close() left something
		// queued that congestion/pacing would not let out yet, and OnWritable() keeps calling
		// this from later real ticks until it does.
		if (m_flushInProgress || (m_closed && !m_draining) || m_writeQueued == 0)
			return;
		m_flushInProgress = true;
		handle = m_handle;
		front = m_writeChunks.front().data() + m_writeFrontOffset;
		frontLength = m_writeChunks.front().size() - m_writeFrontOffset;
	}
	// Read once per call, not cached: ngtcp2 pacing/RTT math wants the actual time of this write,
	// and this can run long after the last datagram/tick (the application flushing on its own
	// schedule, or OnWritable() resuming a drain).
	const uint64_t nowMs = m_clock();
	const std::ptrdiff_t result = m_operations.WriteStream(handle, front, frontLength, nowMs);
	IStreamTransportEvents *writable = nullptr;
	IStreamTransportEvents *again = nullptr;
	IStreamTransportEvents *lost = nullptr;
	IQuicStreamOperations::Handle failedHandle = nullptr;
	IQuicStreamOperations::Handle finishedHandle = nullptr;
	{
		std::lock_guard<std::mutex> l(m_mutex);
		m_flushInProgress = false;
		if (result < 0) {
			if (!m_closed) {
				m_closed = true;
				m_connected = false;
				m_error = static_cast<int>(-result);
				lost = m_events;
			}
			m_draining = false;
			failedHandle = m_handle;
			m_handle = nullptr;
			ClearWriteLocked();
		} else {
			// Advancing the queue first, then deciding what the close (if any) does with the
			// handle, applies the same whether this flush was already draining a closed
			// transport or Close() only arrived while this call was still in flight -- the two
			// used to be different branches here, which is what let the latter skip queue
			// accounting entirely and hand back the handle regardless of what this very write
			// just got out.
			const size_t n = std::min<size_t>(static_cast<size_t>(result), frontLength);
			m_writeFrontOffset += n;
			m_writeQueued -= n;
			if (m_writeFrontOffset == m_writeChunks.front().size()) {
				m_writeChunks.pop_front();
				m_writeFrontOffset = 0;
			}
			if (m_closed) {
				finishedHandle = TryFinishClosingLocked();
			} else {
				const bool wasBlocked = m_blocksWrite;
				m_blocksWrite = result == 0 || m_writeQueued >= kWriteBound;
				if (wasBlocked && !m_blocksWrite)
					writable = m_events;
				else if (n && m_writeQueued == 0)
					writable = m_events;
				// Only when something actually went out: a zero-byte result means flow
				// control or congestion is blocking every byte right now, and asking to be
				// flushed again immediately would just repeat that same zero-byte result in a
				// tight loop until something external changes. OnWritable() is what resumes a
				// blocked flush once the engine notices that something did
				// (IQuicNgtcp2Engine::NotifyWritable()), which is also what drives further
				// draining rounds once m_closed is set -- so this request is deliberately not
				// made in that case.
				if (n > 0) {
					RequestFlushLocked(again);
				}
			}
		}
	}
	if (failedHandle)
		m_operations.AbortStream(failedHandle);
	if (finishedHandle)
		m_operations.CloseStream(finishedHandle, nowMs);
	if (writable)
		writable->OnStreamWritable();
	if (again)
		again->OnFlushRequested();
	if (lost)
		lost->OnStreamLost();
}

void CQuicSocketTransport::Close()
{
	bool canFlushHere;
	{
		std::lock_guard<std::mutex> l(m_mutex);
		canFlushHere = !m_closed && !m_flushInProgress;
	}
	if (canFlushHere) {
		// Bounded, not an unbounded drain: Close() still has to return even against a peer that
		// stops acknowledging midway. One Flush() call hands ngtcp2 at most one packet's worth
		// (kMaxUdpPayload) regardless of how much is queued (up to kWriteBound, 256KiB), so a
		// single call was silently dropping everything past the first packet -- the gap that let
		// a clean eD2k close lose its tail. kMaxCloseFlushRounds covers kWriteBound at the
		// smallest packet Flush() ever sends, with margin; CQuicNgtcp2Connection::CloseStream()
		// below only protects what ngtcp2 has already accepted (ConnectionInfo::
		// unackedSendChunks), never what never left this queue in the first place.
		constexpr int kMaxCloseFlushRounds = 256;
		for (int round = 0; round < kMaxCloseFlushRounds; ++round) {
			size_t queuedBefore;
			{
				std::lock_guard<std::mutex> l(m_mutex);
				queuedBefore = m_closed ? 0 : m_writeQueued;
			}
			if (queuedBefore == 0) {
				break;
			}
			Flush();
			size_t queuedAfter;
			{
				std::lock_guard<std::mutex> l(m_mutex);
				queuedAfter = m_closed ? 0 : m_writeQueued;
			}
			// Stop retrying once something else already closed this transport (Flush()'s own
			// fatal-error path), or once a round makes no progress at all (flow-control/
			// congestion blocked right now) -- further identical calls would not help.
			if (queuedAfter >= queuedBefore) {
				break;
			}
		}
	}
	IQuicStreamOperations::Handle handle = nullptr;
	{
		std::lock_guard<std::mutex> l(m_mutex);
		if (m_closed)
			return;
		m_closed = true;
		m_connected = false;
		// If a flush is still in flight, its own completion path (Flush(), above) is the one that
		// calls TryFinishClosingLocked() once it has the chance to advance the queue with
		// whatever this very write gets out -- doing it here too would race that decision.
		if (!m_flushInProgress) {
			handle = TryFinishClosingLocked();
		}
	}
	if (handle)
		m_operations.CloseStream(handle, m_clock());
	// Deliberately does not call OnStreamLost: a local Close() is not a peer
	// loss. Match CUtpSocketTransport::Close() which has the same invariant.
}

IQuicStreamOperations::Handle CQuicSocketTransport::TryFinishClosingLocked()
{
	// m_handle can already be null here if OnEnded() ran first (the connection itself ended
	// independently of this close): nothing is left to drain towards in that case, regardless
	// of m_writeQueued, since there is no connection left to keep calling OnWritable().
	if (m_writeQueued > 0 && m_handle != nullptr) {
		// Congestion/pacing would not let the bounded loop in Close() (or this very Flush()
		// call) get everything out: keep the handle attached so OnWritable() -- driven by real
		// ticks on the still-live connection -- keeps calling Flush() until the queue empties on
		// its own, or ~CQuicSocketTransport() decides to cut it short instead.
		m_draining = true;
		return nullptr;
	}
	m_draining = false;
	IQuicStreamOperations::Handle handle = m_handle;
	m_handle = nullptr;
	return handle;
}

void CQuicSocketTransport::FinishDrainingForDestruction()
{
	// Only ~CQuicSocketTransport() calls this, and only this object's own destruction can cut
	// a still-draining close short: nothing else will ever call OnWritable() on it again once
	// it is gone, so whatever is still queued at this point stays queued forever otherwise.
	IQuicStreamOperations::Handle handle = nullptr;
	{
		std::lock_guard<std::mutex> l(m_mutex);
		if (!m_draining || m_flushInProgress)
			return;
		m_draining = false;
		handle = m_handle;
		m_handle = nullptr;
		ClearWriteLocked();
	}
	// ClearWriteLocked() just discarded bytes the peer never acknowledged: reset the stream
	// rather than CloseStream()'s graceful FIN, which would tell the peer this eD2k exchange
	// ended cleanly instead of being cut short (got3nks' review on #1710, finding D).
	if (handle)
		m_operations.AbortStream(handle);
}

void CQuicSocketTransport::MarkConnected()
{
	IStreamTransportEvents *event = nullptr;
	{
		std::lock_guard<std::mutex> l(m_mutex);
		if (!m_closed && !m_connected) {
			m_connected = true;
			event = m_events;
		}
	}
	if (event)
		event->OnStreamConnected();
}
size_t CQuicSocketTransport::OnPayload(const uint8_t *data, size_t length)
{
	if (data == nullptr || length == 0)
		return 0;
	IStreamTransportEvents *event = nullptr;
	{
		std::lock_guard<std::mutex> l(m_mutex);
		if (m_closed)
			return 0;
		m_read.insert(m_read.end(), data, data + length);
		event = m_events;
	}
	if (event)
		event->OnStreamReadable();
	return length;
}
void CQuicSocketTransport::OnWritable()
{
	Flush();
}
void CQuicSocketTransport::OnEnded(int error)
{
	IStreamTransportEvents *event = nullptr;
	{
		std::lock_guard<std::mutex> l(m_mutex);
		const bool firstNotice = !m_closed;
		if (firstNotice) {
			m_closed = true;
			m_connected = false;
			m_error = error;
			event = m_events;
		}
		// The connection this handle names is gone either way, even if m_closed was already true
		// from a local Close() still draining: that handle is the one this very call is warning
		// is about to become dangling, so draining further is no longer possible regardless of
		// which happened first. Without this, FinishDrainingForDestruction() would later call
		// back into a connection that no longer exists. Not ClearWriteLocked() here, though: this
		// can run reentrantly from inside WriteStream() (a test proves it), while Flush() still
		// holds raw pointers into m_writeChunks.front() for that very call -- freeing it out from
		// under that read would itself be a use-after-free. Flush()'s own completion path cleans
		// up once that call returns, same as any other fatal result.
		m_draining = false;
		m_handle = nullptr;
	}
	if (event)
		event->OnStreamLost();
}
