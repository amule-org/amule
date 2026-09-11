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

// The buffering between libutp and the eD2k stack.
//
// The property worth the coverage is the would-block contract, because
// CEMSocket depends on it rather than merely tolerating it: 0 bytes with the
// Blocks flag set means "not yet", 0 bytes with an error set means the peer is
// gone. Getting that backwards disconnects a peer whose send window would have
// opened a millisecond later, and the symptom -- peers dropping under load --
// looks nothing like its cause.
//
// The other one is the close: libutp's utp_close() must happen exactly once,
// and a destructor plus a DESTROYING callback both closing is a double free
// that only shows up when a peer leaves at the wrong moment.

#include <muleunit/test.h>

#include <UtpStream.h>

using namespace muleunit;

DECLARE_SIMPLE(UtpStream)

namespace
{
//! Fills a buffer with a recognisable, position-dependent pattern.
std::vector<uint8_t> Pattern(size_t length, uint8_t seed = 0)
{
	std::vector<uint8_t> bytes(length);
	for (size_t i = 0; i < length; ++i) {
		bytes[i] = static_cast<uint8_t>((i * 31 + seed) & 0xFF);
	}
	return bytes;
}
} // namespace

TEST(UtpStream, EmptyReadBlocksAndIsNotAnError)
{
	CUtpStream stream;
	uint8_t out[16] = { 0 };

	ASSERT_EQUALS(0u, stream.Read(out, sizeof(out)));
	ASSERT_TRUE(stream.BlocksRead());
	// The whole point: a would-block must not look like a dead connection.
	ASSERT_EQUALS(0, stream.LastError());
	ASSERT_FALSE(stream.IsTerminal());
}

TEST(UtpStream, PayloadArrivesInOrderAndClearsTheBlock)
{
	CUtpStream stream;
	uint8_t out[4] = { 0 };
	ASSERT_EQUALS(0u, stream.Read(out, sizeof(out)));
	ASSERT_TRUE(stream.BlocksRead());

	const std::vector<uint8_t> sent = Pattern(10);
	stream.OnPayload(sent.data(), sent.size());
	ASSERT_FALSE(stream.BlocksRead());
	ASSERT_EQUALS(10u, (unsigned)stream.ReadBufferSize());

	// A short read takes a prefix and leaves the rest, in order.
	ASSERT_EQUALS(4u, stream.Read(out, sizeof(out)));
	for (size_t i = 0; i < 4; ++i) {
		ASSERT_EQUALS((int)sent[i], (int)out[i]);
	}
	ASSERT_EQUALS(6u, (unsigned)stream.ReadBufferSize());

	uint8_t rest[6] = { 0 };
	ASSERT_EQUALS(6u, stream.Read(rest, sizeof(rest)));
	for (size_t i = 0; i < 6; ++i) {
		ASSERT_EQUALS((int)sent[i + 4], (int)rest[i]);
	}
}

TEST(UtpStream, ReadDrainedIsDueExactlyOncePerDrain)
{
	CUtpStream stream;
	const std::vector<uint8_t> sent = Pattern(8);
	uint8_t out[8] = { 0 };

	// Nothing owed before anything has been buffered.
	ASSERT_FALSE(stream.ConsumeReadDrainedEdge());

	stream.OnPayload(sent.data(), sent.size());
	ASSERT_EQUALS(4u, stream.Read(out, 4));
	// Still four bytes buffered: the reader is not behind, nothing is owed.
	ASSERT_FALSE(stream.ConsumeReadDrainedEdge());

	ASSERT_EQUALS(4u, stream.Read(out, 4));
	ASSERT_TRUE(stream.ConsumeReadDrainedEdge());
	// Consumed. Notifying twice is a wakeup for an already-empty buffer.
	ASSERT_FALSE(stream.ConsumeReadDrainedEdge());

	stream.OnPayload(sent.data(), sent.size());
	ASSERT_EQUALS(8u, stream.Read(out, 8));
	ASSERT_TRUE(stream.ConsumeReadDrainedEdge());
}

TEST(UtpStream, ReadBoundIsReportedRatherThanEnforced)
{
	// libutp can deliver more than the bound in one callback, and a byte
	// dropped here is a hole in a file the peer already paid to send. So the
	// bound is what UTP_GET_READ_BUFFER_SIZE reports -- the peer stops a round
	// trip later -- not something this class refuses.
	CUtpStream stream(CUtpStream::kDefaultWriteBound, 8);
	const std::vector<uint8_t> first = Pattern(20);
	const std::vector<uint8_t> second = Pattern(12, 200);

	stream.OnPayload(first.data(), first.size());
	ASSERT_TRUE(stream.ReadBufferAboveBound());
	ASSERT_EQUALS(8u, (unsigned)stream.ReadBound());

	// The delivery that matters: already past the bound, and it must still be
	// kept in full. Refusing here is the byte-dropping this bound must not do.
	stream.OnPayload(second.data(), second.size());
	ASSERT_EQUALS(32u, (unsigned)stream.ReadBufferSize());
	ASSERT_EQUALS(0, stream.LastError());

	uint8_t out[32] = { 0 };
	ASSERT_EQUALS(32u, stream.Read(out, sizeof(out)));
	for (size_t i = 0; i < first.size(); ++i) {
		ASSERT_EQUALS((int)first[i], (int)out[i]);
	}
	for (size_t i = 0; i < second.size(); ++i) {
		ASSERT_EQUALS((int)second[i], (int)out[first.size() + i]);
	}
	ASSERT_FALSE(stream.ReadBufferAboveBound());
}

TEST(UtpStream, ErrorValuesCannotBeMistakenForWxSocketErrors)
{
	// LastError() stands in for the wx-backed CLibSocket::LastError() under the
	// same name and type, so a call site reaching for wxSOCKET_INVOP (1) or
	// wxSOCKET_IOERR (2) must not match one of ours by coincidence.
	CUtpStream reset;
	reset.OnFailure(EUtpTransportFailure::Reset);
	ASSERT_TRUE(reset.LastError() > 6);
}

TEST(UtpStream, WriteBoundBlocksWithoutFailing)
{
	CUtpStream stream(64);
	const std::vector<uint8_t> payload = Pattern(64, 7);

	ASSERT_EQUALS(64u, stream.Write(payload.data(), 64));
	ASSERT_TRUE(stream.BlocksWrite());
	ASSERT_EQUALS(0, stream.LastError());

	// Full queue: refused, but the stream is alive.
	ASSERT_EQUALS(0u, stream.Write(payload.data(), 8));
	ASSERT_TRUE(stream.BlocksWrite());
	ASSERT_EQUALS(0, stream.LastError());
	ASSERT_FALSE(stream.IsTerminal());
}

TEST(UtpStream, PartialWriteTakesWhatFits)
{
	CUtpStream stream(10);
	const std::vector<uint8_t> payload = Pattern(16, 3);

	// Accepting what fits rather than refusing the whole write is what keeps
	// the caller making progress against a slow peer.
	ASSERT_EQUALS(10u, stream.Write(payload.data(), 16));
	ASSERT_TRUE(stream.BlocksWrite());

	const std::vector<uint8_t> queued = stream.TakeQueuedBytes();
	ASSERT_EQUALS(10u, (unsigned)queued.size());
	for (size_t i = 0; i < queued.size(); ++i) {
		ASSERT_EQUALS((int)payload[i], (int)queued[i]);
	}
	// Draining the queue is what unblocks the writer.
	ASSERT_FALSE(stream.BlocksWrite());
	ASSERT_EQUALS(0u, (unsigned)stream.WriteBufferSize());
}

TEST(UtpStream, WritableReopensAWindowOnlyWhenThereIsRoom)
{
	CUtpStream stream(16);
	const std::vector<uint8_t> payload = Pattern(16, 5);
	ASSERT_EQUALS(16u, stream.Write(payload.data(), 16));
	ASSERT_TRUE(stream.BlocksWrite());

	// The peer's window opening does not empty our queue, so it does not
	// unblock a writer that is bounded by our own buffer.
	stream.OnWritable();
	ASSERT_TRUE(stream.BlocksWrite());

	stream.TakeQueuedBytes();
	stream.OnWritable();
	ASSERT_FALSE(stream.BlocksWrite());
}

TEST(UtpStream, EofAndDestroyingAreEndsRatherThanErrors)
{
	CUtpStream eof;
	eof.OnFailure(EUtpTransportFailure::Eof);
	ASSERT_TRUE(eof.IsTerminal());
	ASSERT_EQUALS(0, eof.LastError());
	ASSERT_FALSE(IsUtpFailure(eof.Failure()));
	// A finished transfer must not be charged as a failure.
	ASSERT_FALSE(eof.BlocksRead());

	CUtpStream destroying;
	destroying.OnFailure(EUtpTransportFailure::Destroying);
	ASSERT_TRUE(destroying.IsTerminal());
	ASSERT_EQUALS(0, destroying.LastError());
}

TEST(UtpStream, TheThreeFailuresStayDistinct)
{
	// A refused connection, a silent peer and a torn-down connection are three
	// different facts about a peer, and the source list acts differently on
	// each. Collapsing them to "failed" is what makes a firewalled peer
	// indistinguishable from a dead one.
	CUtpStream refused;
	refused.OnFailure(EUtpTransportFailure::Refused);
	CUtpStream timedOut;
	timedOut.OnFailure(EUtpTransportFailure::TimedOut);
	CUtpStream reset;
	reset.OnFailure(EUtpTransportFailure::Reset);

	ASSERT_TRUE(refused.Failure() == EUtpTransportFailure::Refused);
	ASSERT_TRUE(timedOut.Failure() == EUtpTransportFailure::TimedOut);
	ASSERT_TRUE(reset.Failure() == EUtpTransportFailure::Reset);

	ASSERT_TRUE(refused.LastError() != 0);
	ASSERT_TRUE(timedOut.LastError() != 0);
	ASSERT_TRUE(reset.LastError() != 0);
	ASSERT_TRUE(refused.LastError() != timedOut.LastError());
	ASSERT_TRUE(timedOut.LastError() != reset.LastError());
}

TEST(UtpStream, TheFirstEndWins)
{
	CUtpStream stream;
	stream.OnFailure(EUtpTransportFailure::Eof);
	// A reset arriving after a clean close must not retroactively fail a
	// transfer that finished.
	stream.OnFailure(EUtpTransportFailure::Reset);
	ASSERT_TRUE(stream.Failure() == EUtpTransportFailure::Eof);
	ASSERT_EQUALS(0, stream.LastError());
}

TEST(UtpStream, WritingToAnEndedStreamIsRefused)
{
	CUtpStream stream;
	const std::vector<uint8_t> payload = Pattern(4);
	stream.OnFailure(EUtpTransportFailure::Reset);
	ASSERT_EQUALS(0u, stream.Write(payload.data(), 4));
	ASSERT_FALSE(stream.BlocksWrite());
}

TEST(UtpStream, ReadingAnEndedStreamReportsTheEndRatherThanBlocking)
{
	CUtpStream stream;
	uint8_t out[4] = { 0 };
	stream.OnFailure(EUtpTransportFailure::Eof);
	// 0 bytes and no block: no more bytes are coming, as opposed to not yet.
	ASSERT_EQUALS(0u, stream.Read(out, sizeof(out)));
	ASSERT_FALSE(stream.BlocksRead());
}

TEST(UtpStream, BufferedBytesSurviveTheEnd)
{
	CUtpStream stream;
	const std::vector<uint8_t> sent = Pattern(5, 9);
	stream.OnPayload(sent.data(), sent.size());
	stream.OnFailure(EUtpTransportFailure::Eof);

	// A peer that sent its last bytes and closed in the same tick has still
	// sent them; dropping the buffer on EOF loses the tail of every transfer.
	uint8_t out[5] = { 0 };
	ASSERT_EQUALS(5u, stream.Read(out, sizeof(out)));
	for (size_t i = 0; i < 5; ++i) {
		ASSERT_EQUALS((int)sent[i], (int)out[i]);
	}
}

TEST(UtpStream, CloseOwnershipIsHandedOutExactlyOnce)
{
	CUtpStream stream;
	ASSERT_FALSE(stream.CloseTaken());
	ASSERT_TRUE(stream.TakeCloseOwnership());
	ASSERT_TRUE(stream.CloseTaken());
	// The destructor, the DESTROYING callback and an explicit Close() all ask;
	// exactly one may call utp_close().
	ASSERT_FALSE(stream.TakeCloseOwnership());
	ASSERT_FALSE(stream.TakeCloseOwnership());
}

// File_checked_for_headers
