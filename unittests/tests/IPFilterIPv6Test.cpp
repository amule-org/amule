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

extern FILE *yyipout;

namespace
{
struct Entry
{
	int family;
	uint32 start, end, level, bits;
	CNetworkAddress network;
	CNetworkAddress last;
	std::string description;
};

// Bytes the scanner wrote to its output during the last Scan().
long g_echoed = 0;

// Drive the actual generated scanner, including restart across consecutive files.
std::vector<Entry> Scan(const std::string &text)
{
	FILE *file = std::tmpfile();
	if (!file) {
		throw std::runtime_error("Could not create scanner input");
	}
	std::fwrite(text.data(), 1, text.size(), file);
	std::rewind(file);
	FILE *echo = std::tmpfile();
	if (!echo) {
		throw std::runtime_error("Could not create scanner output");
	}
	yyipout = echo;
	yyiprestart(file);
	yyip_Line = 1;
	yyip_Bad = 0;
	std::vector<Entry> result;
	Entry entry{};
	char *description = nullptr;
	while ((entry.family = yyiplex(entry.start,
			entry.end,
			entry.level,
			description,
			entry.network,
			entry.bits,
			entry.last))) {
		entry.description = description;
		result.push_back(entry);
	}
	g_echoed = std::ftell(echo);
	yyipout = stdout;
	std::fclose(echo);
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
		ASSERT_EQUALS(0L, g_echoed);
	}
}

TEST(IPFilterIPv6, BadLinesAreConsumedWhole)
{
	const auto entries = Scan("garbage line\n"
				  "Org:1.2.3.4-1.2.3.5 trailing text\n"
				  "2001:db8::/32,0,valid\n");
	ASSERT_EQUALS(1u, entries.size());
	ASSERT_EQUALS(2, yyip_Bad);
	ASSERT_EQUALS(2, entries[0].family);
	ASSERT_EQUALS(0L, g_echoed);
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

TEST(IPFilterIPv6, NulBytesInAntiP2PLinesAreBadLines)
{
	// The AntiP2P action finds the IP with strrchr/strchr, which stop at a NUL.
	static const char kText[] = "0.0.0.0-0.255.255.255,0,bogon\n"
				    "\0:1.2.3.4-1.2.3.5\n"
				    "x:abcdefgh\0:1.2.3.4-1.2.3.5\n"
				    "a:b\0:1.2.3.4-1.2.3.5\n"
				    "Org:5.6.7.8-5.6.7.9\n";
	const auto entries = Scan(std::string(kText, sizeof(kText) - 1));
	ASSERT_EQUALS(2u, entries.size());
	ASSERT_EQUALS(3, yyip_Bad);
	ASSERT_EQUALS(std::string("bogon"), entries[0].description);
	ASSERT_EQUALS(std::string("Org"), entries[1].description);
	ASSERT_EQUALS(0x05060708u, entries[1].start);
}

// flex ends its buffer with a NUL, so a NUL in the input made it rescan the whole token: a line
// of NULs took quadratic time. The scanner reads NUL as \x01, which a description shows.
TEST(IPFilterIPv6, NulIsReadAsAnOrdinaryByte)
{
	static const char kText[] = "1.2.3.4-1.2.3.5,0,ab\0cd\n";
	const auto entries = Scan(std::string(kText, sizeof(kText) - 1));
	ASSERT_EQUALS(1u, entries.size());
	ASSERT_EQUALS(std::string("ab\x01"
				  "cd"),
		entries[0].description);
}

// A token longer than one read was rescanned at every refill, so lines are cut at 64 KiB. This
// one is a valid rule only past the cut, so it shows the cut, and the next line must still load.
TEST(IPFilterIPv6, OverlongLinesAreCut)
{
	const auto entries = Scan(std::string(70000, 'x') + ":1.2.3.4-1.2.3.5\n" + "Org:5.6.7.8-5.6.7.9\n");
	ASSERT_EQUALS(1u, entries.size());
	ASSERT_EQUALS(1, yyip_Bad);
	ASSERT_EQUALS(std::string("Org"), entries[0].description);
}

// The forms that loaded before the eMuleQt ones were added must give the same rules.
TEST(IPFilterIPv6, EarlierFormsAreUnchanged)
{
	const auto entries = Scan("1.2.3.4       - 1.2.3.5       , 100 , padded\n"
				  "dead:beef org:1.2.3.4-1.2.3.5\n"
				  "2001:db8::/32,7,cidr\n"
				  "2001:db8::/48 , 8 , spaced\r\n");
	ASSERT_EQUALS(4u, entries.size());
	ASSERT_EQUALS(0, yyip_Bad);
	ASSERT_EQUALS(1, entries[0].family);
	ASSERT_EQUALS(std::string(" padded"), entries[0].description);
	ASSERT_EQUALS(1, entries[1].family);
	ASSERT_EQUALS(std::string("dead:beef org"), entries[1].description);
	ASSERT_EQUALS(2, entries[2].family);
	ASSERT_EQUALS(7u, entries[2].level);
	ASSERT_EQUALS(std::string("cidr"), entries[2].description);
	ASSERT_EQUALS(2, entries[3].family);
	ASSERT_EQUALS(48u, entries[3].bits);
	ASSERT_EQUALS(8u, entries[3].level);
	ASSERT_EQUALS(0L, g_echoed);
}

// eMuleQt saves IPv6 rules as explicit bounds, with a padded level and a description.
TEST(IPFilterIPv6, EmuleQtRangesLoad)
{
	const auto entries = Scan("2001:db8::5 - 2001:db8::1:2 ,   0 , saved by eMuleQt\n"
				  "  2001:db8::9-2001:db8::9,200,single\r\n"
				  ":: - ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff , 100 , \n"
				  "2001:db8::1 - 2001:db8::2 , 1 , no final newline");
	ASSERT_EQUALS(4u, entries.size());
	ASSERT_EQUALS(0, yyip_Bad);
	ASSERT_EQUALS(3, entries[0].family);
	ASSERT_TRUE(entries[0].network == Addr("2001:db8::5"));
	ASSERT_TRUE(entries[0].last == Addr("2001:db8::1:2"));
	ASSERT_EQUALS(0u, entries[0].level);
	ASSERT_EQUALS(std::string(" saved by eMuleQt"), entries[0].description);
	ASSERT_TRUE(entries[1].network == entries[1].last);
	ASSERT_EQUALS(200u, entries[1].level);
	ASSERT_TRUE(entries[2].network == Addr("::"));
	ASSERT_TRUE(entries[2].last == Addr("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"));
	ASSERT_EQUALS(std::string(" "), entries[2].description);
	ASSERT_EQUALS(3, entries[3].family);
	ASSERT_EQUALS(0L, g_echoed);
}

TEST(IPFilterIPv6, InvalidRangesAreBadLines)
{
	const char *invalid[] = { "2001:db8::2 - 2001:db8::1 , 0 , reversed",
		"::ffff:c000:201 - ::ffff:c000:209 , 0 , mapped",
		"::ffff:192.0.2.1 - ::ffff:192.0.2.9 , 0 , dotted",
		"fe80::1%3 - fe80::2 , 0 , scoped",
		"2001:db8::1 - 2001:db8::2 , 256 , level",
		"2001:db8::1 - 2001:db8::2 , 0",
		"2001:db8::1 - 2001:db8::2",
		"2001:::1 - 2001:db8::2 , 0 , malformed",
		"2001:db8::1 - 1.2.3.4 , 0 , mixed" };
	for (const char *line : invalid) {
		const auto entries = Scan(std::string(line) + "\n2001:db8::1/128,0,valid\n");
		ASSERT_EQUALS(1u, entries.size());
		ASSERT_EQUALS(1, yyip_Bad);
		ASSERT_EQUALS(2, entries[0].family);
		ASSERT_EQUALS(0L, g_echoed);
	}
}

// A bare CIDR gets eMule's level for a line without one, below aMule's default threshold of 127.
TEST(IPFilterIPv6, BareCIDRsLoadWithTheDefaultLevel)
{
	const auto entries = Scan("2001:db8::1234/32\n"
				  " \t2001:db8:1::/48 \r\n"
				  "2001:db8:2::/128");
	ASSERT_EQUALS(3u, entries.size());
	ASSERT_EQUALS(0, yyip_Bad);
	ASSERT_EQUALS(2, entries[0].family);
	ASSERT_TRUE(entries[0].network == Addr("2001:db8::"));
	ASSERT_EQUALS(32u, entries[0].bits);
	ASSERT_EQUALS(100u, entries[0].level);
	ASSERT_EQUALS(std::string(), entries[0].description);
	ASSERT_EQUALS(48u, entries[1].bits);
	ASSERT_EQUALS(128u, entries[2].bits);
	const char *invalid[] = { "2001:db8::/129",
		"2001:db8::/32 junk",
		"::ffff:c000:201/128",
		"2001:db8::/32,0",
		"2001:db8::/1234" };
	for (const char *line : invalid) {
		const auto bad = Scan(std::string(line) + "\n2001:db8::1/128,0,valid\n");
		ASSERT_EQUALS(1u, bad.size());
		ASSERT_EQUALS(1, yyip_Bad);
		ASSERT_EQUALS(0L, g_echoed);
	}
}

TEST(IPFilterIPv6, RangeRulesResolveWithPrefixRules)
{
	CIPFilterIPv6Ranges ranges;
	ASSERT_TRUE(ranges.AddRange(Addr("2001:db8::5"), Addr("2001:db8::1:2"), 10));
	ASSERT_FALSE(ranges.IsFiltered(Addr("2001:db8::4"), 11));
	ASSERT_TRUE(ranges.IsFiltered(Addr("2001:db8::5"), 11));
	ASSERT_TRUE(ranges.IsFiltered(Addr("2001:db8::1:2"), 11));
	ASSERT_FALSE(ranges.IsFiltered(Addr("2001:db8::1:3"), 11));
	ASSERT_FALSE(ranges.IsFiltered(Addr("2001:db8::5"), 10));
	// Last rule wins in both directions: a later prefix exempts part, a later range blocks again.
	ASSERT_TRUE(ranges.Add(Addr("2001:db8::"), 112, 200));
	ASSERT_FALSE(ranges.IsFiltered(Addr("2001:db8::5"), 127));
	ASSERT_TRUE(ranges.IsFiltered(Addr("2001:db8::1:0"), 127));
	ASSERT_TRUE(ranges.AddRange(Addr("2001:db8::7"), Addr("2001:db8::8"), 0));
	ASSERT_TRUE(ranges.IsFiltered(Addr("2001:db8::7"), 127));
	ASSERT_FALSE(ranges.IsFiltered(Addr("2001:db8::9"), 127));
	ASSERT_EQUALS(2u, ranges.BanCount(127));
	ASSERT_FALSE(ranges.IsFiltered(Addr("192.0.2.1"), 256));
	ASSERT_FALSE(ranges.AddRange(Addr("2001:db8::2"), Addr("2001:db8::1"), 0));
	ASSERT_FALSE(ranges.AddRange(Addr("::ffff:192.0.2.1"), Addr("::ffff:192.0.2.9"), 0));
	ASSERT_FALSE(ranges.AddRange(Addr("fe80::1%3"), Addr("fe80::2"), 0));
	ASSERT_FALSE(ranges.AddRange(Addr("2001:db8::1"), Addr("2001:db8::2"), 256));
	// As with ::/0, the unspecified address is never a matching host.
	CIPFilterIPv6Ranges all;
	ASSERT_TRUE(all.AddRange(Addr("::"), Addr("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"), 0));
	ASSERT_FALSE(all.IsFiltered(Addr("::"), 1));
	ASSERT_TRUE(all.IsFiltered(Addr("::1"), 1));
	ASSERT_TRUE(all.IsFiltered(Addr("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"), 1));
}
