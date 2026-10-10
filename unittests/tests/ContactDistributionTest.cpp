//								-*- C++ -*-
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
#include <utility>
#include <kademlia/utils/ContactDistribution.h>
using namespace muleunit;
using Kademlia::ContactDistribution;
using Kademlia::ContactDistributionBuilder;
DECLARE_SIMPLE(ContactDistribution)
TEST(ContactDistribution, PrefixBoundariesAndLocalMarker)
{
	ContactDistributionBuilder builder;
	builder.Add(0, false);
	builder.Add(0x000fffff, true);
	builder.Add(0x00100000, true);
	builder.Add(0xffffffff, false);
	auto data = builder.Get(3, 0x12345678);
	ASSERT_EQUALS(2u, data.contacts[0]);
	ASSERT_EQUALS(1u, data.contacts[1]);
	ASSERT_EQUALS(1u, data.contacts[4095]);
	ASSERT_EQUALS(4u, data.Total());
	ASSERT_EQUALS(2u, data.Verified());
	ASSERT_EQUALS(3u, data.subnets);
	ASSERT_TRUE(data.hasLocalID);
	ASSERT_EQUALS(0x12345678u, data.localID);
}
TEST(ContactDistribution, PortableSparseWireAndSnapshot)
{
	ContactDistributionBuilder builder;
	builder.Add(0, true);
	builder.Add(0xffffffff, false);
	auto data = builder.Get(2, 0x12345678);
	const auto wire = data.Encode();
	ASSERT_EQUALS(size_t(32), wire.size());
	ASSERT_EQUALS(uint8_t(2), wire[0]);
	ASSERT_EQUALS(uint8_t(1), wire[1]);
	ASSERT_EQUALS(uint8_t(0x12), wire[2]);
	ASSERT_EQUALS(uint8_t(0x78), wire[5]);
	ASSERT_EQUALS(uint8_t(2), wire[9]);
	ASSERT_EQUALS(uint8_t(2), wire[11]);
	ASSERT_EQUALS(uint8_t(1), wire[17]);
	ASSERT_EQUALS(uint8_t(1), wire[21]);
	ASSERT_EQUALS(uint8_t(0x0f), wire[22]);
	ASSERT_EQUALS(uint8_t(0xff), wire[23]);
	ContactDistribution decoded;
	ASSERT_TRUE(ContactDistribution::Decode(wire.data(), wire.size(), decoded));
	ASSERT_TRUE(data.contacts == decoded.contacts);
	ASSERT_TRUE(data.verified == decoded.verified);
	ASSERT_EQUALS(data.localID, decoded.localID);
	ASSERT_TRUE(decoded.hasLocalID);
	builder.Add(0xffffffff, false);
	ASSERT_EQUALS(2u, data.Total());
	ASSERT_EQUALS(3u, builder.Get(2, 0).Total());
}
TEST(ContactDistribution, RejectMalformedWithoutOverwritingSnapshot)
{
	ContactDistribution data;
	data.contacts[0] = 1;
	data.verified[0] = 1;
	data.contacts[4095] = 1;
	data.subnets = 1;
	auto original = data.Encode();
	ContactDistribution out = data;
	ASSERT_FALSE(ContactDistribution::Decode(nullptr, original.size(), out));
	ASSERT_FALSE(ContactDistribution::Decode(original.data(), original.size() - 1, out));
	for (const auto change : { std::pair<size_t, uint8_t>{ 0, 3 },
		     { 1, 2 },
		     { 9, 3 },
		     { 11, 3 },
		     { 12, 0x10 },
		     { 21, 2 },
		     { 17, 0 } }) {
		auto wire = original;
		wire[change.first] = change.second;
		ASSERT_FALSE(ContactDistribution::Decode(wire.data(), wire.size(), out));
	}
	auto duplicate = original;
	duplicate[22] = duplicate[23] = 0;
	ASSERT_FALSE(ContactDistribution::Decode(duplicate.data(), duplicate.size(), out));
	ASSERT_EQUALS(2u, out.Total());
	ContactDistribution empty;
	auto wire = empty.Encode();
	ASSERT_EQUALS(size_t(12), wire.size());
	ASSERT_TRUE(ContactDistribution::Decode(wire.data(), wire.size(), out));
	ASSERT_EQUALS(0u, out.Total());
	ASSERT_FALSE(out.hasLocalID);
}
TEST(ContactDistribution, LargeSnapshotAndOverflowPayload)
{
	ContactDistributionBuilder builder;
	for (uint32_t i = 0; i < 100000; ++i) {
		builder.Add((i % 4096) << 20, i % 2 == 0);
	}
	const auto snapshot = builder.Get(391, 0);
	ASSERT_EQUALS(100000u, snapshot.Total());
	ASSERT_EQUALS(50000u, snapshot.Verified());
	const auto wire = snapshot.Encode();
	ContactDistribution decoded;
	ASSERT_TRUE(ContactDistribution::Decode(wire.data(), wire.size(), decoded));
	ASSERT_TRUE(snapshot.contacts == decoded.contacts);
	ASSERT_TRUE(snapshot.verified == decoded.verified);
	ContactDistribution overflow;
	overflow.contacts[0] = UINT32_MAX;
	overflow.contacts[1] = 1;
	const auto invalid = overflow.Encode();
	ASSERT_FALSE(ContactDistribution::Decode(invalid.data(), invalid.size(), decoded));
	ASSERT_EQUALS(100000u, decoded.Total());
	// A crowded single bin retains full 32-bit counts, without 16-bit truncation.
	ContactDistribution crowded;
	crowded.contacts[2048] = 100000;
	const auto crowdedWire = crowded.Encode();
	ASSERT_TRUE(ContactDistribution::Decode(crowdedWire.data(), crowdedWire.size(), decoded));
	ASSERT_EQUALS(100000u, decoded.contacts[2048]);
}

TEST(ContactDistribution, RemoteLoadingAndRequestedReplies)
{
	Kademlia::ContactDistributionCache cache;
	ContactDistribution snapshot;
	ASSERT_TRUE(cache.Get(snapshot) == Kademlia::ContactDistributionState::Loading);
	cache.Update(false, false, nullptr, 0); // a hidden-panel stats reply, without a tag
	ASSERT_TRUE(cache.Get(snapshot) == Kademlia::ContactDistributionState::Loading);
	cache.Update(true, false, nullptr, 0); // older core answered an opted-in request
	ASSERT_TRUE(cache.Get(snapshot) == Kademlia::ContactDistributionState::Unsupported);
	ContactDistribution data;
	data.contacts[123] = 12;
	auto wire = data.Encode();
	cache.Update(true, true, wire.data(), wire.size());
	ASSERT_TRUE(cache.Get(snapshot) == Kademlia::ContactDistributionState::Available);
	ASSERT_EQUALS(12u, snapshot.contacts[123]);
	cache.Update(false, false, nullptr, 0); // hiding does not clear the snapshot
	ASSERT_TRUE(cache.Get(snapshot) == Kademlia::ContactDistributionState::Available);
	ASSERT_EQUALS(12u, snapshot.Total());
	cache.Reset(); // reconnect to a different core
	ASSERT_TRUE(cache.Get(snapshot) == Kademlia::ContactDistributionState::Loading);
	ASSERT_EQUALS(0u, snapshot.Total());
	wire[0] = 99;
	cache.Update(true, true, wire.data(), wire.size());
	ASSERT_TRUE(cache.Get(snapshot) == Kademlia::ContactDistributionState::Invalid);
	ASSERT_EQUALS(0u, snapshot.Total());
	cache.Update(true, true, nullptr, 0); // wrong EC tag type or empty custom tag
	ASSERT_TRUE(cache.Get(snapshot) == Kademlia::ContactDistributionState::Invalid);
}
