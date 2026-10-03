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
#include <QuicGnuTlsSession.h>
#include <QuicNattProtocol.h>
#include <QuicNgtcp2Adapter.h>
#include <QuicSocketTransport.h>
#include <QuicStreamAcceptor.h>
#include <NetworkAddress.h>

#include <ngtcp2/ngtcp2.h>

#include <memory>
#include <string>
#include <vector>
using namespace muleunit;
DECLARE_SIMPLE(QuicNgtcp2Adapter)

namespace
{
struct Session : IQuicNgtcp2TlsSession
{
	uint8_t token = 1;
	gnutls_session_t native = reinterpret_cast<gnutls_session_t>(&token);
	bool ConfigureTls13Alpn(
		const IQuicTlsCredentials &, const IQuicTlsVerifier *, const uint8_t *, size_t, bool) override
	{
		return true;
	}
	gnutls_session_t NativeGnuTlsSession() const override { return native; }
};
struct Credentials : IQuicTlsCredentials
{
};
struct Verifier : IQuicTlsVerifier
{
};
struct Sink : IQuicDatagramSink
{
	std::vector<uint8_t> sent;
	bool SendDatagram(const uint8_t *p, size_t n, const CNetworkAddress &, uint16_t) override
	{
		sent.assign(p, p + n);
		return true;
	}
};
struct Engine : IQuicNgtcp2Engine
{
	bool create = true;
	bool read = true;
	bool flush = true;
	unsigned created = 0;
	unsigned destroyed = 0;
	unsigned reads = 0;
	CNetworkAddress lastAddress;
	uint16_t lastPort = 0;
	CQuicInitialMetadata lastMetadata{};
	uint8_t token = 7;
	Handle CreateServer(const CQuicTlsPolicy &,
		const CNetworkAddress &,
		uint16_t,
		const CQuicInitialMetadata &metadata,
		uint64_t) override
	{
		lastMetadata = metadata;
		++created;
		return create ? &token : nullptr;
	}
	bool Read(Handle handle, const uint8_t *, size_t, uint64_t) override
	{
		if (handle == nullptr) {
			return false;
		}
		++reads;
		return read;
	}
	bool Flush(Handle handle,
		IQuicDatagramSink &sink,
		const CNetworkAddress &address,
		uint16_t port,
		uint64_t) override
	{
		if (handle == nullptr) {
			return false;
		}
		lastAddress = address;
		lastPort = port;
		uint8_t p = 9;
		return flush && sink.SendDatagram(&p, 1, address, port);
	}
	std::string issuedCid;
	bool OwnsConnectionId(Handle handle, const std::string &cid) const override
	{
		return handle != nullptr && cid == issuedCid;
	}
	std::string GetIssuedConnectionId(Handle handle) const override
	{
		return handle != nullptr ? issuedCid : std::string();
	}
	std::vector<uint8_t> streamData;
	std::vector<uint8_t> DrainStreamData(Handle handle) override
	{
		if (handle == nullptr) {
			return {};
		}
		std::vector<uint8_t> data;
		data.swap(streamData);
		return data;
	}
	unsigned extendedReadWindowBytes = 0;
	void ExtendStreamReadWindow(Handle handle, size_t bytes) override
	{
		if (handle != nullptr) {
			extendedReadWindowBytes += static_cast<unsigned>(bytes);
		}
	}
	bool tickResult = true;
	unsigned ticks = 0;
	bool Tick(Handle handle, IQuicDatagramSink &, const CNetworkAddress &, uint16_t, uint64_t) override
	{
		if (handle == nullptr) {
			return false;
		}
		++ticks;
		return tickResult;
	}
	uint64_t GetAdvertisedReadWindow(Handle handle) const override { return handle != nullptr ? 1u : 0u; }
	bool hasOpenStream = false;
	bool HasOpenStream(Handle handle) const override { return handle != nullptr && hasOpenStream; }
	CQuicSocketTransport *attachedTransport = nullptr;
	void AttachTransport(Handle handle, CQuicSocketTransport *transport) override
	{
		if (handle != nullptr) {
			attachedTransport = transport;
		}
	}
	//! Lets a test simulate pacing/congestion/flow control deferring a write (WriteStreamData()'s
	//! own 0-byte, not negative, contract -- STREAM_DATA_BLOCKED, pacing, and a flow-control
	//! window are all reported identically): each of the first blockedWriteCount calls accepts
	//! nothing, after which writes succeed in full.
	unsigned blockedWriteCount = 0;
	unsigned writeStreamDataCalls = 0;
	std::ptrdiff_t WriteStreamData(Handle handle,
		const uint8_t *,
		size_t length,
		IQuicDatagramSink &,
		const CNetworkAddress &,
		uint16_t,
		uint64_t) override
	{
		if (handle == nullptr) {
			return -1;
		}
		++writeStreamDataCalls;
		if (blockedWriteCount > 0) {
			--blockedWriteCount;
			return 0;
		}
		return static_cast<std::ptrdiff_t>(length);
	}
	unsigned shutdownStreamCalls = 0;
	void ShutdownStream(Handle handle) override
	{
		if (handle != nullptr) {
			++shutdownStreamCalls;
		}
	}
	unsigned closeStreamGracefullyCalls = 0;
	void CloseStreamGracefully(
		Handle handle, IQuicDatagramSink &, const CNetworkAddress &, uint16_t, uint64_t) override
	{
		if (handle != nullptr) {
			++closeStreamGracefullyCalls;
		}
	}
	bool streamEnded = false;
	bool IsStreamEnded(Handle handle) const override { return handle != nullptr && streamEnded; }
	unsigned endConnectionCalls = 0;
	void EndConnection(
		Handle handle, IQuicDatagramSink &, const CNetworkAddress &, uint16_t, uint64_t) override
	{
		if (handle != nullptr) {
			++endConnectionCalls;
		}
	}
	unsigned notifyWritableCalls = 0;
	void NotifyWritable(Handle handle) override
	{
		if (handle != nullptr) {
			++notifyWritableCalls;
		}
	}
	void Destroy(Handle handle) override
	{
		if (handle != nullptr) {
			++destroyed;
		}
	}
};
const std::string kDcid = "BBBBBBBB";
const CNetworkAddress kPeer = CNetworkAddress::FromString("192.0.2.1");
const std::array<uint8_t, 16> kTestIdentity{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
const std::array<uint8_t, 16> kTestPeerIdentity{
	101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116
};

//! Admits every offered stream, mirroring QuicNgtcp2HandshakeTest.cpp's AcceptingAcceptor:
//! TryExchangeEaqn1Proof()'s retry logic is what this file's own tests need to observe, not
//! admission policy.
struct StubAcceptor : IQuicStreamAcceptor
{
	std::unique_ptr<IStreamTransport> accepted;
	bool AcceptStream(
		std::unique_ptr<IStreamTransport> &transport, const CNetworkAddress &, uint16_t) override
	{
		accepted = std::move(transport);
		return true;
	}
};

std::vector<uint8_t> Initial(const std::string &dcid = kDcid,
	const std::string &scid = "\x99",
	const std::string &token = std::string(),
	size_t datagram = 1200)
{
	std::vector<uint8_t> p = { 0xc0, 0, 0, 0, 1 };
	p.push_back(static_cast<uint8_t>(dcid.size()));
	p.insert(p.end(), dcid.begin(), dcid.end());
	p.push_back(static_cast<uint8_t>(scid.size()));
	p.insert(p.end(), scid.begin(), scid.end());
	p.push_back(static_cast<uint8_t>(token.size()));
	p.insert(p.end(), token.begin(), token.end());
	const size_t remainder = datagram > p.size() + 2 ? datagram - p.size() - 2 : 1;
	p.push_back(static_cast<uint8_t>(0x40 | (remainder >> 8)));
	p.push_back(static_cast<uint8_t>(remainder & 0xff));
	p.resize(p.size() + remainder);
	return p;
}
CQuicTlsPolicy Policy(Session &, Credentials &c, Verifier &)
{
	return { &c };
}

// CProductionNgtcp2Engine::CreateServer() builds its own real GnuTLS session per connection
// from policy.credentials, so the production-engine tests need real credentials there.
CQuicTlsPolicy ProductionPolicy(Session &, CQuicEphemeralCredentials &c, Verifier &)
{
	return { &c };
}
} // namespace

TEST(QuicNgtcp2Adapter, MissingDependenciesFailClosed)
{
	Session s;
	Credentials c;
	Verifier v;
	auto sink = std::make_shared<Sink>();
	auto engine = std::make_shared<Engine>();
	auto p = Initial();
	CQuicTlsPolicy policy = Policy(s, c, v);

	CQuicTlsPolicy noCredentials = policy;
	noCredentials.credentials = nullptr;
	ASSERT_TRUE(!CQuicNgtcp2Factory(noCredentials, sink, engine, kTestIdentity)
			     .CreateInbound(p.data(), p.size(), kPeer, 2, kDcid, 0));
	ASSERT_EQUALS(0u, engine->created);

	ASSERT_TRUE(!CQuicNgtcp2Factory(policy, nullptr, engine, kTestIdentity)
			     .CreateInbound(p.data(), p.size(), kPeer, 2, kDcid, 0));
	ASSERT_TRUE(!CQuicNgtcp2Factory(policy, sink, nullptr, kTestIdentity)
			     .CreateInbound(p.data(), p.size(), kPeer, 2, kDcid, 0));
	ASSERT_EQUALS(0u, engine->created);
}

TEST(QuicNgtcp2Adapter, InitialMetadataIsCaptured)
{
	Session s;
	Credentials c;
	Verifier v;
	auto sink = std::make_shared<Sink>();
	auto engine = std::make_shared<Engine>();
	auto p = Initial();
	auto connection = CQuicNgtcp2Factory(Policy(s, c, v), sink, engine, kTestIdentity)
				  .CreateInbound(p.data(), p.size(), kPeer, 2, kDcid, 0);
	ASSERT_TRUE(connection != nullptr);
	ASSERT_EQUALS(1u, engine->lastMetadata.version);
	ASSERT_EQUALS(kDcid, engine->lastMetadata.destinationCid);
	ASSERT_EQUALS(std::string("\x99", 1), engine->lastMetadata.sourceCid);
}

TEST(QuicNgtcp2Adapter, InitialAcceptanceFollowsRfc9000)
{
	Session s;
	Credentials c;
	Verifier v;
	auto sink = std::make_shared<Sink>();
	auto engine = std::make_shared<Engine>();
	CQuicNgtcp2Factory factory(Policy(s, c, v), sink, engine, kTestIdentity);
	auto accepts = [&](const std::vector<uint8_t> &p, const std::string &dcid) {
		return factory.CreateInbound(p.data(), p.size(), kPeer, 2, dcid, 0) != nullptr;
	};

	ASSERT_TRUE(!accepts(Initial(kDcid, "\x99", "", 1199), kDcid));
	ASSERT_TRUE(accepts(Initial(kDcid, "\x99", "", 1200), kDcid));

	auto p = Initial();
	p[0] = 0x40;
	ASSERT_TRUE(!accepts(p, kDcid));
	p = Initial();
	p[0] = 0x80;
	ASSERT_TRUE(!accepts(p, kDcid));
	p = Initial();
	p[0] = 0xd0;
	ASSERT_TRUE(!accepts(p, kDcid));
	p = Initial();
	p[0] = 0xe0;
	ASSERT_TRUE(!accepts(p, kDcid));
	p = Initial();
	p[4] = 2;
	ASSERT_TRUE(!accepts(p, kDcid));

	ASSERT_TRUE(!accepts(Initial("DDDDDDD"), "DDDDDDD"));
	ASSERT_TRUE(!accepts(Initial(""), ""));
	ASSERT_TRUE(accepts(Initial("DDDDDDD", "\x99", "retry-token"), "DDDDDDD"));
	ASSERT_TRUE(accepts(Initial(kDcid, ""), kDcid));
	ASSERT_EQUALS(std::string(), engine->lastMetadata.sourceCid);

	ASSERT_TRUE(!accepts(Initial(std::string(21, 'd')), std::string(21, 'd')));
	ASSERT_TRUE(!accepts(Initial(kDcid, std::string(21, 's')), kDcid));
	ASSERT_TRUE(accepts(Initial(std::string(20, 'd'), std::string(20, 's')), std::string(20, 'd')));
	ASSERT_EQUALS(4u, engine->created);
}

TEST(QuicNgtcp2Adapter, MalformedInitialAndEngineCreationFailureFailClosed)
{
	Session s;
	Credentials c;
	Verifier v;
	auto sink = std::make_shared<Sink>();
	auto engine = std::make_shared<Engine>();
	auto p = Initial();
	CQuicNgtcp2Factory factory(Policy(s, c, v), sink, engine, kTestIdentity);

	p[4] = 2;
	ASSERT_TRUE(!factory.CreateInbound(p.data(), p.size(), kPeer, 2, kDcid, 0));
	p = Initial();
	ASSERT_TRUE(!factory.CreateInbound(p.data(), p.size(), kPeer, 2, "", 0));
	ASSERT_TRUE(!factory.CreateInbound(p.data(), p.size(), kPeer, 2, "CCCCCCCC", 0));
	ASSERT_TRUE(!factory.CreateInbound(p.data(), p.size(), kPeer, 2, std::string(21, 'x'), 0));
	ASSERT_TRUE(!factory.CreateInbound(nullptr, p.size(), kPeer, 2, kDcid, 0));
	ASSERT_EQUALS(0u, engine->created);

	engine->create = false;
	ASSERT_TRUE(!factory.CreateInbound(p.data(), p.size(), kPeer, 2, kDcid, 0));
	ASSERT_EQUALS(1u, engine->created);
}

TEST(QuicNgtcp2Adapter, OwnsEngineAndFlushesOutput)
{
	Session s;
	Credentials c;
	Verifier v;
	auto sink = std::make_shared<Sink>();
	auto engine = std::make_shared<Engine>();
	auto p = Initial();
	CQuicNgtcp2Factory factory(Policy(s, c, v), sink, engine, kTestIdentity);
	{
		auto connection = factory.CreateInbound(p.data(), p.size(), kPeer, 2, kDcid, 0);
		ASSERT_TRUE(connection != nullptr);
		ASSERT_TRUE(connection->ProcessDatagram(p.data(), p.size(), 0));
		ASSERT_EQUALS(1u, engine->reads);
		ASSERT_EQUALS(1u, sink->sent.size());
		ASSERT_EQUALS(9u, sink->sent[0]);
		ASSERT_TRUE(engine->lastAddress == kPeer);
		ASSERT_EQUALS(2u, engine->lastPort);
		connection->Close();
		connection->Close();
		ASSERT_EQUALS(1u, engine->destroyed);
		ASSERT_TRUE(!connection->ProcessDatagram(p.data(), p.size(), 0));
	}
	ASSERT_EQUALS(1u, engine->destroyed);
}

TEST(QuicNgtcp2Adapter, ReadFailureCloses)
{
	Session s;
	Credentials c;
	Verifier v;
	auto sink = std::make_shared<Sink>();
	auto engine = std::make_shared<Engine>();
	engine->read = false;
	auto p = Initial();
	CQuicNgtcp2Factory factory(Policy(s, c, v), sink, engine, kTestIdentity);
	auto connection = factory.CreateInbound(p.data(), p.size(), kPeer, 2, kDcid, 0);
	ASSERT_TRUE(!connection->ProcessDatagram(p.data(), p.size(), 0));
	ASSERT_TRUE(connection->IsClosed());
	ASSERT_EQUALS(1u, engine->destroyed);
	ASSERT_TRUE(!connection->ProcessDatagram(p.data(), p.size(), 0));
	ASSERT_EQUALS(1u, engine->destroyed);
}

TEST(QuicNgtcp2Adapter, FlushFailureCloses)
{
	Session s;
	Credentials c;
	Verifier v;
	auto sink = std::make_shared<Sink>();
	auto engine = std::make_shared<Engine>();
	engine->flush = false;
	auto p = Initial();
	CQuicNgtcp2Factory factory(Policy(s, c, v), sink, engine, kTestIdentity);
	auto connection = factory.CreateInbound(p.data(), p.size(), kPeer, 2, kDcid, 0);
	ASSERT_TRUE(!connection->ProcessDatagram(p.data(), p.size(), 0));
	ASSERT_TRUE(connection->IsClosed());
	ASSERT_EQUALS(1u, engine->destroyed);
	ASSERT_TRUE(sink->sent.empty());
}

TEST(QuicNgtcp2Adapter, ProductionEngineCreatesConnection)
{
	Session s;
	CQuicEphemeralCredentials c;
	Verifier v;
	auto sink = std::make_shared<Sink>();
	auto engine = CreateProductionQuicNgtcp2Engine();
	auto p = Initial();
	CQuicNgtcp2Factory factory(ProductionPolicy(s, c, v), sink, engine, kTestIdentity);
	auto connection = factory.CreateInbound(p.data(), p.size(), kPeer, 2, kDcid, 0);
	ASSERT_TRUE(connection != nullptr);
}

TEST(QuicNgtcp2Adapter, ProductionEngineOwnsIssuedScid)
{
	Session s;
	CQuicEphemeralCredentials c;
	Verifier v;
	auto sink = std::make_shared<Sink>();
	auto engine = CreateProductionQuicNgtcp2Engine();
	auto p = Initial();
	CQuicNgtcp2Factory factory(ProductionPolicy(s, c, v), sink, engine, kTestIdentity);
	auto connection = factory.CreateInbound(p.data(), p.size(), kPeer, 2, kDcid, 0);
	ASSERT_TRUE(connection != nullptr);
	const std::string issuedCid = connection->GetIssuedConnectionId();
	ASSERT_TRUE(!issuedCid.empty());
	ASSERT_TRUE(connection->OwnsConnectionId(issuedCid));
}

TEST(QuicNgtcp2Adapter, ProductionEngineOwnsNoConnectionIdAfterClose)
{
	Session s;
	CQuicEphemeralCredentials c;
	Verifier v;
	auto sink = std::make_shared<Sink>();
	auto engine = CreateProductionQuicNgtcp2Engine();
	auto p = Initial();
	CQuicNgtcp2Factory factory(ProductionPolicy(s, c, v), sink, engine, kTestIdentity);
	auto connection = factory.CreateInbound(p.data(), p.size(), kPeer, 2, kDcid, 0);
	ASSERT_TRUE(connection != nullptr);
	const std::string issuedCid = connection->GetIssuedConnectionId();
	connection->Close();
	ASSERT_TRUE(!connection->OwnsConnectionId(issuedCid));
}

TEST(QuicNgtcp2Adapter, ProductionEngineTickServicesRealExpiryTimersWithoutCrashing)
{
	Session s;
	CQuicEphemeralCredentials c;
	Verifier v;
	auto sink = std::make_shared<Sink>();
	auto engine = CreateProductionQuicNgtcp2Engine();
	auto p = Initial();
	CQuicNgtcp2Factory factory(ProductionPolicy(s, c, v), sink, engine, kTestIdentity);
	auto connection = factory.CreateInbound(p.data(), p.size(), kPeer, 2, kDcid, 0);
	ASSERT_TRUE(connection != nullptr);

	// Nothing is due immediately after creation; ngtcp2's own PTO/loss-detection timer should
	// still be armed well before an incomplete handshake would ever be abandoned.
	connection->Tick(0);
	ASSERT_TRUE(!connection->IsClosed());
}

// got3nks' review on #1710 (finding #3, High): without a handshake timeout, handle_expiry()
// never ends a connection stuck at Initial, so spoofed Initials could occupy admission slots
// permanently. Proves CreateServer() actually configures one (QuicNgtcp2Adapter.cpp's
// kHandshakeTimeoutMs), not just that Tick() can be called safely.
TEST(QuicNgtcp2Adapter, ProductionEngineTimesOutAHandshakeThatNeverCompletes)
{
	Session s;
	CQuicEphemeralCredentials c;
	Verifier v;
	auto sink = std::make_shared<Sink>();
	auto engine = CreateProductionQuicNgtcp2Engine();
	auto p = Initial();
	CQuicNgtcp2Factory factory(ProductionPolicy(s, c, v), sink, engine, kTestIdentity);
	auto connection = factory.CreateInbound(p.data(), p.size(), kPeer, 2, kDcid, 0);
	ASSERT_TRUE(connection != nullptr);

	// Still well within the timeout: the handshake stalling this long is not itself the bug
	// under test, only that the slot it occupies is not held forever.
	connection->Tick(5000);
	ASSERT_TRUE(!connection->IsClosed());

	connection->Tick(11000);
	ASSERT_TRUE(connection->IsClosed());
}

TEST(QuicNgtcp2Adapter, ProductionEngineFailsClosedWithoutCredentials)
{
	CQuicInitialMetadata metadata;
	metadata.version = NGTCP2_PROTO_VER_V1;
	metadata.destinationCid = kDcid;
	metadata.sourceCid = std::string("\x99", 1);
	auto engine = CreateProductionQuicNgtcp2Engine();
	CQuicTlsPolicy policy{};
	auto handle = engine->CreateServer(policy, kPeer, 2, metadata, 0);
	ASSERT_TRUE(handle == nullptr);
}

TEST(QuicNgtcp2Adapter, ProductionEngineFailsClosedWithNonGnuTlsCredentials)
{
	Session s;
	Credentials c; // not GnuTLS-backed
	Verifier v;
	auto sink = std::make_shared<Sink>();
	auto engine = CreateProductionQuicNgtcp2Engine();
	auto p = Initial();
	CQuicNgtcp2Factory factory(Policy(s, c, v), sink, engine, kTestIdentity);
	auto connection = factory.CreateInbound(p.data(), p.size(), kPeer, 2, kDcid, 0);
	ASSERT_TRUE(connection == nullptr);
}

TEST(QuicNgtcp2Adapter, ProductionEngineAdvertisesTheSocketTransportReadWindow)
{
	CQuicInitialMetadata metadata;
	metadata.version = NGTCP2_PROTO_VER_V1;
	metadata.destinationCid = kDcid;
	metadata.sourceCid = std::string("\x99", 1);
	auto engine = CreateProductionQuicNgtcp2Engine();
	CQuicEphemeralCredentials credentials;
	CQuicTlsPolicy policy{};
	policy.credentials = &credentials;
	auto handle = engine->CreateServer(policy, kPeer, 2, metadata, 0);
	ASSERT_TRUE(handle != nullptr);
	ASSERT_EQUALS(static_cast<uint64_t>(CQuicSocketTransport::kReadWindow),
		engine->GetAdvertisedReadWindow(handle));
	engine->Destroy(handle);
}

TEST(QuicNgtcp2Adapter, ProductionEngineReadRejectsAnUnencryptedInitial)
{
	Session s;
	CQuicEphemeralCredentials c;
	Verifier v;
	auto sink = std::make_shared<Sink>();
	auto engine = CreateProductionQuicNgtcp2Engine();
	auto p = Initial();
	CQuicNgtcp2Factory factory(ProductionPolicy(s, c, v), sink, engine, kTestIdentity);
	auto connection = factory.CreateInbound(p.data(), p.size(), kPeer, 2, kDcid, 0);
	ASSERT_TRUE(connection != nullptr);
	// Initial() is structurally valid enough for ngtcp2_accept()'s admission check, but carries
	// no real CRYPTO frame or AEAD protection -- a genuine handshake needs an actual TLS 1.3
	// ClientHello, which this codebase has no client implementation to produce. The real engine
	// must reject it through ngtcp2_conn_read_pkt() rather than crash; this is what ASan
	// verifies, not just the boolean result.
	ASSERT_TRUE(!connection->ProcessDatagram(p.data(), p.size(), 0));
}

TEST(QuicNgtcp2Adapter, ProductionEngineIssuesDistinctScidsPerConnection)
{
	Session s;
	CQuicEphemeralCredentials c;
	Verifier v;
	auto sink = std::make_shared<Sink>();
	auto engine = CreateProductionQuicNgtcp2Engine();
	CQuicNgtcp2Factory factory(ProductionPolicy(s, c, v), sink, engine, kTestIdentity);

	auto p1 = Initial(kDcid, "\x01");
	auto connection1 = factory.CreateInbound(p1.data(), p1.size(), kPeer, 2, kDcid, 0);
	ASSERT_TRUE(connection1 != nullptr);

	const std::string kOtherDcid = "CCCCCCCC";
	auto p2 = Initial(kOtherDcid, "\x02");
	auto connection2 = factory.CreateInbound(p2.data(), p2.size(), kPeer, 2, kOtherDcid, 0);
	ASSERT_TRUE(connection2 != nullptr);

	const std::string cid1 = connection1->GetIssuedConnectionId();
	const std::string cid2 = connection2->GetIssuedConnectionId();
	ASSERT_TRUE(!cid1.empty());
	ASSERT_TRUE(!cid2.empty());
	ASSERT_TRUE(cid1 != cid2);
	ASSERT_TRUE(!connection1->OwnsConnectionId(cid2));
	ASSERT_TRUE(!connection2->OwnsConnectionId(cid1));
}

TEST(QuicNgtcp2Adapter, IssuedConnectionIdsComeFromEngineUntilClosed)
{
	Session s;
	Credentials c;
	Verifier v;
	auto sink = std::make_shared<Sink>();
	auto engine = std::make_shared<Engine>();
	engine->issuedCid = "SSSSSSSS";
	auto p = Initial();
	CQuicNgtcp2Factory factory(Policy(s, c, v), sink, engine, kTestIdentity);
	auto connection = factory.CreateInbound(p.data(), p.size(), kPeer, 2, kDcid, 0);
	ASSERT_TRUE(connection != nullptr);
	ASSERT_TRUE(connection->OwnsConnectionId("SSSSSSSS"));
	ASSERT_TRUE(!connection->OwnsConnectionId(kDcid));
	connection->Close();
	ASSERT_TRUE(!connection->OwnsConnectionId("SSSSSSSS"));
}

// A deferred (0-byte) write of our EAQN1 proof, e.g. gated by ngtcp2 pacing at a realistic RTT,
// must be retried rather than close the connection, and the hand-off completes once it goes out.
TEST(QuicNgtcp2Adapter, ZeroByteProofWriteIsRetriedNotClosed)
{
	Session s;
	Credentials c;
	Verifier v;
	auto sink = std::make_shared<Sink>();
	auto engine = std::make_shared<Engine>();
	auto p = Initial();
	CQuicNgtcp2Factory factory(Policy(s, c, v), sink, engine, kTestIdentity);
	StubAcceptor acceptor;
	factory.SetAcceptor(&acceptor);

	auto connection = factory.CreateInbound(p.data(), p.size(), kPeer, 2, kDcid, 0);
	ASSERT_TRUE(connection != nullptr);

	// nullptr target: this connection has no prior rendezvous context, same as every real
	// inbound connection (QuicNgtcp2Adapter.cpp's TryExchangeEaqn1Proof() comment).
	const auto peerProof = QuicNatt::BuildEaqn1Proof(kTestPeerIdentity, nullptr);
	engine->streamData.assign(peerProof.begin(), peerProof.end());
	engine->hasOpenStream = true;
	// The first two attempts to write our own proof are deferred (pacing/congestion/flow
	// control -- indistinguishable by this contract); the third succeeds.
	engine->blockedWriteCount = 2;

	ASSERT_TRUE(connection->ProcessDatagram(p.data(), p.size(), 0));
	ASSERT_TRUE(!connection->IsClosed());
	ASSERT_TRUE(acceptor.accepted == nullptr);
	ASSERT_EQUALS(1u, engine->writeStreamDataCalls);

	connection->Tick(1);
	ASSERT_TRUE(!connection->IsClosed());
	ASSERT_TRUE(acceptor.accepted == nullptr);
	ASSERT_EQUALS(2u, engine->writeStreamDataCalls);

	// Third attempt: blockedWriteCount has run out, the write finally goes through in full, and
	// the stream -- already validated on the peer's side two ticks ago -- is offered right away.
	connection->Tick(2);
	ASSERT_TRUE(!connection->IsClosed());
	ASSERT_TRUE(acceptor.accepted != nullptr);
	ASSERT_EQUALS(3u, engine->writeStreamDataCalls);

	// QuicSocketTransport.h's own documented contract: the caller must detach the transport
	// (AttachTransport(handle, nullptr)) before destroying it, not after the connection whose
	// handle it holds is already gone -- CQuicSocketTransport::Close() reaches back into
	// m_operations (this connection) to send a graceful FIN, which would otherwise run on a
	// dangling reference once connection's own destructor has already run.
	acceptor.accepted.reset();
}
