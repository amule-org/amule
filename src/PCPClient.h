#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/udp.hpp>

namespace pcp
{

struct LeaseSnapshot
{
	bool active = false;
	std::uint16_t internalPort = 0;
	std::uint32_t grantedLifetime = 0;
	std::uint64_t generation = 0;
};

class Client
{
public:
	Client(const boost::asio::ip::udp::endpoint &gateway,
		const boost::asio::ip::address_v6 &localAddress);
	~Client();
	Client(const Client &) = delete;
	Client &operator=(const Client &) = delete;

	// Replace the desired TCP mapping. A zero target port disables mapping.
	void SetMapping(std::uint16_t internalPort,
		const boost::asio::ip::address_v6 &externalAddress,
		std::uint16_t externalPort);
	void ClearMapping();
	LeaseSnapshot Snapshot() const;
	void Stop();

private:
	struct State;
	std::shared_ptr<State> m_state;
};

} // namespace pcp
