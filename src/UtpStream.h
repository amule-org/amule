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

#ifndef UTPSTREAM_H
#define UTPSTREAM_H

#include "UtpTransportFailure.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

/**
 * The buffering a uTP stream needs, with no libutp in sight.
 *
 * libutp hands bytes up in a callback and takes bytes down through a call, and neither happens when
 * the eD2k stack above wants it to. This holds the two queues in between and answers the would-
 * block questions CEMSocket asks, so the state machine can be tested without a library, a socket or
 * a peer.
 *
 * It calls nothing. Read-drained and window notifications are the transport's to send, from the
 * main thread; this only reports when they are due, because a class that both buffers bytes and re-
 * enters libutp is the one that deadlocks when libutp calls back into it.
 */
class CUtpStream
{
public:
	//! Default bound on unsent bytes. One eD2k block plus headroom.
	static constexpr size_t kDefaultWriteBound = 256 * 1024;

	//! PeekQueuedBytes() default: no cap, the whole queue.
	static constexpr size_t kNoPeekLimit = static_cast<size_t>(-1);

	/**
	 * Default bound on buffered received bytes.
	 *
	 * @b Not a cap that drops bytes: libutp can deliver more than this in one callback and
	 * received data is never discarded, because a byte thrown away here is a hole in a file the
	 * peer already paid to send. It is the number UTP_GET_READ_BUFFER_SIZE reports, which is
	 * how libutp decides to stop advertising receive window -- so the bound is applied by the
	 * peer slowing down, one round trip later, rather than by this class refusing anything.
	 *
	 * Inert until the acceptor wires that callback. It is decided here so the acceptor inherits
	 * a value rather than inventing one, and so both directions are bounded: an unbounded read
	 * buffer is how a peer that sends faster than the application reads grows memory without
	 * limit.
	 */
	static constexpr size_t kDefaultReadBound = 64 * 1024;

	explicit CUtpStream(size_t writeBound = kDefaultWriteBound, size_t readBound = kDefaultReadBound)
	: m_writeBound(writeBound)
	, m_readBound(readBound)
	{
	}

	// -- inbound ------------------------------------------------------

	//! Bytes arrived from the peer. Called from libutp's read callback.
	void OnPayload(const uint8_t *data, size_t length)
	{
		if (data == nullptr || length == 0) {
			return;
		}
		m_readBuffer.insert(m_readBuffer.end(), data, data + length);
		m_blocksRead = false;
	}

	/**
	 * Moves buffered bytes out. An empty buffer is a would-block, not an end: 0 bytes,
	 * BlocksRead() set, LastError() still 0. Once the peer has sent EOF an empty buffer is the
	 * end of the stream instead, and that is the one case where 0 means no more bytes are
	 * coming.
	 */
	uint32_t Read(void *buffer, uint32_t length)
	{
		if (buffer == nullptr || length == 0) {
			return 0;
		}
		const size_t available = m_readBuffer.size();
		if (available == 0) {
			m_blocksRead = !IsTerminal();
			return 0;
		}
		const size_t taken = available < length ? available : length;
		const auto consumed = static_cast<std::deque<uint8_t>::difference_type>(taken);
		std::copy(m_readBuffer.begin(),
			m_readBuffer.begin() + consumed,
			static_cast<uint8_t *>(buffer));
		m_readBuffer.erase(m_readBuffer.begin(), m_readBuffer.begin() + consumed);
		m_blocksRead = false;
		if (m_readBuffer.empty()) {
			// libutp stops delivering while the application is behind, and resumes on
			// utp_read_drained(). Owed exactly once per drain: sending it again with an
			// already-empty buffer is a wakeup for nothing, and never sending it stalls
			// the peer permanently.
			m_readDrainedDue = true;
		}
		return static_cast<uint32_t>(taken);
	}

	//! What libutp's read-buffer-size callback should report.
	size_t ReadBufferSize() const { return m_readBuffer.size(); }

	/**
	 * The value the acceptor passes to utp_setsockopt(UTP_RCVBUF). libutp applies the bound
	 * itself: get_rcv_window() advertises opt_rcvbuf minus what ReadBufferSize() reports, so a
	 * reader that falls behind shrinks the window to zero and the peer stops. Nothing here
	 * compares the two, because a predicate over them would only be useful for refusing a
	 * payload, and refusing one drops bytes the peer already paid to send.
	 */
	size_t ReadBound() const { return m_readBound; }

	//! True once per drain, for the caller that owns the libutp notification.
	bool ConsumeReadDrainedEdge()
	{
		const bool due = m_readDrainedDue;
		m_readDrainedDue = false;
		return due;
	}

	// -- outbound -----------------------------------------------------

	/**
	 * Queues bytes for the peer. A full queue is a would-block: 0 bytes, BlocksWrite() set, no
	 * error. The bound exists because libutp accepts writes into its own send buffer without
	 * limit, so an unbounded queue here would let a stalled peer grow memory until the upload
	 * thread noticed, which it has no way to do.
	 */
	uint32_t Write(const void *buffer, uint32_t length)
	{
		if (IsTerminal() || buffer == nullptr || length == 0) {
			return 0;
		}
		const size_t queued = m_writeBuffer.size();
		if (queued >= m_writeBound) {
			m_blocksWrite = true;
			return 0;
		}
		const size_t room = m_writeBound - queued;
		const size_t taken = room < length ? room : length;
		const uint8_t *in = static_cast<const uint8_t *>(buffer);
		m_writeBuffer.insert(m_writeBuffer.end(), in, in + taken);
		m_blocksWrite = m_writeBuffer.size() >= m_writeBound;
		return static_cast<uint32_t>(taken);
	}

	size_t WriteBufferSize() const { return m_writeBuffer.size(); }

	/**
	 * The queued bytes, for the caller that will offer them to libutp.
	 *
	 * @a limit caps the copy. libutp takes at most a window per call anyway,
	 * so offering the whole backlog copies bytes that cannot be accepted --
	 * and a blocked socket would pay that copy again on every attempt.
	 */
	std::vector<uint8_t> PeekQueuedBytes(size_t limit = kNoPeekLimit) const
	{
		const size_t queued = m_writeBuffer.size();
		const size_t taken = limit < queued ? limit : queued;
		return std::vector<uint8_t>(m_writeBuffer.begin(),
			m_writeBuffer.begin() + static_cast<std::deque<uint8_t>::difference_type>(taken));
	}

	/**
	 * Drops the bytes libutp accepted and keeps the rest queued. utp_write() returns how much
	 * it took, which is less than it was offered as soon as the congestion window is full, and
	 * zero while the socket is not connected. Handing the whole queue out and clearing it would
	 * leave the caller holding the refused tail with nowhere to put it back, so it would need a
	 * second queue that WriteBufferSize() cannot see and m_writeBound does not bound. Peek,
	 * offer, then consume what was taken.
	 */
	void ConsumeQueuedBytes(size_t accepted)
	{
		const size_t queued = m_writeBuffer.size();
		const size_t drop = accepted < queued ? accepted : queued;
		m_writeBuffer.erase(m_writeBuffer.begin(),
			m_writeBuffer.begin() + static_cast<std::deque<uint8_t>::difference_type>(drop));
		m_blocksWrite = m_writeBuffer.size() >= m_writeBound;
	}

	// -- ending -------------------------------------------------------

	/**
	 * Records how the stream ended; the first end wins. A reset arriving after a clean EOF does
	 * not turn a finished transfer into a failed one, which is what would happen if the last
	 * writer won.
	 */
	void OnFailure(EUtpTransportFailure failure)
	{
		if (failure != EUtpTransportFailure::None && m_failure == EUtpTransportFailure::None) {
			m_failure = failure;
		}
		m_blocksRead = false;
		m_blocksWrite = false;
	}

	EUtpTransportFailure Failure() const { return m_failure; }
	bool IsTerminal() const { return IsUtpTerminal(m_failure); }

	/**
	 * Whether the stream is still usable, as IStreamTransport::IsOk() means it. A clean EOF
	 * ends the stream without failing it, so Write() refuses with 0 while BlocksWrite() and
	 * LastError() are both still 0. Those two alone describe a would-block, which this is not,
	 * so the difference has to be askable rather than inferred from the pair.
	 */
	bool IsOk() const { return !IsTerminal(); }

	/**
	 * Nonzero only for a real failure. EOF and destroying are ends, not errors.
	 *
	 * The value is opaque: only its truthiness is defined. It is offset out of the way on
	 * purpose, because this stands in for CLibSocket::LastError() under the same name and type
	 * and a call site comparing against a known constant would otherwise match by coincidence
	 * -- wrong, and silent. CLibSocket::LastError() returns a boost error_code value (see
	 * m_ErrorCode in LibSocketAsio.cpp), so the range to clear is errno on POSIX and the
	 * WinSock codes on Windows, not wxSocketError. Callers wanting the reason ask Failure().
	 */
	int LastError() const
	{
		return IsUtpFailure(m_failure) ? kErrorBase + static_cast<int>(m_failure) : 0;
	}

	bool BlocksRead() const { return m_blocksRead; }
	bool BlocksWrite() const { return m_blocksWrite; }

private:
	//! Above errno, the WinSock range (10000-11999) and wxSocketError alike,
	//! so a stray comparison against any of them cannot match.
	static constexpr int kErrorBase = 0x7500;

	std::deque<uint8_t> m_readBuffer;
	std::deque<uint8_t> m_writeBuffer;
	size_t m_writeBound;
	size_t m_readBound;
	bool m_blocksRead = false;
	bool m_blocksWrite = false;
	bool m_readDrainedDue = false;
	EUtpTransportFailure m_failure = EUtpTransportFailure::None;
};

#endif // UTPSTREAM_H
// File_checked_for_headers
