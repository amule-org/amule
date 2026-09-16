// This file is part of the aMule Project.
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#include <muleunit/test.h>
#include "ClientList.h"

using namespace muleunit;

static CNetworkAddress IPv6(uint8_t suffix)
{
	CNetworkAddress::Octets bytes{};
	bytes[0] = 0x20;
	bytes[1] = 0x01;
	bytes[15] = suffix;
	return CNetworkAddress::IPv6FromOctets(bytes);
}

static CNetworkAddress MappedIPv4()
{
	CNetworkAddress::Octets bytes{};
	bytes[10] = bytes[11] = 0xff;
	bytes[12] = 192;
	bytes[14] = 2;
	bytes[15] = 1;
	return CNetworkAddress::IPv6FromOctets(bytes);
}

DECLARE_SIMPLE(ClientList)

TEST(ClientList, TrackedIPv6EndpointsKeepSeparateHashes)
{
	CTrackedClientRecord record;
	int first, second;
	ASSERT_TRUE(record.Add(IPv6(1), 4662, &first, 100));
	ASSERT_TRUE(record.Add(IPv6(2), 4662, &second, 100));
	ASSERT_TRUE(record.Compare(IPv6(1), 4662, &first));
	ASSERT_FALSE(record.Compare(IPv6(1), 4662, &second));
	ASSERT_TRUE(record.Compare(IPv6(2), 4662, &second));
	ASSERT_FALSE(record.Compare(IPv6(2), 4662, &first));
	ASSERT_TRUE(record.Compare(IPv6(1), 4663, &second));
}

TEST(ClientList, TrackedMappedAndPlainIPv4ShareHistory)
{
	CTrackedClientRecord record;
	int first, second;
	const auto plain = CNetworkAddress::FromIPv4NetworkOrder(0x010200c0);
	ASSERT_TRUE(record.Add(MappedIPv4(), 4662, &first, 100));
	ASSERT_TRUE(record.Compare(plain, 4662, &first));
	ASSERT_FALSE(record.Compare(plain, 4662, &second));
	ASSERT_TRUE(record.Add(plain, 4662, &second, 200));
	ASSERT_TRUE(record.Compare(MappedIPv4(), 4662, &second));
	ASSERT_FALSE(record.Compare(MappedIPv4(), 4662, &first));
}

TEST(ClientList, TrackedAbsenceNeverStoresHistory)
{
	CTrackedClientRecord record;
	int first, second;
	ASSERT_FALSE(record.Add(CNetworkAddress::Absent(), 4662, &first, 100));
	ASSERT_TRUE(record.Compare(CNetworkAddress::Absent(), 4662, &second));
	ASSERT_TRUE(record.Add(IPv6(1), 4662, &first, 100));
	ASSERT_TRUE(record.Compare(CNetworkAddress::Absent(), 4662, &second));
}

TEST(ClientList, TrackedRefreshExpiryAndClearPreserveLegacyBoundaries)
{
	CTrackedClientRecord record;
	int first, second;
	record.Add(IPv6(1), 4662, &first, 100);
	record.Add(IPv6(1), 4663, &second, 200);
	record.DropLapsed(300, 100);
	ASSERT_FALSE(record.Compare(IPv6(1), 4662, &second));
	record.DropLapsed(301, 100);
	ASSERT_TRUE(record.Compare(IPv6(1), 4662, &second));
	record.Add(IPv6(1), 4662, &first, 400);
	record.Clear();
	ASSERT_TRUE(record.Compare(IPv6(1), 4662, &second));
}
