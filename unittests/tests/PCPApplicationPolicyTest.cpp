#include <muleunit/test.h>
#include "PCPApplicationPolicy.h"

using namespace muleunit;
DECLARE_SIMPLE(PCPApplicationPolicy)

TEST(PCPApplicationPolicy, RequiresEnabledIPv6ListenerAddressAndTCPPort)
{
	const auto address = boost::asio::ip::make_address_v6("2001:db8::2");
	const std::optional<boost::asio::ip::address_v6> selected = address;
	const auto valid = pcp::ApplicationTarget(true, true, selected, 4662);
	ASSERT_TRUE(valid.has_value());
	ASSERT_TRUE(*valid == address);
	ASSERT_FALSE(pcp::ApplicationTarget(false, true, selected, 4662).has_value());
	ASSERT_FALSE(pcp::ApplicationTarget(true, false, selected, 4662).has_value());
	ASSERT_FALSE(pcp::ApplicationTarget(true, true, std::nullopt, 4662).has_value());
	ASSERT_FALSE(pcp::ApplicationTarget(true, true, selected, 0).has_value());
	ASSERT_FALSE(pcp::ApplicationTarget(true, true, boost::asio::ip::address_v6{}, 4662).has_value());
	ASSERT_FALSE(pcp::ApplicationTarget(true, true, boost::asio::ip::make_address_v6("ff02::1"), 4662)
			.has_value());
	ASSERT_TRUE(*selected == address);
}
