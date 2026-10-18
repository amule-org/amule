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

#include <muleunit/test.h>
#include "ServerIPv6Update.h"

using namespace muleunit;

DECLARE_SIMPLE(ServerIPv6Update)

namespace
{
const std::uint8_t ALL_ZEROS[16] = {};

CNetworkAddress MakeAddress(std::uint8_t lastByte)
{
	CNetworkAddress::Octets octets = {
		0x20, 0x01, 0x48, 0x60, 0x48, 0x60, 0, 0, 0, 0, 0, 0, 0, 0, 0, lastByte
	};
	return CNetworkAddress::IPv6FromOctets(octets);
}

bool PayloadMatches(const std::uint8_t (&payload)[16], const CNetworkAddress &address)
{
	std::uint8_t expected[16];
	address.ToIPv6Bytes(expected);
	return std::memcmp(payload, expected, 16) == 0;
}

bool PayloadMatches(const std::uint8_t (&payload)[16], const std::uint8_t (&expected)[16])
{
	return std::memcmp(payload, expected, 16) == 0;
}
} // namespace

TEST(ServerIPv6Update, StartsAfterLogin)
{
	const CNetworkAddress address = MakeAddress(0x88);
	const SServerIPv6UpdateDecision decision = DecideServerIPv6Update(true, std::nullopt, address);
	ASSERT_TRUE(decision.send);
	ASSERT_TRUE(PayloadMatches(decision.payload, address));
}

TEST(ServerIPv6Update, Changes)
{
	const CNetworkAddress oldAddress = MakeAddress(0x88);
	const CNetworkAddress newAddress = MakeAddress(0x99);
	const SServerIPv6UpdateDecision decision = DecideServerIPv6Update(true, oldAddress, newAddress);
	ASSERT_TRUE(decision.send);
	ASSERT_TRUE(PayloadMatches(decision.payload, newAddress));
}

TEST(ServerIPv6Update, StopsSendsAllZeros)
{
	const CNetworkAddress address = MakeAddress(0x88);
	const SServerIPv6UpdateDecision decision = DecideServerIPv6Update(true, address, std::nullopt);
	ASSERT_TRUE(decision.send);
	ASSERT_TRUE(PayloadMatches(decision.payload, ALL_ZEROS));
}

TEST(ServerIPv6Update, UnchangedDoesNotSend)
{
	const CNetworkAddress address = MakeAddress(0x88);
	const SServerIPv6UpdateDecision decision = DecideServerIPv6Update(true, address, address);
	ASSERT_FALSE(decision.send);
}

TEST(ServerIPv6Update, ServerWithoutTheFlagNeverSends)
{
	const CNetworkAddress oldAddress = MakeAddress(0x88);
	const CNetworkAddress newAddress = MakeAddress(0x99);
	const SServerIPv6UpdateDecision decision = DecideServerIPv6Update(false, oldAddress, newAddress);
	ASSERT_FALSE(decision.send);
}

TEST(ServerIPv6Update, NothingSentAndNothingAdvertisedDoesNotSend)
{
	const SServerIPv6UpdateDecision decision = DecideServerIPv6Update(true, std::nullopt, std::nullopt);
	ASSERT_FALSE(decision.send);
}
