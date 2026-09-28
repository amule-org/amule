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
#include <algorithm>
#include <cstring>

CQuicSocketTransport::CQuicSocketTransport(IQuicStreamOperations &operations,
	IQuicStreamOperations::Handle handle,
	const CNetworkAddress &peer,
	uint16_t port,
	IStreamTransportEvents *events,
	bool inbound)
: m_operations(operations)
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
		if (m_flushInProgress || m_closed || m_writeQueued == 0)
			return;
		m_flushInProgress = true;
		handle = m_handle;
		front = m_writeChunks.front().data() + m_writeFrontOffset;
		frontLength = m_writeChunks.front().size() - m_writeFrontOffset;
	}
	const std::ptrdiff_t result = m_operations.WriteStream(handle, front, frontLength);
	IStreamTransportEvents *writable = nullptr;
	IStreamTransportEvents *again = nullptr;
	IStreamTransportEvents *lost = nullptr;
	IQuicStreamOperations::Handle failedHandle = nullptr;
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
			failedHandle = m_handle;
			m_handle = nullptr;
			ClearWriteLocked();
		} else if (m_closed) {
			// Whoever closed during the write (Close() or OnEnded()) owns the notification.
			failedHandle = m_handle;
			m_handle = nullptr;
			ClearWriteLocked();
		} else {
			const size_t n = std::min<size_t>(static_cast<size_t>(result), frontLength);
			m_writeFrontOffset += n;
			m_writeQueued -= n;
			if (m_writeFrontOffset == m_writeChunks.front().size()) {
				m_writeChunks.pop_front();
				m_writeFrontOffset = 0;
			}
			const bool wasBlocked = m_blocksWrite;
			m_blocksWrite = result == 0 || m_writeQueued >= kWriteBound;
			if (wasBlocked && !m_blocksWrite)
				writable = m_events;
			else if (n && m_writeQueued == 0)
				writable = m_events;
			RequestFlushLocked(again);
		}
	}
	if (failedHandle)
		m_operations.CloseStream(failedHandle);
	if (writable)
		writable->OnStreamWritable();
	if (again)
		again->OnFlushRequested();
	if (lost)
		lost->OnStreamLost();
}

void CQuicSocketTransport::Close()
{
	IQuicStreamOperations::Handle handle = nullptr;
	{
		std::lock_guard<std::mutex> l(m_mutex);
		if (m_closed)
			return;
		m_closed = true;
		m_connected = false;
		if (!m_flushInProgress) {
			handle = m_handle;
			m_handle = nullptr;
		}
	}
	if (handle)
		m_operations.CloseStream(handle);
	// Deliberately does not call OnStreamLost: a local Close() is not a peer
	// loss. Match CUtpSocketTransport::Close() which has the same invariant.
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
		if (m_closed)
			return;
		m_closed = true;
		m_connected = false;
		m_error = error;
		m_handle = nullptr;
		event = m_events;
	}
	if (event)
		event->OnStreamLost();
}
