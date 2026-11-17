#pragma once
#include "PCPGateway.h"
#include <chrono>
#include <functional>
#include <memory>
#include <optional>

namespace pcp
{
class MappingClient
{
public:
	virtual ~MappingClient() = default;
	virtual void SetMapping(
		std::uint16_t port, boost::asio::ip::address_v6 address, std::uint16_t externalPort) = 0;
	virtual void Stop() = 0;
};

class PCPManager
{
public:
	using Discovery = std::function<std::optional<Gateway>(boost::asio::ip::address_v6)>;
	using Factory = std::function<std::unique_ptr<MappingClient>(const Gateway &)>;
	PCPManager(Discovery discovery = {},
		Factory factory = {},
		std::chrono::milliseconds retryInterval = std::chrono::seconds(30));
	~PCPManager();
	PCPManager(const PCPManager &) = delete;
	PCPManager &operator=(const PCPManager &) = delete;
	void SetTarget(boost::asio::ip::address_v6 address, std::uint16_t port);
	void ClearTarget();
	void RefreshRoute();
	void Stop();

private:
	struct State;
	std::shared_ptr<State> m_state;
};
} // namespace pcp
