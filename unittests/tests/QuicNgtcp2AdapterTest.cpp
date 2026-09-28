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
#include <QuicNgtcp2Adapter.h>
#include <NetworkAddress.h>

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
		const CQuicInitialMetadata &metadata) override
	{
		lastMetadata = metadata;
		++created;
		return create ? &token : nullptr;
	}
	bool Read(Handle handle, const uint8_t *, size_t) override
	{
		if (handle == nullptr) {
			return false;
		}
		++reads;
		return read;
	}
	bool
	Flush(Handle handle, IQuicDatagramSink &sink, const CNetworkAddress &address, uint16_t port) override
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
	void Destroy(Handle handle) override
	{
		if (handle != nullptr) {
			++destroyed;
		}
	}
};
const std::string kDcid = "BBBBBBBB";
const CNetworkAddress kPeer = CNetworkAddress::FromString("192.0.2.1");

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
CQuicTlsPolicy Policy(Session &s, Credentials &c, Verifier &v)
{
	return { &s, &c, &v, &s };
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

	CQuicTlsPolicy noSession = policy;
	noSession.session = nullptr;
	ASSERT_TRUE(!CQuicNgtcp2Factory(noSession, sink, engine)
			     .CreateInbound(p.data(), p.size(), kPeer, 2, kDcid));

	CQuicTlsPolicy noCredentials = policy;
	noCredentials.credentials = nullptr;
	ASSERT_TRUE(!CQuicNgtcp2Factory(noCredentials, sink, engine)
			     .CreateInbound(p.data(), p.size(), kPeer, 2, kDcid));

	CQuicTlsPolicy noVerifier = policy;
	noVerifier.verifier = nullptr;
	ASSERT_TRUE(!CQuicNgtcp2Factory(noVerifier, sink, engine)
			     .CreateInbound(p.data(), p.size(), kPeer, 2, kDcid));

	CQuicTlsPolicy noNativeSession = policy;
	noNativeSession.ngtcp2Session = nullptr;
	ASSERT_TRUE(!CQuicNgtcp2Factory(noNativeSession, sink, engine)
			     .CreateInbound(p.data(), p.size(), kPeer, 2, kDcid));

	s.native = nullptr;
	ASSERT_TRUE(
		!CQuicNgtcp2Factory(policy, sink, engine).CreateInbound(p.data(), p.size(), kPeer, 2, kDcid));
	ASSERT_EQUALS(0u, engine->created);
	s.native = reinterpret_cast<gnutls_session_t>(&s.token);

	ASSERT_TRUE(!CQuicNgtcp2Factory(policy, nullptr, engine)
			     .CreateInbound(p.data(), p.size(), kPeer, 2, kDcid));
	ASSERT_TRUE(!CQuicNgtcp2Factory(policy, sink, nullptr)
			     .CreateInbound(p.data(), p.size(), kPeer, 2, kDcid));
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
	auto connection = CQuicNgtcp2Factory(Policy(s, c, v), sink, engine)
				  .CreateInbound(p.data(), p.size(), kPeer, 2, kDcid);
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
	CQuicNgtcp2Factory factory(Policy(s, c, v), sink, engine);
	auto accepts = [&](const std::vector<uint8_t> &p, const std::string &dcid) {
		return factory.CreateInbound(p.data(), p.size(), kPeer, 2, dcid) != nullptr;
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
	CQuicNgtcp2Factory factory(Policy(s, c, v), sink, engine);

	p[4] = 2;
	ASSERT_TRUE(!factory.CreateInbound(p.data(), p.size(), kPeer, 2, kDcid));
	p = Initial();
	ASSERT_TRUE(!factory.CreateInbound(p.data(), p.size(), kPeer, 2, ""));
	ASSERT_TRUE(!factory.CreateInbound(p.data(), p.size(), kPeer, 2, "CCCCCCCC"));
	ASSERT_TRUE(!factory.CreateInbound(p.data(), p.size(), kPeer, 2, std::string(21, 'x')));
	ASSERT_TRUE(!factory.CreateInbound(nullptr, p.size(), kPeer, 2, kDcid));
	ASSERT_EQUALS(0u, engine->created);

	engine->create = false;
	ASSERT_TRUE(!factory.CreateInbound(p.data(), p.size(), kPeer, 2, kDcid));
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
	CQuicNgtcp2Factory factory(Policy(s, c, v), sink, engine);
	{
		auto connection = factory.CreateInbound(p.data(), p.size(), kPeer, 2, kDcid);
		ASSERT_TRUE(connection != nullptr);
		ASSERT_TRUE(connection->ProcessDatagram(p.data(), p.size()));
		ASSERT_EQUALS(1u, engine->reads);
		ASSERT_EQUALS(1u, sink->sent.size());
		ASSERT_EQUALS(9u, sink->sent[0]);
		ASSERT_TRUE(engine->lastAddress == kPeer);
		ASSERT_EQUALS(2u, engine->lastPort);
		connection->Close();
		connection->Close();
		ASSERT_EQUALS(1u, engine->destroyed);
		ASSERT_TRUE(!connection->ProcessDatagram(p.data(), p.size()));
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
	CQuicNgtcp2Factory factory(Policy(s, c, v), sink, engine);
	auto connection = factory.CreateInbound(p.data(), p.size(), kPeer, 2, kDcid);
	ASSERT_TRUE(!connection->ProcessDatagram(p.data(), p.size()));
	ASSERT_TRUE(connection->IsClosed());
	ASSERT_EQUALS(1u, engine->destroyed);
	ASSERT_TRUE(!connection->ProcessDatagram(p.data(), p.size()));
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
	CQuicNgtcp2Factory factory(Policy(s, c, v), sink, engine);
	auto connection = factory.CreateInbound(p.data(), p.size(), kPeer, 2, kDcid);
	ASSERT_TRUE(!connection->ProcessDatagram(p.data(), p.size()));
	ASSERT_TRUE(connection->IsClosed());
	ASSERT_EQUALS(1u, engine->destroyed);
	ASSERT_TRUE(sink->sent.empty());
}

TEST(QuicNgtcp2Adapter, ProductionEngineFailsClosed)
{
	Session s;
	Credentials c;
	Verifier v;
	auto sink = std::make_shared<Sink>();
	auto engine = CreateProductionQuicNgtcp2Engine();
	auto p = Initial();
	CQuicNgtcp2Factory factory(Policy(s, c, v), sink, engine);
	ASSERT_TRUE(!factory.CreateInbound(p.data(), p.size(), kPeer, 2, kDcid));
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
	CQuicNgtcp2Factory factory(Policy(s, c, v), sink, engine);
	auto connection = factory.CreateInbound(p.data(), p.size(), kPeer, 2, kDcid);
	ASSERT_TRUE(connection != nullptr);
	ASSERT_TRUE(connection->OwnsConnectionId("SSSSSSSS"));
	ASSERT_TRUE(!connection->OwnsConnectionId(kDcid));
	connection->Close();
	ASSERT_TRUE(!connection->OwnsConnectionId("SSSSSSSS"));
}
