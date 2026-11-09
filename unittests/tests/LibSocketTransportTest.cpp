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

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// Access to the production io_context pool lets the asynchronous tests drain ready
// handlers after joining the worker pool, without adding a socket API just for tests.
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "LibSocket.cpp"

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
// address helpers, which pull in the application. Most tests only count queued
// notifications; the loopback tests also clone and deliver them on the main thread.
namespace
{
std::atomic<int> s_queuedNotifications{ 0 };
std::atomic<bool> s_deliverDirectly{ false };
std::mutex s_notificationMutex;
bool s_captureNotifications = false;
std::vector<std::unique_ptr<MuleNotify::CMuleNotiferBase>> s_notifications;

//! Delivers notifications in the calling thread for its lifetime.
struct CDirectNotifications
{
	CDirectNotifications() { s_deliverDirectly.store(true); }
	~CDirectNotifications() { s_deliverDirectly.store(false); }
};
} // namespace

namespace MuleNotify
{
class CMuleNotiferBase;
void HandleNotificationAlways(const CMuleNotiferBase &);
void HandleNotificationAlways(const CMuleNotiferBase &notification)
{
	++s_queuedNotifications;
	if (s_deliverDirectly.load()) {
		notification.Notify();
		return;
	}
	std::lock_guard<std::mutex> lock(s_notificationMutex);
	if (s_captureNotifications) {
		s_notifications.emplace_back(notification.Clone());
	}
}
} // namespace MuleNotify

bool StringIPtoUint32(const wxString &, uint32 &);
bool StringIPtoUint32(const wxString &, uint32 &)
{
	return false;
}

namespace
{
void DrainSocketContexts()
{
	// The workers have been joined. A handler can enqueue work on another context,
	// so keep polling all of them until a complete pass makes no progress.
	size_t handled;
	do {
		handled = 0;
		for (auto &context : s_ioContexts) {
			context.restart();
			handled += context.poll();
		}
	} while (handled);
}

class CSendEventSocket : public CLibSocket
{
public:
	void OnSend(int error) override
	{
		ASSERT_EQUALS(0, error);
		++sendEvents;
	}
	void OnProxyEvent(int event) override
	{
		if (event == MULE_SOCKET_OUTPUT) {
			++proxyOutputEvents;
		}
	}
	int sendEvents = 0;
	int proxyOutputEvents = 0;
};

class CAsyncSendPeer
{
public:
	CAsyncSendPeer()
	: m_listener(m_io, { boost::asio::ip::address_v4::loopback(), 0 })
	, m_peer(m_io)
	{
		amuleIPV4Address address;
		address.Hostname(wxString("127.0.0.1"));
		address.Service(m_listener.local_endpoint().port());
		// wait=true connects synchronously while retaining the asynchronous write path.
		ASSERT_TRUE(socket.Connect(address, true));
		m_listener.accept(m_peer);
		m_peer.non_blocking(true);
		{
			std::lock_guard<std::mutex> lock(s_notificationMutex);
			s_captureNotifications = true;
		}
		StartWorkers();
	}

	~CAsyncSendPeer()
	{
		m_service->Stop();
		socket.Close();
		DrainSocketContexts();
		std::lock_guard<std::mutex> lock(s_notificationMutex);
		s_captureNotifications = false;
		s_notifications.clear();
	}

	void StartWorkers() { m_service = std::make_unique<CAsioService>(); }

	void FinishSend(size_t bytes)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		char buffer[65536];
		while (bytes) {
			boost::system::error_code error;
			const size_t received = m_peer.read_some(
				boost::asio::buffer(buffer, std::min(bytes, sizeof(buffer))), error);
			if (error == boost::asio::error::would_block ||
				error == boost::asio::error::try_again) {
				ASSERT_TRUE_M(std::chrono::steady_clock::now() < deadline,
					"Timed out draining the loopback send");
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				continue;
			}
			ASSERT_TRUE(!error);
			ASSERT_TRUE(received > 0);
			bytes -= received;
		}
		// Receiving the bytes alone does not prove HandleSend has run. Join the
		// workers, then drain ready handlers on this thread before checking events.
		m_service->Stop();
		DrainSocketContexts();
		std::vector<std::unique_ptr<MuleNotify::CMuleNotiferBase>> notifications;
		{
			std::lock_guard<std::mutex> lock(s_notificationMutex);
			notifications.swap(s_notifications);
		}
		for (const auto &notification : notifications) {
			notification->Notify();
		}
	}

	CSendEventSocket socket;

private:
	boost::asio::io_context m_io;
	boost::asio::ip::tcp::acceptor m_listener;
	boost::asio::ip::tcp::socket m_peer;
	std::unique_ptr<CAsioService> m_service;
};

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
	bool BlocksRead() const override { return blocksRead; }
	bool HasReadableBytes() const override { return readable; }
	bool BlocksWrite() const override { return true; }
	int LastError() const override { return lastError; }
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
	bool blocksRead = true;
	bool readable = false;
	int lastError = 0x7501;
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

TEST(LibSocketTransport, ContextPoolReleasesCrossContextAcceptsBeforeDestroyingServices)
{
	std::weak_ptr<boost::asio::ip::tcp::socket> pending;
	{
		CAsioContextPool pool;
		// Keep the acceptor alive through its pending handler, as the production listener does.
		auto listener = std::make_shared<boost::asio::ip::tcp::acceptor>(pool.contexts.front(),
			boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
		auto peer = std::make_shared<boost::asio::ip::tcp::socket>(pool.contexts.back());
		pending = peer;
		// Even an error completion owns an open peer until the handler is released.
		peer->open(boost::asio::ip::tcp::v4());
		listener->async_accept(*peer, [listener, peer](const boost::system::error_code &) {});
	}
	ASSERT_TRUE(pending.expired());
}

TEST(LibSocketTransport, ListenerSpreadsBurstAndIdleAcceptsAcrossAllWorkers)
{
	CDirectNotifications directNotifications;
	class CReadProbe : public CLibSocket
	{
	public:
		void OnReceive(int error) override
		{
			if (!announced.exchange(true)) {
				char byte = 0;
				const bool valid = !error && Read(&byte, 1) == 1 && byte == 'x';
				completed.set_value({ valid, std::this_thread::get_id() });
			}
		}
		std::atomic<bool> announced{ false };
		std::promise<std::pair<bool, std::thread::id>> completed;
	};
	for (bool burst : { false, true }) {
		amuleIPV4Address address;
		address.Hostname(wxString("127.0.0.1"));
		address.Service(0);
		auto listener = std::make_shared<CAsioSocketServerImpl>(address, nullptr);
		listener->Init();
		ASSERT_TRUE(listener->IsOk());
		boost::asio::io_context clientsContext;
		std::vector<std::unique_ptr<boost::asio::ip::tcp::socket>> clients;
		std::vector<std::unique_ptr<CReadProbe>> peers;
		std::vector<std::future<std::pair<bool, std::thread::id>>> results;
		const auto connect = [&]() {
			auto client = std::make_unique<boost::asio::ip::tcp::socket>(clientsContext);
			client->connect(listener->local_endpoint());
			clients.push_back(std::move(client));
		};
		if (burst) {
			for (int i = 0; i < 2 * s_numberOfAsioThreads; ++i) {
				connect();
			}
		}
		for (int i = 0; i < 2 * s_numberOfAsioThreads; ++i) {
			if (!burst) {
				connect();
			}
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			do {
				DrainSocketContexts();
				if (listener->SocketAvailable()) {
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			} while (std::chrono::steady_clock::now() < deadline);
			ASSERT_TRUE(listener->SocketAvailable());
			// Its constructor creates the temporary impl that used to skip a worker.
			auto peer = std::make_unique<CReadProbe>();
			results.push_back(peer->completed.get_future());
			ASSERT_TRUE(listener->AcceptWith(*peer));
			peers.push_back(std::move(peer));
			DrainSocketContexts();
		}
		CAsioService service;
		for (const auto &client : clients) {
			boost::asio::write(*client, boost::asio::buffer("x", 1));
		}
		std::vector<std::pair<bool, std::thread::id>> received;
		for (auto &result : results) {
			if (result.wait_for(std::chrono::seconds(2)) == std::future_status::ready) {
				received.push_back(result.get());
			}
		}
		service.Stop();
		for (const auto &peer : peers) {
			peer->Close();
		}
		listener->Close();
		DrainSocketContexts();
		ASSERT_EQUALS(peers.size(), received.size());
		std::vector<std::thread::id> workers;
		for (const auto &result : received) {
			ASSERT_TRUE(result.first);
			ASSERT_TRUE(result.second != std::this_thread::get_id());
			if (std::find(workers.begin(), workers.end(), result.second) == workers.end()) {
				workers.push_back(result.second);
			}
		}
		ASSERT_EQUALS(static_cast<size_t>(s_numberOfAsioThreads), workers.size());
		for (const auto &worker : workers) {
			ASSERT_EQUALS(
				2, std::count_if(received.begin(), received.end(), [&](const auto &result) {
					return result.second == worker;
				}));
		}
	}
}

TEST(LibSocketTransport, AcceptedPeerReadsOnItsOwnContext)
{
	boost::asio::ip::tcp::acceptor listener(
		s_ioContexts.front(), { boost::asio::ip::address_v4::loopback(), 0 });
	boost::asio::ip::tcp::socket peer(s_ioContexts.back());
	boost::asio::io_context clientContext;
	boost::asio::ip::tcp::socket client(clientContext);
	std::promise<std::pair<int, std::thread::id>> accepted;
	std::promise<std::pair<int, std::thread::id>> received;
	auto acceptResult = accepted.get_future();
	auto readResult = received.get_future();
	char payload = 0;
	// Declare last so workers are joined before any callback captures go out of scope.
	CAsioService service;
	listener.async_accept(peer, [&](const boost::system::error_code &error) {
		accepted.set_value({ error.value(), std::this_thread::get_id() });
		if (!error) {
			boost::asio::async_read(peer,
				boost::asio::buffer(&payload, 1),
				[&](const boost::system::error_code &readError, size_t) {
					received.set_value({ readError.value(), std::this_thread::get_id() });
				});
		}
	});
	boost::system::error_code clientError;
	client.connect(listener.local_endpoint(), clientError);
	if (!clientError) {
		boost::asio::write(client, boost::asio::buffer("x", 1), clientError);
	}
	const auto acceptStatus = acceptResult.wait_for(std::chrono::seconds(2));
	const auto readStatus = readResult.wait_for(std::chrono::seconds(2));
	service.Stop();
	boost::system::error_code ignored;
	peer.close(ignored);
	listener.close(ignored);
	DrainSocketContexts();
	ASSERT_TRUE(!clientError);
	ASSERT_TRUE(acceptStatus == std::future_status::ready);
	ASSERT_TRUE(readStatus == std::future_status::ready);
	const auto accept = acceptResult.get();
	const auto read = readResult.get();
	ASSERT_EQUALS(0, accept.first);
	ASSERT_EQUALS(0, read.first);
	ASSERT_EQUALS('x', payload);
	ASSERT_TRUE(accept.second != std::this_thread::get_id());
	ASSERT_TRUE(read.second != std::this_thread::get_id());
	ASSERT_TRUE(accept.second != read.second);
}

// Check actual socket executors, including wraparound, rather than only the selector.
TEST(LibSocketTransport, TcpSocketsUseEveryContextInRoundRobinOrder)
{
	std::vector<boost::asio::execution_context *> contexts;
	for (int i = 0; i < 2 * s_numberOfAsioThreads; ++i) {
		CAsioSocketImpl socket(nullptr);
		contexts.push_back(&socket.GetAsioSocket().get_executor().context());
	}
	for (int i = 0; i < s_numberOfAsioThreads; ++i) {
		ASSERT_TRUE(contexts[i] == contexts[i + s_numberOfAsioThreads]);
		for (int j = 0; j < i; ++j) {
			ASSERT_TRUE(contexts[i] != contexts[j]);
		}
	}
}

TEST(LibSocketTransport, RecreatedServicesRunEveryContextOnADistinctWorker)
{
	for (int generation = 0; generation < 8; ++generation) {
		std::array<std::promise<std::thread::id>, s_numberOfAsioThreads> completed;
		std::array<std::future<std::thread::id>, s_numberOfAsioThreads> futures;
		for (int i = 0; i < s_numberOfAsioThreads; ++i) {
			futures[i] = completed[i].get_future();
			boost::asio::post(s_ioContexts[i],
				[&completed, i]() { completed[i].set_value(std::this_thread::get_id()); });
		}
		{
			CAsioService service;
			std::vector<std::thread::id> workers;
			for (auto &future : futures) {
				ASSERT_TRUE(future.wait_for(std::chrono::seconds(2)) ==
					    std::future_status::ready);
				const auto worker = future.get();
				ASSERT_TRUE(worker != std::this_thread::get_id());
				for (const auto &previous : workers) {
					ASSERT_TRUE(worker != previous);
				}
				workers.push_back(worker);
			}
			if (generation % 2) {
				service.Stop();
				service.Stop();
			}
		} // Also exercise automatic stop and join in the destructor.
		for (auto &context : s_ioContexts) {
			ASSERT_TRUE(context.stopped());
		}
	}
}

TEST(LibSocketTransport, BoundedSynchronousConnectRunsOnEverySocketContext)
{
	boost::asio::io_context io;
	boost::asio::ip::tcp::acceptor listener(io, { boost::asio::ip::address_v4::loopback(), 0 });
	amuleIPV4Address address;
	address.Hostname(wxString("127.0.0.1"));
	address.Service(listener.local_endpoint().port());
	for (int i = 0; i < 2 * s_numberOfAsioThreads; ++i) {
		CLibSocket socket;
		socket.Notify(false);
		socket.SetConnectTimeout(1000);
		ASSERT_TRUE(socket.Connect(address, false));
		ASSERT_TRUE(socket.IsConnected());
		boost::asio::ip::tcp::socket peer(io);
		listener.accept(peer);
		socket.Close();
	}
	// A failed connect must also consume its timer before the stack frame disappears.
	listener.close();
	for (int i = 0; i < s_numberOfAsioThreads; ++i) {
		CLibSocket socket;
		socket.Notify(false);
		socket.SetConnectTimeout(1000);
		ASSERT_FALSE(socket.Connect(address, false));
		ASSERT_FALSE(socket.IsConnected());
		ASSERT_TRUE(socket.LastError() != 0);
	}
	DrainSocketContexts();
}

TEST(LibSocketTransport, AcceptedAsyncWriteQueuesNoSendEvent)
{
	for (int context = 0; context < s_numberOfAsioThreads; ++context) {
		CAsyncSendPeer peer;
		const char payload[] = "accepted";
		for (int completion = 0; completion < 2; ++completion) {
			// The second accepted write also proves the first completion cleared its buffer.
			ASSERT_EQUALS(sizeof(payload), peer.socket.Write(payload, sizeof(payload)));
			peer.FinishSend(sizeof(payload));
			ASSERT_EQUALS(0, peer.socket.sendEvents);
			ASSERT_FALSE(peer.socket.BlocksWrite());
			peer.StartWorkers();
		}
	}
}

TEST(LibSocketTransport, RefusedAsyncWritesQueueOneRetryEvent)
{
	CAsyncSendPeer peer;
	// Larger than the TCP buffers: the send cannot finish until the peer reads.
	const std::vector<char> payload(32 * 1024 * 1024, 'x');
	ASSERT_EQUALS(payload.size(), peer.socket.Write(payload.data(), payload.size()));
	const char retry = 'y';
	for (int refusal = 0; refusal < 3; ++refusal) {
		ASSERT_EQUALS(0u, peer.socket.Write(&retry, 1));
		ASSERT_TRUE(peer.socket.BlocksWrite());
	}
	peer.FinishSend(payload.size());
	ASSERT_EQUALS(1, peer.socket.sendEvents);
	ASSERT_FALSE(peer.socket.BlocksWrite());
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	ASSERT_TRUE(peer.socket.TakeSendWakeDelay() >= 10);
	ASSERT_EQUALS(uint64{ 0 }, peer.socket.TakeSendWakeDelay());
	// Consuming the refusal must not leak another event into the next completion.
	peer.StartWorkers();
	ASSERT_EQUALS(1u, peer.socket.Write(&retry, 1));
	peer.FinishSend(1);
	ASSERT_EQUALS(1, peer.socket.sendEvents);
	ASSERT_EQUALS(uint64{ 0 }, peer.socket.TakeSendWakeDelay());
}

TEST(LibSocketTransport, ProxyNegotiationQueuesEverySendCompletion)
{
	CAsyncSendPeer peer;
	amuleIPV4Address destination;
	destination.Hostname(wxString("127.0.0.1"));
	peer.socket.SetProxyState(true, &destination);
	const char payload = 'p';
	for (int completion = 1; completion <= 3; ++completion) {
		ASSERT_EQUALS(1u, peer.socket.Write(&payload, 1));
		peer.FinishSend(1);
		ASSERT_EQUALS(completion, peer.socket.proxyOutputEvents);
		ASSERT_EQUALS(0, peer.socket.sendEvents);
		peer.StartWorkers();
	}
	peer.socket.SetProxyState(false);
	ASSERT_EQUALS(1u, peer.socket.Write(&payload, 1));
	peer.FinishSend(1);
	ASSERT_EQUALS(3, peer.socket.proxyOutputEvents);
	ASSERT_EQUALS(0, peer.socket.sendEvents);
}

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

TEST(LibSocketTransport, DataLeftAfterAReadIsAnnouncedOnceUntilTheEventIsProcessed)
{
	CLibSocket socket;
	CFakeTransport *fake = Attach(socket);
	char out[4];
	const int before = s_queuedNotifications.load();

	// A drained stream has nothing more to announce.
	fake->blocksRead = true;
	socket.Read(out, sizeof(out));
	ASSERT_EQUALS(before, s_queuedNotifications.load());

	// A packet reader takes one packet per event, so data left behind must raise the next one.
	fake->blocksRead = false;
	socket.Read(out, sizeof(out));
	ASSERT_EQUALS(before + 1, s_queuedNotifications.load());
	socket.Read(out, sizeof(out));
	ASSERT_EQUALS(before + 1, s_queuedNotifications.load());

	// Handling the event re-arms the announcement.
	socket.EventProcessed();
	socket.Read(out, sizeof(out));
	ASSERT_EQUALS(before + 2, s_queuedNotifications.load());
}

TEST(LibSocketTransport, ArrivingPayloadsShareOneEventUntilItIsProcessed)
{
	CLibSocket socket;
	Attach(socket);
	const int before = s_queuedNotifications.load();

	socket.OnStreamReadable();
	socket.OnStreamReadable();
	ASSERT_EQUALS(before + 1, s_queuedNotifications.load());

	socket.EventProcessed();
	socket.OnStreamReadable();
	ASSERT_EQUALS(before + 2, s_queuedNotifications.load());
}

TEST(LibSocketTransport, AReadThatLeavesDataSharesTheEventWithAnArrivingPayload)
{
	CLibSocket socket;
	CFakeTransport *fake = Attach(socket);
	char out[4];
	const int before = s_queuedNotifications.load();

	fake->blocksRead = false;
	socket.Read(out, sizeof(out));
	socket.OnStreamReadable();
	ASSERT_EQUALS(before + 1, s_queuedNotifications.load());
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

class CScopedBindInterface
{
public:
	explicit CScopedBindInterface(const wxString &iface) { SetSocketBindInterface(iface); }
	~CScopedBindInterface() { SetSocketBindInterface(wxEmptyString); }
	CScopedBindInterface(const CScopedBindInterface &) = delete;
	CScopedBindInterface &operator=(const CScopedBindInterface &) = delete;
};

// The backlog tells whether a connect was attempted: a refused attempt leaves nothing to accept.
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

	// A blocking connect returns once the client has the SYN-ACK; the server may queue the
	// connection a moment later (seen on macOS), so an immediate accept can find nothing.
	std::optional<boost::asio::ip::address> AcceptWithin(std::chrono::milliseconds timeout)
	{
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		for (;;) {
			std::optional<boost::asio::ip::address> remote = AcceptPending();
			if (remote || std::chrono::steady_clock::now() >= deadline) {
				return remote;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
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
	const std::optional<boost::asio::ip::address> remote = listener.AcceptWithin(std::chrono::seconds(2));
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
	ASSERT_EQUALS(static_cast<int>(boost::asio::error::address_family_not_supported), socket.LastError());
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
		ASSERT_EQUALS(static_cast<int>(boost::asio::error::address_family_not_supported),
			socket.LastError());
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
	ASSERT_EQUALS(static_cast<int>(boost::asio::error::address_family_not_supported), socket.LastError());
	ASSERT_FALSE(listener.AcceptPending().has_value());
}

// The pin keeps traffic on the configured interface (VPN-leak fix, #173); an IPv6 connect it
// cannot pin is refused rather than sent through the default route.
TEST(LibSocketTransport, ConnectIPv6IsRefusedWhenTheInterfacePinFails)
{
	CLoopbackListener listener(AsioLoopbackIPv6());
	if (!listener.IsOk()) {
		return;
	}
	CScopedFamilies families(AddressFamilyPolicy::Families::DualStack);
	CScopedBindInterface pin(wxString("amulenoif0"));
	CLibSocket socket;
	socket.Notify(false);

	ASSERT_FALSE(socket.ConnectIPv6(LoopbackIPv6(), listener.Port(), false));
	ASSERT_EQUALS(static_cast<int>(boost::asio::error::no_such_device), socket.LastError());
	ASSERT_FALSE(listener.AcceptPending().has_value());
}

// With notifications on, false means "pending": a refusal must still reach OnConnect() as a
// failed connect, or the caller waits for its connect timeout.
TEST(LibSocketTransport, ARefusedConnectIPv6IsReportedAsAFailedConnectWhenNotifying)
{
	CLoopbackListener listener(AsioLoopbackIPv6());
	if (!listener.IsOk()) {
		return;
	}
	CScopedFamilies families(AddressFamilyPolicy::Families::IPv4Only);
	CLibSocket socket;
	const int queuedBefore = s_queuedNotifications;

	ASSERT_FALSE(socket.ConnectIPv6(LoopbackIPv6(), listener.Port(), false));
	ASSERT_EQUALS(queuedBefore + 1, s_queuedNotifications);
	ASSERT_EQUALS(static_cast<int>(boost::asio::error::address_family_not_supported), socket.LastError());
	ASSERT_FALSE(listener.AcceptPending().has_value());
}

// An attached transport already carries a live stream; a failed-connect event would tear it down.
TEST(LibSocketTransport, ARefusalLeavesAnAttachedTransportUnnotified)
{
	CLoopbackListener listener(AsioLoopbackIPv6());
	if (!listener.IsOk()) {
		return;
	}
	CScopedFamilies families(AddressFamilyPolicy::Families::DualStack);
	CLibSocket socket;
	Attach(socket);
	const int queuedBefore = s_queuedNotifications;

	ASSERT_FALSE(socket.ConnectIPv6(LoopbackIPv6(), listener.Port(), false));
	ASSERT_EQUALS(queuedBefore, s_queuedNotifications);
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

#ifdef __APPLE__
TEST(LibSocketTransport, RawSocketBindUsesTheOptionOfItsFamily)
{
	for (const int family : { AF_INET, AF_INET6 }) {
		const int fd = ::socket(family, SOCK_STREAM, 0);
		ASSERT_TRUE(fd >= 0);
		const bool bound = BindRawSocketToInterface(static_cast<uintptr_t>(fd), wxString("lo0"));
		::close(fd);
		ASSERT_TRUE(bound);
	}
}
#endif

namespace
{
class CEndProbe : public CLibSocket
{
public:
	void OnReceive(int) override { ++reads; }
	void OnLost(int) override { ++lost; }
	int reads = 0;
	int lost = 0;
};
} // namespace

TEST(LibSocketTransport, ACleanEndWaitsForItsUnreadBytes)
{
	// One packet per read event: an end reported at once would overtake the read events still
	// owed for bytes already received, and those bytes would be lost with the socket.
	CDirectNotifications directNotifications;
	CEndProbe socket;
	CFakeTransport *fake = Attach(socket);
	fake->lastError = 0;
	fake->readable = true;
	socket.OnStreamLost();
	ASSERT_EQUALS(0, socket.lost);
	ASSERT_EQUALS(1, socket.reads);

	char out[4];
	fake->blocksRead = false;
	socket.EventProcessed();
	socket.Read(out, sizeof(out));
	ASSERT_EQUALS(0, socket.lost);

	// The read that empties the transport is the end.
	fake->readable = false;
	socket.Read(out, sizeof(out));
	ASSERT_EQUALS(1, socket.lost);

	// With nothing unread, the end is reported at once.
	CEndProbe drained;
	Attach(drained);
	drained.OnStreamLost();
	ASSERT_EQUALS(1, drained.lost);
}

TEST(LibSocketTransport, AFailedEndIsReportedAtOnce)
{
	// CEMSocket drops every read once LastError() is set, so waiting would only delay the end.
	CDirectNotifications directNotifications;
	CEndProbe socket;
	CFakeTransport *fake = Attach(socket);
	fake->readable = true;
	socket.OnStreamLost();
	ASSERT_EQUALS(1, socket.lost);
	ASSERT_EQUALS(0, socket.reads);
}

// File_checked_for_headers
