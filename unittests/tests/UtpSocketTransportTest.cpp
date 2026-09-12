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

// The transport between CUtpStream and one libutp socket.
//
// Two properties carry the weight. First, a partial write: utp_write() takes
// less than it is offered as soon as the congestion window fills, and nothing
// at all before the socket is connected, so the refused tail has to stay in the
// queue the bound can see. Second, the close: utp_close() must happen exactly
// once, and UTP_STATE_DESTROYING invalidates the handle before it arrives, so
// a destructor that closes after a DESTROYING callback is a double free.

#include <muleunit/test.h>

#include <UtpSocketTransport.h>

#include <functional>
#include <stdexcept>
#include <thread>

using namespace muleunit;

DECLARE_SIMPLE(UtpSocketTransport)

namespace
{
//! Records every libutp call and decides how much a write may take.
class FakeOperations : public IUtpSocketOperations
{
public:
	std::ptrdiff_t WriteToSocket(Handle socket, const uint8_t *data, size_t length) override
	{
		lastWriteSocket = socket;
		if (onWrite) {
			onWrite();
		}
		if (refuseWithError) {
			return -1;
		}
		const size_t taken = length < acceptLimit ? length : acceptLimit;
		offered.insert(offered.end(), data, data + length);
		accepted.insert(accepted.end(), data, data + taken);
		return taken;
	}
	void NotifyReadDrained(Handle) override { ++drainedCalls; }
	void CloseSocket(Handle socket) override
	{
		++closeCalls;
		lastClosed = socket;
		if (onClose) {
			onClose();
		}
	}
	void SetReceiveBuffer(Handle, size_t bytes) override { receiveBound = bytes; }

	std::function<void()> onWrite;
	std::function<void()> onClose;
	bool refuseWithError = false;
	size_t acceptLimit = 1024 * 1024;
	std::vector<uint8_t> offered;
	std::vector<uint8_t> accepted;
	Handle lastWriteSocket = nullptr;
	Handle lastClosed = nullptr;
	int drainedCalls = 0;
	int closeCalls = 0;
	size_t receiveBound = 0;
};

class FakeEvents : public IStreamTransportEvents
{
public:
	void OnStreamReadable() override { ++readable; }
	void OnStreamWritable() override { ++writable; }
	void OnStreamLost() override { ++lost; }
	void OnFlushRequested() override { ++flushRequests; }

	int readable = 0, writable = 0, lost = 0, flushRequests = 0;
};

//! A handle value that is never dereferenced, only compared.
IUtpSocketOperations::Handle Handle()
{
	static int marker = 0;
	return &marker;
}

CUtpSocketTransport MakeTransport(FakeOperations &ops, IStreamTransportEvents *events = nullptr)
{
	return CUtpSocketTransport(ops, Handle(), CNetworkAddress::FromString("192.0.2.7"), 4662, events);
}

std::vector<uint8_t> Pattern(size_t length, uint8_t seed = 0)
{
	std::vector<uint8_t> bytes(length);
	for (size_t i = 0; i < length; ++i) {
		bytes[i] = static_cast<uint8_t>((i * 17 + seed) & 0xFF);
	}
	return bytes;
}
} // namespace

TEST(UtpSocketTransport, PeerIdentityIsCarriedWhole)
{
	FakeOperations ops;
	CUtpSocketTransport transport = MakeTransport(ops);
	ASSERT_TRUE(transport.GetPeerAddress() == CNetworkAddress::FromString("192.0.2.7"));
	ASSERT_EQUALS(4662, (int)transport.GetPeerPort());
}

TEST(UtpSocketTransport, WriteQueuesWithoutTouchingTheLibrary)
{
	// Write() is reached from the upload bandwidth thread, so it must not make
	// a library call: utp_write() can produce callbacks on a thread libutp
	// knows nothing about.
	FakeOperations ops;
	CUtpSocketTransport transport = MakeTransport(ops);
	const std::vector<uint8_t> payload = Pattern(64);

	ASSERT_EQUALS(64u, transport.Write(payload.data(), 64));
	ASSERT_EQUALS(0u, (unsigned)ops.offered.size());

	transport.Flush();
	ASSERT_EQUALS(64u, (unsigned)ops.accepted.size());
	for (size_t i = 0; i < payload.size(); ++i) {
		ASSERT_EQUALS((int)payload[i], (int)ops.accepted[i]);
	}
}

TEST(UtpSocketTransport, RefusedBytesAreOfferedAgainOnTheNextFlush)
{
	FakeOperations ops;
	ops.acceptLimit = 10;
	CUtpSocketTransport transport = MakeTransport(ops);
	const std::vector<uint8_t> payload = Pattern(25, 3);
	ASSERT_EQUALS(25u, transport.Write(payload.data(), 25));

	transport.Flush();
	ASSERT_EQUALS(10u, (unsigned)ops.accepted.size());

	ops.acceptLimit = 1024;
	transport.Flush();
	ASSERT_EQUALS(25u, (unsigned)ops.accepted.size());
	// Every byte exactly once, in order: the refused tail was kept, not resent
	// from the start and not dropped.
	for (size_t i = 0; i < payload.size(); ++i) {
		ASSERT_EQUALS((int)payload[i], (int)ops.accepted[i]);
	}
}

TEST(UtpSocketTransport, AWriteRefusedEntirelyLosesNothing)
{
	// utp_write() returns zero while the socket is not yet connected.
	FakeOperations ops;
	ops.acceptLimit = 0;
	CUtpSocketTransport transport = MakeTransport(ops);
	const std::vector<uint8_t> payload = Pattern(8, 9);
	transport.Write(payload.data(), 8);

	transport.Flush();
	transport.Flush();
	ASSERT_EQUALS(0u, (unsigned)ops.accepted.size());

	ops.acceptLimit = 8;
	transport.Flush();
	ASSERT_EQUALS(8u, (unsigned)ops.accepted.size());
	for (size_t i = 0; i < payload.size(); ++i) {
		ASSERT_EQUALS((int)payload[i], (int)ops.accepted[i]);
	}
}

TEST(UtpSocketTransport, FlushWithNothingQueuedMakesNoCall)
{
	FakeOperations ops;
	CUtpSocketTransport transport = MakeTransport(ops);
	transport.Flush();
	ASSERT_TRUE(ops.lastWriteSocket == nullptr);
}

TEST(UtpSocketTransport, ReadingToEmptyTellsTheLibraryOnce)
{
	FakeOperations ops;
	FakeEvents events;
	CUtpSocketTransport transport = MakeTransport(ops, &events);
	const std::vector<uint8_t> payload = Pattern(6);
	transport.OnPayload(payload.data(), payload.size());
	ASSERT_EQUALS(1, events.readable);

	uint8_t out[6] = { 0 };
	ASSERT_EQUALS(3u, transport.Read(out, 3));
	// Still buffered, so libutp has not been kept waiting.
	ASSERT_EQUALS(0, ops.drainedCalls);

	ASSERT_EQUALS(3u, transport.Read(out, 3));
	ASSERT_EQUALS(1, ops.drainedCalls);

	// Reading an empty buffer is a would-block, not another drain.
	ASSERT_EQUALS(0u, transport.Read(out, 3));
	ASSERT_EQUALS(1, ops.drainedCalls);
	ASSERT_TRUE(transport.BlocksRead());
	ASSERT_EQUALS(0, transport.LastError());
}

TEST(UtpSocketTransport, CloseHappensExactlyOnce)
{
	FakeOperations ops;
	{
		CUtpSocketTransport transport = MakeTransport(ops);
		transport.Close();
		ASSERT_EQUALS(1, ops.closeCalls);
		transport.Close();
		ASSERT_EQUALS(1, ops.closeCalls);
	}
	// The destructor must not close again.
	ASSERT_EQUALS(1, ops.closeCalls);
}

TEST(UtpSocketTransport, AClosedHandleIsNeverUsedAgain)
{
	// The cleared handle is the single guard, so every other call has to
	// respect it too -- a flush or a drained notification against a closed
	// socket is the same use-after-free as a second close.
	FakeOperations ops;
	CUtpSocketTransport transport = MakeTransport(ops);
	const std::vector<uint8_t> payload = Pattern(4);
	FakeEvents events;
	transport.OnPayload(payload.data(), payload.size());
	transport.Write(payload.data(), 4);

	transport.Close();
	ASSERT_EQUALS(1, ops.closeCalls);

	transport.Flush();
	ASSERT_EQUALS(0u, (unsigned)ops.offered.size());

	uint8_t out[4] = { 0 };
	transport.Read(out, sizeof(out));
	ASSERT_EQUALS(0, ops.drainedCalls);
}

TEST(UtpSocketTransport, DestroyingNeverClosesTheDeadHandle)
{
	// UTP_STATE_DESTROYING invalidates the handle before the callback returns,
	// so closing it afterwards is a use-after-free rather than tidiness.
	FakeOperations ops;
	FakeEvents events;
	{
		CUtpSocketTransport transport = MakeTransport(ops, &events);
		transport.OnEnded(EUtpTransportFailure::Destroying);
		ASSERT_EQUALS(1, events.lost);
		ASSERT_EQUALS(0, ops.closeCalls);
		transport.Close();
		ASSERT_EQUALS(0, ops.closeCalls);
	}
	ASSERT_EQUALS(0, ops.closeCalls);
}

TEST(UtpSocketTransport, NothingReachesTheLibraryAfterTheStreamEnds)
{
	FakeOperations ops;
	FakeEvents events;
	CUtpSocketTransport transport = MakeTransport(ops, &events);
	const std::vector<uint8_t> payload = Pattern(4);
	transport.Write(payload.data(), 4);
	transport.OnEnded(EUtpTransportFailure::Reset);

	transport.Flush();
	ASSERT_EQUALS(0u, (unsigned)ops.offered.size());
	ASSERT_FALSE(transport.IsOk());
	ASSERT_FALSE(transport.IsConnected());
	ASSERT_TRUE(transport.LastError() != 0);
}

TEST(UtpSocketTransport, ACleanEndIsNotAnError)
{
	FakeOperations ops;
	FakeEvents events;
	CUtpSocketTransport transport = MakeTransport(ops, &events);
	transport.OnEnded(EUtpTransportFailure::Eof);
	ASSERT_FALSE(transport.IsOk());
	ASSERT_EQUALS(0, transport.LastError());
	ASSERT_EQUALS(1, events.lost);
}

TEST(UtpSocketTransport, AcceptorMarksInboundConnectedWithoutOutgoingConnectCallback)
{
	FakeOperations ops;
	FakeEvents events;
	CUtpSocketTransport transport = MakeTransport(ops, &events);
	ASSERT_FALSE(transport.IsConnected());
	transport.MarkConnected();
	ASSERT_TRUE(transport.IsConnected());
	ASSERT_EQUALS(1, events.writable);
}

TEST(UtpSocketTransport, AWindowOpeningFlushesAndUnblocksOnlyWhenItHelps)
{
	FakeOperations ops;
	ops.acceptLimit = 0;
	FakeEvents events;
	CUtpSocketTransport transport(ops, Handle(), CNetworkAddress::FromString("192.0.2.7"), 4662, &events);

	// Fill the queue past its bound so the writer is blocked.
	const std::vector<uint8_t> payload = Pattern(CUtpStream::kDefaultWriteBound + 16, 5);
	transport.Write(payload.data(), static_cast<uint32_t>(payload.size()));
	ASSERT_TRUE(transport.BlocksWrite());

	// A window that opens while libutp still takes nothing leaves the writer
	// blocked: our own queue is what is full.
	transport.OnWritable();
	ASSERT_TRUE(transport.BlocksWrite());
	ASSERT_EQUALS(0, events.writable);

	ops.acceptLimit = CUtpStream::kDefaultWriteBound;
	transport.OnWritable();
	ASSERT_FALSE(transport.BlocksWrite());
	ASSERT_EQUALS(1, events.writable);
}

TEST(UtpSocketTransport, QueueingAsksForAFlushOncePerIdlePeriod)
{
	// The only other trigger is a full window reopening, so bytes queued while
	// the window was never full would otherwise sit there. Raised on the
	// idle-to-busy edge, so a busy socket does not post an event per write.
	FakeOperations ops;
	ops.acceptLimit = 0;
	FakeEvents events;
	CUtpSocketTransport transport = MakeTransport(ops, &events);
	const std::vector<uint8_t> payload = Pattern(8);

	transport.Write(payload.data(), 8);
	ASSERT_EQUALS(1, events.flushRequests);
	transport.Write(payload.data(), 8);
	transport.Write(payload.data(), 8);
	ASSERT_EQUALS(1, events.flushRequests);

	transport.Flush();
	transport.Write(payload.data(), 8);
	ASSERT_EQUALS(2, events.flushRequests);
}

TEST(UtpSocketTransport, ConnectingFlushesWhatTheHandshakeRefused)
{
	// utp_write() takes nothing before the handshake completes, so everything
	// queued until then is still waiting and has no writable edge coming.
	FakeOperations ops;
	ops.acceptLimit = 0;
	FakeEvents events;
	CUtpSocketTransport transport = MakeTransport(ops, &events);
	const std::vector<uint8_t> payload = Pattern(12, 4);
	transport.Write(payload.data(), 12);
	transport.Flush();
	ASSERT_EQUALS(0u, (unsigned)ops.accepted.size());

	ops.acceptLimit = 64;
	transport.MarkConnected();
	ASSERT_EQUALS(12u, (unsigned)ops.accepted.size());
	for (size_t i = 0; i < payload.size(); ++i) {
		ASSERT_EQUALS((int)payload[i], (int)ops.accepted[i]);
	}
}

TEST(UtpSocketTransport, ReadBufferSizeTracksCurrentOccupancy)
{
	FakeOperations ops;
	CUtpSocketTransport transport = MakeTransport(ops);
	const CUtpSocketTransport &reader = transport;
	ASSERT_EQUALS(0u, (unsigned)reader.ReadBufferSize());
	const std::vector<uint8_t> payload = Pattern(40);
	transport.OnPayload(payload.data(), payload.size());

	transport.ApplyReceiveBound();
	ASSERT_EQUALS((unsigned)CUtpStream::kDefaultReadBound, (unsigned)ops.receiveBound);
	ASSERT_EQUALS(40u, (unsigned)reader.ReadBufferSize());

	uint8_t out[40] = { 0 };
	ASSERT_EQUALS(15u, transport.Read(out, 15));
	ASSERT_EQUALS(25u, (unsigned)reader.ReadBufferSize());
	ASSERT_EQUALS(25u, transport.Read(out, sizeof(out)));
	ASSERT_EQUALS(0u, (unsigned)reader.ReadBufferSize());
	ASSERT_EQUALS(0u, transport.Read(out, sizeof(out)));
	ASSERT_EQUALS(0u, (unsigned)reader.ReadBufferSize());
}

TEST(UtpSocketTransport, AnOfferIsBoundedRatherThanTheWholeBacklog)
{
	// A blocked socket would otherwise have its entire queue copied on every
	// attempt, for bytes libutp cannot take in one call anyway.
	FakeOperations ops;
	ops.acceptLimit = 0;
	CUtpSocketTransport transport = MakeTransport(ops);
	const std::vector<uint8_t> payload = Pattern(CUtpStream::kDefaultWriteBound, 2);
	transport.Write(payload.data(), static_cast<uint32_t>(payload.size()));

	transport.Flush();
	ASSERT_TRUE(ops.offered.size() > 0u);
	ASSERT_TRUE(ops.offered.size() <= 64u * 1024u);
}

TEST(UtpSocketTransport, WritingWhileFlushingDoesNotCorruptTheQueue)
{
	// Concurrency smoke coverage only; passing is not proof of locking.
	FakeOperations ops;
	ops.acceptLimit = 32;
	CUtpSocketTransport transport = MakeTransport(ops);
	const std::vector<uint8_t> chunk = Pattern(32, 11);

	std::thread writer([&transport, &chunk]() {
		for (int i = 0; i < 500; ++i) {
			transport.Write(chunk.data(), static_cast<uint32_t>(chunk.size()));
		}
	});
	for (int i = 0; i < 500; ++i) {
		transport.Flush();
	}
	writer.join();
	while (transport.PendingWriteBytes() != 0) {
		transport.Flush();
	}

	// Every byte that was accepted came out in the pattern's order, so nothing
	// was duplicated, dropped or interleaved.
	ASSERT_TRUE(ops.accepted.size() % chunk.size() == 0);
	for (size_t i = 0; i < ops.accepted.size(); ++i) {
		ASSERT_EQUALS((int)chunk[i % chunk.size()], (int)ops.accepted[i]);
	}
}

TEST(UtpSocketTransport, BoundedFlushSchedulesTheTailAndUnblocksWriter)
{
	FakeOperations ops;
	FakeEvents events;
	auto transport = MakeTransport(ops, &events);
	const auto payload = Pattern(CUtpStream::kDefaultWriteBound);
	transport.Write(payload.data(), static_cast<uint32_t>(payload.size()));
	transport.Flush();
	ASSERT_EQUALS(2, events.flushRequests);
	ASSERT_EQUALS(1, events.writable);
	while (transport.PendingWriteBytes() != 0) {
		transport.Flush();
	}
	ASSERT_TRUE(payload == ops.accepted);
	ASSERT_EQUALS(4, events.flushRequests);
	ASSERT_EQUALS(1, events.writable);
}

TEST(UtpSocketTransport, DirectFlushNotifiesWhenTheQueueUnblocks)
{
	FakeOperations ops;
	FakeEvents events;
	auto transport = MakeTransport(ops, &events);
	const auto payload = Pattern(CUtpStream::kDefaultWriteBound);
	transport.Write(payload.data(), static_cast<uint32_t>(payload.size()));
	transport.Flush();
	ASSERT_FALSE(transport.BlocksWrite());
	ASSERT_EQUALS(1, events.writable);
}

TEST(UtpSocketTransport, LocalCloseEndsTheStreamBeforeCallingTheLibrary)
{
	FakeOperations ops;
	auto transport = MakeTransport(ops);
	transport.MarkConnected();
	ops.onClose = [&]() {
		ASSERT_FALSE(transport.IsOk());
		ASSERT_FALSE(transport.IsConnected());
		const uint8_t byte = 1;
		ASSERT_EQUALS(0u, transport.Write(&byte, 1));
		transport.OnEnded(EUtpTransportFailure::Destroying);
	};
	transport.Close();
	ASSERT_EQUALS(0, transport.LastError());
	ASSERT_EQUALS(1, ops.closeCalls);
}

TEST(UtpSocketTransport, DestructorDetachesSinkBeforeDestroyingReentry)
{
	FakeOperations ops;
	FakeEvents events;
	{
		auto transport = MakeTransport(ops, &events);
		ops.onClose = [&]() { transport.OnEnded(EUtpTransportFailure::Destroying); };
	}
	ASSERT_EQUALS(1, ops.closeCalls);
	ASSERT_EQUALS(0, events.lost);
}

TEST(UtpSocketTransport, NegativeAcceptancePreservesTheQueue)
{
	FakeOperations ops;
	FakeEvents events;
	auto transport = MakeTransport(ops, &events);
	const auto payload = Pattern(12);
	transport.Write(payload.data(), static_cast<uint32_t>(payload.size()));
	ops.refuseWithError = true;
	transport.Flush();
	ASSERT_EQUALS(12u, (unsigned)transport.PendingWriteBytes());
	ASSERT_EQUALS(1, events.flushRequests);
	ops.refuseWithError = false;
	transport.Flush();
	ASSERT_TRUE(payload == ops.accepted);
}

TEST(UtpSocketTransport, ZeroAcceptanceDoesNotScheduleAnotherFlush)
{
	FakeOperations ops;
	FakeEvents events;
	auto transport = MakeTransport(ops, &events);
	const auto payload = Pattern(12);
	transport.Write(payload.data(), static_cast<uint32_t>(payload.size()));
	ops.acceptLimit = 0;
	transport.Flush();
	ASSERT_EQUALS(1, events.flushRequests);
	ASSERT_EQUALS(12u, (unsigned)transport.PendingWriteBytes());
	ops.acceptLimit = 12;
	transport.OnWritable();
	ASSERT_TRUE(payload == ops.accepted);
	ASSERT_EQUALS(1, events.flushRequests);
}

TEST(UtpSocketTransport, PartialAcceptanceWaitsForWritableWithoutScheduling)
{
	FakeOperations ops;
	FakeEvents events;
	auto transport = MakeTransport(ops, &events);
	const auto payload = Pattern(12);
	transport.Write(payload.data(), static_cast<uint32_t>(payload.size()));
	ops.acceptLimit = 5;
	transport.Flush();
	ASSERT_EQUALS(1, events.flushRequests);
	ASSERT_EQUALS(7u, (unsigned)transport.PendingWriteBytes());
	ops.acceptLimit = 12;
	transport.OnWritable();
	ASSERT_TRUE(payload == ops.accepted);
	ASSERT_EQUALS(0u, (unsigned)transport.PendingWriteBytes());
	ASSERT_EQUALS(1, events.flushRequests);
}

TEST(UtpSocketTransport, ReentrantFlushNeverOffersTheSameBytesTwice)
{
	FakeOperations ops;
	FakeEvents events;
	auto transport = MakeTransport(ops, &events);
	const auto payload = Pattern(96 * 1024);
	transport.Write(payload.data(), static_cast<uint32_t>(payload.size()));
	bool reentered = false;
	ops.onWrite = [&]() {
		if (!reentered) {
			reentered = true;
			transport.Flush();
		}
	};
	transport.Flush();
	ASSERT_TRUE(reentered);
	ASSERT_EQUALS(64u * 1024u, (unsigned)ops.accepted.size());
	ASSERT_EQUALS(2, events.flushRequests);
	transport.Flush();
	ASSERT_TRUE(payload == ops.accepted);
	ASSERT_TRUE(payload == ops.offered);
}

TEST(UtpSocketTransport, ConnectingReportsOneWritableEvenWhenDrainingUnblocks)
{
	// One transition, one notification. Flush() already reports the
	// blocked-to-writable edge, so MarkConnected() must not report it again.
	FakeOperations ops;
	ops.acceptLimit = 0;
	FakeEvents events;
	CUtpSocketTransport transport = MakeTransport(ops, &events);
	const std::vector<uint8_t> payload = Pattern(CUtpStream::kDefaultWriteBound, 9);
	transport.Write(payload.data(), static_cast<uint32_t>(payload.size()));
	ASSERT_TRUE(transport.BlocksWrite());

	ops.acceptLimit = CUtpStream::kDefaultWriteBound;
	transport.MarkConnected();
	ASSERT_FALSE(transport.BlocksWrite());
	ASSERT_EQUALS(1, events.writable);
}

TEST(UtpSocketTransport, AReentrantFlushRequestIsHonouredNotDropped)
{
	// The outer flush is offering bytes the reentrant caller never saw, so
	// returning silently would lose whatever edge asked for that flush.
	FakeOperations ops;
	ops.acceptLimit = 8;
	FakeEvents events;
	CUtpSocketTransport transport = MakeTransport(ops, &events);
	const std::vector<uint8_t> payload = Pattern(64, 3);
	transport.Write(payload.data(), static_cast<uint32_t>(payload.size()));
	const int queued = events.flushRequests;

	bool reentered = false;
	ops.onWrite = [&]() {
		if (!reentered) {
			reentered = true;
			transport.Flush();
		}
	};
	transport.Flush();
	ASSERT_TRUE(reentered);
	// Partial acceptance alone would schedule nothing; the swallowed
	// reentrant request is what must still be answered.
	ASSERT_EQUALS(queued + 1, events.flushRequests);
}

TEST(UtpSocketTransport, ALocalCloseIsNotReportedAsThePeersEof)
{
	FakeOperations ops;
	CUtpSocketTransport transport = MakeTransport(ops);
	transport.MarkConnected();
	transport.Close();
	ASSERT_TRUE(transport.Failure() == EUtpTransportFailure::Closed);
	ASSERT_EQUALS(0, transport.LastError());
	ASSERT_FALSE(transport.IsOk());
}

TEST(UtpSocketTransport, IncomingPayloadReleasesAReplyQueuedAtAccept)
{
	// libutp completes an inbound handshake silently: CS_SYN_RECV becomes
	// CS_CONNECTED on the peer's first ST_DATA with no callback, and until
	// then utp_writev refuses everything without arming a writable edge. So
	// this payload is the only signal that the reply can now go out.
	FakeOperations ops;
	ops.acceptLimit = 0;
	FakeEvents events;
	CUtpSocketTransport transport = MakeTransport(ops, &events);
	transport.MarkConnected();
	const std::vector<uint8_t> reply = Pattern(16, 7);
	transport.Write(reply.data(), static_cast<uint32_t>(reply.size()));
	transport.Flush();
	ASSERT_EQUALS(0u, (unsigned)ops.accepted.size());
	const int before = events.flushRequests;

	const std::vector<uint8_t> incoming = Pattern(8, 1);
	transport.OnPayload(incoming.data(), incoming.size());
	ASSERT_EQUALS(before + 1, events.flushRequests);

	ops.acceptLimit = 64;
	transport.Flush();
	ASSERT_TRUE(reply == ops.accepted);
}

TEST(UtpSocketTransport, AThrowingWritableSinkDoesNotWedgeTheFlushPath)
{
	// The in-progress flag is what stops a reentrant flush duplicating bytes.
	// Left set by an exception it would stop Write() requesting flushes at
	// all -- silently, with IsOk() still true.
	class CThrowingEvents : public FakeEvents
	{
	public:
		void OnStreamWritable() override { throw std::runtime_error("sink"); }
	};
	FakeOperations ops;
	ops.acceptLimit = CUtpStream::kDefaultWriteBound;
	CThrowingEvents events;
	CUtpSocketTransport transport = MakeTransport(ops, &events);
	const std::vector<uint8_t> payload = Pattern(CUtpStream::kDefaultWriteBound, 5);
	transport.Write(payload.data(), static_cast<uint32_t>(payload.size()));

	bool threw = false;
	try {
		transport.Flush();
	} catch (const std::runtime_error &) {
		threw = true;
	}
	ASSERT_TRUE(threw);

	// Still usable: the queue drains and queueing still asks for a flush.
	const int before = events.flushRequests;
	transport.Flush();
	const std::vector<uint8_t> more = Pattern(8, 2);
	transport.Write(more.data(), static_cast<uint32_t>(more.size()));
	ASSERT_TRUE(events.flushRequests > before);
}

// File_checked_for_headers
