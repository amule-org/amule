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

#include "PCPGateway.h"
#include "NetworkInterfaces.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#ifdef __linux__
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <net/route.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#elif defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <netioapi.h>
#include <iphlpapi.h>
#endif

// Discovery deadlines (the bounded netlink poll) assume a monotonic clock.
static_assert(std::chrono::steady_clock::is_steady, "discovery deadlines need a steady clock");

namespace pcp
{
namespace
{
constexpr std::uint16_t NetlinkDone = 3;
constexpr std::uint16_t NetlinkError = 2;
constexpr std::uint16_t RouteNew = 24;
constexpr std::uint16_t RouteGateway = 5;
constexpr std::uint16_t RouteOif = 4;
constexpr std::uint16_t RoutePriority = 6;
constexpr std::uint8_t RouteUnicast = 1;
} // namespace

namespace
{
using Bytes = std::array<std::uint8_t, 16>;
bool Zero(const Bytes &bytes)
{
	return std::all_of(bytes.begin(), bytes.end(), [](auto x) { return x == 0; });
}
bool Multicast(const Bytes &bytes)
{
	return bytes[0] == 0xff;
}
bool LinkLocal(const Bytes &bytes)
{
	return bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80;
}
} // namespace

namespace detail
{
namespace
{
template <typename T> bool Read(const std::vector<std::uint8_t> &buffer, std::size_t offset, T &value)
{
	if (offset > buffer.size() || buffer.size() - offset < sizeof(T))
		return false;
	std::memcpy(&value, buffer.data() + offset, sizeof(T));
	return true;
}
} // namespace
bool ParseLinuxRouteDump(
	const std::vector<std::uint8_t> &dump, std::uint32_t sequence, std::vector<Route> &routes)
{
	std::vector<Route> parsed;
	std::size_t offset = 0;
	bool done = false;
	while (offset < dump.size()) {
		std::uint32_t length = 0;
		std::uint16_t type = 0, flags = 0;
		std::uint32_t messageSequence = 0;
		if (!Read(dump, offset, length) || !Read(dump, offset + 4, type) ||
			!Read(dump, offset + 6, flags) || !Read(dump, offset + 8, messageSequence) ||
			length < 16 || length > dump.size() - offset)
			return false;
		if (messageSequence == sequence && (flags & 0x10) != 0) { // NLM_F_DUMP_INTR.
			return false;
		}
		if (messageSequence == sequence && type == NetlinkDone) {
			std::int32_t status = 0;
			if (length != 16 &&
				(length < 20 || !Read(dump, offset + 16, status) || status != 0)) {
				return false;
			}
			done = true;
			offset += length;
			break;
		}
		if (messageSequence == sequence && type == NetlinkError)
			return false;
		if (messageSequence == sequence && type == RouteNew) {
			if (length < 28)
				return false;
			const auto *payload = dump.data() + offset + 16;
			// Netlink's address-family and attribute values are Linux ABI, not the host's BSD
			// values.
			if (payload[0] == 10 && payload[1] == 0 && payload[2] == 0 &&
				payload[7] == RouteUnicast) {
				Route route{};
				route.isDefault = true;
				std::size_t attrOffset = offset + 28;
				while (attrOffset < offset + length) {
					std::uint16_t attrLength = 0, attrType = 0;
					if (!Read(dump, attrOffset, attrLength) ||
						!Read(dump, attrOffset + 2, attrType) || attrLength < 4 ||
						attrLength > offset + length - attrOffset)
						return false;
					const auto attrPayload = attrLength - 4;
					if (attrType == RouteGateway) {
						if (attrPayload != 16)
							return false;
						std::memcpy(route.gateway.data(),
							dump.data() + attrOffset + 4,
							16);
					}
					if (attrType == RouteOif) {
						if (attrPayload != 4)
							return false;
						std::memcpy(&route.interfaceIndex,
							dump.data() + attrOffset + 4,
							4);
					}
					if (attrType == RoutePriority) {
						if (attrPayload != 4)
							return false;
						std::memcpy(&route.metric, dump.data() + attrOffset + 4, 4);
					}
					attrOffset += (attrLength + 3u) & ~std::size_t(3);
					if (attrOffset > offset + length)
						return false;
				}
				if (route.interfaceIndex && !Zero(route.gateway))
					parsed.push_back(route);
			}
		}
		const std::size_t aligned = (static_cast<std::size_t>(length) + 3u) & ~std::size_t(3);
		if (aligned > dump.size() - offset) {
			if (offset + length == dump.size())
				offset += length;
			else
				return false;
		} else
			offset += aligned;
	}
	if (!done || offset != dump.size())
		return false;
	routes.insert(routes.end(), parsed.begin(), parsed.end());
	return true;
}

#if defined(__APPLE__)
// Parse a native macOS PF_ROUTE sysctl route dump; invalid dumps do not mutate routes.
bool ParseMacRouteDump(const std::vector<unsigned char> &dump, std::vector<Route> &routes)
{
	std::vector<Route> parsed;
	for (const auto *message = dump.data(), *dumpEnd = message + dump.size(); message < dumpEnd;) {
		if (static_cast<std::size_t>(dumpEnd - message) < sizeof(rt_msghdr))
			return false;
		const auto *header = reinterpret_cast<const rt_msghdr *>(message);
		if (header->rtm_msglen < sizeof(*header) || header->rtm_msglen > dumpEnd - message)
			return false;
		if (header->rtm_version == RTM_VERSION && (header->rtm_flags & RTF_GATEWAY) &&
			(header->rtm_flags & RTF_UP) && !(header->rtm_flags & RTF_HOST)) {
			const auto *sockaddrs = message + sizeof(*header);
			const auto *messageEnd = message + header->rtm_msglen;
			const sockaddr_in6 *destination = nullptr, *gateway = nullptr;
			bool defaultMask = true;
			unsigned int addressFlag = 1;
			for (int slot = 0; slot < RTAX_MAX; ++slot, addressFlag <<= 1)
				if (header->rtm_addrs & addressFlag) {
					if (sockaddrs >= messageEnd)
						return false;
					const auto *sockaddr =
						reinterpret_cast<const ::sockaddr *>(sockaddrs);
					if (static_cast<std::size_t>(messageEnd - sockaddrs) < 2)
						return false;
					// XNU packs sockaddrs on 4-byte boundaries (ROUNDUP32), not on the
					// ABI's sizeof(long); rounding to 8 bytes misreads every sockaddr
					// that follows a 28-byte sockaddr_in6.
					std::size_t roundedLength =
						sockaddr->sa_len
							? ((sockaddr->sa_len + sizeof(std::uint32_t) - 1) &
								  ~(sizeof(std::uint32_t) - 1))
							: sizeof(std::uint32_t);
					if (roundedLength > static_cast<std::size_t>(messageEnd - sockaddrs))
						return false;
					if (sockaddr->sa_family == AF_INET6 &&
						sockaddr->sa_len >= sizeof(sockaddr_in6)) {
						if (slot == RTAX_DST)
							destination = reinterpret_cast<const sockaddr_in6 *>(
								sockaddr);
						if (slot == RTAX_GATEWAY)
							gateway = reinterpret_cast<const sockaddr_in6 *>(
								sockaddr);
					}
					if (slot == RTAX_NETMASK) {
						for (std::size_t byte = 8;
							byte < std::min<std::size_t>(sockaddr->sa_len, 24);
							++byte) {
							defaultMask = defaultMask && sockaddrs[byte] == 0;
						}
					}
					sockaddrs += roundedLength;
				}
			if (destination && gateway && defaultMask &&
				IN6_IS_ADDR_UNSPECIFIED(&destination->sin6_addr)) {
				Route route{};
				memcpy(route.gateway.data(), &gateway->sin6_addr, 16);
				if (LinkLocal(route.gateway)) {
					// BSD embeds the scope in address bytes; Asio needs a clean address
					// plus scope_id.
					route.gateway[2] = 0;
					route.gateway[3] = 0;
				}
				route.interfaceIndex = header->rtm_index;
				route.metric = static_cast<std::uint32_t>(header->rtm_rmx.rmx_hopcount);
				route.isDefault = true;
				parsed.push_back(route);
			}
		}
		message += header->rtm_msglen;
	}
	routes.insert(routes.end(), parsed.begin(), parsed.end());
	return true;
}
#endif
} // namespace detail

std::optional<Gateway> SelectGateway(
	const std::vector<Route> &routes, const std::vector<LocalAddress> &addresses, const Bytes &selected)
{
	if (Zero(selected) || Multicast(selected))
		return std::nullopt;
	std::optional<unsigned int> index;
	for (const auto &localAddress : addresses)
		if (localAddress.address == selected && localAddress.interfaceIndex &&
			(!index || localAddress.interfaceIndex < *index))
			index = localAddress.interfaceIndex;
	if (!index)
		return std::nullopt;
	const Route *best = nullptr;
	for (const auto &route : routes)
		if (route.isDefault && route.interfaceIndex == *index && !Zero(route.gateway) &&
			!Multicast(route.gateway) &&
			(!best || route.metric < best->metric ||
				(route.metric == best->metric && route.gateway < best->gateway)))
			best = &route;
	if (!best)
		return std::nullopt;
	auto gw = boost::asio::ip::address_v6(best->gateway);
	if (LinkLocal(best->gateway))
		gw.scope_id(*index);
	return Gateway{ boost::asio::ip::address_v6(selected), { gw, 5351 } };
}

#if defined(__linux__)
namespace
{
bool ReadLinuxRoutes(std::vector<Route> &routes)
{
	int socketFd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (socketFd < 0)
		return false;
	struct
	{
		nlmsghdr header;
		rtmsg routeMessage;
	} request{};
	request.header.nlmsg_len = NLMSG_LENGTH(sizeof(rtmsg));
	request.header.nlmsg_type = RTM_GETROUTE;
	request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	request.header.nlmsg_seq = 1;
	request.routeMessage.rtm_family = AF_INET6;
	sockaddr_nl kernel{};
	kernel.nl_family = AF_NETLINK;
	if (sendto(socketFd,
		    &request,
		    request.header.nlmsg_len,
		    0,
		    reinterpret_cast<sockaddr *>(&kernel),
		    sizeof(kernel)) != static_cast<ssize_t>(request.header.nlmsg_len)) {
		close(socketFd);
		return false;
	}
	std::vector<std::uint8_t> dump;
	// Receive into heap storage: a 64 KiB buffer must not sit on a worker thread's
	// stack (musl's default thread stack is 128 KiB).
	std::vector<std::uint8_t> buffer(65536);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	bool done = false, ok = true;
	while (!done && ok) {
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
			deadline - std::chrono::steady_clock::now())
					  .count();
		if (left <= 0) {
			ok = false;
			break;
		}
		pollfd ready{ socketFd, POLLIN, 0 };
		if (poll(&ready, 1, static_cast<int>(left)) <= 0 ||
			(ready.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
			ok = false;
			break;
		}
		sockaddr_nl sender{};
		socklen_t senderLength = sizeof(sender);
		const auto count = recvfrom(socketFd,
			buffer.data(),
			buffer.size(),
			MSG_DONTWAIT,
			reinterpret_cast<sockaddr *>(&sender),
			&senderLength);
		if (count <= 0 || senderLength != sizeof(sender) || sender.nl_pid != 0 ||
			static_cast<std::size_t>(count) > 1024 * 1024 - dump.size()) {
			ok = false;
			break;
		}
		const auto size = static_cast<std::size_t>(count);
		for (std::size_t offset = 0; offset < size;) {
			if (size - offset < sizeof(nlmsghdr)) {
				ok = false;
				break;
			}
			nlmsghdr header{};
			std::memcpy(&header, buffer.data() + offset, sizeof(header));
			if (header.nlmsg_len < sizeof(header) || header.nlmsg_len > size - offset) {
				ok = false;
				break;
			}
			if (header.nlmsg_seq == 1 && header.nlmsg_type == NLMSG_DONE) {
				done = true;
			}
			const auto aligned =
				(static_cast<std::size_t>(header.nlmsg_len) + 3) & ~std::size_t{ 3 };
			if (aligned > size - offset) {
				ok = offset + header.nlmsg_len == size;
				break;
			}
			offset += aligned;
		}
		auto received = std::next(buffer.begin(), static_cast<std::ptrdiff_t>(size));
		dump.insert(dump.end(), buffer.begin(), received);
	}
	close(socketFd);
	return ok && done && detail::ParseLinuxRouteDump(dump, 1, routes);
}
} // namespace
#elif defined(__APPLE__)
namespace
{
bool ReadMacRoutes(std::vector<Route> &routes)
{
	int mib[6] = { CTL_NET, PF_ROUTE, 0, AF_INET6, NET_RT_DUMP, 0 };
	std::size_t dumpSize = 0;
	if (sysctl(mib, 6, nullptr, &dumpSize, nullptr, 0) || !dumpSize)
		return false;
	std::vector<unsigned char> dump(dumpSize);
	if (sysctl(mib, 6, dump.data(), &dumpSize, nullptr, 0))
		return false;
	return detail::ParseMacRouteDump(dump, routes);
}
} // namespace
#elif defined(_WIN32)
namespace
{
bool ReadWindowsRoutes(std::vector<Route> &routes)
{
	PMIB_IPFORWARD_TABLE2 table = nullptr;
	if (GetIpForwardTable2(AF_INET6, &table) != NO_ERROR || table == nullptr) {
		return false;
	}
	for (ULONG i = 0; i < table->NumEntries; ++i) {
		const auto &entry = table->Table[i];
		if (entry.DestinationPrefix.PrefixLength != 0 ||
			entry.DestinationPrefix.Prefix.si_family != AF_INET6 ||
			entry.NextHop.si_family != AF_INET6 || entry.InterfaceIndex == 0 ||
			entry.ValidLifetime == 0) {
			continue;
		}
		Route route{};
		std::memcpy(route.gateway.data(), &entry.NextHop.Ipv6.sin6_addr, 16);
		route.interfaceIndex = entry.InterfaceIndex;
		route.metric = entry.Metric;
		route.isDefault = true;
		routes.push_back(route);
	}
	FreeMibTable(table);
	return true;
}
} // namespace
#endif

std::optional<Gateway> DiscoverGateway(const boost::asio::ip::address_v6 &selected)
{
	std::vector<Route> routes;
	std::vector<LocalAddress> localAddresses;
	bool ok = false;
#if defined(__linux__)
	ok = ReadLinuxRoutes(routes);
#elif defined(__APPLE__)
	ok = ReadMacRoutes(routes);
#elif defined(_WIN32)
	ok = ReadWindowsRoutes(routes);
#endif
	if (!ok)
		return std::nullopt;
	for (const auto &networkInterface : DetectNetworkInterfaces())
		for (const auto &ipv6Address : networkInterface.ipv6)
			localAddresses.push_back({ ipv6Address.bytes, networkInterface.index });
	return SelectGateway(routes, localAddresses, selected.to_bytes());
}
} // namespace pcp
