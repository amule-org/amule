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

// OP_UDPRESERVEDPROT2 (0xB2) carries no eD2k opcode: the byte after the
// protocol byte is a frame type, and the rest is that frame's payload
// (eMuleAI, srchybrid/ClientUDPSocket.cpp:982). Everything about that layout
// is wire format, including the fact that a datagram consisting of the
// protocol byte alone has no type byte to read.
//
// CClientUDPSocket needs theApp, so the classification is a free function in a
// header of its own and the socket does nothing but dispatch on the result.
// What this test therefore covers is the part that decides: truncation, the
// five registered types, the payload window handed to each handler, and the
// unknown-type drop. The switch in CClientUDPSocket::ProcessReservedProt2Frame
// is a five-way jump over the same constants.

#include <muleunit/test.h>

#include <ReservedProtocolFrames.h>
#include <protocol/Protocols.h>

using namespace muleunit;

DECLARE_SIMPLE(ReservedProtocolFrames)

// The frame types are wire format. Literal values, not a restatement of the
// header they check.
TEST(ReservedProtocolFrames, FrameTypeValuesAreExact)
{
	ASSERT_EQUALS(0xB2, (int)OP_UDPRESERVEDPROT2);
	ASSERT_EQUALS(0x00, (int)OP_NATT_FRAME_UTP);
	ASSERT_EQUALS(0x01, (int)OP_NATT_FRAME_QUIC);
	ASSERT_EQUALS(0x02, (int)OP_NATT_FRAME_CAPS);
	ASSERT_EQUALS(0x03, (int)OP_NATT_FRAME_CAPS_ACK);
	ASSERT_EQUALS(0xFF, (int)OP_NATT_FRAME_KEY);
}

// Spec delta, "Truncated frame": an empty payload must be dropped and the read
// must not reach past the end. The classifier is handed the frame window only
// (the byte after the protocol byte onwards), so an empty window is exactly
// the datagram that carried nothing but 0xB2.
TEST(ReservedProtocolFrames, EmptyFrameIsTruncated)
{
	// A deliberately non-zero guard byte one position before the window: if
	// the classifier read backwards or past its length it would find 0xAA
	// and report it as a frame type.
	const uint8_t buffer[2] = { 0xAA, 0xAA };

	SReservedProt2Frame frame = ClassifyReservedProt2Frame(buffer + 1, 0);
	ASSERT_EQUALS((int)RP2_TRUNCATED, (int)frame.disposition);
	ASSERT_EQUALS(0u, (unsigned)frame.payloadLength);

	// A null window is the same condition, not a crash.
	frame = ClassifyReservedProt2Frame(NULL, 0);
	ASSERT_EQUALS((int)RP2_TRUNCATED, (int)frame.disposition);
}

// Each registered type reaches its handler with the payload that follows it,
// and a type byte with nothing after it is a valid zero-length payload -- not
// truncation. Only the type byte itself is mandatory.
TEST(ReservedProtocolFrames, EachKnownTypeIsRecognised)
{
	const uint8_t known[5] = { OP_NATT_FRAME_UTP,
		OP_NATT_FRAME_QUIC,
		OP_NATT_FRAME_CAPS,
		OP_NATT_FRAME_CAPS_ACK,
		OP_NATT_FRAME_KEY };

	for (int i = 0; i < 5; ++i) {
		const uint8_t buffer[4] = { known[i], 0x11, 0x22, 0x33 };

		SReservedProt2Frame frame = ClassifyReservedProt2Frame(buffer, 4);
		ASSERT_EQUALS((int)RP2_KNOWN_TYPE, (int)frame.disposition);
		ASSERT_EQUALS((int)known[i], (int)frame.type);
		ASSERT_EQUALS(3u, (unsigned)frame.payloadLength);
		ASSERT_TRUE(frame.payload == buffer + 1);
		ASSERT_EQUALS(0x11, (int)frame.payload[0]);

		// Type byte only: an empty payload for that handler to reject on
		// its own terms.
		frame = ClassifyReservedProt2Frame(buffer, 1);
		ASSERT_EQUALS((int)RP2_KNOWN_TYPE, (int)frame.disposition);
		ASSERT_EQUALS((int)known[i], (int)frame.type);
		ASSERT_EQUALS(0u, (unsigned)frame.payloadLength);
	}
}

// Spec delta, "Frame type the client cannot handle": every type outside the
// registered five is dropped. Sweeping the whole byte range is what keeps a
// later type addition from silently landing in the wrong branch.
TEST(ReservedProtocolFrames, EveryUnregisteredTypeIsDropped)
{
	for (unsigned type = 0; type <= 0xFF; ++type) {
		const bool registered = (type == OP_NATT_FRAME_UTP || type == OP_NATT_FRAME_QUIC ||
					 type == OP_NATT_FRAME_CAPS || type == OP_NATT_FRAME_CAPS_ACK ||
					 type == OP_NATT_FRAME_KEY);

		const uint8_t buffer[2] = { (uint8_t)type, 0x55 };
		SReservedProt2Frame frame = ClassifyReservedProt2Frame(buffer, 2);

		if (registered) {
			ASSERT_EQUALS((int)RP2_KNOWN_TYPE, (int)frame.disposition);
		} else {
			ASSERT_EQUALS((int)RP2_UNKNOWN_TYPE, (int)frame.disposition);
			// The type still travels out, because the drop is logged.
			ASSERT_EQUALS((int)type, (int)frame.type);
		}
	}
}

// The classifier is what the flood-accounting exemption rests on: it returns a
// disposition and touches nothing else, so a dropped frame cannot reach
// CPacketTracking. Only OP_KADEMLIAHEADER does
// (KademliaUDPListener.cpp:263), and this classifier has no way to get there.
// Asserting the function is const-correct over its input is the closest a unit
// test gets to that; the placement is what actually guarantees it.
TEST(ReservedProtocolFrames, ClassificationDoesNotMutateTheDatagram)
{
	uint8_t buffer[4] = { 0x7E, 0x11, 0x22, 0x33 };
	const uint8_t before[4] = { 0x7E, 0x11, 0x22, 0x33 };

	SReservedProt2Frame frame = ClassifyReservedProt2Frame(buffer, 4);
	ASSERT_EQUALS((int)RP2_UNKNOWN_TYPE, (int)frame.disposition);

	for (int i = 0; i < 4; ++i) {
		ASSERT_EQUALS((int)before[i], (int)buffer[i]);
	}
}

// An unknown-frame flood must not become a log flood. First occurrence logs,
// the rest of the window is counted, and the count is reported with the next
// line that does get through.
TEST(ReservedProtocolFrames, UnknownFrameLogIsRateLimited)
{
	CUnknownFrameLogThrottle throttle(1000);

	ASSERT_TRUE(throttle.ShouldLog(10000));
	ASSERT_EQUALS(0u, throttle.TakeSuppressedCount());

	ASSERT_FALSE(throttle.ShouldLog(10001));
	ASSERT_FALSE(throttle.ShouldLog(10500));
	ASSERT_FALSE(throttle.ShouldLog(10999));

	ASSERT_TRUE(throttle.ShouldLog(11000));
	ASSERT_EQUALS(3u, throttle.TakeSuppressedCount());
	ASSERT_EQUALS(0u, throttle.TakeSuppressedCount());

	ASSERT_FALSE(throttle.ShouldLog(11001));
	ASSERT_TRUE(throttle.ShouldLog(99999));
	ASSERT_EQUALS(1u, throttle.TakeSuppressedCount());
}

// A tick counter that appears to move backwards (it should not, but the
// throttle must not answer with a silence that lasts until it catches up)
// opens the window rather than closing it.
TEST(ReservedProtocolFrames, ThrottleToleratesNonMonotonicClock)
{
	CUnknownFrameLogThrottle throttle(1000);

	ASSERT_TRUE(throttle.ShouldLog(10000));
	ASSERT_TRUE(throttle.ShouldLog(500));
}
