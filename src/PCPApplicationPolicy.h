#pragma once

#include <boost/asio.hpp>
#include <optional>

namespace pcp
{
inline std::optional<boost::asio::ip::address_v6> ApplicationTarget(bool enabled,
	bool listenerAcceptsIPv6,
	const std::optional<boost::asio::ip::address_v6> &address,
	unsigned short port)
{
	if (!enabled || !listenerAcceptsIPv6 || !address || address->is_unspecified() ||
		address->is_multicast() || address->is_v4_mapped() || port == 0) {
		return std::nullopt;
	}
	return address;
}
} // namespace pcp
