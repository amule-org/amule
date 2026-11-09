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

TEST(NatSourceEcho, OnlyADatagramThatFailedEveryKeyCounts)
{
	ASSERT_TRUE(FailedEveryKey(0x01, 20, false));
	ASSERT_FALSE(FailedEveryKey(0x01, 20, true));
	ASSERT_FALSE(FailedEveryKey(0x01, kCryptHeaderBytes, false));
	// DecryptReceivedClient() passes these through untried, so they are no failed key.
	for (std::uint8_t plain : { 0xC5, 0xE5, 0xE4, 0xA3, 0xB2, 0xD4 }) {
		ASSERT_FALSE(FailedEveryKey(plain, 20, false));
	}
}

namespace
{
const std::uint32_t kSplit = Ip(192, 0, 2, 20);
Hash MakeHash(std::uint8_t seed)
{
	Hash hash{};
	hash.fill(seed);
	return hash;
}
} // namespace

TEST(NatSourceEcho, EchoesOncePerWindowToAPunchedSourceWithAHash)
{
	CPunchedPeers punched(8);
	ASSERT_FALSE(punched.TakeEcho(kSplit, 4672, 0x01, 20, false, 0).has_value());
	punched.Punched(kSplit, 4672, MakeHash(1), 0);
	const auto hash = punched.TakeEcho(kSplit, 4672, 0x01, 20, false, 10);
	ASSERT_TRUE(hash.has_value());
	ASSERT_TRUE(*hash == MakeHash(1));
	ASSERT_FALSE(punched.TakeEcho(kSplit, 4672, 0x01, 20, false, 20).has_value());
	// A plain datagram or one that decoded never triggers one.
	punched.EchoNotSent(kSplit, 4672, 20);
	ASSERT_FALSE(punched.TakeEcho(kSplit, 4672, 0xC5, 20, false, 30).has_value());
	ASSERT_FALSE(punched.TakeEcho(kSplit, 4672, 0x01, 20, true, 30).has_value());
	ASSERT_TRUE(punched.TakeEcho(kSplit, 4672, 0x01, 20, false, 30).has_value());
	// The window closes 30 s after the punch.
	punched.EchoNotSent(kSplit, 4672, 30);
	ASSERT_FALSE(punched.TakeEcho(kSplit, 4672, 0x01, 20, false, kWindowMs).has_value());
}

TEST(NatSourceEcho, NoEchoWithoutAVouchedHash)
{
	CPunchedPeers punched(8);
	punched.Punched(kSplit, 4672, std::nullopt, 0);
	ASSERT_TRUE(punched.IsPunched(kSplit, 4672, 0));
	ASSERT_FALSE(punched.TakeEcho(kSplit, 4672, 0x01, 20, false, 1).has_value());
	// Two hashes vouched for one endpoint: neither is trusted.
	punched.Punched(kSplit, 4673, MakeHash(1), 0);
	punched.Punched(kSplit, 4673, MakeHash(2), 1);
	ASSERT_FALSE(punched.TakeEcho(kSplit, 4673, 0x01, 20, false, 2).has_value());
}

TEST(NatSourceEcho, AMovedPortEarnsAnEchoOnlyForTheOnePunchOfItsIp)
{
	CPunchedPeers punched(8);
	punched.Punched(kSplit, 4672, MakeHash(1), 0);
	ASSERT_TRUE(punched.TakeEcho(kSplit, 50001, 0x01, 20, false, 1).has_value());
	// Two punches to one IP: a third port cannot be told apart.
	punched.Punched(kSplit, 4673, MakeHash(1), 2);
	punched.EchoNotSent(kSplit, 50001, 2);
	ASSERT_FALSE(punched.TakeEcho(kSplit, 50002, 0x01, 20, false, 3).has_value());
}

TEST(NatSourceEcho, MovedPortsShareOneEchoPerWindow)
{
	// A moved port matches every port of the IP, so spoofed junk from each one must not each
	// earn an echo back to that IP. The punched endpoint keeps its own.
	CPunchedPeers punched(8);
	punched.Punched(kSplit, 4672, MakeHash(1), 0);
	ASSERT_TRUE(punched.TakeEcho(kSplit, 50001, 0x01, 20, false, 1).has_value());
	ASSERT_FALSE(punched.TakeEcho(kSplit, 50002, 0x01, 20, false, 2).has_value());
	ASSERT_TRUE(punched.TakeEcho(kSplit, 4672, 0x01, 20, false, 3).has_value());
	ASSERT_FALSE(punched.TakeEcho(kSplit, 4672, 0x01, 20, false, 4).has_value());
}

TEST(NatSourceEcho, TwoPeersBehindOneIpEachEarnAnEcho)
{
	CPunchedPeers punched(8);
	punched.Punched(kSplit, 4672, MakeHash(1), 0);
	punched.Punched(kSplit, 4673, MakeHash(2), 0);
	const auto first = punched.TakeEcho(kSplit, 4672, 0x01, 20, false, 1);
	ASSERT_TRUE(first.has_value() && *first == MakeHash(1));
	const auto second = punched.TakeEcho(kSplit, 4673, 0x01, 20, false, 2);
	ASSERT_TRUE(second.has_value() && *second == MakeHash(2));
	ASSERT_FALSE(punched.TakeEcho(kSplit, 4673, 0x01, 20, false, 3).has_value());
	// An echo not sent frees only its own endpoint.
	punched.EchoNotSent(kSplit, 4672, 4);
	ASSERT_TRUE(punched.TakeEcho(kSplit, 4672, 0x01, 20, false, 5).has_value());
	ASSERT_FALSE(punched.TakeEcho(kSplit, 4673, 0x01, 20, false, 5).has_value());
}

TEST(NatSourceEcho, TakesAnEchoOnlyFromTheExactPunchedEndpoint)
{
	// The echo sets the key IP toward the whole IP, so a neighbour behind the same carrier NAT,
	// on another port, must not be able to send it.
	CPunchedPeers punched(8);
	punched.Punched(kSplit, 4672, MakeHash(1), 0);
	ASSERT_TRUE(AcceptsEcho(punched, kSplit, 4672, true, true, 1));
	ASSERT_FALSE(AcceptsEcho(punched, kSplit, 50001, true, true, 1));
}

TEST(NatSourceEcho, APlainEchoIsNotThePeers)
{
	CPunchedPeers punched(8);
	punched.Punched(kPeer, 4672, MakeHash(1), 0);
	ASSERT_FALSE(AcceptsEcho(punched, kPeer, 4672, true, false, 1));
}

TEST(NatSourceEcho, TakesAnEchoOnlyFromAPunchedSourceAndOnlyAPublicAddress)
{
	CPunchedPeers punched(8);
	ASSERT_FALSE(AcceptsEcho(punched, kPeer, 4672, true, true, 0));
	punched.Punched(kPeer, 4672, MakeHash(1), 0);
	ASSERT_TRUE(AcceptsEcho(punched, kPeer, 4672, true, true, 1));
	ASSERT_FALSE(AcceptsEcho(punched, kPeer, 4672, false, true, 1));
	ASSERT_FALSE(AcceptsEcho(punched, kPeer, 4672, true, true, kWindowMs));
}

TEST(NatSourceEcho, KeepsOnlyAKeyIpThatDiffersFromTheLoginAddress)
{
	CKeyIps keys(4, 1000);
	keys.Set(kPeer, kLogin, kLogin, CKeyIps::From::Peer, 0);
	ASSERT_FALSE(keys.Lookup(kPeer, kLogin, 0).has_value());
	keys.Set(kPeer, kUdp, kLogin, CKeyIps::From::Peer, 0);
	ASSERT_EQUALS(kUdp, *keys.Lookup(kPeer, kLogin, 0));
	// A later report of the login address means the default key works again.
	keys.Set(kPeer, kLogin, kLogin, CKeyIps::From::Peer, 10);
	ASSERT_FALSE(keys.Lookup(kPeer, kLogin, 10).has_value());
}

TEST(NatSourceEcho, APeerEchoWinsOverTheServerField)
{
	CKeyIps keys(4, 1000);
	const std::uint32_t other = Ip(203, 0, 113, 3);
	keys.Set(kPeer, kUdp, kLogin, CKeyIps::From::Server, 0);
	keys.Set(kPeer, other, kLogin, CKeyIps::From::Peer, 1);
	ASSERT_EQUALS(other, *keys.Lookup(kPeer, kLogin, 2));
	// The server's value for a later punch, even the login address, leaves the echo in place.
	keys.Set(kPeer, kUdp, kLogin, CKeyIps::From::Server, 3);
	keys.Set(kPeer, kLogin, kLogin, CKeyIps::From::Server, 4);
	ASSERT_EQUALS(other, *keys.Lookup(kPeer, kLogin, 5));
}

TEST(NatSourceEcho, AKeyIpEndsAfterItsIdleTimeAndUseExtendsIt)
{
	CKeyIps keys(4, 1000);
	keys.Set(kPeer, kUdp, kLogin, CKeyIps::From::Server, 0);
	ASSERT_TRUE(keys.Lookup(kPeer, kLogin, 900).has_value());
	ASSERT_TRUE(keys.Lookup(kPeer, kLogin, 1800).has_value());
	ASSERT_FALSE(keys.Lookup(kPeer, kLogin, 2800).has_value());
	ASSERT_EQUALS(0, static_cast<int>(keys.Size()));
}

TEST(NatSourceEcho, AKeyIpEndsWhenOurLoginAddressChanges)
{
	CKeyIps keys(4, 1000);
	const std::uint32_t newLogin = Ip(203, 0, 113, 9);
	keys.Set(kPeer, kUdp, kLogin, CKeyIps::From::Peer, 0);
	// Between server logins (0) the entry is hidden but kept, and a new value changes nothing.
	ASSERT_FALSE(keys.Lookup(kPeer, 0, 1).has_value());
	keys.Set(kPeer, Ip(203, 0, 113, 11), 0, CKeyIps::From::Peer, 1);
	ASSERT_EQUALS(kUdp, *keys.Lookup(kPeer, kLogin, 2));
	ASSERT_FALSE(keys.Lookup(kPeer, newLogin, 3).has_value());
	ASSERT_EQUALS(0, static_cast<int>(keys.Size()));
	// And a peer's echo from the old login does not block the server's value under the new one.
	keys.Set(kPeer, kUdp, kLogin, CKeyIps::From::Peer, 4);
	keys.Set(kPeer, Ip(203, 0, 113, 10), newLogin, CKeyIps::From::Server, 5);
	ASSERT_EQUALS(Ip(203, 0, 113, 10), *keys.Lookup(kPeer, newLogin, 6));
}

TEST(NatSourceEcho, TheTieBreakUsesTheAddressThePeerSees)
{
	ASSERT_EQUALS(kUdp, LocalUdpIP(kUdp, Ip(203, 0, 113, 5), kLogin));
	ASSERT_EQUALS(Ip(203, 0, 113, 5), LocalUdpIP(0, Ip(203, 0, 113, 5), kLogin));
	ASSERT_EQUALS(kLogin, LocalUdpIP(0, 0, kLogin));
}

TEST(NatSourceEcho, StaysWithinItsCapacity)
{
	CKeyIps keys(2, 1000);
	keys.Set(Ip(198, 51, 100, 1), kUdp, kLogin, CKeyIps::From::Peer, 0);
	keys.Set(Ip(198, 51, 100, 2), kUdp, kLogin, CKeyIps::From::Peer, 1);
	keys.Set(Ip(198, 51, 100, 3), kUdp, kLogin, CKeyIps::From::Peer, 2);
	ASSERT_EQUALS(2, static_cast<int>(keys.Size()));
	ASSERT_FALSE(keys.Lookup(Ip(198, 51, 100, 1), kLogin, 3).has_value());
	ASSERT_TRUE(keys.Lookup(Ip(198, 51, 100, 3), kLogin, 3).has_value());
}
