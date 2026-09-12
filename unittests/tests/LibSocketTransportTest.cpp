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
#include <vector>

using namespace muleunit;

DECLARE_SIMPLE(LibSocketTransport)

// Link seams. CLibSocket's asio implementation reaches the notification layer
// and the address helpers, both of which pull in the application. Nothing here
// drives an asio callback -- every test answers from an attached transport or
// from a socket that was never connected -- so these are never called.
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
/**
 * A transport whose answers an asio socket would never give.
 *
 * That is the whole design of these tests: a fresh CLibSocket is not connected,
 * not ok, and has no peer, so every assertion below can only pass if the call
 * reached the transport instead of the socket underneath.
 */
class CFakeTransport : public IStreamTransport
{
public:
	bool IsConnected() const override { return true; }
	bool IsOk() const override { return ok; }
	bool BlocksRead() const override { return true; }
	bool BlocksWrite() const override { return true; }
	int LastError() const override { return 0x7501; }
	CNetworkAddress GetPeerAddress() const override { return CNetworkAddress::FromString("192.0.2.7"); }
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

	bool ok = true;
	std::vector<uint8_t> written;
	int closeCalls = 0;
};

//! Attaches a fake and hands back a borrowed pointer; the socket owns it.
CFakeTransport *Attach(CLibSocket &socket)
{
	auto owned = std::make_unique<CFakeTransport>();
	CFakeTransport *borrowed = owned.get();
	socket.AttachTransport(std::move(owned));
	return borrowed;
}
} // namespace

/**
 * Every stream accessor answers from the transport once one is attached.
 *
 * One table rather than one test each, because the value here is that the list
 * is exhaustive: none of these are virtual in CLibSocket, CEncryptedStreamSocket
 * or CEMSocket, so any one left unrouted resolves statically to the asio socket
 * and quietly reports on a stream nobody is using. That is exactly how
 * CEMSocket::Send()'s !IsOk() arm would stay dead after wiring.
 */
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

	// Opaque by contract; what matters is that it is the transport's value and
	// not the socket's zero.
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
	// Narrowed at this accessor only, because its type is the ed2k wire form.
	ASSERT_TRUE(socket.GetPeerInt() != 0);
	ASSERT_TRUE(wxString(socket.GetIP()) == wxString("192.0.2.7"));
}

TEST(LibSocketTransport, DiallingIsRefusedWhileATransportIsAttached)
{
	// An accepted stream already has a peer. Dialling from here would open a
	// second, unrelated connection while the caller believed it had reconnected
	// this one.
	//
	// This pins the intent and does NOT discriminate: an unconnected asio socket
	// refuses this address too, so the assertion holds with the guard removed.
	// Making it discriminate needs a connectable peer, which belongs to the
	// acceptor's loopback test rather than here.
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
	// The transport owns close-once itself; what matters here is that the calls
	// arrive there at all rather than at the asio socket.
	ASSERT_EQUALS(2, fake->closeCalls);
}

TEST(LibSocketTransport, AnUnattachedSocketStillAnswersForItself)
{
	// The branch has to be a branch: a socket with no transport must behave
	// exactly as it did before this façade existed.
	CLibSocket socket;
	ASSERT_FALSE(socket.HasTransport());
	ASSERT_FALSE(socket.IsConnected());
	ASSERT_FALSE(socket.IsOk());
}

// File_checked_for_headers
