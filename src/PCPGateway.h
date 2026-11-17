#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/udp.hpp>

namespace pcp
{

struct Route
{
	std::array<std::uint8_t, 16> destination{};
	std::uint8_t prefixLength = 0;
	std::array<std::uint8_t, 16> gateway{};
	unsigned int interfaceIndex = 0;
	std::uint32_t metric = 0;
	bool isDefault = false;
};

struct LocalAddress
{
	std::array<std::uint8_t, 16> address{};
	unsigned int interfaceIndex = 0;
};

struct Gateway
{
	boost::asio::ip::address_v6 localAddress;
	boost::asio::ip::udp::endpoint endpoint;
};

// Select the lowest-metric default route whose interface owns the selected address bytes.
std::optional<Gateway> SelectGateway(const std::vector<Route> &routes,
	const std::vector<LocalAddress> &addresses,
	const std::array<std::uint8_t, 16> &selectedAddress);

// Discover the native default IPv6 router for an explicitly selected local address.
std::optional<Gateway> DiscoverGateway(const boost::asio::ip::address_v6 &selectedAddress);

namespace detail
{
// Parse native-endian Linux netlink route-dump datagrams; invalid/incomplete dumps do not mutate routes.
bool ParseLinuxRouteDump(
	const std::vector<std::uint8_t> &datagram, std::uint32_t sequence, std::vector<Route> &routes);
} // namespace detail

} // namespace pcp
