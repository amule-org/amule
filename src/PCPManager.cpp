#include "PCPManager.h"
#include "PCPClient.h"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace pcp
{
namespace
{
class ClientAdapter final : public MappingClient
{
public:
	explicit ClientAdapter(const Gateway &gateway)
	: client(gateway.endpoint, gateway.localAddress)
	{
	}
	void SetMapping(
		std::uint16_t port, boost::asio::ip::address_v6 address, std::uint16_t externalPort) override
	{
		client.SetMapping(port, address, externalPort);
	}
	void Stop() override { client.Stop(); }

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
	, factory(create ? std::move(create) : Factory{ [](const Gateway &gateway) {
		return std::make_unique<ClientAdapter>(gateway);
	} })
	, retryInterval(interval > std::chrono::milliseconds::zero() ? interval : std::chrono::seconds(30))
	{
	}

	void Run()
	{
		std::unique_lock<std::mutex> lock(mutex);
		std::optional<Target> current;
		std::optional<Gateway> currentGateway;
		std::unique_ptr<MappingClient> client;
		bool due = false;
		for (;;) {
			if (stopping) {
				break;
			}
			const auto desired = target;
			const auto version = revision;
			if (current && (!desired || !(*current == *desired))) {
				auto old = std::move(client);
				current.reset();
				currentGateway.reset();
				lock.unlock();
				old->Stop();
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
				if (current &&
					(!gateway || !currentGateway ||
						gateway->endpoint != currentGateway->endpoint ||
						gateway->localAddress != currentGateway->localAddress)) {
					auto old = std::move(client);
					current.reset();
					currentGateway.reset();
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
						current = desired;
						currentGateway = gateway;
					}
				}
			}
			if (!changed.wait_for(
				    lock, retryInterval, [&] { return stopping || revision != version; })) {
				due = true;
			}
		}
		lock.unlock();
		if (client) {
			client->Stop();
		}
	}

	Discovery discovery;
	Factory factory;
	std::chrono::milliseconds retryInterval;
	std::mutex mutex;
	std::mutex stopMutex;
	std::condition_variable changed;
	std::optional<Target> target;
	std::uint64_t revision = 0;
	std::uint64_t appliedRevision = 0;
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

void PCPManager::SetTarget(boost::asio::ip::address_v6 address, std::uint16_t port)
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
	if (state->target == next) {
		return;
	}
	state->target = next;
	++state->revision;
	state->changed.notify_one();
}

void PCPManager::ClearTarget()
{
	SetTarget({}, 0);
}

void PCPManager::RefreshRoute()
{
	const auto state = m_state;
	std::lock_guard<std::mutex> lock(state->mutex);
	if (!state->stopping && state->target) {
		++state->revision;
		state->changed.notify_one();
	}
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
}
} // namespace pcp
