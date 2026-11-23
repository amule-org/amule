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

#include "PCPManager.h"
#include <muleunit/test.h>
#include <functional>

#include <array>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

using namespace muleunit;
DECLARE_SIMPLE(PCPManagerTest)

namespace
{
struct FakeClient final : pcp::MappingClient
{
	FakeClient(int id,
		std::vector<int> &events,
		std::mutex &mutex,
		std::condition_variable &changed,
		bool ready = true)
	: id(id)
	, events(events)
	, mutex(mutex)
	, changed(changed)
	, ready(ready)
	{
	}
	void SetMapping(std::uint16_t, boost::asio::ip::address_v6, std::uint16_t) override
	{
		std::lock_guard<std::mutex> lock(mutex);
		events.push_back(id);
		changed.notify_all();
	}
	void Stop() override
	{
		std::lock_guard<std::mutex> lock(mutex);
		events.push_back(-id);
		changed.notify_all();
	}
	void Release() override
	{
		std::lock_guard<std::mutex> lock(mutex);
		events.push_back(-(100 + id));
		changed.notify_all();
	}
	pcp::LeaseSnapshot Snapshot() const override
	{
		std::lock_guard<std::mutex> lock(mutex);
		return lease;
	}
	bool Ready() const override
	{
		std::lock_guard<std::mutex> lock(mutex);
		return ready;
	}
	int id;
	std::vector<int> &events;
	std::mutex &mutex;
	std::condition_variable &changed;
	bool ready;
	pcp::LeaseSnapshot lease;
};

struct Harness
{
	std::mutex mutex;
	std::condition_variable changed;
	int discoveries = 0;
	bool available = true;
	std::vector<int> events;
	std::thread::id discoveryThread;
	bool leaseActive = false;
	bool nextClientReady = true;
	bool clientFailed = false;

	pcp::PCPManager Make()
	{
		return pcp::PCPManager(
			[this](boost::asio::ip::address_v6 local) -> std::optional<pcp::Gateway> {
				std::lock_guard<std::mutex> lock(mutex);
				discoveryThread = std::this_thread::get_id();
				++discoveries;
				changed.notify_all();
				if (!available)
					return std::nullopt;
				return pcp::Gateway{ local,
					{ boost::asio::ip::make_address("2001:db8::1"), 5351 } };
			},
			[this](const pcp::Gateway &gateway) -> std::unique_ptr<pcp::MappingClient> {
				std::lock_guard<std::mutex> lock(mutex);
				const int id = gateway.endpoint.address().to_v6().to_bytes()[15];
				auto client = std::make_unique<FakeClient>(
					id, events, mutex, changed, nextClientReady);
				client->lease.active = leaseActive;
				client->lease.failed = clientFailed;
				return client;
			},
			std::chrono::milliseconds(20));
	}
	bool Wait(const std::function<bool()> &predicate)
	{
		std::unique_lock<std::mutex> lock(mutex);
		return changed.wait_for(lock, std::chrono::seconds(2), predicate);
	}
};

const auto local = boost::asio::ip::make_address_v6("2001:db8::2");

// A gateway on loopback for the manager's real client.
class FakeGateway
{
public:
	FakeGateway()
	: m_socket(m_io, boost::asio::ip::udp::endpoint(boost::asio::ip::address_v6::loopback(), 0))
	{
	}
	boost::asio::ip::udp::endpoint Endpoint() const { return m_socket.local_endpoint(); }
	// The next datagram within the timeout, or an empty vector.
	std::vector<std::uint8_t> Next(std::chrono::milliseconds timeout)
	{
		std::vector<std::uint8_t> packet;
		m_socket.async_receive_from(boost::asio::buffer(m_buffer),
			m_peer,
			[&](boost::system::error_code error, std::size_t size) {
				if (!error) {
					packet.assign(m_buffer.begin(), m_buffer.begin() + size);
				}
			});
		m_io.restart();
		m_io.run_for(timeout);
		if (packet.empty()) {
			m_socket.cancel();
			m_io.restart();
			m_io.run();
		}
		return packet;
	}
	void Grant(const std::vector<std::uint8_t> &request, std::uint32_t lifetime)
	{
		std::vector<std::uint8_t> response(60, 0);
		response[0] = 2;
		response[1] = 0x81;
		for (unsigned i = 0; i < 4; ++i) {
			response[4 + i] = static_cast<std::uint8_t>(lifetime >> (24 - 8 * i));
			response[11 - i] = static_cast<std::uint8_t>(100 >> (8 * i));
		}
		std::copy(request.begin() + 24, request.begin() + 60, response.begin() + 24);
		m_socket.send_to(boost::asio::buffer(response), m_peer);
	}

private:
	boost::asio::io_context m_io;
	boost::asio::ip::udp::socket m_socket;
	boost::asio::ip::udp::endpoint m_peer;
	std::array<std::uint8_t, 1100> m_buffer{};
};

std::uint32_t Lifetime(const std::vector<std::uint8_t> &packet)
{
	return (std::uint32_t{ packet[4] } << 24) | (std::uint32_t{ packet[5] } << 16) |
	       (std::uint32_t{ packet[6] } << 8) | packet[7];
}
} // namespace

TEST(PCPManagerTest, LeaseStateTracksClientLease)
{
	Harness h;
	h.leaseActive = true;
	auto manager = h.Make();
	ASSERT_FALSE(manager.LeaseState().active);
	manager.SetTarget(local, 4662);
	ASSERT_TRUE(h.Wait([&] { return h.events.size() >= 1; }));
	// The lease of the applied client is what the IGD2 fallback must defer to.
	ASSERT_TRUE(manager.LeaseState().active);
	manager.ClearTarget();
	ASSERT_TRUE(h.Wait([&] { return h.events.size() >= 2; }));
	ASSERT_FALSE(manager.LeaseState().active);
	manager.Stop();
}

TEST(PCPManagerTest, ReplacesBindFailedClientOnDuePass)
{
	Harness h;
	h.nextClientReady = false;
	auto manager = h.Make();
	manager.SetTarget(local, 4662);
	// The first client cannot bind: it is adopted but never becomes ready. The manager
	// must replace it on the due pass instead of leaving it dead forever.
	ASSERT_TRUE(h.Wait([&] { return h.events.size() >= 1; }));
	h.nextClientReady = true;
	ASSERT_TRUE(h.Wait([&] { return h.events.size() >= 3; }));
	{
		std::lock_guard<std::mutex> lock(h.mutex);
		ASSERT_EQUALS(1, h.events[0]);
		ASSERT_EQUALS(-1, h.events[1]);
		ASSERT_EQUALS(1, h.events[2]);
	}
	// Not under the harness mutex: the worker's discovery and factory callbacks take it.
	manager.Stop();
}

TEST(PCPManagerTest, DiscoversAndAppliesOffCallerThread)
{
	Harness h;
	auto manager = h.Make();
	const auto caller = std::this_thread::get_id();
	manager.SetTarget(local, 4662);
	ASSERT_TRUE(h.Wait([&] { return !h.events.empty(); }));
	ASSERT_TRUE(h.discoveryThread != caller);
	ASSERT_EQUALS(1, h.events.front());
	manager.Stop();
}

TEST(PCPManagerTest, ClearDuringLookupCannotOpenStaleMapping)
{
	Harness h;
	std::mutex gateMutex;
	std::condition_variable gate;
	bool release = false;
	bool started = false;
	pcp::PCPManager manager(
		[&](boost::asio::ip::address_v6 address) -> std::optional<pcp::Gateway> {
			std::unique_lock<std::mutex> lock(gateMutex);
			started = true;
			gate.notify_all();
			gate.wait(lock, [&] { return release; });
			return pcp::Gateway{ address,
				{ boost::asio::ip::make_address("2001:db8::1"), 5351 } };
		},
		[&](const pcp::Gateway &g) -> std::unique_ptr<pcp::MappingClient> {
			return std::make_unique<FakeClient>(1, h.events, h.mutex, h.changed);
		});
	manager.SetTarget(local, 4662);
	bool lookupStarted = false;
	{
		std::unique_lock<std::mutex> lock(gateMutex);
		lookupStarted = gate.wait_for(lock, std::chrono::seconds(2), [&] { return started; });
	}
	manager.ClearTarget();
	{
		std::lock_guard<std::mutex> lock(gateMutex);
		release = true;
	}
	gate.notify_all();
	manager.Stop();
	ASSERT_TRUE(lookupStarted);
	std::lock_guard<std::mutex> lock(h.mutex);
	ASSERT_TRUE(h.events.empty());
}

TEST(PCPManagerTest, TargetReplacementStopsPreviousMapping)
{
	Harness h;
	auto manager = h.Make();
	manager.SetTarget(local, 4662);
	ASSERT_TRUE(h.Wait([&] { return !h.events.empty(); }));
	manager.SetTarget(local, 4663);
	ASSERT_TRUE(h.Wait([&] { return h.events.size() >= 3; }));
	ASSERT_EQUALS(1, h.events[0]);
	ASSERT_EQUALS(-1, h.events[1]);
	ASSERT_EQUALS(1, h.events[2]);
	manager.Stop();
}

TEST(PCPManagerTest, RepeatedTargetDoesNotRestartMapping)
{
	Harness h;
	auto manager = h.Make();
	manager.SetTarget(local, 4662);
	ASSERT_TRUE(h.Wait([&] { return !h.events.empty(); }));
	manager.SetTarget(local, 4662);
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	{
		std::lock_guard<std::mutex> lock(h.mutex);
		ASSERT_EQUALS(1U, h.events.size());
	}
	manager.Stop();
}

TEST(PCPManagerTest, UnavailableGatewayDoesNotBlockUpdate)
{
	Harness h;
	h.available = false;
	auto manager = h.Make();
	const auto start = std::chrono::steady_clock::now();
	manager.SetTarget(local, 4662);
	ASSERT_TRUE(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100));
	ASSERT_TRUE(h.Wait([&] { return h.discoveries > 0; }));
	manager.Stop();
}

TEST(PCPManagerTest, StandByReleasesWithoutDeleteAndCanResume)
{
	Harness h;
	auto manager = h.Make();
	manager.SetTarget(local, 4662);
	ASSERT_TRUE(h.Wait([&] { return h.events.size() >= 1; }));
	// IGD2 holds the pinhole: PCP must let go without a delete that would close it.
	manager.StandBy();
	ASSERT_TRUE(h.Wait([&] { return h.events.size() >= 2; }));
	{
		std::lock_guard<std::mutex> lock(h.mutex);
		ASSERT_EQUALS(-101, h.events[1]);
	}
	manager.SetTarget(local, 4662);
	ASSERT_TRUE(h.Wait([&] { return h.events.size() >= 3; }));
	{
		std::lock_guard<std::mutex> lock(h.mutex);
		ASSERT_EQUALS(1, h.events[2]);
	}
	manager.Stop();
	std::lock_guard<std::mutex> lock(h.mutex);
	ASSERT_EQUALS(4U, h.events.size());
	ASSERT_EQUALS(-1, h.events[3]);
}

TEST(PCPManagerTest, MissingGatewayReportsFailure)
{
	Harness h;
	h.available = false;
	auto manager = h.Make();
	ASSERT_FALSE(manager.LeaseState().failed);
	manager.SetTarget(local, 4662);
	ASSERT_TRUE(h.Wait([&] { return h.discoveries > 0; }));
	const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!manager.LeaseState().failed && std::chrono::steady_clock::now() < end) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	ASSERT_TRUE(manager.LeaseState().failed);
	ASSERT_FALSE(manager.LeaseState().active);
	// Without a target nothing has failed.
	manager.ClearTarget();
	ASSERT_FALSE(manager.LeaseState().failed);
	manager.Stop();
}

TEST(PCPManagerTest, ClientFailureIsReported)
{
	Harness h;
	h.clientFailed = true;
	auto manager = h.Make();
	manager.SetTarget(local, 4662);
	ASSERT_TRUE(h.Wait([&] { return h.events.size() >= 1; }));
	ASSERT_TRUE(manager.LeaseState().failed);
	manager.Stop();
}

TEST(PCPManagerTest, ClearTargetDeletesRealMappingAtOnce)
{
	FakeGateway gateway;
	const auto loopback = boost::asio::ip::address_v6::loopback();
	pcp::PCPManager manager([&](boost::asio::ip::address_v6 address) -> std::optional<pcp::Gateway> {
		return pcp::Gateway{ address, gateway.Endpoint() };
	});
	manager.SetTarget(loopback, 4662);
	const auto request = gateway.Next(std::chrono::milliseconds(1500));
	ASSERT_EQUALS(60U, request.size());
	ASSERT_EQUALS(3600U, Lifetime(request));
	gateway.Grant(request, 3600);
	const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!manager.LeaseState().active && std::chrono::steady_clock::now() < end) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	ASSERT_TRUE(manager.LeaseState().active);
	// Turning forwarding off must close the pinhole now, not when the lease or aMule ends.
	manager.ClearTarget();
	const auto deletion = gateway.Next(std::chrono::milliseconds(1000));
	ASSERT_EQUALS(60U, deletion.size());
	ASSERT_EQUALS(0U, Lifetime(deletion));
	manager.Stop();
}
