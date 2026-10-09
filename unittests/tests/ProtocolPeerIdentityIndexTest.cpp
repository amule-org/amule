//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
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
#include "ProtocolPeerIdentityIndex.h"

using namespace muleunit;

namespace
{
CNetworkAddress IPv6(unsigned char last)
{
	CNetworkAddress::Octets bytes{};
	bytes[0] = 0x20;
	bytes[1] = 1;
	bytes[15] = last;
	return CNetworkAddress::IPv6FromOctets(bytes);
}
} // namespace

DECLARE_SIMPLE(CProtocolPeerIdentityIndex)

TEST(CProtocolPeerIdentityIndex, NativeAddFindAndExactness)
{
	CProtocolPeerIdentityIndex<int> index;
	const auto key = CProtocolPeerIdentity::FromNativeIPv6(IPv6(1));
	index.Add(key, 7);
	ASSERT_EQUALS(7, *index.Find(key));
	ASSERT_FALSE(index.Find(CProtocolPeerIdentity::FromNativeIPv6(IPv6(2))));
}

TEST(CProtocolPeerIdentityIndex, NonNativeIdentitiesAreIgnored)
{
	CProtocolPeerIdentityIndex<int> index;
	index.Add(CProtocolPeerIdentity::FromIPv4HighID(0x01020304), 1);
	index.Add(CProtocolPeerIdentity::FromServerScopedLowID(42, IPv6(3), 4662), 2);
	index.Add(CProtocolPeerIdentity::Absent(), 3);
	ASSERT_FALSE(index.Find(CProtocolPeerIdentity::FromIPv4HighID(0x01020304)));
	ASSERT_FALSE(index.Find(CProtocolPeerIdentity::FromServerScopedLowID(42, IPv6(3), 4662)));
	ASSERT_FALSE(index.Find(CProtocolPeerIdentity::Absent()));
}

TEST(CProtocolPeerIdentityIndex, UpdateAndDuplicateRemovalAreSymmetric)
{
	CProtocolPeerIdentityIndex<int> index;
	const auto oldKey = CProtocolPeerIdentity::FromNativeIPv6(IPv6(4));
	const auto newKey = CProtocolPeerIdentity::FromNativeIPv6(IPv6(5));
	index.Add(oldKey, 1);
	index.Add(oldKey, 2);
	index.Remove(oldKey, 1);
	ASSERT_EQUALS(2, *index.Find(oldKey));
	index.Remove(oldKey, 2);
	ASSERT_FALSE(index.Find(oldKey));
	index.Add(oldKey, 3);
	index.Update(oldKey, newKey, 3);
	ASSERT_FALSE(index.Find(oldKey));
	ASSERT_EQUALS(3, *index.Find(newKey));
}
