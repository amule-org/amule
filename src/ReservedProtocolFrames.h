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

#ifndef RESERVEDPROTOCOLFRAMES_H
#define RESERVEDPROTOCOLFRAMES_H

#include <cstddef>
#include <cstdint>

#include <protocol/Protocols.h> // Needed for the OP_NATT_FRAME_* frame types

/**
 * Demultiplexing for OP_UDPRESERVEDPROT2 (0xB2).
 *
 * Unlike every other UDP protocol byte aMule handles, 0xB2 carries no eD2k
 * opcode. The byte after the protocol byte is a frame type and the rest is
 * that frame's payload, which is why this cannot go through
 * CClientUDPSocket::ProcessPacket -- that function's second argument is an
 * opcode and these are not opcodes.
 *
 * The classification lives here rather than in CClientUDPSocket because
 * CClientUDPSocket needs theApp and cannot be linked into a unit test, and
 * because the two things worth getting wrong -- the truncation guard and the
 * unknown-type drop -- are exactly the two that never produce a visible
 * symptom when they are wrong.
 */

//! What the caller should do with an OP_UDPRESERVEDPROT2 frame.
enum EReservedProt2Disposition
{
	//! No frame-type byte to read. Drop; do not read the window.
	RP2_TRUNCATED,
	//! One of the five registered types. Dispatch on SReservedProt2Frame::type.
	RP2_KNOWN_TYPE,
	//! Not a type this protocol defines. Drop.
	RP2_UNKNOWN_TYPE
};

/**
 * A classified frame.
 *
 * `payload` and `payloadLength` describe the bytes after the type byte and are
 * only meaningful for RP2_KNOWN_TYPE. A zero-length payload is legitimate --
 * only the type byte is mandatory, and each handler decides what it needs.
 */
struct SReservedProt2Frame
{
	EReservedProt2Disposition disposition;
	uint8_t type;
	const uint8_t *payload;
	size_t payloadLength;
};

/**
 * Classify an OP_UDPRESERVEDPROT2 frame.
 *
 * @param frame  points at the frame-type byte, i.e. one past the protocol
 *               byte of the datagram. May be NULL when frameLength is 0.
 * @param frameLength  bytes available from `frame` onwards. Zero is the
 *                     datagram that carried nothing but the protocol byte;
 *                     the type byte is never read in that case.
 *
 * Reads nothing beyond frame[0] and never writes through `frame`. The caller
 * must not count a dropped frame against any flood or ban threshold: this
 * function reaches no accounting of its own, and CPacketTracking is only
 * entered from the Kad listener, so a drop here is exempt by placement.
 */
inline SReservedProt2Frame ClassifyReservedProt2Frame(const uint8_t *frame, size_t frameLength)
{
	SReservedProt2Frame result = { RP2_TRUNCATED, 0, nullptr, 0 };

	if (frame == nullptr || frameLength == 0) {
		return result;
	}

	result.type = frame[0];
	result.payload = frame + 1;
	result.payloadLength = frameLength - 1;

	switch (result.type) {
	case OP_NATT_FRAME_UTP:
	case OP_NATT_FRAME_QUIC:
	case OP_NATT_FRAME_CAPS:
	case OP_NATT_FRAME_CAPS_ACK:
	case OP_NATT_FRAME_KEY:
		result.disposition = RP2_KNOWN_TYPE;
		break;
	default:
		result.disposition = RP2_UNKNOWN_TYPE;
		break;
	}

	return result;
}

/**
 * Keeps an unknown-frame flood from becoming a log flood.
 *
 * A peer speaking a frame type this build does not know sends one per
 * connection attempt and retries, so the interesting information is "it
 * happened" plus a count -- not one line each. Logs the first occurrence, then
 * at most one per interval, reporting how many were suppressed in between.
 *
 * Not thread-safe, and does not need to be: the client UDP socket's receive
 * path is posted to the main thread.
 */
class CUnknownFrameLogThrottle
{
public:
	//! @param intervalMs minimum gap between two logged lines.
	explicit CUnknownFrameLogThrottle(uint64_t intervalMs)
	: m_intervalMs(intervalMs)
	{
	}

	/**
	 * @param nowMs a millisecond tick count.
	 * @return true when the caller should log. A tick count that appears to
	 *         move backwards opens the window rather than closing it: the
	 *         alternative is silence until the clock catches up, which on a
	 *         64-bit wrap would be forever.
	 */
	bool ShouldLog(uint64_t nowMs)
	{
		if (!m_everLogged || nowMs < m_lastLoggedMs || nowMs - m_lastLoggedMs >= m_intervalMs) {
			m_everLogged = true;
			m_lastLoggedMs = nowMs;
			// The live counter is moved aside here rather than in
			// TakeSuppressedCount(), so it is reset even in a build
			// where the debug log line it feeds compiles away.
			m_reportable = m_suppressed;
			m_suppressed = 0;
			return true;
		}

		++m_suppressed;
		return false;
	}

	//! How many occurrences were dropped since the previous logged line.
	//! Reads the snapshot ShouldLog() took, so the number is reported once.
	uint32_t TakeSuppressedCount()
	{
		const uint32_t suppressed = m_reportable;
		m_reportable = 0;
		return suppressed;
	}

private:
	uint64_t m_intervalMs;
	uint64_t m_lastLoggedMs = 0;
	uint32_t m_suppressed = 0;
	uint32_t m_reportable = 0;
	bool m_everLogged = false;
};

#endif // RESERVEDPROTOCOLFRAMES_H
