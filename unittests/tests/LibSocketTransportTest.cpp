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

#include <muleunit/test.h>

#include <libs/common/Format.h>
#include <LibSocket.h>
#include <amuleIPV4Address.h>
#include <StreamTransport.h>

#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#ifdef ENABLE_IPV6
#include <AddressFamilyPolicy.h>

#include <optional>

#include "WarningsPush_Asio.h"
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include "WarningsPop.h"
#endif

using namespace muleunit;

DECLARE_SIMPLE(LibSocketTransport)

// Link seams: CLibSocket's asio side reaches the notification layer and the
// address helpers, which pull in the application. Nothing here drives an asio
// callback, so these are never called.
namespace MuleNotify
{
class CMuleNotiferBase;
void HandleNotificationAlways(const CMuleNotiferBase &);
void HandleNotificationAlways(const CMuleNotiferBase &) {}
} // namespace MuleNotify

bool StringIPtoUint32(const wxString &, uint32 &);
bool StringIPtoUint32(const wxString &, uint32 &)
{
	return false;
}

namespace
{
// Answers an asio socket would never give: a fresh CLibSocket is not connected,
// not ok, and has no peer, so an assertion can only pass through the transport.
class CFakeTransport : public IStreamTransport
{
public:
	explicit CFakeTransport(CNetworkAddress peer = CNetworkAddress::FromString("192.0.2.7"))
	: peer(std::move(peer))
	{
	}

	bool IsConnected() const override { return true; }
	bool IsInbound() const override { return false; }
	void SetEvents(IStreamTransportEvents *) override {}
	bool ObfuscatesStream() const override { return false; }
	bool IsOk() const override { return ok; }
	bool BlocksRead() const override { return true; }
	bool BlocksWrite() const override { return true; }
	int LastError() const override { return 0x7501; }
	CNetworkAddress GetPeerAddress() const override { return peer; }
	uint16_t GetPeerPort() const override { return 4662; }

	uint32_t Read(void *buffer, uint32_t length) override
	{
		const uint32_t taken = length < 4 ? length : 4;
		std::memcpy(buffer, "utp!", taken);
		return taken;
	}

	uint32_t Write(const void *buffer, uint32_t length) override
	{
		const auto *in = static_cast<const uint8_t *>(buffer);
		written.insert(written.end(), in, in + length);
		return length;
	}

	void Close() override { ++closeCalls; }
	void Flush() override { ++flushCalls; }

	bool ok = true;
	std::vector<uint8_t> written;
	int closeCalls = 0;
	int flushCalls = 0;
	CNetworkAddress peer;
};

//! Attaches a fake and hands back a borrowed pointer; the socket owns it.
CFakeTransport *Attach(CLibSocket &socket, CNetworkAddress peer = CNetworkAddress::FromString("192.0.2.7"))
{
	auto owned = std::make_unique<CFakeTransport>(std::move(peer));
	CFakeTransport *borrowed = owned.get();
	socket.AttachTransport(std::move(owned));
	return borrowed;
}
} // namespace

// One table because the value is that the list is exhaustive: none of these are
// virtual, so one left unrouted resolves statically to the asio socket -- which
// is how CEMSocket::Send()'s !IsOk() arm would stay dead after wiring.
TEST(LibSocketTransport, EveryStreamAccessorAnswersFromTheTransport)
{
	CLibSocket socket;
	Attach(socket);

	const struct
	{
		const char *label;
		bool fromTransport;
		bool fromSocket;
	} cases[] = {
		{ "IsConnected", socket.IsConnected(), false },
		{ "IsOk", socket.IsOk(), false },
		{ "BlocksRead", socket.BlocksRead(), false },
		{ "BlocksWrite", socket.BlocksWrite(), false },
	};
	for (const auto &row : cases) {
		CFormat format("%s answered from the socket, not the transport");
		const wxString message = format % row.label;
		ASSERT_TRUE_M(row.fromTransport != row.fromSocket, message);
	}

	// Opaque by contract; what matters is whose value it is.
	ASSERT_EQUALS(0x7501, socket.LastError());
}

TEST(LibSocketTransport, ReadAndWriteReachTheTransport)
{
	CLibSocket socket;
	CFakeTransport *fake = Attach(socket);

	char out[8] = { 0 };
	ASSERT_EQUALS(4u, socket.Read(out, sizeof(out)));
	ASSERT_TRUE(std::string(out, 4) == "utp!");

	const char payload[] = "abc";
	ASSERT_EQUALS(3u, socket.Write(payload, 3));
	ASSERT_EQUALS(3u, (unsigned)fake->written.size());
}

TEST(LibSocketTransport, ThePeerIsTheTransportsPeer)
{
	CLibSocket socket;
	Attach(socket);

	ASSERT_TRUE(socket.GetPeer() == wxString("192.0.2.7"));
	ASSERT_TRUE(socket.GetPeerAddress() == CNetworkAddress::FromString("192.0.2.7"));
	// Narrowed at this accessor only, because its type is the ed2k wire form.
	ASSERT_TRUE(socket.GetPeerInt() != 0);
	ASSERT_TRUE(wxString(socket.GetIP()) == wxString("192.0.2.7"));
}

TEST(LibSocketTransport, NativePeerAddressSurvivesTheTransportFacade)
{
	const CNetworkAddress peer = CNetworkAddress::IPv6FromOctets(
		{ 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 });
	CLibSocket socket;
	Attach(socket, peer);

	ASSERT_TRUE(socket.GetPeerAddress() == peer);
	ASSERT_EQUALS(0u, socket.GetPeerInt());
}

TEST(LibSocketTransport, DiallingIsRefusedWhileATransportIsAttached)
{
	// Pins intent and does NOT discriminate: an unconnected asio socket refuses
	// this address too. Discriminating needs a connectable peer.
	CLibSocket socket;
	Attach(socket);

	amuleIPV4Address address;
	address.Hostname(wxString("192.0.2.9"));
	address.Service(4662);
	ASSERT_FALSE(socket.Connect(address, false));
}

TEST(LibSocketTransport, ClosingGoesToTheTransportAndIsIdempotent)
{
	CLibSocket socket;
	CFakeTransport *fake = Attach(socket);

	socket.Close();
	socket.Close();
	// Close-once lives in the transport; what matters is the calls arrive there.
	ASSERT_EQUALS(2, fake->closeCalls);
}

TEST(LibSocketTransport, AnUnattachedSocketStillAnswersForItself)
{
	// A socket with no transport behaves as it did before the facade existed.
	CLibSocket socket;
	ASSERT_FALSE(socket.HasTransport());
	ASSERT_FALSE(socket.IsConnected());
	ASSERT_FALSE(socket.IsOk());
}

#ifdef ENABLE_IPV6
namespace
{
class CScopedFamilies
{
public:
	explicit CScopedFamilies(AddressFamilyPolicy::Families families)
	: m_previous(AddressFamilyPolicy::Configured())
	{
		AddressFamilyPolicy::SetConfigured(families);
	}
	~CScopedFamilies() { AddressFamilyPolicy::SetConfigured(m_previous); }
	CScopedFamilies(const CScopedFamilies &) = delete;
	CScopedFamilies &operator=(const CScopedFamilies &) = delete;

private:
	AddressFamilyPolicy::Families m_previous;
};

// The backlog tells whether a connect was attempted: a blocking loopback connect has completed
// the handshake by the time it returns, so a refused attempt leaves nothing to accept.
class CLoopbackListener
{
public:
	explicit CLoopbackListener(const boost::asio::ip::address &address)
	: m_acceptor(m_io)
	{
		const boost::asio::ip::tcp::endpoint endpoint(address, 0);
		boost::system::error_code ec;
		m_acceptor.open(endpoint.protocol(), ec);
		if (!ec) {
			m_acceptor.bind(endpoint, ec);
		}
		if (!ec) {
			m_acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
		}
		if (!ec) {
			m_acceptor.non_blocking(true, ec);
		}
		m_ok = !ec;
	}

	bool IsOk() const { return m_ok; }
	uint16 Port() const { return m_acceptor.local_endpoint().port(); }

	std::optional<boost::asio::ip::address> AcceptPending()
	{
		boost::asio::ip::tcp::socket peer(m_io);
		boost::system::error_code ec;
		m_acceptor.accept(peer, ec);
		if (ec) {
			return std::nullopt;
		}
		const boost::asio::ip::tcp::endpoint remote = peer.remote_endpoint(ec);
		if (ec) {
			return std::nullopt;
		}
		return remote.address();
	}

private:
	boost::asio::io_context m_io;
	boost::asio::ip::tcp::acceptor m_acceptor;
	bool m_ok = false;
};

CNetworkAddress LoopbackIPv6()
{
	return CNetworkAddress::IPv6FromOctets({ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 });
}

boost::asio::ip::address AsioLoopbackIPv6()
{
	return boost::asio::ip::address(boost::asio::ip::address_v6::loopback());
}

boost::asio::ip::address AsioLoopbackIPv4()
{
	return boost::asio::ip::address(boost::asio::ip::address_v4::loopback());
}
} // namespace

// Every test below returns early when the host cannot listen on [::1]: that is the environment,
// not the behavior under test.
TEST(LibSocketTransport, ConnectIPv6ReachesANativeIPv6Peer)
{
	CLoopbackListener listener(AsioLoopbackIPv6());
	if (!listener.IsOk()) {
		return;
	}
	CScopedFamilies families(AddressFamilyPolicy::Families::DualStack);
	CLibSocket socket;
	socket.Notify(false);

	ASSERT_TRUE(socket.ConnectIPv6(LoopbackIPv6(), listener.Port(), false));
	ASSERT_TRUE(socket.IsConnected());
	ASSERT_TRUE(socket.GetPeerAddress() == LoopbackIPv6());
	ASSERT_EQUALS(0u, socket.GetPeerInt());
	const std::optional<boost::asio::ip::address> remote = listener.AcceptPending();
	ASSERT_TRUE(remote.has_value());
	ASSERT_TRUE(*remote == AsioLoopbackIPv6());
}

TEST(LibSocketTransport, ConnectIPv6IsRefusedUnlessThePolicyPermitsIPv6)
{
	CLoopbackListener listener(AsioLoopbackIPv6());
	if (!listener.IsOk()) {
		return;
	}
	CScopedFamilies families(AddressFamilyPolicy::Families::IPv4Only);
	CLibSocket socket;
	socket.Notify(false);

	ASSERT_FALSE(socket.ConnectIPv6(LoopbackIPv6(), listener.Port(), false));
	ASSERT_FALSE(listener.AcceptPending().has_value());
}

TEST(LibSocketTransport, ConnectIPv6LeavesIPv4TargetsToConnect)
{
	CLoopbackListener listener(AsioLoopbackIPv4());
	if (!listener.IsOk()) {
		return;
	}
	CScopedFamilies families(AddressFamilyPolicy::Families::DualStack);
	const CNetworkAddress mapped =
		CNetworkAddress::IPv6FromOctets({ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 127, 0, 0, 1 });
	for (const CNetworkAddress &target : { CNetworkAddress::FromString("127.0.0.1"), mapped }) {
		CLibSocket socket;
		socket.Notify(false);
		ASSERT_FALSE(socket.ConnectIPv6(target, listener.Port(), false));
	}
	ASSERT_FALSE(listener.AcceptPending().has_value());
}

// Discriminates where DiallingIsRefusedWhileATransportIsAttached cannot: this peer is connectable.
TEST(LibSocketTransport, ConnectIPv6IsRefusedWhileATransportIsAttached)
{
	CLoopbackListener listener(AsioLoopbackIPv6());
	if (!listener.IsOk()) {
		return;
	}
	CScopedFamilies families(AddressFamilyPolicy::Families::DualStack);
	CLibSocket socket;
	socket.Notify(false);
	Attach(socket);

	ASSERT_FALSE(socket.ConnectIPv6(LoopbackIPv6(), listener.Port(), false));
	ASSERT_FALSE(listener.AcceptPending().has_value());
}

// A configured bind address opens the socket as IPv4 before any connect (CEMSocket's ctor).
TEST(LibSocketTransport, ConnectIPv6IsRefusedOnASocketBoundToALocalIPv4Address)
{
	CLoopbackListener listener(AsioLoopbackIPv6());
	if (!listener.IsOk()) {
		return;
	}
	CScopedFamilies families(AddressFamilyPolicy::Families::DualStack);
	CLibSocket socket;
	socket.Notify(false);
	amuleIPV4Address local;
	local.Hostname(wxString("127.0.0.1"));
	local.Service(0);
	socket.SetLocal(local);

	ASSERT_FALSE(socket.ConnectIPv6(LoopbackIPv6(), listener.Port(), false));
	ASSERT_FALSE(listener.AcceptPending().has_value());
}

TEST(LibSocketTransport, ConnectIPv6ReportsAClosedPort)
{
	uint16 closedPort = 0;
	{
		CLoopbackListener listener(AsioLoopbackIPv6());
		if (!listener.IsOk()) {
			return;
		}
		closedPort = listener.Port();
	}
	CScopedFamilies families(AddressFamilyPolicy::Families::DualStack);
	CLibSocket socket;
	socket.Notify(false);

	ASSERT_FALSE(socket.ConnectIPv6(LoopbackIPv6(), closedPort, false));
	ASSERT_FALSE(socket.IsConnected());
	ASSERT_TRUE(socket.LastError() != 0);
}
#endif

// File_checked_for_headers
