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
#include <QuicContext.h>
#include <NetworkAddress.h>

#include <algorithm>
#include <memory>
#include <vector>

using namespace muleunit;
DECLARE_SIMPLE(QuicContext)

namespace
{
std::vector<uint8_t> Initial(uint8_t cid, size_t length = CQuicContext::kMinInitialDatagram)
{
	std::vector<uint8_t> packet = { 0xc0, 0x00, 0x00, 0x00, 0x01, 0x01, cid, 0x00 };
	packet.resize(length);
	return packet;
}

const CNetworkAddress kPeer = CNetworkAddress::FromString("192.0.2.1");

CNetworkAddress PeerNumber(unsigned n)
{
	return CNetworkAddress::FromIPv4HostOrder(0xc6120000u + n);
}

std::vector<uint8_t> ShortPacket()
{
	return { 0x40, 0x01, 0x02 };
}

struct FakeConnection : IQuicConnection
{
	bool accepted = true;
	bool closed = false;
	bool closeCalled = false;
	bool *closeObserved = nullptr;
	unsigned calls = 0;

	bool ProcessDatagram(const uint8_t *, size_t, uint64_t) override
	{
		++calls;
		return accepted;
	}

	bool IsClosed() const override { return closed; }

	std::vector<std::string> issuedCids;
	bool OwnsConnectionId(const std::string &cid) const override
	{
		return std::find(issuedCids.begin(), issuedCids.end(), cid) != issuedCids.end();
	}
	std::string GetIssuedConnectionId() const override
	{
		return issuedCids.empty() ? std::string() : issuedCids.front();
	}

	unsigned ticks = 0;
	//! Same reason as closeObserved: a tick that closes this connection makes it fair game for
	//! CQuicContext::Tick()'s own EraseClosedConnections() before the caller gets to look, so
	//! the count has to be readable through something that outlives this object.
	unsigned *ticksObserved = nullptr;
	bool closeOnTick = false;
	void Tick(uint64_t) override
	{
		++ticks;
		if (ticksObserved != nullptr) {
			*ticksObserved = ticks;
		}
		if (closeOnTick) {
			closed = true;
		}
	}

	void Close() override
	{
		closeCalled = true;
		if (closeObserved != nullptr) {
			*closeObserved = true;
		}
		closed = true;
	}
};

struct FakeFactory : IQuicConnectionFactory
{
	std::vector<FakeConnection *> created;
	std::unique_ptr<FakeConnection> next;
	unsigned calls = 0;
	bool alwaysCreate = false;
	CNetworkAddress lastAddress;
	uint16_t lastPort = 0;
	std::string lastCid;

	std::unique_ptr<IQuicConnection> CreateInbound(const uint8_t *,
		size_t,
		const CNetworkAddress &address,
		uint16_t port,
		const std::string &cid,
		uint64_t) override
	{
		++calls;
		lastAddress = address;
		lastPort = port;
		lastCid = cid;
		if (alwaysCreate && !next) {
			next = std::make_unique<FakeConnection>();
		}
		if (!next) {
			return nullptr;
		}
		created.push_back(next.get());
		return std::move(next);
	}
};
} // namespace

TEST(QuicContext, DeclinesUntilConnectionOwnerExists)
{
	CQuicContext context;
	auto payload = Initial(0x11);
	ASSERT_TRUE(!context.ProcessDatagram(payload.data(), payload.size(), kPeer, 4662, 0));
}

TEST(QuicContext, EmptyDatagramIsAlsoFailClosed)
{
	CQuicContext context;
	ASSERT_TRUE(!context.ProcessDatagram(nullptr, 0, CNetworkAddress(), 0, 0));
}

TEST(QuicContext, UnknownNonInitialDoesNotCreateConnection)
{
	FakeFactory factory;
	factory.next = std::make_unique<FakeConnection>();
	CQuicContext context(&factory);
	auto payload = ShortPacket();
	ASSERT_TRUE(!context.ProcessDatagram(payload.data(), payload.size(), kPeer, 4662, 0));
	ASSERT_EQUALS(0u, factory.calls);
}

TEST(QuicContext, InitialCreatesOwnedConnection)
{
	FakeFactory factory;
	factory.next = std::make_unique<FakeConnection>();
	CQuicContext context(&factory);
	auto payload = Initial(0x22);
	ASSERT_TRUE(context.ProcessDatagram(payload.data(), payload.size(), kPeer, 4662, 0));
	ASSERT_EQUALS(1u, factory.calls);
	ASSERT_TRUE(factory.lastAddress == kPeer);
	ASSERT_EQUALS(4662u, factory.lastPort);
	ASSERT_EQUALS(1u, factory.created[0]->calls);
}

TEST(QuicContext, SubsequentPacketRoutesToOwnedEndpoint)
{
	FakeFactory factory;
	factory.next = std::make_unique<FakeConnection>();
	CQuicContext context(&factory);
	auto initial = Initial(0x33);
	ASSERT_TRUE(context.ProcessDatagram(initial.data(), initial.size(), kPeer, 4662, 0));
	auto packet = ShortPacket();
	ASSERT_TRUE(context.ProcessDatagram(packet.data(), packet.size(), kPeer, 4662, 0));
	ASSERT_EQUALS(1u, factory.calls);
	ASSERT_EQUALS(2u, factory.created[0]->calls);
}

TEST(QuicContext, DuplicateInitialDoesNotReplaceOwnedConnection)
{
	FakeFactory factory;
	factory.next = std::make_unique<FakeConnection>();
	CQuicContext context(&factory);
	auto initial = Initial(0x44);
	ASSERT_TRUE(context.ProcessDatagram(initial.data(), initial.size(), kPeer, 4662, 0));
	factory.next = std::make_unique<FakeConnection>();
	ASSERT_TRUE(context.ProcessDatagram(initial.data(), initial.size(), kPeer, 4662, 0));
	ASSERT_EQUALS(1u, factory.calls);
	ASSERT_EQUALS(2u, factory.created[0]->calls);
}

TEST(QuicContext, AmbiguousNonInitialFromSharedEndpointIsRefused)
{
	FakeFactory factory;
	factory.next = std::make_unique<FakeConnection>();
	CQuicContext context(&factory);
	auto first = Initial(0x45);
	ASSERT_TRUE(context.ProcessDatagram(first.data(), first.size(), kPeer, 4662, 0));
	factory.next = std::make_unique<FakeConnection>();
	auto second = Initial(0x46);
	ASSERT_TRUE(context.ProcessDatagram(
		second.data(), second.size(), kPeer, 4662, NatRendezvous::kRequestThrottleMs));
	auto packet = ShortPacket();
	ASSERT_TRUE(!context.ProcessDatagram(packet.data(), packet.size(), kPeer, 4662, 0));
	ASSERT_EQUALS(2u, factory.calls);
	ASSERT_EQUALS(1u, factory.created[0]->calls);
	ASSERT_EQUALS(1u, factory.created[1]->calls);
}

TEST(QuicContext, NonInitialRoutesByCidWhenConnectionsExposeOne)
{
	FakeFactory factory;
	auto first = std::make_unique<FakeConnection>();
	first->issuedCids = { std::string("\x01", 1) };
	factory.next = std::move(first);
	CQuicContext context(&factory);
	auto firstInitial = Initial(0x51);
	ASSERT_TRUE(context.ProcessDatagram(firstInitial.data(), firstInitial.size(), kPeer, 4662, 0));

	auto second = std::make_unique<FakeConnection>();
	second->issuedCids = { std::string("\x02", 1) };
	factory.next = std::move(second);
	auto secondInitial = Initial(0x52);
	ASSERT_TRUE(context.ProcessDatagram(
		secondInitial.data(), secondInitial.size(), kPeer, 4662, NatRendezvous::kRequestThrottleMs));

	// Short header: flags, then the DCID this context must match against each connection's
	// own issued CID -- not "the sole connection at this endpoint", since there are two.
	std::vector<uint8_t> packet = { 0x40, 0x02, 0x99 };
	ASSERT_TRUE(context.ProcessDatagram(packet.data(), packet.size(), kPeer, 4662, 0));
	ASSERT_EQUALS(2u, factory.calls);
	ASSERT_EQUALS(1u, factory.created[0]->calls);
	ASSERT_EQUALS(2u, factory.created[1]->calls);
}

// got3nks' review on #1710 (finding #8, Low): a non-Initial packet is not necessarily a short
// header -- a Handshake packet is a long header too, with its CID at a different offset and an
// explicit length byte (RFC 9000 section 17.2), unlike a short header's. Reading it at the short
// header's offset 1 would read into the version field instead and never match, falling back to
// endpoint routing, which this test's two connections at one endpoint make ambiguous.
TEST(QuicContext, NonInitialLongHeaderRoutesByCidWhenConnectionsExposeOne)
{
	FakeFactory factory;
	auto first = std::make_unique<FakeConnection>();
	first->issuedCids = { std::string("\x01", 1) };
	factory.next = std::move(first);
	CQuicContext context(&factory);
	auto firstInitial = Initial(0x53);
	ASSERT_TRUE(context.ProcessDatagram(firstInitial.data(), firstInitial.size(), kPeer, 4662, 0));

	auto second = std::make_unique<FakeConnection>();
	second->issuedCids = { std::string("\x02", 1) };
	factory.next = std::move(second);
	auto secondInitial = Initial(0x54);
	ASSERT_TRUE(context.ProcessDatagram(
		secondInitial.data(), secondInitial.size(), kPeer, 4662, NatRendezvous::kRequestThrottleMs));

	// Long header: flags (bit 0x80 set), a 4-byte version, a DCID length byte, then the DCID
	// itself -- the offset this fix actually reads from.
	std::vector<uint8_t> packet = { 0xE0, 0x00, 0x00, 0x00, 0x01, 0x01, 0x02 };
	ASSERT_TRUE(context.ProcessDatagram(packet.data(), packet.size(), kPeer, 4662, 0));
	ASSERT_EQUALS(2u, factory.calls);
	ASSERT_EQUALS(1u, factory.created[0]->calls);
	ASSERT_EQUALS(2u, factory.created[1]->calls);
}

TEST(QuicContext, ConnectionFailureRemovesOwnership)
{
	FakeFactory factory;
	factory.next = std::make_unique<FakeConnection>();
	CQuicContext context(&factory);
	auto initial = Initial(0x55);
	ASSERT_TRUE(context.ProcessDatagram(initial.data(), initial.size(), kPeer, 4662, 0));
	factory.created[0]->accepted = false;
	auto packet = ShortPacket();
	ASSERT_TRUE(!context.ProcessDatagram(packet.data(), packet.size(), kPeer, 4662, 0));
	factory.next = std::make_unique<FakeConnection>();
	ASSERT_TRUE(context.ProcessDatagram(
		initial.data(), initial.size(), kPeer, 4662, NatRendezvous::kRequestThrottleMs));
	ASSERT_EQUALS(2u, factory.calls);
}

TEST(QuicContext, TickServicesEveryLiveConnectionAndErasesClosedOnes)
{
	FakeFactory factory;
	factory.next = std::make_unique<FakeConnection>();
	CQuicContext context(&factory);
	auto first = Initial(0x70);
	ASSERT_TRUE(context.ProcessDatagram(first.data(), first.size(), kPeer, 4662, 0));

	factory.next = std::make_unique<FakeConnection>();
	auto second = Initial(0x71);
	ASSERT_TRUE(context.ProcessDatagram(
		second.data(), second.size(), kPeer, 4662, NatRendezvous::kRequestThrottleMs));

	// factory.created[1] does not survive this Tick(): closeOnTick makes it IsClosed(), and
	// CQuicContext::Tick() erases (destroys) closed connections in the same call. Read its tick
	// count through ticksObserved, which outlives it, not through the now-dangling pointer.
	unsigned secondTicks = 0;
	factory.created[1]->closeOnTick = true;
	factory.created[1]->ticksObserved = &secondTicks;
	context.Tick(123);

	ASSERT_EQUALS(1u, factory.created[0]->ticks);
	ASSERT_EQUALS(1u, secondTicks);
	ASSERT_EQUALS(1u, context.ConnectionCount());
}

TEST(QuicContext, DestructionClosesOwnedConnections)
{
	FakeFactory factory;
	factory.next = std::make_unique<FakeConnection>();
	bool closeObserved = false;
	{
		CQuicContext context(&factory);
		auto initial = Initial(0x66);
		ASSERT_TRUE(context.ProcessDatagram(initial.data(), initial.size(), kPeer, 4662, 0));
		factory.created[0]->closeObserved = &closeObserved;
		ASSERT_TRUE(!factory.created[0]->closeCalled);
	}
	ASSERT_TRUE(closeObserved);
}

TEST(QuicContext, UndersizedInitialIsNotAdmitted)
{
	FakeFactory factory;
	factory.alwaysCreate = true;
	CQuicContext context(&factory);
	auto payload = Initial(0x70, CQuicContext::kMinInitialDatagram - 1);
	ASSERT_TRUE(!context.ProcessDatagram(payload.data(), payload.size(), kPeer, 4662, 0));
	ASSERT_EQUALS(0u, factory.calls);
}

TEST(QuicContext, NewConnectionsFromOneScopeAreRateLimited)
{
	FakeFactory factory;
	factory.alwaysCreate = true;
	CQuicContext context(&factory);
	auto first = Initial(0x71);
	auto second = Initial(0x72);
	ASSERT_TRUE(context.ProcessDatagram(first.data(), first.size(), kPeer, 4662, 0));
	ASSERT_TRUE(!context.ProcessDatagram(second.data(), second.size(), kPeer, 4663, 1));
	ASSERT_TRUE(context.ProcessDatagram(
		second.data(), second.size(), kPeer, 4663, NatRendezvous::kRequestThrottleMs));
	ASSERT_EQUALS(2u, factory.calls);
}

TEST(QuicContext, RefusedCreationStillCountsAgainstRateLimit)
{
	FakeFactory factory;
	CQuicContext context(&factory);
	auto payload = Initial(0x73);
	ASSERT_TRUE(!context.ProcessDatagram(payload.data(), payload.size(), kPeer, 4662, 0));
	factory.next = std::make_unique<FakeConnection>();
	ASSERT_TRUE(!context.ProcessDatagram(payload.data(), payload.size(), kPeer, 4662, 1));
	ASSERT_EQUALS(1u, factory.calls);
}

TEST(QuicContext, CapsConnectionsPerScope)
{
	FakeFactory factory;
	factory.alwaysCreate = true;
	CQuicContext context(&factory);
	uint64_t now = 0;
	for (unsigned i = 0; i < CQuicContext::kMaxConnectionsPerScope; ++i) {
		auto payload = Initial(static_cast<uint8_t>(i + 1));
		ASSERT_TRUE(context.ProcessDatagram(
			payload.data(), payload.size(), kPeer, static_cast<uint16_t>(1000 + i), now));
		now += NatRendezvous::kRequestThrottleMs;
	}
	auto extra = Initial(0xfe);
	ASSERT_TRUE(!context.ProcessDatagram(extra.data(), extra.size(), kPeer, 2000, now));
	ASSERT_EQUALS(CQuicContext::kMaxConnectionsPerScope, factory.calls);
}

TEST(QuicContext, CapsTotalConnections)
{
	FakeFactory factory;
	factory.alwaysCreate = true;
	CQuicContext context(&factory);
	auto payload = Initial(0x74);
	for (unsigned i = 0; i < CQuicContext::kMaxConnections; ++i) {
		ASSERT_TRUE(
			context.ProcessDatagram(payload.data(), payload.size(), PeerNumber(i + 1), 4662, 0));
	}
	ASSERT_TRUE(!context.ProcessDatagram(
		payload.data(), payload.size(), PeerNumber(CQuicContext::kMaxConnections + 1), 4662, 0));
	ASSERT_EQUALS(CQuicContext::kMaxConnections, context.ConnectionCount());
}

TEST(QuicContext, ClosedConnectionsReleaseCapacity)
{
	FakeFactory factory;
	factory.alwaysCreate = true;
	CQuicContext context(&factory);
	auto payload = Initial(0x75);
	for (unsigned i = 0; i < CQuicContext::kMaxConnections; ++i) {
		ASSERT_TRUE(
			context.ProcessDatagram(payload.data(), payload.size(), PeerNumber(i + 1), 4662, 0));
	}
	factory.created[0]->closed = true;
	ASSERT_TRUE(context.ProcessDatagram(
		payload.data(), payload.size(), PeerNumber(CQuicContext::kMaxConnections + 1), 4662, 0));
	ASSERT_EQUALS(CQuicContext::kMaxConnections, context.ConnectionCount());
}

TEST(QuicContext, IPv6PeerIsKeyedByItsOwnAddress)
{
	FakeFactory factory;
	factory.alwaysCreate = true;
	CQuicContext context(&factory);
	const CNetworkAddress peer = CNetworkAddress::FromString("2001:db8::1");
	auto initial = Initial(0x76);
	ASSERT_TRUE(context.ProcessDatagram(initial.data(), initial.size(), peer, 4662, 0));
	ASSERT_TRUE(factory.lastAddress == peer);
	auto packet = ShortPacket();
	ASSERT_TRUE(context.ProcessDatagram(packet.data(), packet.size(), peer, 4662, 1));
	ASSERT_TRUE(!context.ProcessDatagram(packet.data(), packet.size(), kPeer, 4662, 1));
	ASSERT_EQUALS(2u, factory.created[0]->calls);
}

TEST(QuicContext, InitialWithServerIssuedDcidRoutesToExistingConnection)
{
	FakeFactory factory;
	factory.alwaysCreate = true;
	CQuicContext context(&factory);
	auto first = Initial(0x77);
	ASSERT_TRUE(context.ProcessDatagram(first.data(), first.size(), kPeer, 4662, 0));
	factory.created[0]->issuedCids.push_back(std::string(1, '\x78'));
	auto second = Initial(0x78);
	ASSERT_TRUE(context.ProcessDatagram(second.data(), second.size(), kPeer, 4662, 1));
	ASSERT_EQUALS(1u, factory.calls);
	ASSERT_EQUALS(2u, factory.created[0]->calls);
	ASSERT_EQUALS(1u, context.ConnectionCount());
	auto packet = ShortPacket();
	ASSERT_TRUE(context.ProcessDatagram(packet.data(), packet.size(), kPeer, 4662, 2));
	ASSERT_EQUALS(3u, factory.created[0]->calls);
}

TEST(QuicContext, InitialWithForeignDcidIsNotRoutedToEndpointConnection)
{
	FakeFactory factory;
	factory.alwaysCreate = true;
	CQuicContext context(&factory);
	auto first = Initial(0x79);
	ASSERT_TRUE(context.ProcessDatagram(first.data(), first.size(), kPeer, 4662, 0));
	factory.created[0]->issuedCids.push_back(std::string(1, '\x7a'));
	auto foreign = Initial(0x7b);
	ASSERT_TRUE(context.ProcessDatagram(
		foreign.data(), foreign.size(), kPeer, 4662, NatRendezvous::kRequestThrottleMs));
	ASSERT_EQUALS(2u, factory.calls);
	ASSERT_EQUALS(1u, factory.created[0]->calls);
}

TEST(QuicContext, IssuedDcidFromAnotherEndpointIsNotRouted)
{
	FakeFactory factory;
	factory.alwaysCreate = true;
	CQuicContext context(&factory);
	auto first = Initial(0x7c);
	ASSERT_TRUE(context.ProcessDatagram(first.data(), first.size(), kPeer, 4662, 0));
	factory.created[0]->issuedCids.push_back(std::string(1, '\x7d'));
	auto second = Initial(0x7d);
	ASSERT_TRUE(context.ProcessDatagram(second.data(), second.size(), PeerNumber(1), 4662, 0));
	ASSERT_EQUALS(2u, factory.calls);
	ASSERT_EQUALS(1u, factory.created[0]->calls);
}
