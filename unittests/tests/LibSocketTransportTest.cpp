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

// File_checked_for_headers
