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

struct PCPManager::State : std::enable_shared_from_this<PCPManager::State>
{
	State(Discovery discover, Factory create, std::chrono::milliseconds interval)
	: discovery(discover ? std::move(discover) : Discovery{ DiscoverGateway })
	, factory(create ? std::move(create) : Factory{ [this](const Gateway &gateway) {
		return std::make_unique<ClientAdapter>(gateway, clientIo);
	} })
	, retryInterval(interval > std::chrono::milliseconds::zero() ? interval : std::chrono::seconds(30))
	{
	}

	// Runs on the manager thread only, posted by SetTarget() and by the pass timer.
	void Reconcile(bool due)
	{
		std::unique_lock<std::mutex> lock(mutex);
		if (stopping) {
			return;
		}
		const auto desired = target;
		const auto version = revision;
		if (current && (!desired || !(*current == *desired))) {
			const bool handover = !desired && release;
			auto old = Detach();
			lock.unlock();
			if (handover) {
				old->Release();
			} else {
				old->Stop();
			}
			lock.lock();
			if (stopping || revision != version) {
				return;
			}
		}
		if (!desired) {
			passTimer.cancel();
			return;
		}
		if (due || !current || appliedRevision != version) {
			appliedRevision = version;
			lock.unlock();
			const auto gateway = discovery(desired->address);
			lock.lock();
			// A newer target posted its own pass, which takes over from here.
			if (stopping || revision != version) {
				return;
			}
			noGateway = !gateway;
			const bool gatewayChanged =
				current && (!gateway || !currentGateway ||
						   gateway->endpoint != currentGateway->endpoint ||
						   gateway->localAddress != currentGateway->localAddress);
			// A client whose bind() failed never becomes usable and stays dead forever; the
			// due pass recreates it against the same gateway.
			const bool deadClient = current && client && !client->Ready();
			if (gatewayChanged || deadClient) {
				auto old = Detach();
				lock.unlock();
				old->Stop();
				lock.lock();
				if (stopping || revision != version) {
					return;
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
					return;
				}
				if (next) {
					// SetMapping is asynchronous. Holding the admission lock prevents a
					// stale lookup from reopening after ClearTarget has returned.
					next->SetMapping(desired->port, desired->address, desired->port);
					client = std::move(next);
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
		// The next due pass refreshes the router and replaces a dead client.
		passTimer.expires_after(retryInterval);
		passTimer.async_wait([self = shared_from_this()](boost::system::error_code error) {
			if (!error) {
				self->Reconcile(true);
			}
		});
	}

	// Guarded by mutex.
	std::unique_ptr<MappingClient> Detach()
	{
		auto old = std::move(client);
		client = nullptr;
		current.reset();
		currentGateway.reset();
		return old;
	}

	Discovery discovery;
	Factory factory;
	std::chrono::milliseconds retryInterval;
	// The manager's event loop: its thread runs it, and every client it creates lives on it,
	// so a posted delete goes out at once and the thread sleeps when nothing is due.
	boost::asio::io_context clientIo;
	boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work{
		boost::asio::make_work_guard(clientIo)
	};
	boost::asio::steady_timer passTimer{ clientIo };
	std::mutex mutex;
	std::mutex stopMutex;
	std::optional<Target> target;
	// Set with an empty target: the client is released, not deleted (a handover to IGD2).
	bool release = false;
	// The last discovery for the current target found no gateway.
	bool noGateway = false;
	std::uint64_t revision = 0;
	std::uint64_t appliedRevision = 0;
	// Guarded by mutex; non-null only while a client is applied to the current target.
	std::unique_ptr<MappingClient> client;
	std::optional<Target> current;
	std::optional<Gateway> currentGateway;
	bool stopping = false;
	std::thread thread;
};

PCPManager::PCPManager(Discovery discovery, Factory factory, std::chrono::milliseconds retryInterval)
: m_state(std::make_shared<State>(std::move(discovery), std::move(factory), retryInterval))
{
	const auto state = m_state;
	state->thread = std::thread([state] { state->clientIo.run(); });
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
	boost::asio::post(state->clientIo, [state] { state->Reconcile(false); });
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
		if (state->stopping) {
			return;
		}
		state->stopping = true;
		++state->revision;
	}
	boost::asio::post(state->clientIo, [state] {
		std::unique_ptr<MappingClient> last;
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			last = state->Detach();
		}
		if (last) {
			// The client posts its delete onto this loop, which runs it before run() returns.
			last->Stop();
		}
		state->passTimer.cancel();
		state->work.reset();
	});
	if (state->thread.joinable()) {
		state->thread.join();
	}
}
} // namespace pcp
