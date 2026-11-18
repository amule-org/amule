// This file is part of the aMule Project.
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#include <muleunit/test.h>
#include "PCPGateway.h"

#include <cstring>

#ifdef __APPLE__
#include <net/route.h>
#include <netinet/in.h>
#endif

using namespace muleunit;
using namespace pcp;

DECLARE_SIMPLE(PCPGateway)

namespace
{
const std::array<std::uint8_t, 16> Local = { 0x20, 1, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
const std::array<std::uint8_t, 16> RouterA = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 1 };
const std::array<std::uint8_t, 16> RouterB = { 0x20, 1, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2 };

Route Default(const std::array<std::uint8_t, 16> &router, unsigned int index, std::uint32_t metric)
{
	Route route{};
	route.gateway = router;
	route.interfaceIndex = index;
	route.metric = metric;
	route.isDefault = true;
	return route;
}
} // namespace

TEST(PCPGateway, SelectsLowestMetricOnSelectedAddressInterface)
{
	const std::vector<Route> routes = { Default(RouterB, 7, 100), Default(RouterA, 3, 10) };
	const std::vector<LocalAddress> addresses = { { Local, 3 }, { Local, 7 } };
	const auto result = SelectGateway(routes, addresses, Local);
	ASSERT_TRUE(result.has_value());
	ASSERT_TRUE(result->localAddress.to_bytes() == Local);
	ASSERT_TRUE(result->endpoint.address().to_v6().to_bytes() == RouterA);
	ASSERT_EQUALS(3U, result->endpoint.address().to_v6().scope_id());
	ASSERT_EQUALS(5351, result->endpoint.port());
}

TEST(PCPGateway, RejectsWrongInterfaceAndNonDefaultRoutes)
{
	auto route = Default(RouterA, 3, 1);
	route.isDefault = false;
	ASSERT_FALSE(SelectGateway({ route }, { { Local, 7 } }, Local).has_value());
	ASSERT_FALSE(SelectGateway({ Default(RouterA, 3, 1) }, { { Local, 7 } }, Local).has_value());
}

TEST(PCPGateway, RejectsUnusableGatewaysAndBreaksMetricTiesDeterministically)
{
	std::array<std::uint8_t, 16> zero{};
	std::array<std::uint8_t, 16> multicast{};
	multicast[0] = 0xff;
	ASSERT_FALSE(SelectGateway({ Default(zero, 3, 1) }, { { Local, 3 } }, Local).has_value());
	ASSERT_FALSE(SelectGateway({ Default(multicast, 3, 1) }, { { Local, 3 } }, Local).has_value());
	const auto result =
		SelectGateway({ Default(RouterB, 3, 5), Default(RouterA, 3, 5) }, { { Local, 3 } }, Local);
	ASSERT_TRUE(result.has_value());
	ASSERT_TRUE(result->endpoint.address().to_v6().to_bytes() == RouterB);
}

namespace
{
template <typename T> void PutNative(std::vector<std::uint8_t> &bytes, std::size_t offset, T value)
{
	std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

std::vector<std::uint8_t> LinuxMessage(std::uint16_t type, std::uint32_t size)
{
	std::vector<std::uint8_t> bytes(size, 0);
	PutNative(bytes, 0, size);
	PutNative(bytes, 4, type);
	PutNative(bytes, 8, std::uint32_t{ 77 });
	return bytes;
}

std::vector<std::uint8_t> LinuxRoute()
{
	auto bytes = LinuxMessage(24, 64); // RTM_NEWROUTE, native Linux layout.
	bytes[16] = 10;                    // Linux AF_INET6, independent of the test host's socket constants.
	bytes[20] = 254;
	bytes[23] = 1; // RTN_UNICAST.
	PutNative(bytes, 28, std::uint16_t{ 20 });
	PutNative(bytes, 30, std::uint16_t{ 5 }); // RTA_GATEWAY.
	std::memcpy(bytes.data() + 32, RouterA.data(), RouterA.size());
	PutNative(bytes, 48, std::uint16_t{ 8 });
	PutNative(bytes, 50, std::uint16_t{ 4 }); // RTA_OIF.
	PutNative(bytes, 52, std::uint32_t{ 3 });
	PutNative(bytes, 56, std::uint16_t{ 8 });
	PutNative(bytes, 58, std::uint16_t{ 6 }); // RTA_PRIORITY.
	PutNative(bytes, 60, std::uint32_t{ 10 });
	return bytes;
}
} // namespace

TEST(PCPGateway, ParsesLinuxRouteAndTerminalStatus)
{
	auto bytes = LinuxRoute();
	const auto done = LinuxMessage(3, 20); // NLMSG_DONE includes a zero status word.
	bytes.insert(bytes.end(), done.begin(), done.end());
	std::vector<Route> routes;
	ASSERT_TRUE(detail::ParseLinuxRouteDump(bytes, 77, routes));
	ASSERT_EQUALS(1U, routes.size());
	ASSERT_TRUE(routes[0].gateway == RouterA);
	ASSERT_EQUALS(3U, routes[0].interfaceIndex);
	ASSERT_EQUALS(10U, routes[0].metric);
	ASSERT_TRUE(routes[0].isDefault);
}

TEST(PCPGateway, AcceptsEmptyCompletedLinuxDump)
{
	for (const auto size : { 16U, 20U }) {
		std::vector<Route> routes;
		ASSERT_TRUE(detail::ParseLinuxRouteDump(LinuxMessage(3, size), 77, routes));
		ASSERT_TRUE(routes.empty());
	}
}

TEST(PCPGateway, RejectsMalformedInterruptedAndIncompleteLinuxDumpsWithoutMutation)
{
	const auto route = LinuxRoute();
	const auto done = LinuxMessage(3, 20);
	auto complete = route;
	complete.insert(complete.end(), done.begin(), done.end());
	std::vector<std::vector<std::uint8_t>> badPackets;
	for (std::size_t n = 0; n < complete.size(); ++n) {
		badPackets.emplace_back(complete.begin(), complete.begin() + n);
	}
	auto interrupted = complete;
	PutNative(interrupted, route.size() + 6, std::uint16_t{ 0x10 }); // NLM_F_DUMP_INTR.
	badPackets.push_back(interrupted);
	auto error = LinuxMessage(2, 20);
	PutNative(error, 16, std::int32_t{ -1 });
	badPackets.push_back(error);
	auto failedDone = done;
	PutNative(failedDone, 16, std::int32_t{ -1 });
	badPackets.push_back(failedDone);
	auto badAttribute = complete;
	PutNative(badAttribute, 28, std::uint16_t{ 3 });
	badPackets.push_back(badAttribute);
	auto oversizedAttribute = complete;
	PutNative(oversizedAttribute, 28, std::uint16_t{ 100 });
	badPackets.push_back(oversizedAttribute);

	for (const auto &bytes : badPackets) {
		std::vector<Route> routes{ Default(RouterB, 7, 100) };
		ASSERT_FALSE(detail::ParseLinuxRouteDump(bytes, 77, routes));
		ASSERT_EQUALS(1U, routes.size());
		ASSERT_TRUE(routes[0].gateway == RouterB);
		ASSERT_EQUALS(7U, routes[0].interfaceIndex);
		ASSERT_EQUALS(100U, routes[0].metric);
	}
}

#ifdef __APPLE__
namespace
{
// One PF_ROUTE message: a default route whose destination is the full 28-byte
// sockaddr_in6 the kernel emits. XNU packs sockaddrs on 4-byte boundaries, so the
// gateway that follows must be parsed immediately after those 28 bytes; rounding
// the length up to sizeof(long) would advance 4 bytes too far and misread it.
std::vector<std::uint8_t> MacRouteWith28ByteDestination()
{
	std::vector<std::uint8_t> bytes(sizeof(rt_msghdr), 0);
	auto *header = reinterpret_cast<rt_msghdr *>(bytes.data());
	header->rtm_version = RTM_VERSION;
	header->rtm_type = RTM_GET;
	header->rtm_index = 3;
	header->rtm_flags = RTF_UP | RTF_GATEWAY; // No RTF_HOST: a network default route.
	header->rtm_addrs = RTA_DST | RTA_GATEWAY;

	sockaddr_in6 destination{};
	destination.sin6_len = sizeof(destination); // 28 bytes.
	destination.sin6_family = AF_INET6;         // Unspecified address: the default route.
	sockaddr_in6 gatewaySockaddr{};
	gatewaySockaddr.sin6_len = sizeof(gatewaySockaddr);
	gatewaySockaddr.sin6_family = AF_INET6;
	std::memcpy(&gatewaySockaddr.sin6_addr, RouterA.data(), RouterA.size());

	const auto append = [&bytes](const sockaddr_in6 &sockaddr) {
		const auto *raw = reinterpret_cast<const std::uint8_t *>(&sockaddr);
		bytes.insert(bytes.end(), raw, raw + sizeof(sockaddr));
	};
	append(destination);
	append(gatewaySockaddr);
	// Re-take the header pointer: the inserts above may have reallocated the buffer.
	reinterpret_cast<rt_msghdr *>(bytes.data())->rtm_msglen = static_cast<u_short>(bytes.size());
	return bytes;
}
} // namespace

TEST(PCPGateway, ParsesGatewayAfterPacked28ByteMacDestination)
{
	std::vector<Route> routes;
	std::vector<std::uint8_t> dump = MacRouteWith28ByteDestination();
	ASSERT_TRUE(detail::ParseMacRouteDump(dump, routes));
	ASSERT_EQUALS(1U, routes.size());
	ASSERT_TRUE(routes[0].gateway == RouterA);
	ASSERT_EQUALS(3U, routes[0].interfaceIndex);
	ASSERT_TRUE(routes[0].isDefault);
}
#endif
