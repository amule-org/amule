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
#include "ServerLoginIPv6.h"

using namespace muleunit;

DECLARE_SIMPLE(ServerLoginIPv6)

TEST(ServerLoginIPv6, BothTheTagAndTheBitAreSentWhenAnAddressIsAdvertised)
{
	const CNetworkAddress::Octets octets = {
		0x20, 0x01, 0x48, 0x60, 0x48, 0x60, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x88
	};
	const CNetworkAddress address = CNetworkAddress::IPv6FromOctets(octets);

	const SServerLoginIPv6Decision decision = DecideServerLoginIPv6(address);
	ASSERT_TRUE(decision.advertise);
	ASSERT_TRUE(decision.address == address);
}

TEST(ServerLoginIPv6, NeitherTheTagNorTheBitAreSentWithoutAnAddress)
{
	const SServerLoginIPv6Decision decision = DecideServerLoginIPv6(std::nullopt);
	ASSERT_FALSE(decision.advertise);
}
