// This file is part of the aMule Project.
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#include <muleunit/test.h>
#include <IPFilter.h>
#include <IPFilterScanner.h>
#include <cstdio>
#include <string>
#include <stdexcept>

using namespace muleunit;
DECLARE_SIMPLE(IPFilterIPv6)

namespace
{
struct Entry
{
	int family;
	uint32 start, end, level, bits;
	CNetworkAddress network;
	std::string description;
};

// Drive the actual generated scanner, including restart across consecutive files.
std::vector<Entry> Scan(const std::string &text)
{
	FILE *file = std::tmpfile();
	if (!file) {
		throw std::runtime_error("Could not create scanner input");
	}
	std::fwrite(text.data(), 1, text.size(), file);
	std::rewind(file);
	yyiprestart(file);
	yyip_Line = 1;
	yyip_Bad = 0;
	std::vector<Entry> result;
	Entry entry{};
	char *description = nullptr;
	while ((entry.family = yyiplex(
			entry.start, entry.end, entry.level, description, entry.network, entry.bits))) {
		entry.description = description;
		result.push_back(entry);
	}
	std::fclose(file);
	return result;
}

CNetworkAddress Addr(const char *text)
{
	return CNetworkAddress::FromString(text);
}
} // namespace

TEST(IPFilterIPv6, MixedInputPreservesLegacyRangesAndDescriptions)
{
	const auto entries = Scan("# comment\n"
				  "1.2.3.4 - 1.2.3.5,100,Duh:2.3.4.5-2.3.4.6\n"
				  "old list: 192.0.2.1-192.0.2.9\n"
				  " \t2001:db8::1234/64 , 99 ,description:with,commas\r\n"
				  "2001:db8::2/128,0,no final newline");
	ASSERT_EQUALS(4u, entries.size());
	ASSERT_EQUALS(0, yyip_Bad);
	ASSERT_EQUALS(1, entries[0].family);
	ASSERT_EQUALS(0x01020304u, entries[0].start);
	ASSERT_EQUALS(0x01020305u, entries[0].end);
	ASSERT_EQUALS(100u, entries[0].level);
	ASSERT_EQUALS(std::string("Duh:2.3.4.5-2.3.4.6"), entries[0].description);
	ASSERT_EQUALS(0xc0000201u, entries[1].start);
	ASSERT_EQUALS(0xc0000209u, entries[1].end);
	ASSERT_EQUALS(0u, entries[1].level);
	ASSERT_EQUALS(2, entries[2].family);
	ASSERT_TRUE(entries[2].network == Addr("2001:db8::"));
	ASSERT_EQUALS(64u, entries[2].bits);
	ASSERT_EQUALS(99u, entries[2].level);
	ASSERT_EQUALS(128u, entries[3].bits);
}

TEST(IPFilterIPv6, InvalidCIDRsDoNotBecomeRules)
{
	const char *invalid[] = { "2001:db8::/129,0,bad",
		"2001:db8::/64,256,bad",
		"2001:::1/64,0,bad",
		"::ffff:c000:201/128,0,mapped",
		"::ffff:192.0.2.1/128,0,mapped",
		"fe80::1%3/64,0,scoped",
		"[2001:db8::]/64,0,brackets",
		"2001:db8::/-1,0,negative",
		"2001:db8::/64x,0,junk",
		"2001:db8::/64,0" };
	for (const char *line : invalid) {
		const auto entries = Scan(std::string(line) + "\n2001:db8::1/128,0,valid\n");
		ASSERT_EQUALS(1u, entries.size());
		ASSERT_EQUALS(1, yyip_Bad);
		ASSERT_EQUALS(2, entries[0].family);
	}
}

TEST(IPFilterIPv6, PrefixBoundariesAndFamilySeparation)
{
	CIPFilterIPv6Ranges ranges;
	ASSERT_TRUE(ranges.Add(Addr("2001:db8::ffff"), 65, 99));
	ASSERT_TRUE(ranges.IsFiltered(Addr("2001:db8::"), 100));
	ASSERT_TRUE(ranges.IsFiltered(Addr("2001:db8::7fff:ffff:ffff:ffff"), 100));
	ASSERT_FALSE(ranges.IsFiltered(Addr("2001:db8:0:0:8000::"), 100));
	ASSERT_FALSE(ranges.IsFiltered(Addr("2001:db8::1"), 99));
	ASSERT_FALSE(ranges.IsFiltered(Addr("192.0.2.1"), 100));
	ASSERT_FALSE(ranges.IsFiltered(Addr("::ffff:192.0.2.1"), 100));
	ASSERT_FALSE(ranges.IsFiltered(CNetworkAddress::Absent(), 100));
	ASSERT_FALSE(ranges.Add(Addr("192.0.2.1"), 32, 0));
	ASSERT_FALSE(ranges.Add(Addr("::ffff:192.0.2.1"), 128, 0));
	ASSERT_FALSE(ranges.Add(Addr("fe80::1%3"), 64, 0));
	ASSERT_FALSE(ranges.Add(Addr("::"), 129, 0));
	ASSERT_FALSE(ranges.Add(Addr("::"), 0, 256));
}

TEST(IPFilterIPv6, ResolvedRangesPreserveGapsFragmentsAndThresholds)
{
	CIPFilterIPv6Ranges ranges;
	ASSERT_TRUE(ranges.Add(Addr("2001:db8::"), 120, 10));
	ASSERT_TRUE(ranges.Add(Addr("2001:db8::80"), 125, 200));
	ASSERT_TRUE(ranges.Add(Addr("2001:db8::83"), 128, 20));
	for (unsigned host = 0; host < 256; ++host) {
		auto bytes = Addr("2001:db8::").GetOctets();
		bytes[15] = static_cast<uint8>(host);
		const auto address = CNetworkAddress::IPv6FromOctets(bytes);
		const unsigned expected = host == 131 ? 20 : (host >= 128 && host <= 135 ? 200 : 10);
		ASSERT_FALSE(ranges.IsFiltered(address, expected));
		ASSERT_TRUE(ranges.IsFiltered(address, expected + 1));
	}
	ASSERT_FALSE(ranges.IsFiltered(Addr("2001:db8::100"), 256));
	ASSERT_FALSE(ranges.IsFiltered(Addr("2001:db7:ffff:ffff:ffff:ffff:ffff:ffff"), 256));
	// A later wider prefix replaces all fragments, irrespective of specificity.
	ASSERT_TRUE(ranges.Add(Addr("2001:db8::"), 119, 255));
	ASSERT_FALSE(ranges.IsFiltered(Addr("2001:db8::83"), 255));
	ASSERT_TRUE(ranges.IsFiltered(Addr("2001:db8::1ff"), 256));
	ASSERT_FALSE(ranges.IsFiltered(Addr("2001:db8::200"), 256));
}

TEST(IPFilterIPv6, FullWidthEndpointsAndSecurityPrefixPolicy)
{
	CIPFilterIPv6Ranges ranges;
	ASSERT_TRUE(ranges.Add(Addr("::"), 0, 0));
	ASSERT_FALSE(ranges.IsFiltered(Addr("::"), 1));
	ASSERT_TRUE(ranges.IsFiltered(Addr("::1"), 1));
	ASSERT_TRUE(ranges.IsFiltered(Addr("fe80::1%3"), 1));
	ASSERT_TRUE(ranges.Add(Addr("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"), 128, 200));
	ASSERT_FALSE(ranges.IsFiltered(Addr("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"), 100));
	ASSERT_TRUE(ranges.IsFiltered(Addr("ffff:ffff:ffff:ffff:ffff:ffff:ffff:fffe"), 100));
	ASSERT_TRUE(ranges.Add(Addr("::"), 128, 200));
	ASSERT_TRUE(ranges.IsFiltered(Addr("::1"), 100));
	ASSERT_TRUE(ranges.Add(Addr("::"), 0, 255));
	ASSERT_FALSE(ranges.IsFiltered(Addr("::1"), 255));
	ASSERT_TRUE(ranges.IsFiltered(Addr("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"), 256));
}

TEST(IPFilterIPv6, LaterStaticRulesOverrideAndReplacementDropsOldRules)
{
	CIPFilterIPv6Ranges ranges;
	for (const auto &entry : Scan("::/0,0,all\n2001:db8::/32,200,exception\n")) {
		ASSERT_TRUE(ranges.Add(entry.network, entry.bits, entry.level));
	}
	for (const auto &entry : Scan("2001:db8::1/128,0,static\n")) {
		ASSERT_TRUE(ranges.Add(entry.network, entry.bits, entry.level));
	}
	ASSERT_TRUE(ranges.IsFiltered(Addr("2001:db9::1"), 127));
	ASSERT_FALSE(ranges.IsFiltered(Addr("2001:db8::2"), 127));
	ASSERT_TRUE(ranges.IsFiltered(Addr("2001:db8::1"), 127));
	ASSERT_EQUALS(2u, ranges.BanCount(127));
	ASSERT_EQUALS(0u, ranges.BanCount(0));
	CIPFilterIPv6Ranges replacement;
	std::swap(ranges, replacement);
	ASSERT_FALSE(ranges.IsFiltered(Addr("2001:db8::1"), 127));
	ASSERT_EQUALS(0u, ranges.BanCount(127));
}
