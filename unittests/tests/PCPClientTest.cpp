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
	pcp::Client client(server.Endpoint(), Local());
	client.SetMapping(4662, Local(), 4662);
	const auto initial = server.Next();
	ASSERT_TRUE(initial.has_value());
	ASSERT_EQUALS(64U, initial->bytes.size());
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
	pcp::Client client(server.Endpoint(), Local());
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
	pcp::Client client(server.Endpoint(), Local());
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
	pcp::Client client(server.Endpoint(), Local());
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

TEST(PCPClient, RecreatesAfterGatewayEpochRollsBack)
{
	Server server;
	pcp::Client client(server.Endpoint(), Local());
	client.SetMapping(4662, Local(), 4662);
	const auto first = server.Next();
	ASSERT_TRUE(first.has_value());
	server.Reply(*first, 0, 1, 100);
	ASSERT_TRUE(Active(client, true));
	const auto renewal = server.Next();
	ASSERT_TRUE(renewal.has_value());
	server.Reply(*renewal, 0, 1, 1);
	const auto recreated = server.Next();
	ASSERT_TRUE(recreated.has_value());
	ASSERT_TRUE(SameNonce(*first, *recreated));
	server.Reply(*recreated, 0, 2, 1);
	ASSERT_TRUE(Active(client, true));
}
