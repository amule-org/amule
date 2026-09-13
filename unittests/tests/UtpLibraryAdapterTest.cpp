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

#include <StreamTransport.h>
#include <UtpSocketTransport.h>
#include <UtpStream.h>
#include <UtpLibraryAdapter.h>
#include <libs/common/Format.h>

#include <libutp/utp.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <vector>

using namespace muleunit;

DECLARE_SIMPLE(UtpLibraryAdapter)

namespace
{
constexpr uint32_t kPeerIp = 0x0100007F; // 127.0.0.1 in aMule's low-byte-first form
constexpr uint16_t kPeerPort = 4672;

/**
 * A loopback through real libutp. The client is built only here, so production
 * gains no dial API, and datagrams are handed over by hand to stay
 * deterministic.
 */
struct SLoopback
{
	std::unique_ptr<IUtpLibrary> server;
	utp_context *client = nullptr;
	utp_socket *clientSocket = nullptr;
	std::deque<std::vector<uint8_t>> toServer, toClient;
	std::vector<uint8_t> clientRead;
	bool clientConnected = false;
};

SLoopback *g_loop = nullptr;

//! The server's datagram sink: everything it sends goes to the client.
class CServerSink : public IUtpDatagramSink
{
public:
	void SendUtpDatagram(const uint8_t *payload,
		size_t length,
		uint32_t,
		uint16_t,
		bool encrypt,
		const uint8_t *) override
	{
		lastEncrypt = encrypt;
		++datagrams;
		g_loop->toClient.emplace_back(payload, payload + length);
	}
	bool lastEncrypt = false;
	unsigned datagrams = 0;
};

//! Admission, reduced to the one decision this test varies.
class CFakeAcceptor : public IUtpStreamAcceptor
{
public:
	bool AcceptStream(std::unique_ptr<IStreamTransport> &transport, uint32_t ip, uint16_t port) override
	{
		++offers;
		lastIp = ip;
		lastPort = port;
		if (!admit) {
			return false;
		}
		accepted = std::move(transport);
		return true;
	}

	bool admit = true;
	int offers = 0;
	uint32_t lastIp = 0;
	uint16_t lastPort = 0;
	std::unique_ptr<IStreamTransport> accepted;
};

uint64 ClientSendTo(utp_callback_arguments *args)
{
	g_loop->toServer.emplace_back(args->buf, args->buf + args->len);
	return 0;
}

uint64 ClientOnState(utp_callback_arguments *args)
{
	if (args->state == UTP_STATE_CONNECT || args->state == UTP_STATE_WRITABLE) {
		g_loop->clientConnected = true;
	}
	return 0;
}

uint64 ClientOnRead(utp_callback_arguments *args)
{
	g_loop->clientRead.insert(g_loop->clientRead.end(), args->buf, args->buf + args->len);
	utp_read_drained(args->socket);
	return 0;
}

sockaddr_in Address(uint32_t ip, uint16_t port)
{
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	auto *bytes = reinterpret_cast<uint8_t *>(&address.sin_addr.s_addr);
	for (unsigned i = 0; i < 4; ++i) {
		bytes[i] = static_cast<uint8_t>(ip >> (8 * i));
	}
	return address;
}

//! Hands queued datagrams across until both directions are idle.
void Pump(SLoopback &loop, int rounds = 64)
{
	const sockaddr_in peer = Address(kPeerIp, kPeerPort);
	// Before the loop: utp_read_drained() defers the window update through
	// schedule_ack(), and with both queues empty the loop never runs.
	loop.server->IssueDeferredAcks();
	utp_issue_deferred_acks(loop.client);
	for (int i = 0; i < rounds && (!loop.toServer.empty() || !loop.toClient.empty()); ++i) {
		while (!loop.toServer.empty()) {
			const std::vector<uint8_t> datagram = loop.toServer.front();
			loop.toServer.pop_front();
			loop.server->ProcessDatagram(datagram.data(), datagram.size(), kPeerIp, kPeerPort);
		}
		while (!loop.toClient.empty()) {
			const std::vector<uint8_t> datagram = loop.toClient.front();
			loop.toClient.pop_front();
			utp_process_udp(loop.client,
				datagram.data(),
				datagram.size(),
				reinterpret_cast<const sockaddr *>(&peer),
				sizeof(peer));
		}
		loop.server->IssueDeferredAcks();
		utp_issue_deferred_acks(loop.client);
	}
}

void StartClient(SLoopback &loop)
{
	loop.client = utp_init(2);
	utp_set_callback(loop.client, UTP_SENDTO, ClientSendTo);
	utp_set_callback(loop.client, UTP_ON_STATE_CHANGE, ClientOnState);
	utp_set_callback(loop.client, UTP_ON_READ, ClientOnRead);
	loop.clientSocket = utp_create_socket(loop.client);
	const sockaddr_in peer = Address(kPeerIp, kPeerPort);
	utp_connect(loop.clientSocket, reinterpret_cast<const sockaddr *>(&peer), sizeof(peer));
}

void Teardown(SLoopback &loop)
{
	if (loop.client != nullptr) {
		utp_destroy(loop.client);
		loop.client = nullptr;
	}
	loop.server.reset();
	g_loop = nullptr;
}
} // namespace

TEST(UtpLibraryAdapter, AnInboundSynReachesAdmissionWithItsPeer)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	CFakeAcceptor acceptor;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	loop.server->SetAcceptor(&acceptor);
	StartClient(loop);

	Pump(loop);

	ASSERT_EQUALS(1, acceptor.offers);
	ASSERT_TRUE(acceptor.accepted != nullptr);
	ASSERT_EQUALS(kPeerIp, acceptor.lastIp);
	ASSERT_EQUALS(kPeerPort, acceptor.lastPort);
	// Registered only once admission succeeded, which is what lets an
	// established peer's later non-SYN frames through the ingress filter.
	ASSERT_TRUE(loop.server->HasRegisteredPeer(kPeerIp, kPeerPort));
	Teardown(loop);
}

TEST(UtpLibraryAdapter, WithNoAcceptorInstalledEveryInboundSynIsRefused)
{
	// The state this build was in before an acceptor existed, and the state it
	// must return to if one is ever detached.
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	StartClient(loop);

	Pump(loop);

	ASSERT_FALSE(loop.server->HasRegisteredPeer(kPeerIp, kPeerPort));
	Teardown(loop);
}

TEST(UtpLibraryAdapter, ARefusedStreamIsNeverRegistered)
{
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	CFakeAcceptor acceptor;
	acceptor.admit = false;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	loop.server->SetAcceptor(&acceptor);
	StartClient(loop);

	Pump(loop);

	ASSERT_EQUALS(1, acceptor.offers);
	ASSERT_TRUE(acceptor.accepted == nullptr);
	ASSERT_FALSE(loop.server->HasRegisteredPeer(kPeerIp, kPeerPort));
	Teardown(loop);
}

TEST(UtpLibraryAdapter, BytesCrossTheStreamInOrder)
{
	// The whole point of the series: an accepted stream actually carries bytes.
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	CFakeAcceptor acceptor;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	loop.server->SetAcceptor(&acceptor);
	StartClient(loop);
	Pump(loop);
	ASSERT_TRUE(acceptor.accepted != nullptr);

	std::vector<uint8_t> payload(4096);
	for (size_t i = 0; i < payload.size(); ++i) {
		payload[i] = static_cast<uint8_t>(i & 0xFF);
	}
	// Looped because utp_writev takes only what the window allows: ignoring
	// the return would "prove" the stream carries one packet.
	std::vector<uint8_t> received(payload.size());
	size_t offered = 0;
	uint32_t total = 0;
	for (int round = 0; round < 64 && (offered < payload.size() || total < payload.size()); ++round) {
		if (offered < payload.size()) {
			utp_iovec vector{ payload.data() + offered, payload.size() - offered };
			const ssize_t accepted = utp_writev(loop.clientSocket, &vector, 1);
			if (accepted > 0) {
				offered += static_cast<size_t>(accepted);
			}
		}
		Pump(loop);
		while (total < payload.size()) {
			const uint32_t taken = acceptor.accepted->Read(
				received.data() + total, static_cast<uint32_t>(payload.size() - total));
			if (taken == 0) {
				break;
			}
			total += taken;
		}
	}
	ASSERT_EQUALS((unsigned)payload.size(), (unsigned)total);
	for (size_t i = 0; i < payload.size(); ++i) {
		CFormat format("byte %u differs");
		ASSERT_EQUALS_M((int)payload[i], (int)received[i], format % unsigned(i));
	}
	Teardown(loop);
}

TEST(UtpLibraryAdapter, DeliveryStopsAtTheConfiguredBoundAndResumesOnTheCrossing)
{
	// UTP_RCVBUF makes 64 KiB the real bound, and the window is opt_rcvbuf
	// minus reported occupancy, so a reader that stops stops the peer too.
	// Resuming needs the crossing back below the bound, not an empty buffer.
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	CFakeAcceptor acceptor;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	loop.server->SetAcceptor(&acceptor);
	StartClient(loop);
	Pump(loop);
	ASSERT_TRUE(acceptor.accepted != nullptr);

	const size_t bound = CUtpStream::kDefaultReadBound;
	std::vector<uint8_t> payload(bound * 2);
	for (size_t i = 0; i < payload.size(); ++i) {
		payload[i] = static_cast<uint8_t>((i * 7) & 0xFF);
	}

	// Offer everything without reading a byte. Delivery must stall at the
	// bound: more than that in flight would mean the 64 KiB never took effect.
	size_t offered = 0;
	for (int round = 0; round < 64 && offered < payload.size(); ++round) {
		utp_iovec vector{ payload.data() + offered, payload.size() - offered };
		const ssize_t accepted = utp_writev(loop.clientSocket, &vector, 1);
		if (accepted > 0) {
			offered += static_cast<size_t>(accepted);
		}
		Pump(loop);
	}
	// The adapter builds this concrete type; occupancy is its business rather
	// than something every stream transport must expose.
	auto *utp = static_cast<CUtpSocketTransport *>(acceptor.accepted.get());
	const size_t buffered = utp->ReadBufferSize();
	// Short of the bound, not on it: libutp stops once the window cannot hold
	// another packet. Without UTP_RCVBUF the 1 MiB default would govern and
	// the whole payload would be here.
	CFormat stalled("buffered %u, bound %u, payload %u");
	const wxString detail = stalled % unsigned(buffered) % unsigned(bound) % unsigned(payload.size());
	ASSERT_TRUE_M(buffered < payload.size(), detail);
	ASSERT_TRUE_M(buffered + 2048 >= bound, detail);

	// Read just past the bound, leaving the buffer full but below it. An
	// empty-buffer rule would signal nothing here and the peer would stall.
	// Enough to reopen the window without emptying the buffer. Computed from
	// the slack rather than from the bound, because occupancy stalls below it.
	const size_t toRead = CUtpStream::kWindowSlackBytes * 2;
	std::vector<uint8_t> received(payload.size());
	size_t total = 0;
	while (total < toRead) {
		const uint32_t taken = acceptor.accepted->Read(
			received.data() + total, static_cast<uint32_t>(toRead - total));
		if (taken == 0) {
			break;
		}
		total += taken;
	}
	ASSERT_TRUE(utp->ReadBufferSize() != 0);

	// The rest must now arrive.
	for (int round = 0; round < 64 && (offered < payload.size() || total < payload.size()); ++round) {
		if (offered < payload.size()) {
			utp_iovec vector{ payload.data() + offered, payload.size() - offered };
			const ssize_t accepted = utp_writev(loop.clientSocket, &vector, 1);
			if (accepted > 0) {
				offered += static_cast<size_t>(accepted);
			}
		}
		Pump(loop);
		// Read the way CEMSocket::OnReceive does: a six-byte header, then a
		// bounded body, then return with the rest still queued. A reader that
		// keeps pace this way never empties the buffer, which is precisely why
		// an empty-buffer rule would never reopen the window and this transfer
		// would stall short of the payload.
		for (int packet = 0; packet < 6 && total < payload.size(); ++packet) {
			uint8_t header[6] = { 0 };
			const uint32_t headerTaken = acceptor.accepted->Read(header, sizeof(header));
			if (headerTaken == 0) {
				break;
			}
			std::copy(header, header + headerTaken, received.begin() + total);
			total += headerTaken;
			const size_t body = std::min<size_t>(1024, payload.size() - total);
			if (body == 0) {
				break;
			}
			total +=
				acceptor.accepted->Read(received.data() + total, static_cast<uint32_t>(body));
		}
	}
	CFormat progress("offered=%u total=%u buffered=%u drained-edges=%u");
	const wxString diag = progress % unsigned(offered) % unsigned(total) %
			      unsigned(utp->ReadBufferSize()) % unsigned(sink.datagrams);
	ASSERT_EQUALS_M((unsigned)payload.size(), (unsigned)total, diag);
	for (size_t i = 0; i < payload.size(); ++i) {
		CFormat format("byte %u differs after the window reopened");
		ASSERT_EQUALS_M((int)payload[i], (int)received[i], format % unsigned(i));
	}
	Teardown(loop);
}

TEST(UtpLibraryAdapter, DestroyingTheContextEndsTheStreamsItOwned)
{
	// utp_destroy() destroys the sockets it owns and ~UTPSocket emits
	// DESTROYING, which is what makes each transport drop its handle.
	SLoopback loop;
	g_loop = &loop;
	CServerSink sink;
	CFakeAcceptor acceptor;
	loop.server = CreateUtpLibrary();
	ASSERT_TRUE(loop.server->Create(sink));
	loop.server->SetAcceptor(&acceptor);
	StartClient(loop);
	Pump(loop);
	ASSERT_TRUE(acceptor.accepted != nullptr);
	ASSERT_TRUE(loop.server->HasRegisteredPeer(kPeerIp, kPeerPort));
	// Live before the teardown, or what follows proves nothing.
	ASSERT_TRUE(acceptor.accepted->IsOk());

	loop.server->Destroy();

	// The stream was told, so it has dropped its handle rather than keeping one
	// into a context that no longer exists.
	ASSERT_FALSE(acceptor.accepted->IsOk());
	ASSERT_FALSE(loop.server->HasRegisteredPeer(kPeerIp, kPeerPort));
	// Destroying again, and closing the stream the owner still holds, must both
	// be harmless: this is the shutdown order a real teardown produces.
	loop.server->Destroy();
	acceptor.accepted->Close();
	Teardown(loop);
}

// File_checked_for_headers
