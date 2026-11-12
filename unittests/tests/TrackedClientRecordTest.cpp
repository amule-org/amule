// This file is part of the aMule Project.
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#include <muleunit/test.h>
#include "TrackedClientRecord.h"

using namespace muleunit;

static CNetworkAddress IPv6(uint8_t suffix, uint8_t subnet = 0)
{
	CNetworkAddress::Octets bytes{};
	bytes[0] = 0x20;
	bytes[1] = 0x01;
	bytes[7] = subnet;
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

DECLARE_SIMPLE(TrackedClientRecord)

TEST(TrackedClientRecord, TrackedIPv6EndpointsKeepSeparateHashes)
{
	CTrackedClientRecord record;
	int first, second;
	ASSERT_TRUE(record.Add(IPv6(1), 4662, &first, true, 100));
	ASSERT_TRUE(record.Add(IPv6(2), 4662, &second, true, 100));
	ASSERT_TRUE(record.Compare(IPv6(1), 4662, &first, true));
	ASSERT_FALSE(record.Compare(IPv6(1), 4662, &second, true));
	ASSERT_TRUE(record.Compare(IPv6(2), 4662, &second, true));
	ASSERT_FALSE(record.Compare(IPv6(2), 4662, &first, true));
	ASSERT_TRUE(record.Compare(IPv6(1), 4663, &second, true));
}

TEST(TrackedClientRecord, TrackedMappedAndPlainIPv4ShareHistory)
{
	CTrackedClientRecord record;
	int first, second;
	const auto plain = CNetworkAddress::FromIPv4NetworkOrder(0x010200c0);
	ASSERT_TRUE(record.Add(MappedIPv4(), 4662, &first, true, 100));
	ASSERT_TRUE(record.Compare(plain, 4662, &first, true));
	ASSERT_FALSE(record.Compare(plain, 4662, &second, true));
	ASSERT_TRUE(record.Add(plain, 4662, &second, true, 200));
	ASSERT_TRUE(record.Compare(MappedIPv4(), 4662, &second, true));
	ASSERT_FALSE(record.Compare(MappedIPv4(), 4662, &first, true));
}

TEST(TrackedClientRecord, TrackedAbsenceNeverStoresHistory)
{
	CTrackedClientRecord record;
	int first, second;
	ASSERT_FALSE(record.Add(CNetworkAddress::Absent(), 4662, &first, true, 100));
	ASSERT_TRUE(record.Compare(CNetworkAddress::Absent(), 4662, &second, true));
	ASSERT_TRUE(record.Add(IPv6(1), 4662, &first, true, 100));
	ASSERT_TRUE(record.Compare(CNetworkAddress::Absent(), 4662, &second, true));
}

TEST(TrackedClientRecord, TrackedRefreshAndExpiryPreserveLegacyBoundaries)
{
	CTrackedClientRecord record;
	int first, second;
	record.Add(IPv6(1), 4662, &first, true, 100);
	record.Add(IPv6(1), 4663, &second, true, 200);
	record.DropLapsed(300, 100);
	ASSERT_FALSE(record.Compare(IPv6(1), 4662, &second, true));
	record.DropLapsed(301, 100);
	ASSERT_TRUE(record.Compare(IPv6(1), 4662, &second, true));
}

TEST(TrackedClientRecord, TrackedComparesOnlyWhenBothOwnTheAddress)
{
	CTrackedClientRecord record;
	int first, second;
	record.Add(IPv6(1), 4662, &first, false, 100);
	// A NAT neighbour, or the tracked peer itself, did not own the address: no verdict.
	ASSERT_TRUE(record.Compare(IPv6(1), 4662, &second, true));
	ASSERT_TRUE(record.Compare(IPv6(1), 4662, &second, false));
	record.Add(IPv6(1), 4662, &first, true, 200);
	ASSERT_TRUE(record.Compare(IPv6(1), 4662, &second, false));
	ASSERT_FALSE(record.Compare(IPv6(1), 4662, &second, true));
	ASSERT_TRUE(record.Compare(IPv6(1), 4662, &first, true));
}

TEST(TrackedClientRecord, IPv4OwnershipNeedsTheHelloIDToBeTheAddress)
{
	const auto address = CNetworkAddress::FromIPv4NetworkOrder(0x010200c0); // 192.0.2.1
	const auto none = CNetworkAddress::Absent();
	ASSERT_TRUE(OwnsTrackedAddress(address, 0x010200c0, none));
	ASSERT_TRUE(OwnsTrackedAddress(MappedIPv4(), 0x010200c0, none));
	ASSERT_FALSE(OwnsTrackedAddress(address, 12345, none));      // LowID
	ASSERT_FALSE(OwnsTrackedAddress(address, 1, none));          // Kad firewalled
	ASSERT_FALSE(OwnsTrackedAddress(address, 0, none));          // no network
	ASSERT_FALSE(OwnsTrackedAddress(address, 0x020200c0, none)); // HighID elsewhere
	ASSERT_FALSE(OwnsTrackedAddress(address, 12345, IPv6(1)));   // IPv6 says nothing about IPv4
	// Kad sends its IP as ID, which for x.y.z.0 falls in the LowID range.
	const auto zeroOctet = CNetworkAddress::FromIPv4NetworkOrder(0x000200c0);
	ASSERT_TRUE(OwnsTrackedAddress(zeroOctet, 0x000200c0, none));
	ASSERT_FALSE(OwnsTrackedAddress(CNetworkAddress::Absent(), 0, none));
}

TEST(TrackedClientRecord, IPv6OwnershipNeedsAnAdvertisedAddressInTheSame64)
{
	ASSERT_TRUE(OwnsTrackedAddress(IPv6(1), 12345, IPv6(1)));
	ASSERT_TRUE(OwnsTrackedAddress(IPv6(2), 12345, IPv6(1)));     // temporary address, same /64
	ASSERT_FALSE(OwnsTrackedAddress(IPv6(1), 12345, IPv6(1, 1))); // behind NAT66
	ASSERT_FALSE(OwnsTrackedAddress(IPv6(1), 12345, CNetworkAddress::Absent()));
	// The IPv4 claim does not carry over to IPv6.
	ASSERT_FALSE(OwnsTrackedAddress(IPv6(1), 0x010200c0, CNetworkAddress::Absent()));
}
