// This file is part of the aMule Project.
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#include <muleunit/test.h>
#include "IPv6PinholePolicy.h"
#include <IPv6PinholePolicy.h>

using namespace muleunit;

DECLARE_SIMPLE(IPv6PinholePolicy)

TEST(IPv6PinholePolicy, RequiresIPv6ListenerAndBackend)
{
	ASSERT_FALSE(ShouldRequestIPv6Pinhole(false, true, true));
	ASSERT_FALSE(ShouldRequestIPv6Pinhole(true, false, true));
	ASSERT_FALSE(ShouldRequestIPv6Pinhole(true, true, false));
	ASSERT_TRUE(ShouldRequestIPv6Pinhole(true, true, true));
}

TEST(IPv6PinholePolicy, RenewsAtOrInsideMargin)
{
	ASSERT_FALSE(ShouldRenewIPv6Pinhole(61, 60));
	ASSERT_TRUE(ShouldRenewIPv6Pinhole(60, 60));
	ASSERT_TRUE(ShouldRenewIPv6Pinhole(59, 60));
}

TEST(IPv6PinholePolicy, ReplacesWhenPortOrAddressChanges)
{
	ASSERT_FALSE(IsIPv6PinholeStale(4662, "2001:db8::1", 4662, "2001:db8::1"));
	ASSERT_TRUE(IsIPv6PinholeStale(4663, "2001:db8::1", 4662, "2001:db8::1"));
	ASSERT_TRUE(IsIPv6PinholeStale(4662, "2001:db8::2", 4662, "2001:db8::1"));
}
