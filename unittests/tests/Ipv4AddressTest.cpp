//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// Any parts of this program derived from the xMule, lMule or eMule project,
// or contributed by third-party developers are copyrighted by their
// respective authors.
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
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
//
// amuleapi's dotted-quad parsing and the two byte orders EC carries addresses in. The Kad
// bootstrap sent its request to d.c.b.a for as long as the order was left implicit, and the
// response echo could not show it, so the orders are pinned here rather than through a daemon.

#include <muleunit/test.h>

#include <Ipv4Address.h>

using namespace muleunit;
using webapi::ParseIpv4Dotted;
using webapi::ToKadIpOrder;

DECLARE_SIMPLE(Ipv4Address)

namespace
{
bool Rejects(const std::string &text)
{
	std::uint32_t ip = 0x12345678;
	return !ParseIpv4Dotted(text, ip) && ip == 0x12345678;
}
} // namespace

TEST(Ipv4Address, ParsesLeastSignificantOctetFirst)
{
	std::uint32_t ip = 0;
	ASSERT_TRUE(ParseIpv4Dotted("192.168.1.2", ip));
	ASSERT_EQUALS(0x0201A8C0u, ip);
}

TEST(Ipv4Address, KadOrderPutsTheFirstOctetHighest)
{
	std::uint32_t ip = 0;
	ASSERT_TRUE(ParseIpv4Dotted("192.168.1.2", ip));
	ASSERT_EQUALS(0xC0A80102u, ToKadIpOrder(ip));
	ASSERT_EQUALS(ip, ToKadIpOrder(ToKadIpOrder(ip)));
}

TEST(Ipv4Address, AcceptsTheWholeRange)
{
	std::uint32_t ip = 1;
	ASSERT_TRUE(ParseIpv4Dotted("0.0.0.0", ip));
	ASSERT_EQUALS(0u, ip);
	ASSERT_TRUE(ParseIpv4Dotted("255.255.255.255", ip));
	ASSERT_EQUALS(0xFFFFFFFFu, ip);
	ASSERT_TRUE(ParseIpv4Dotted("010.000.001.002", ip));
	ASSERT_EQUALS(0x0201000Au, ip);
}

TEST(Ipv4Address, RejectsAnythingButFourOctets)
{
	ASSERT_TRUE(Rejects("192.168.1.2:4672"));
	ASSERT_TRUE(Rejects("192.168.1.2.5"));
	ASSERT_TRUE(Rejects("192.168.1"));
	ASSERT_TRUE(Rejects("192.168..2"));
	ASSERT_TRUE(Rejects(".168.1.2"));
	ASSERT_TRUE(Rejects("192.168.1."));
	ASSERT_TRUE(Rejects(" 192.168.1.2"));
	ASSERT_TRUE(Rejects("192.168.1.2 "));
	ASSERT_TRUE(Rejects("+192.168.1.2"));
	ASSERT_TRUE(Rejects(""));
}

TEST(Ipv4Address, RejectsOutOfRangeOctets)
{
	ASSERT_TRUE(Rejects("256.1.1.1"));
	ASSERT_TRUE(Rejects("1.1.1.999"));
	ASSERT_TRUE(Rejects("0001.1.1.1"));
	ASSERT_TRUE(Rejects("4294967297.1.1.1"));
}
