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
#include "Logger.h"

#include <boost/asio.hpp>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

namespace pcp
{
namespace
{
class ClientAdapter final : public MappingClient
{
public:
	ClientAdapter(const Gateway &gateway, boost::asio::io_context &io)
	: client(gateway.endpoint, gateway.localAddress, io)
	{
	}
	void SetMapping(
		std::uint16_t port, boost::asio::ip::address_v6 address, std::uint16_t externalPort) override
	{
		client.SetMapping(port, address, externalPort);
	}
	void Stop() override { client.Stop(); }
	void Release() override { client.Release(); }
	LeaseSnapshot Snapshot() const override { return client.Snapshot(); }
	bool Ready() const override { return client.Ready(); }

private:
	Client client;
};

struct Target
{
	boost::asio::ip::address_v6 address;
	std::uint16_t port;
	bool operator==(const Target &other) const { return address == other.address && port == other.port; }
};
} // namespace

struct PCPManager::State
{
	State(Discovery discover, Factory create, std::chrono::milliseconds interval)
	: discovery(discover ? std::move(discover) : Discovery{ DiscoverGateway })
	, factory(create ? std::move(create) : Factory{ [this](const Gateway &gateway) {
		return std::make_unique<ClientAdapter>(gateway, clientIo);
	} })
	, retryInterval(interval > std::chrono::milliseconds::zero() ? interval : std::chrono::seconds(30))
	{
	}

	void Run()
	{
		std::unique_lock<std::mutex> lock(mutex);
		std::optional<Target> current;
		std::optional<Gateway> currentGateway;
		bool due = false;
		for (;;) {
			if (stopping) {
				break;
			}
			const auto desired = target;
			const auto version = revision;
			if (current && (!desired || !(*current == *desired))) {
				auto old = std::move(client);
				// The handle is moved-from now; record that so no later path reads it.
				client = nullptr;
				current.reset();
				currentGateway.reset();
				const bool handover = !desired && release;
				lock.unlock();
				if (handover) {
					old->Release();
				} else {
					old->Stop();
				}
				lock.lock();
				continue;
			}
			if (!desired) {
				changed.wait(lock, [&] { return stopping || revision != version; });
				continue;
			}
			if (due || !current || appliedRevision != version) {
				due = false;
				appliedRevision = version;
				lock.unlock();
				const auto gateway = discovery(desired->address);
				lock.lock();
				if (stopping || revision != version) {
					continue;
				}
				noGateway = !gateway;
				if (current &&
					(!gateway || !currentGateway ||
						gateway->endpoint != currentGateway->endpoint ||
						gateway->localAddress != currentGateway->localAddress)) {
					auto old = std::move(client);
					client = nullptr;
					current.reset();
					currentGateway.reset();
					lock.unlock();
					old->Stop();
					lock.lock();
					if (stopping || revision != version) {
						continue;
					}
				}
				if (current && client && !client->Ready()) {
					// A client whose bind() failed never becomes usable and stays dead
					// forever; the due pass recreates it against the same gateway.
					auto old = std::move(client);
					client = nullptr;
					current.reset();
					lock.unlock();
					old->Stop();
					lock.lock();
					if (stopping || revision != version) {
						continue;
					}
				}
				if (gateway && !current) {
					lock.unlock();
					auto next = factory(*gateway);
					lock.lock();
					if (stopping || revision != version) {
						lock.unlock();
						if (next) {
							next->Stop();
						}
						lock.lock();
						continue;
					}
					if (next) {
						// SetMapping is asynchronous. Holding the admission lock
						// prevents a stale lookup from reopening after ClearTarget
						// has returned.
						next->SetMapping(
							desired->port, desired->address, desired->port);
						client = std::move(next);
						// Reset the moved-from handle so a later iteration cannot
						// re-adopt it.
						next = nullptr;
						current = desired;
						currentGateway = gateway;
						std::ostringstream msg;
						msg << "IPv6 PCP: default router "
						    << gateway->endpoint.address().to_string()
						    << " selected for local address "
						    << gateway->localAddress.to_string();
						AddDebugLogLineN(logUPnP, msg);
					}
				}
			}
			// The loop owns the only thread that services the shared io_context, so posted
			// client work runs here, in bounded slices between the waits.
			const auto deadline = std::chrono::steady_clock::now() + retryInterval;
			bool woken = false;
			while (!woken && std::chrono::steady_clock::now() < deadline) {
				clientIo.poll();
				woken = changed.wait_for(lock, std::chrono::milliseconds(50), [&] {
					return stopping || revision != version;
				});
			}
			due = !woken;
		}
		lock.unlock();
		if (client) {
			client->Stop();
		}
	}

	Discovery discovery;
	Factory factory;
	std::chrono::milliseconds retryInterval;
	// One event loop for every client this manager creates; its thread is the manager's
	// own worker, which also services the clients between passes.
	boost::asio::io_context clientIo;
	std::mutex mutex;
	std::mutex stopMutex;
	std::condition_variable changed;
	std::optional<Target> target;
	// Set with an empty target: the client is released, not deleted (a handover to IGD2).
	bool release = false;
	// The last discovery for the current target found no gateway.
	bool noGateway = false;
	std::uint64_t revision = 0;
	std::uint64_t appliedRevision = 0;
	// Guarded by mutex; non-null only while a client is applied to the current target.
	std::unique_ptr<MappingClient> client;
	bool stopping = false;
	std::thread thread;
};

PCPManager::PCPManager(Discovery discovery, Factory factory, std::chrono::milliseconds retryInterval)
: m_state(std::make_shared<State>(std::move(discovery), std::move(factory), retryInterval))
{
	const auto state = m_state;
	state->thread = std::thread([state] { state->Run(); });
}

PCPManager::~PCPManager()
{
	Stop();
}

void PCPManager::SetTarget(const boost::asio::ip::address_v6 &address, std::uint16_t port)
{
	Update(address, port, false);
}

void PCPManager::Update(const boost::asio::ip::address_v6 &address, std::uint16_t port, bool release)
{
	const auto state = m_state;
	std::lock_guard<std::mutex> lock(state->mutex);
	if (state->stopping) {
		return;
	}
	std::optional<Target> next;
	if (port != 0 && !address.is_unspecified() && !address.is_multicast() && !address.is_v4_mapped()) {
		next = Target{ address, port };
	}
	state->release = release;
	if (state->target == next) {
		return;
	}
	state->target = next;
	state->noGateway = false;
	++state->revision;
	state->changed.notify_one();
}

void PCPManager::ClearTarget()
{
	SetTarget({}, 0);
}

void PCPManager::StandBy()
{
	Update({}, 0, true);
}

LeaseSnapshot PCPManager::LeaseState() const
{
	const auto state = m_state;
	std::lock_guard<std::mutex> lock(state->mutex);
	if (!state->client) {
		LeaseSnapshot snapshot;
		snapshot.failed = state->target.has_value() && state->noGateway;
		return snapshot;
	}
	return state->client->Snapshot();
}

void PCPManager::Stop()
{
	const auto state = m_state;
	std::lock_guard<std::mutex> stopLock(state->stopMutex);
	{
		std::lock_guard<std::mutex> lock(state->mutex);
		state->stopping = true;
		++state->revision;
	}
	state->changed.notify_one();
	if (state->thread.joinable()) {
		state->thread.join();
	}
	// Client teardown posts onto the shared loop; drain it here so the best-effort delete
	// and the socket close complete before the manager disappears.
	while (state->clientIo.poll() > 0) {
	}
}
} // namespace pcp
