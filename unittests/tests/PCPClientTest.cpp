//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
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
// along with this program; if not, write to the Free Software Foundation,
// Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA.
// If not, see <https://www.gnu.org/licenses/>.
//

#include <muleunit/test.h>
#include "PCPClient.h"
#include "PCPProtocol.h"

#include <boost/asio.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

using namespace muleunit;
using namespace std::chrono_literals;
using boost::asio::ip::udp;
DECLARE_SIMPLE(PCPClient)

namespace
{
struct Packet
{
	std::vector<std::uint8_t> bytes;
	udp::endpoint sender;
};

class Server
{
public:
	Server()
	: m_socket(m_io, udp::endpoint(boost::asio::ip::address_v6::loopback(), 0))
	{
		Receive();
		m_thread = std::thread([this] { m_io.run(); });
	}
	~Server()
	{
		m_io.stop();
		m_thread.join();
	}
	udp::endpoint Endpoint() const { return m_socket.local_endpoint(); }
	std::optional<Packet> Next(std::chrono::milliseconds timeout = 1500ms)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		if (!m_ready.wait_for(lock, timeout, [this] { return !m_packets.empty(); })) {
			return std::nullopt;
		}
		auto packet = std::move(m_packets.front());
		m_packets.pop_front();
		return packet;
	}
	static std::vector<std::uint8_t> Response(const Packet &request,
		std::uint8_t result = 0,
		std::uint32_t lifetime = 2,
		std::uint32_t epoch = 100)
	{
		std::vector<std::uint8_t> response(60, 0);
		response[0] = 2;
		response[1] = 0x81;
		response[3] = result;
		for (unsigned i = 0; i < 4; ++i) {
			response[4 + i] = static_cast<std::uint8_t>(lifetime >> (24 - 8 * i));
			response[8 + i] = static_cast<std::uint8_t>(epoch >> (24 - 8 * i));
		}
		std::copy(request.bytes.begin() + 24, request.bytes.begin() + 60, response.begin() + 24);
		return response;
	}
	void Reply(const Packet &request,
		std::uint8_t result = 0,
		std::uint32_t lifetime = 2,
		std::uint32_t epoch = 100,
		bool wrongNonce = false)
	{
		auto response = Response(request, result, lifetime, epoch);
		if (wrongNonce) {
			response[24] ^= 0xff;
		}
		boost::asio::post(m_io, [this, response = std::move(response), peer = request.sender] {
			boost::system::error_code error;
			m_socket.send_to(boost::asio::buffer(response), peer, 0, error);
		});
	}

	void ReplyWith(const Packet &request, std::vector<std::uint8_t> response)
	{
		boost::asio::post(m_io, [this, response = std::move(response), peer = request.sender] {
			boost::system::error_code error;
			m_socket.send_to(boost::asio::buffer(response), peer, 0, error);
		});
	}

private:
	void Receive()
	{
		m_socket.async_receive_from(boost::asio::buffer(m_buffer),
			m_sender,
			[this](boost::system::error_code error, std::size_t size) {
				if (!error) {
					std::lock_guard<std::mutex> lock(m_mutex);
					m_packets.push_back(
						{ { m_buffer.begin(), m_buffer.begin() + size }, m_sender });
					m_ready.notify_all();
					Receive();
				}
			});
	}
	boost::asio::io_context m_io;
	udp::socket m_socket;
	udp::endpoint m_sender;
	std::array<std::uint8_t, 2048> m_buffer{};
	std::mutex m_mutex;
	std::condition_variable m_ready;
	std::deque<Packet> m_packets;
	std::thread m_thread;
};

auto Local()
{
	return boost::asio::ip::address_v6::loopback();
}
// The client no longer owns its event loop: the caller supplies the io_context. This runner
// drains posted client cleanup on destruction, so a stopped client's delete is always sent.
struct IoRunner
{
	boost::asio::io_context io;
	boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work{
		boost::asio::make_work_guard(io)
	};
	std::thread thread{ [this] { io.run(); } };
	IoRunner() = default;
	IoRunner(const IoRunner &) = delete;
	IoRunner &operator=(const IoRunner &) = delete;
	~IoRunner()
	{
		work.reset();
		thread.join();
	}
};
bool Active(const pcp::Client &client, bool wanted, std::chrono::milliseconds timeout = 1000ms)
{
	const auto end = std::chrono::steady_clock::now() + timeout;
	do {
		if (client.Snapshot().active == wanted) {
			return true;
		}
		std::this_thread::sleep_for(5ms);
	} while (std::chrono::steady_clock::now() < end);
	return false;
}
bool SameNonce(const Packet &a, const Packet &b)
{
	return std::equal(a.bytes.begin() + 24, a.bytes.begin() + 36, b.bytes.begin() + 24);
}
} // namespace

TEST(PCPClient, RequestsRenewsGrantedLeaseAndClears)
{
	Server server;
	IoRunner runner;
	pcp::Client client(server.Endpoint(), Local(), runner.io);
	client.SetMapping(4662, Local(), 4662);
	const auto initial = server.Next();
	ASSERT_TRUE(initial.has_value());
	// 60 bytes: header and MAP payload only, no trailing PREFER_FAILURE option (RFC 6887
	// section 13.2: never necessary for a client managing its own mappings).
	ASSERT_EQUALS(60U, initial->bytes.size());
	ASSERT_TRUE(initial->sender.address() == Local());
	ASSERT_TRUE(std::equal(
		initial->bytes.begin() + 8, initial->bytes.begin() + 24, Local().to_bytes().begin()));
	ASSERT_EQUALS(3600U, pcp::ReadNetwork32(initial->bytes, 4));
	server.Reply(*initial);
	ASSERT_TRUE(Active(client, true));
	ASSERT_EQUALS(2U, client.Snapshot().grantedLifetime);
	const auto renewal = server.Next();
	ASSERT_TRUE(renewal.has_value());
	ASSERT_TRUE(SameNonce(*initial, *renewal));
	server.Reply(*renewal);
	client.ClearMapping();
	const auto deletion = server.Next();
	ASSERT_TRUE(deletion.has_value());
	ASSERT_EQUALS(0U, pcp::ReadNetwork32(deletion->bytes, 4));
	ASSERT_TRUE(SameNonce(*initial, *deletion));
	ASSERT_TRUE(Active(client, false));
	ASSERT_FALSE(server.Next(1250ms).has_value());
}

TEST(PCPClient, RejectsWrongSourceAndNonceWithoutChangingState)
{
	Server server;
	IoRunner runner;
	pcp::Client client(server.Endpoint(), Local(), runner.io);
	client.SetMapping(4662, Local(), 4662);
	const auto request = server.Next();
	ASSERT_TRUE(request.has_value());
	const auto before = client.Snapshot();
	boost::asio::io_context io;
	udp::socket attacker(io, udp::endpoint(Local(), 0));
	const auto forged = Server::Response(*request);
	attacker.send_to(boost::asio::buffer(forged), request->sender);
	server.Reply(*request, 0, 2, 100, true);
	std::this_thread::sleep_for(100ms);
	ASSERT_FALSE(client.Snapshot().active);
	ASSERT_EQUALS(before.generation, client.Snapshot().generation);
	ASSERT_EQUALS(0U, client.Snapshot().grantedLifetime);
	server.Reply(*request);
	ASSERT_TRUE(Active(client, true));
}

TEST(PCPClient, RefusalBackoffReplacementAndStop)
{
	Server server;
	IoRunner runner;
	pcp::Client client(server.Endpoint(), Local(), runner.io);
	client.SetMapping(4662, Local(), 4662);
	const auto first = server.Next();
	ASSERT_TRUE(first.has_value());
	const auto refusedAt = std::chrono::steady_clock::now();
	server.Reply(*first, 2, 1);
	const auto retry = server.Next();
	ASSERT_TRUE(retry.has_value());
	ASSERT_TRUE(std::chrono::steady_clock::now() - refusedAt >= 900ms);
	ASSERT_TRUE(SameNonce(*first, *retry));
	server.Reply(*retry);
	ASSERT_TRUE(Active(client, true));
	client.SetMapping(4663, Local(), 4663);
	const auto deletion = server.Next();
	ASSERT_TRUE(deletion.has_value());
	ASSERT_EQUALS(0U, pcp::ReadNetwork32(deletion->bytes, 4));
	const auto replacement = server.Next();
	ASSERT_TRUE(replacement.has_value());
	ASSERT_FALSE(SameNonce(*first, *replacement));
	server.Reply(*first);
	std::this_thread::sleep_for(100ms);
	ASSERT_FALSE(client.Snapshot().active);
	server.Reply(*replacement);
	ASSERT_TRUE(Active(client, true));
	client.Stop();
	const auto finalDelete = server.Next();
	ASSERT_TRUE(finalDelete.has_value());
	ASSERT_EQUALS(0U, pcp::ReadNetwork32(finalDelete->bytes, 4));
	ASSERT_TRUE(SameNonce(*replacement, *finalDelete));
	ASSERT_FALSE(client.Snapshot().active);
	ASSERT_FALSE(server.Next(1250ms).has_value());
}

TEST(PCPClient, ConcurrentStopRejectsLaterUpdates)
{
	Server server;
	IoRunner runner;
	pcp::Client client(server.Endpoint(), Local(), runner.io);
	std::atomic<bool> start{ false };
	std::thread updater([&] {
		while (!start.load()) {
		}
		for (unsigned i = 0; i < 500; ++i) {
			client.SetMapping(4662, Local(), 4662);
		}
	});
	std::thread stopper([&] {
		start.store(true);
		client.Stop();
	});
	updater.join();
	stopper.join();
	client.SetMapping(4663, Local(), 4663);
	client.Stop();
	ASSERT_FALSE(client.Snapshot().active);
	// Drain requests already posted before Stop; no later request can be admitted.
	while (server.Next(50ms).has_value()) {
	}
	ASSERT_FALSE(server.Next(250ms).has_value());
}

TEST(PCPClient, AcceptsGrantEvenWhenGatewayEpochRollsBack)
{
	Server server;
	IoRunner runner;
	pcp::Client client(server.Endpoint(), Local(), runner.io);
	client.SetMapping(4662, Local(), 4662);
	const auto first = server.Next();
	ASSERT_TRUE(first.has_value());
	server.Reply(*first, 0, 1, 100);
	ASSERT_TRUE(Active(client, true));
	const auto renewal = server.Next();
	ASSERT_TRUE(renewal.has_value());
	// For a one-mapping client the epoch-back grant is the recreated mapping itself: it is
	// accepted without spending a round trip on re-creation.
	server.Reply(*renewal, 0, 1, 1);
	ASSERT_TRUE(Active(client, true));
	ASSERT_FALSE(server.Next(100ms).has_value());
}

TEST(PCPClient, DeletesAndRefusesGrantWithDifferentPort)
{
	Server server;
	IoRunner runner;
	pcp::Client client(server.Endpoint(), Local(), runner.io);
	client.SetMapping(4662, Local(), 4662);
	const auto request = server.Next();
	ASSERT_TRUE(request.has_value());
	// The server granted a different external port than the one requested.
	auto grant = Server::Response(*request);
	grant[42] = 0x12;
	grant[43] = 0x37;
	server.ReplyWith(*request, std::move(grant));
	// A mapping we did not ask for is not used: it is deleted (lifetime 0) and refused.
	ASSERT_TRUE(Active(client, false));
	const auto deletion = server.Next();
	ASSERT_TRUE(deletion.has_value());
	ASSERT_EQUALS(0U, pcp::ReadNetwork32(deletion->bytes, 4));
	ASSERT_TRUE(SameNonce(*request, *deletion));
	ASSERT_FALSE(server.Next(1250ms).has_value());
}

TEST(PCPClient, RetransmitsAtLeastFourSecondsApart)
{
	Server server;
	IoRunner runner;
	pcp::Client client(server.Endpoint(), Local(), runner.io);
	client.SetMapping(4662, Local(), 4662);
	const auto first = server.Next();
	ASSERT_TRUE(first.has_value());
	const auto firstAt = std::chrono::steady_clock::now();
	// RFC 6887 section 11.2.1: retransmissions are at least 4 seconds apart, with bounded growth.
	// The floor in the client is exactly 4 s and the measurement is packet arrival on loopback,
	// so a 50 ms tolerance absorbs arrival jitter without losing the contract (the old 3 s
	// backoff failed this assertion).
	const auto retry = server.Next(6000ms);
	ASSERT_TRUE(retry.has_value());
	ASSERT_TRUE(std::chrono::steady_clock::now() - firstAt >= 3950ms);
	const auto retryAt = std::chrono::steady_clock::now();
	const auto secondRetry = server.Next(12000ms);
	ASSERT_TRUE(secondRetry.has_value());
	// Growth stays bounded: the second gap is the jittered 6 s step (5.4--6.6 s), never
	// as small as the 4 s floor.
	ASSERT_TRUE(std::chrono::steady_clock::now() - retryAt >= 5350ms);
}

TEST(PCPClient, UnansweredRequestsReportFailureUntilGrant)
{
	Server server;
	IoRunner runner;
	pcp::Client client(server.Endpoint(), Local(), runner.io);
	client.SetMapping(4662, Local(), 4662);
	const auto first = server.Next();
	ASSERT_TRUE(first.has_value());
	ASSERT_FALSE(client.Snapshot().failed);
	// The retransmission means the first request went unanswered.
	const auto retry = server.Next(6000ms);
	ASSERT_TRUE(retry.has_value());
	std::this_thread::sleep_for(50ms);
	ASSERT_TRUE(client.Snapshot().failed);
	server.Reply(*retry);
	ASSERT_TRUE(Active(client, true));
	ASSERT_FALSE(client.Snapshot().failed);
}

TEST(PCPClient, RefusalReportsFailureUntilGrant)
{
	Server server;
	IoRunner runner;
	pcp::Client client(server.Endpoint(), Local(), runner.io);
	client.SetMapping(4662, Local(), 4662);
	const auto first = server.Next();
	ASSERT_TRUE(first.has_value());
	server.Reply(*first, 2, 1);
	std::this_thread::sleep_for(100ms);
	ASSERT_TRUE(client.Snapshot().failed);
	ASSERT_FALSE(client.Snapshot().active);
	const auto retry = server.Next();
	ASSERT_TRUE(retry.has_value());
	server.Reply(*retry);
	ASSERT_TRUE(Active(client, true));
	ASSERT_FALSE(client.Snapshot().failed);
}

TEST(PCPClient, ReleaseStopsWithoutDelete)
{
	Server server;
	IoRunner runner;
	pcp::Client client(server.Endpoint(), Local(), runner.io);
	client.SetMapping(4662, Local(), 4662);
	const auto request = server.Next();
	ASSERT_TRUE(request.has_value());
	server.Reply(*request, 0, 60);
	ASSERT_TRUE(Active(client, true));
	// A handover: the other protocol may share this gateway entry, so no delete is sent.
	client.Release();
	ASSERT_FALSE(server.Next(1250ms).has_value());
	ASSERT_FALSE(client.Snapshot().active);
	client.SetMapping(4662, Local(), 4662);
	ASSERT_FALSE(server.Next(250ms).has_value());
}
