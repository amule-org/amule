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

#include <muleunit/test.h>
#include <QuicSocketTransport.h>
#include <functional>
#include <vector>
using namespace muleunit;
DECLARE_SIMPLE(QuicTransportLifecycle)
DECLARE_SIMPLE(QuicTransportWrites)
DECLARE_SIMPLE(QuicTransportBackpressure)
DECLARE_SIMPLE(QuicTransportReads)
DECLARE_SIMPLE(QuicTransportCallbacks)
DECLARE_SIMPLE(QuicTransportFailures)
DECLARE_SIMPLE(QuicTransportContract)
namespace
{
struct Ops : IQuicStreamOperations
{
	std::ptrdiff_t result = 999;
	int writes = 0, closes = 0;
	std::function<void()> onWrite;
	bool inWrite = false;
	bool closedDuringWrite = false;
	std::vector<uint8_t> sent;
	std::ptrdiff_t WriteStream(Handle, const uint8_t *p, size_t n, uint64_t) override
	{
		++writes;
		inWrite = true;
		if (onWrite)
			onWrite();
		inWrite = false;
		size_t k = result < 0 ? 0 : std::min<size_t>(result, n);
		sent.insert(sent.end(), p, p + k);
		return result < 0 ? result : static_cast<std::ptrdiff_t>(k);
	}
	void CloseStream(Handle, uint64_t) override
	{
		++closes;
		closedDuringWrite = inWrite;
	}
	int aborts = 0;
	void AbortStream(Handle) override { ++aborts; }
	size_t extended = 0;
	void ExtendReadWindow(Handle, size_t bytes) override { extended += bytes; }
};
struct Events : IStreamTransportEvents
{
	int connected = 0, readable = 0, writable = 0, lost = 0, flush = 0;
	void OnStreamConnected() override { ++connected; }
	void OnStreamReadable() override { ++readable; }
	void OnStreamWritable() override { ++writable; }
	void OnStreamLost() override { ++lost; }
	void OnFlushRequested() override { ++flush; }
};
IQuicStreamOperations::Handle H()
{
	static int x;
	return &x;
}
} // namespace
TEST(QuicTransportContract, FreshTransportIsWritable)
{
	Ops o;
	Events e;
	CQuicSocketTransport t(o, H(), CNetworkAddress::FromString("192.0.2.1"), 1, &e, false);
	ASSERT_TRUE(!t.BlocksWrite());
}

TEST(QuicTransportContract, EmptyPayloadIsNotReadable)
{
	Ops o;
	Events e;
	CQuicSocketTransport t(o, H(), CNetworkAddress::FromString("192.0.2.1"), 1, &e, false);
	t.OnPayload(nullptr, 0);
	ASSERT_EQUALS(0, e.readable);
}

TEST(QuicTransportLifecycle, LifecycleAndIdempotentLoss)
{
	Ops o;
	Events e;
	CQuicSocketTransport t(o, H(), CNetworkAddress::FromString("192.0.2.1"), 1, &e, false);
	ASSERT_TRUE(!t.IsConnected());
	t.MarkConnected();
	t.MarkConnected();
	ASSERT_TRUE(t.IsConnected());
	ASSERT_EQUALS(1, e.connected);
	t.OnEnded(7);
	t.OnEnded(8);
	ASSERT_TRUE(!t.IsOk());
	ASSERT_EQUALS(7, t.LastError());
	ASSERT_EQUALS(1, e.lost);
	t.Close();
	ASSERT_EQUALS(0, o.closes);
}
TEST(QuicTransportWrites, QueuesAndFlushes)
{
	Ops o;
	Events e;
	CQuicSocketTransport t(o, H(), CNetworkAddress::FromString("192.0.2.1"), 1, &e, false);
	const uint8_t p[] = { 1, 2, 3 };
	ASSERT_EQUALS(3u, t.Write(p, 3));
	ASSERT_EQUALS(0, o.writes);
	ASSERT_EQUALS(1, e.flush);
	t.Flush();
	ASSERT_EQUALS(1, o.writes);
	ASSERT_EQUALS(3u, o.sent.size());
	ASSERT_EQUALS(1, e.writable);
}
TEST(QuicTransportBackpressure, RetainsTail)
{
	Ops o;
	o.result = 1;
	Events e;
	CQuicSocketTransport t(o, H(), CNetworkAddress::FromString("192.0.2.1"), 1, &e, false);
	const uint8_t p[] = { 1, 2, 3 };
	t.Write(p, 3);
	t.Flush();
	ASSERT_EQUALS(2u, t.PendingWriteBytes());
	ASSERT_TRUE(!t.BlocksWrite());
	o.result = 3;
	t.Flush();
	ASSERT_EQUALS(0u, t.PendingWriteBytes());
}
TEST(QuicTransportReads, ReadBufferAndFailure)
{
	Ops o;
	Events e;
	CQuicSocketTransport t(o, H(), CNetworkAddress::FromString("192.0.2.1"), 1, &e, true);
	const uint8_t p[] = { 4, 5 };
	t.OnPayload(p, 2);
	uint8_t out[2];
	ASSERT_EQUALS(2u, t.Read(out, 2));
	ASSERT_EQUALS(4, (int)out[0]);
	ASSERT_TRUE(t.BlocksRead());
	t.Close();
	ASSERT_EQUALS(1, o.closes);
	t.Close();
	ASSERT_EQUALS(1, o.closes);
}
TEST(QuicTransportReads, NeverDropsAcknowledgedPayload)
{
	Ops o;
	Events e;
	CQuicSocketTransport t(o, H(), CNetworkAddress::FromString("192.0.2.1"), 1, &e, true);
	std::vector<uint8_t> payload(CQuicSocketTransport::kReadWindow + 1);
	for (size_t i = 0; i < payload.size(); ++i)
		payload[i] = static_cast<uint8_t>(i);
	ASSERT_EQUALS(payload.size(), t.OnPayload(payload.data(), payload.size()));
	std::vector<uint8_t> out(payload.size());
	ASSERT_EQUALS(
		static_cast<uint32_t>(out.size()), t.Read(out.data(), static_cast<uint32_t>(out.size())));
	ASSERT_TRUE(out == payload);
}
TEST(QuicTransportReads, ReopensWindowOnlyAsReadDrains)
{
	Ops o;
	Events e;
	CQuicSocketTransport t(o, H(), CNetworkAddress::FromString("192.0.2.1"), 1, &e, true);
	const uint8_t p[] = { 1, 2, 3, 4 };
	t.OnPayload(p, 4);
	ASSERT_EQUALS(0u, o.extended);
	uint8_t out[4];
	t.Read(out, 3);
	ASSERT_EQUALS(3u, o.extended);
	t.Read(out, 4);
	ASSERT_EQUALS(4u, o.extended);
	t.Read(out, 4);
	ASSERT_EQUALS(4u, o.extended);
}
TEST(QuicTransportReads, ClosedStreamRefusesPayloadAndCredit)
{
	Ops o;
	Events e;
	CQuicSocketTransport t(o, H(), CNetworkAddress::FromString("192.0.2.1"), 1, &e, true);
	const uint8_t p[] = { 1, 2 };
	t.OnPayload(p, 2);
	t.OnEnded(0);
	ASSERT_EQUALS(0u, t.OnPayload(p, 2));
	uint8_t out[2];
	ASSERT_EQUALS(2u, t.Read(out, 2));
	ASSERT_EQUALS(0u, o.extended);
	ASSERT_EQUALS(1, e.readable);
}
TEST(QuicTransportCallbacks, DoesNotNotifyAfterReentrantLocalClose)
{
	Ops o;
	Events e;
	CQuicSocketTransport t(o, H(), CNetworkAddress::FromString("192.0.2.1"), 1, &e, false);
	const uint8_t p[] = { 1 };
	t.Write(p, 1);
	o.onWrite = [&] { t.Close(); };
	t.Flush();
	ASSERT_EQUALS(0, e.writable);
	ASSERT_EQUALS(0, e.lost);
	ASSERT_EQUALS(1, o.closes);
	ASSERT_TRUE(!o.closedDuringWrite);
}
TEST(QuicTransportCallbacks, NotifiesLossOnceWhenEndedDuringFlush)
{
	Ops o;
	Events e;
	CQuicSocketTransport t(o, H(), CNetworkAddress::FromString("192.0.2.1"), 1, &e, false);
	const uint8_t p[] = { 1 };
	t.Write(p, 1);
	o.onWrite = [&] { t.OnEnded(3); };
	t.Flush();
	ASSERT_EQUALS(0, e.writable);
	ASSERT_EQUALS(1, e.lost);
	ASSERT_EQUALS(3, t.LastError());
	ASSERT_EQUALS(0, o.closes);
}
TEST(QuicTransportWrites, WritesDuringFlushKeepOrder)
{
	Ops o;
	Events e;
	CQuicSocketTransport t(o, H(), CNetworkAddress::FromString("192.0.2.1"), 1, &e, false);
	const uint8_t first[] = { 1, 2 };
	const uint8_t second[] = { 3 };
	const uint8_t third[] = { 4, 5 };
	t.Write(first, 2);
	bool appended = false;
	o.onWrite = [&] {
		if (appended)
			return;
		appended = true;
		t.Write(second, 1);
		t.Write(third, 2);
	};
	t.Flush();
	ASSERT_EQUALS(3u, t.PendingWriteBytes());
	t.Flush();
	ASSERT_EQUALS(0u, t.PendingWriteBytes());
	ASSERT_EQUALS(2, o.writes);
	const std::vector<uint8_t> expected = { 1, 2, 3, 4, 5 };
	ASSERT_TRUE(o.sent == expected);
}
TEST(QuicTransportBackpressure, PartialWriteResumesMidChunk)
{
	Ops o;
	o.result = 1;
	Events e;
	CQuicSocketTransport t(o, H(), CNetworkAddress::FromString("192.0.2.1"), 1, &e, false);
	const uint8_t first[] = { 1, 2, 3 };
	const uint8_t second[] = { 4 };
	t.Write(first, 3);
	t.Flush();
	t.Write(second, 1);
	o.result = 999;
	t.Flush();
	ASSERT_EQUALS(0u, t.PendingWriteBytes());
	const std::vector<uint8_t> expected = { 1, 2, 3, 4 };
	ASSERT_TRUE(o.sent == expected);
}
TEST(QuicTransportFailures, FatalWriteEndsStream)
{
	Ops o;
	o.result = -7;
	Events e;
	CQuicSocketTransport t(o, H(), CNetworkAddress::FromString("192.0.2.1"), 1, &e, false);
	const uint8_t p[] = { 1 };
	t.Write(p, 1);
	t.Flush();
	t.Flush();
	ASSERT_TRUE(!t.IsOk());
	ASSERT_EQUALS(7, t.LastError());
	ASSERT_EQUALS(1, e.lost);
	// A genuine write failure aborts (resets) the stream, not a graceful close -- the two are
	// now distinct calls (got3nks' review on #1710, finding #6).
	ASSERT_EQUALS(0, o.closes);
	ASSERT_EQUALS(1, o.aborts);
	ASSERT_EQUALS(1, o.writes);
}
TEST(QuicTransportLifecycle, DestructionAbortsStreamWithDiscardedBytes)
{
	Ops o;
	o.result = 0; // flow control: WriteStream() never accepts a byte
	Events e;
	{
		CQuicSocketTransport t(o, H(), CNetworkAddress::FromString("192.0.2.1"), 1, &e, false);
		const uint8_t p[] = { 1, 2, 3 };
		t.Write(p, 3);
		t.Close();
		// Close() could not flush anything out, so it left the stream draining rather than
		// finishing it -- this is what destruction is about to cut short.
		ASSERT_EQUALS(3u, t.PendingWriteBytes());
		ASSERT_EQUALS(0, o.closes);
	}
	// Destruction discarded 3 still-unacknowledged bytes: the stream must be reset, not ended
	// with CloseStream()'s graceful FIN, which would tell the peer this eD2k exchange completed
	// cleanly instead of being cut short (got3nks' review on #1710, finding D).
	ASSERT_EQUALS(0, o.closes);
	ASSERT_EQUALS(1, o.aborts);
}
