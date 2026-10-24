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

#include <QuicCapabilityCache.h>

#include <array>

using namespace muleunit;
DECLARE_SIMPLE(QuicCapabilityCache)

namespace
{
std::array<uint8_t, 16> Peer(uint8_t value)
{
	std::array<uint8_t, 16> peer{};
	peer.fill(value);
	return peer;
}
} // namespace

TEST(QuicCapabilityCache, UnknownAndIdentitySeparation)
{
	CQuicCapabilityCache cache(4);
	ASSERT_TRUE(cache.Lookup(Peer(1), 0).state == QuicCapabilityState::Unknown);
	cache.Remember(Peer(1), QuicNatt::EAQC_CAP_QUIC, 0, 10);
	ASSERT_TRUE(cache.Lookup(Peer(1), 1).state == QuicCapabilityState::Compatible);
	ASSERT_TRUE(cache.Lookup(Peer(2), 1).state == QuicCapabilityState::Unknown);
}

TEST(QuicCapabilityCache, PreservesOptionsAndClassifiesQuicBit)
{
	CQuicCapabilityCache cache(2);
	cache.Remember(Peer(1), 0xff, 10, 20);
	cache.Remember(Peer(2), 0x80, 10, 20);
	ASSERT_EQUALS(0xff, cache.Lookup(Peer(1), 11).options);
	ASSERT_TRUE(cache.Lookup(Peer(1), 11).state == QuicCapabilityState::Compatible);
	ASSERT_TRUE(cache.Lookup(Peer(2), 11).state == QuicCapabilityState::Incompatible);
}

TEST(QuicCapabilityCache, SaturatesExpirationAtUint64Boundary)
{
	CQuicCapabilityCache cache(1);
	const auto now = UINT64_MAX - 5;
	cache.Remember(Peer(1), QuicNatt::EAQC_CAP_QUIC, now, 10);
	ASSERT_TRUE(cache.Lookup(Peer(1), UINT64_MAX - 1).state == QuicCapabilityState::Compatible);
}

TEST(QuicCapabilityCache, ExpiryAndCooldownBecomeUnknown)
{
	CQuicCapabilityCache cache(2);
	cache.Remember(Peer(1), QuicNatt::EAQC_CAP_QUIC, 0, 10);
	cache.RememberFailure(Peer(2), 0, 10);
	ASSERT_TRUE(cache.Lookup(Peer(2), 9).state == QuicCapabilityState::Cooldown);
	ASSERT_TRUE(cache.Lookup(Peer(1), 10).state == QuicCapabilityState::Unknown);
	ASSERT_TRUE(cache.Lookup(Peer(2), 10).state == QuicCapabilityState::Unknown);
}

TEST(QuicCapabilityCache, EvictsLeastRecentlyUsedAndZeroCapacityStoresNothing)
{
	CQuicCapabilityCache cache(2);
	cache.Remember(Peer(1), 0, 0, 100);
	cache.Remember(Peer(2), 0, 0, 100);
	(void)cache.Lookup(Peer(1), 1);
	cache.Remember(Peer(3), QuicNatt::EAQC_CAP_QUIC, 2, 100);
	ASSERT_TRUE(cache.Lookup(Peer(2), 3).state == QuicCapabilityState::Unknown);
	ASSERT_TRUE(cache.Lookup(Peer(1), 3).state == QuicCapabilityState::Incompatible);
	CQuicCapabilityCache empty(0);
	empty.Remember(Peer(1), 0xff, 0, 100);
	ASSERT_TRUE(empty.Lookup(Peer(1), 1).state == QuicCapabilityState::Unknown);
}
