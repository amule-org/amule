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
	const auto multicast = boost::asio::ip::make_address_v6("ff02::1");
	ASSERT_FALSE(pcp::ApplicationTarget(true, true, multicast, 4662).has_value());
	ASSERT_TRUE(*selected == address);
}

TEST(PCPApplicationPolicy, NoTargetClosesBothProtocols)
{
	pcp::PinholeOwners owners;
	owners.pcpLease = true;
	owners.igd2Available = true;
	owners.igd2Holds = true;
	ASSERT_TRUE(pcp::DecidePinholeRoute(false, owners) == pcp::PinholeRoute::Close);
	ASSERT_TRUE(pcp::DecidePinholeRoute(false, pcp::PinholeOwners{}) == pcp::PinholeRoute::Close);
}

TEST(PCPApplicationPolicy, PCPLeaseOwnsEvenWhenIGD2Holds)
{
	pcp::PinholeOwners owners;
	owners.pcpLease = true;
	owners.pcpFailed = true;
	owners.igd2Available = true;
	owners.igd2Holds = true;
	ASSERT_TRUE(pcp::DecidePinholeRoute(true, owners) == pcp::PinholeRoute::PCP);
}

TEST(PCPApplicationPolicy, IGD2PinholeOwnsWithoutPCPLease)
{
	pcp::PinholeOwners owners;
	owners.igd2Available = true;
	owners.igd2Holds = true;
	ASSERT_TRUE(pcp::DecidePinholeRoute(true, owners) == pcp::PinholeRoute::IGD2);
	owners.pcpFailed = true;
	ASSERT_TRUE(pcp::DecidePinholeRoute(true, owners) == pcp::PinholeRoute::IGD2);
}

TEST(PCPApplicationPolicy, AsksPCPFirstAndIGD2OnlyAfterPCPFailed)
{
	pcp::PinholeOwners owners;
	owners.igd2Available = true;
	ASSERT_TRUE(pcp::DecidePinholeRoute(true, owners) == pcp::PinholeRoute::PCP);
	owners.pcpFailed = true;
	ASSERT_TRUE(pcp::DecidePinholeRoute(true, owners) == pcp::PinholeRoute::IGD2Fallback);
	// Without an IGD2 service there is nothing to fall back to: PCP keeps asking.
	owners.igd2Available = false;
	ASSERT_TRUE(pcp::DecidePinholeRoute(true, owners) == pcp::PinholeRoute::PCP);
}
