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

using namespace muleunit;

DECLARE_SIMPLE(UtpSocketTransport)

namespace
{
//! Records every libutp call and decides how much a write may take.
class FakeOperations : public IUtpSocketOperations
{
public:
	size_t WriteToSocket(Handle socket, const uint8_t *data, size_t length) override
	{
		lastWriteSocket = socket;
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
	}
	void SetReceiveBuffer(Handle, size_t bytes) override { receiveBound = bytes; }

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

	int readable = 0, writable = 0, lost = 0;
};

//! A handle value that is never dereferenced, only compared.
IUtpSocketOperations::Handle Handle()
{
	static int marker = 0;
	return &marker;
}

CUtpSocketTransport MakeTransport(FakeOperations &ops)
{
	return CUtpSocketTransport(ops, Handle(), CNetworkAddress::FromString("192.0.2.7"), 4662);
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
	CUtpSocketTransport transport = MakeTransport(ops);
	FakeEvents events;
	const std::vector<uint8_t> payload = Pattern(6);
	transport.OnPayload(payload.data(), payload.size(), &events);
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
	transport.OnPayload(payload.data(), payload.size(), &events);
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
		CUtpSocketTransport transport = MakeTransport(ops);
		transport.OnEnded(EUtpTransportFailure::Destroying, &events);
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
	CUtpSocketTransport transport = MakeTransport(ops);
	const std::vector<uint8_t> payload = Pattern(4);
	transport.Write(payload.data(), 4);
	transport.OnEnded(EUtpTransportFailure::Reset, &events);

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
	CUtpSocketTransport transport = MakeTransport(ops);
	transport.OnEnded(EUtpTransportFailure::Eof, &events);
	ASSERT_FALSE(transport.IsOk());
	ASSERT_EQUALS(0, transport.LastError());
	ASSERT_EQUALS(1, events.lost);
}

TEST(UtpSocketTransport, ConnectingReportsWritableOnce)
{
	FakeOperations ops;
	FakeEvents events;
	CUtpSocketTransport transport = MakeTransport(ops);
	ASSERT_FALSE(transport.IsConnected());
	transport.OnConnected(&events);
	ASSERT_TRUE(transport.IsConnected());
	ASSERT_EQUALS(1, events.writable);
}

TEST(UtpSocketTransport, AWindowOpeningFlushesAndUnblocksOnlyWhenItHelps)
{
	FakeOperations ops;
	ops.acceptLimit = 0;
	FakeEvents events;
	CUtpSocketTransport transport(ops, Handle(), CNetworkAddress::FromString("192.0.2.7"), 4662);

	// Fill the queue past its bound so the writer is blocked.
	const std::vector<uint8_t> payload = Pattern(CUtpStream::kDefaultWriteBound + 16, 5);
	transport.Write(payload.data(), static_cast<uint32_t>(payload.size()));
	ASSERT_TRUE(transport.BlocksWrite());

	// A window that opens while libutp still takes nothing leaves the writer
	// blocked: our own queue is what is full.
	transport.OnWritable(&events);
	ASSERT_TRUE(transport.BlocksWrite());
	ASSERT_EQUALS(0, events.writable);

	ops.acceptLimit = CUtpStream::kDefaultWriteBound;
	transport.OnWritable(&events);
	ASSERT_FALSE(transport.BlocksWrite());
	ASSERT_EQUALS(1, events.writable);
}

TEST(UtpSocketTransport, TheReceiveBoundIsWhatReachesTheLibrary)
{
	// Until this is applied, libutp's own default governs rather than ours.
	FakeOperations ops;
	CUtpSocketTransport transport = MakeTransport(ops);
	ASSERT_EQUALS(0u, (unsigned)ops.receiveBound);
	transport.ApplyReceiveBound();
	ASSERT_EQUALS((unsigned)CUtpStream::kDefaultReadBound, (unsigned)ops.receiveBound);
}

// File_checked_for_headers
