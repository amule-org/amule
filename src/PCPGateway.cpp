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
bool Zero(const Bytes &b)
{
	return std::all_of(b.begin(), b.end(), [](auto x) { return x == 0; });
}
bool Multicast(const Bytes &b)
{
	return b[0] == 0xff;
}
bool LinkLocal(const Bytes &b)
{
	return b[0] == 0xfe && (b[1] & 0xc0) == 0x80;
}
} // namespace

namespace detail
{
namespace
{
template <typename T> bool Read(const std::vector<std::uint8_t> &b, std::size_t o, T &v)
{
	if (o > b.size() || b.size() - o < sizeof(T))
		return false;
	std::memcpy(&v, b.data() + o, sizeof(T));
	return true;
}
} // namespace
bool ParseLinuxRouteDump(const std::vector<std::uint8_t> &b, std::uint32_t seq, std::vector<Route> &out)
{
	std::vector<Route> parsed;
	std::size_t offset = 0;
	bool done = false;
	while (offset < b.size()) {
		std::uint32_t length = 0;
		std::uint16_t type = 0, flags = 0;
		std::uint32_t msgseq = 0;
		if (!Read(b, offset, length) || !Read(b, offset + 4, type) || !Read(b, offset + 6, flags) ||
			!Read(b, offset + 8, msgseq) || length < 16 || length > b.size() - offset)
			return false;
		if (msgseq == seq && (flags & 0x10) != 0) { // NLM_F_DUMP_INTR.
			return false;
		}
		if (msgseq == seq && type == NetlinkDone) {
			std::int32_t status = 0;
			if (length != 16 && (length < 20 || !Read(b, offset + 16, status) || status != 0)) {
				return false;
			}
			done = true;
			offset += length;
			break;
		}
		if (msgseq == seq && type == NetlinkError)
			return false;
		if (msgseq == seq && type == RouteNew) {
			if (length < 28)
				return false;
			const auto *p = b.data() + offset + 16;
			// Netlink's address-family and attribute values are Linux ABI, not the host's BSD
			// values.
			if (p[0] == 10 && p[1] == 0 && p[2] == 0 && p[7] == RouteUnicast) {
				Route r{};
				r.isDefault = true;
				std::size_t a = offset + 28;
				while (a < offset + length) {
					std::uint16_t alen = 0, atype = 0;
					if (!Read(b, a, alen) || !Read(b, a + 2, atype) || alen < 4 ||
						alen > offset + length - a)
						return false;
					const auto n = alen - 4;
					if (atype == RouteGateway) {
						if (n != 16)
							return false;
						std::memcpy(r.gateway.data(), b.data() + a + 4, 16);
					}
					if (atype == RouteOif) {
						if (n != 4)
							return false;
						std::memcpy(&r.interfaceIndex, b.data() + a + 4, 4);
					}
					if (atype == RoutePriority) {
						if (n != 4)
							return false;
						std::memcpy(&r.metric, b.data() + a + 4, 4);
					}
					a += (alen + 3u) & ~std::size_t(3);
					if (a > offset + length)
						return false;
				}
				if (r.interfaceIndex && !Zero(r.gateway))
					parsed.push_back(r);
			}
		}
		const std::size_t aligned = (static_cast<std::size_t>(length) + 3u) & ~std::size_t(3);
		if (aligned > b.size() - offset) {
			if (offset + length == b.size())
				offset += length;
			else
				return false;
		} else
			offset += aligned;
	}
	if (!done || offset != b.size())
		return false;
	out.insert(out.end(), parsed.begin(), parsed.end());
	return true;
}
} // namespace detail

std::optional<Gateway> SelectGateway(
	const std::vector<Route> &routes, const std::vector<LocalAddress> &addresses, const Bytes &selected)
{
	if (Zero(selected) || Multicast(selected))
		return std::nullopt;
	std::optional<unsigned int> index;
	for (const auto &a : addresses)
		if (a.address == selected && a.interfaceIndex && (!index || a.interfaceIndex < *index))
			index = a.interfaceIndex;
	if (!index)
		return std::nullopt;
	const Route *best = nullptr;
	for (const auto &r : routes)
		if (r.isDefault && r.interfaceIndex == *index && !Zero(r.gateway) && !Multicast(r.gateway) &&
			(!best || r.metric < best->metric ||
				(r.metric == best->metric && r.gateway < best->gateway)))
			best = &r;
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
	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd < 0)
		return false;
	struct
	{
		nlmsghdr h;
		rtmsg r;
	} req{};
	req.h.nlmsg_len = NLMSG_LENGTH(sizeof(rtmsg));
	req.h.nlmsg_type = RTM_GETROUTE;
	req.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	req.h.nlmsg_seq = 1;
	req.r.rtm_family = AF_INET6;
	sockaddr_nl kernel{};
	kernel.nl_family = AF_NETLINK;
	if (sendto(fd, &req, req.h.nlmsg_len, 0, reinterpret_cast<sockaddr *>(&kernel), sizeof(kernel)) !=
		static_cast<ssize_t>(req.h.nlmsg_len)) {
		close(fd);
		return false;
	}
	std::vector<std::uint8_t> dump;
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
		pollfd ready{ fd, POLLIN, 0 };
		if (poll(&ready, 1, static_cast<int>(left)) <= 0 ||
			(ready.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
			ok = false;
			break;
		}
		std::array<std::uint8_t, 65536> buffer{};
		sockaddr_nl sender{};
		socklen_t senderLength = sizeof(sender);
		const auto count = recvfrom(fd,
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
		dump.insert(dump.end(), buffer.begin(), buffer.begin() + size);
	}
	close(fd);
	return ok && done && detail::ParseLinuxRouteDump(dump, 1, routes);
}
} // namespace
#elif defined(__APPLE__)
namespace
{
bool ReadMacRoutes(std::vector<Route> &routes)
{
	int mib[6] = { CTL_NET, PF_ROUTE, 0, AF_INET6, NET_RT_DUMP, 0 };
	size_t n = 0;
	if (sysctl(mib, 6, nullptr, &n, nullptr, 0) || !n)
		return false;
	std::vector<unsigned char> b(n);
	if (sysctl(mib, 6, b.data(), &n, nullptr, 0))
		return false;
	for (auto *p = b.data(), *end = p + n; p < end;) {
		if (static_cast<size_t>(end - p) < sizeof(rt_msghdr))
			return false;
		auto *m = reinterpret_cast<rt_msghdr *>(p);
		if (m->rtm_msglen < sizeof(*m) || m->rtm_msglen > end - p)
			return false;
		if (m->rtm_version == RTM_VERSION && (m->rtm_flags & RTF_GATEWAY) &&
			(m->rtm_flags & RTF_UP) && !(m->rtm_flags & RTF_HOST)) {
			auto *q = p + sizeof(*m);
			auto *e = p + m->rtm_msglen;
			const sockaddr_in6 *d = nullptr, *g = nullptr;
			bool defaultMask = true;
			unsigned int bit = 1;
			for (int i = 0; i < RTAX_MAX; ++i, bit <<= 1)
				if (m->rtm_addrs & bit) {
					if (q >= e)
						return false;
					auto *sa = reinterpret_cast<sockaddr *>(q);
					if (static_cast<size_t>(e - q) < 2)
						return false;
					size_t z = sa->sa_len ? ((sa->sa_len + sizeof(long) - 1) &
									~(sizeof(long) - 1))
							      : sizeof(long);
					if (z > static_cast<size_t>(e - q))
						return false;
					if (sa->sa_family == AF_INET6 && sa->sa_len >= sizeof(sockaddr_in6)) {
						if (i == RTAX_DST)
							d = reinterpret_cast<sockaddr_in6 *>(sa);
						if (i == RTAX_GATEWAY)
							g = reinterpret_cast<sockaddr_in6 *>(sa);
					}
					if (i == RTAX_NETMASK) {
						for (std::size_t byte = 8;
							byte < std::min<std::size_t>(sa->sa_len, 24);
							++byte) {
							defaultMask = defaultMask && q[byte] == 0;
						}
					}
					q += z;
				}
			if (d && g && defaultMask && IN6_IS_ADDR_UNSPECIFIED(&d->sin6_addr)) {
				Route r{};
				memcpy(r.gateway.data(), &g->sin6_addr, 16);
				if (LinkLocal(r.gateway)) {
					// BSD embeds the scope in address bytes; Asio needs a clean address
					// plus scope_id.
					r.gateway[2] = 0;
					r.gateway[3] = 0;
				}
				r.interfaceIndex = m->rtm_index;
				r.metric = static_cast<std::uint32_t>(m->rtm_rmx.rmx_hopcount);
				r.isDefault = true;
				routes.push_back(r);
			}
		}
		p += m->rtm_msglen;
	}
	return true;
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
	std::vector<Route> r;
	std::vector<LocalAddress> a;
	bool ok = false;
#if defined(__linux__)
	ok = ReadLinuxRoutes(r);
#elif defined(__APPLE__)
	ok = ReadMacRoutes(r);
#elif defined(_WIN32)
	ok = ReadWindowsRoutes(r);
#endif
	if (!ok)
		return std::nullopt;
	for (const auto &i : DetectNetworkInterfaces())
		for (const auto &v : i.ipv6)
			a.push_back({ v.bytes, i.index });
	return SelectGateway(r, a, selected.to_bytes());
}
} // namespace pcp
