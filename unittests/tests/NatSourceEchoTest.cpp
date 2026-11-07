//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// Any parts of this program contributed by third-party developers are copyrighted
// by their respective authors.
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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
//

#include <muleunit/test.h>

#include "NatSourceEcho.h"

using namespace muleunit;
using namespace NatSourceEcho;

DECLARE_SIMPLE(NatSourceEcho)

namespace
{
// aMule's network-order uint32 for a.b.c.d: the octets in memory order.
std::uint32_t Ip(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d)
{
	return static_cast<std::uint32_t>(a) | (static_cast<std::uint32_t>(b) << 8) |
	       (static_cast<std::uint32_t>(c) << 16) | (static_cast<std::uint32_t>(d) << 24);
}

const std::uint32_t kPeer = Ip(198, 51, 100, 7);
const std::uint32_t kLogin = Ip(203, 0, 113, 1);
const std::uint32_t kUdp = Ip(203, 0, 113, 2);
} // namespace

TEST(NatSourceEcho, PayloadIsTheIpOctetsThenTheLittleEndianPort)
{
	const auto payload = BuildPayload({ Ip(9, 8, 7, 6), 0x1234 });
	const std::array<std::uint8_t, 6> expected{ { 9, 8, 7, 6, 0x34, 0x12 } };
	ASSERT_TRUE(payload == expected);
}

TEST(NatSourceEcho, ParsesItsOwnPayloadAndIgnoresSurplus)
{
	std::array<std::uint8_t, 8> bytes{};
	const auto payload = BuildPayload({ kUdp, 4672 });
	std::copy(payload.begin(), payload.end(), bytes.begin());
	const auto parsed = ParsePayload(bytes.data(), bytes.size());
	ASSERT_TRUE(parsed.has_value());
	ASSERT_EQUALS(kUdp, parsed->ip);
	ASSERT_EQUALS(4672, static_cast<int>(parsed->port));
	ASSERT_FALSE(ParsePayload(bytes.data(), 5).has_value());
	ASSERT_FALSE(ParsePayload(nullptr, 6).has_value());
}

TEST(NatSourceEcho, KeepsOnlyAKeyIpThatDiffersFromTheLoginAddress)
{
	CKeyIps keys(4, 1000);
	keys.Set(kPeer, kLogin, kLogin, CKeyIps::From::Peer, 0);
	ASSERT_FALSE(keys.Lookup(kPeer, 0).has_value());
	keys.Set(kPeer, kUdp, kLogin, CKeyIps::From::Peer, 0);
	ASSERT_EQUALS(kUdp, *keys.Lookup(kPeer, 0));
	// A later report of the login address means the default key works again.
	keys.Set(kPeer, kLogin, kLogin, CKeyIps::From::Peer, 10);
	ASSERT_FALSE(keys.Lookup(kPeer, 10).has_value());
}

TEST(NatSourceEcho, APeerEchoWinsOverTheServerField)
{
	CKeyIps keys(4, 1000);
	const std::uint32_t other = Ip(203, 0, 113, 3);
	keys.Set(kPeer, kUdp, kLogin, CKeyIps::From::Server, 0);
	keys.Set(kPeer, other, kLogin, CKeyIps::From::Peer, 1);
	ASSERT_EQUALS(other, *keys.Lookup(kPeer, 2));
	// The server's value for a later punch, even the login address, leaves the echo in place.
	keys.Set(kPeer, kUdp, kLogin, CKeyIps::From::Server, 3);
	keys.Set(kPeer, kLogin, kLogin, CKeyIps::From::Server, 4);
	ASSERT_EQUALS(other, *keys.Lookup(kPeer, 5));
}

TEST(NatSourceEcho, AKeyIpEndsAfterItsIdleTimeAndUseExtendsIt)
{
	CKeyIps keys(4, 1000);
	keys.Set(kPeer, kUdp, kLogin, CKeyIps::From::Server, 0);
	ASSERT_TRUE(keys.Lookup(kPeer, 900).has_value());
	ASSERT_TRUE(keys.Lookup(kPeer, 1800).has_value());
	ASSERT_FALSE(keys.Lookup(kPeer, 2800).has_value());
	ASSERT_EQUALS(0, static_cast<int>(keys.Size()));
}

TEST(NatSourceEcho, StaysWithinItsCapacity)
{
	CKeyIps keys(2, 1000);
	keys.Set(Ip(198, 51, 100, 1), kUdp, kLogin, CKeyIps::From::Peer, 0);
	keys.Set(Ip(198, 51, 100, 2), kUdp, kLogin, CKeyIps::From::Peer, 1);
	keys.Set(Ip(198, 51, 100, 3), kUdp, kLogin, CKeyIps::From::Peer, 2);
	ASSERT_EQUALS(2, static_cast<int>(keys.Size()));
	ASSERT_FALSE(keys.Lookup(Ip(198, 51, 100, 1), 3).has_value());
	ASSERT_TRUE(keys.Lookup(Ip(198, 51, 100, 3), 3).has_value());
}
