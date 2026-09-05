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
#include <kademlia/kademlia/AICHHashList.h>
#include <protocol/kad2/Constants.h>

using namespace muleunit;
using Kademlia::CKadAICHHash;
using Kademlia::CKadAICHHashList;

// Builds a distinguishable 20-byte AICH root hash from a single seed byte.
static CKadAICHHash MakeHash(uint8_t seed)
{
	CKadAICHHash hash;
	for (size_t i = 0; i < hash.size(); ++i) {
		hash[i] = (uint8_t)(seed + i);
	}
	return hash;
}

DECLARE_SIMPLE(KadAICHHashList)

TEST(KadAICHHashList, EmptyListEncodesNothing)
{
	CKadAICHHashList list;
	ASSERT_TRUE(list.IsEmpty());
	ASSERT_EQUALS(0u, (unsigned)list.GetSlotCount());
	ASSERT_EQUALS(0u, (unsigned)list.GetReferencedCount());
	ASSERT_TRUE(list.EncodeResultTag().empty());
}

TEST(KadAICHHashList, AddReferenceReturnsStableIndex)
{
	CKadAICHHashList list;
	const CKadAICHHash first = MakeHash(1);
	const CKadAICHHash second = MakeHash(100);

	ASSERT_EQUALS(0u, (unsigned)list.AddReference(first));
	ASSERT_EQUALS(1u, (unsigned)list.AddReference(second));
	// Re-adding an identical hash reuses its slot and bumps its popularity.
	ASSERT_EQUALS(0u, (unsigned)list.AddReference(first));

	ASSERT_EQUALS(2u, (unsigned)list.GetSlotCount());
	ASSERT_EQUALS(2u, (unsigned)list.GetReferencedCount());
	ASSERT_EQUALS(2u, (unsigned)list.GetPopularityAt(0));
	ASSERT_EQUALS(1u, (unsigned)list.GetPopularityAt(1));
	ASSERT_TRUE(list.GetHashAt(0) == first);
	ASSERT_TRUE(list.GetHashAt(1) == second);
}

TEST(KadAICHHashList, DropReferenceKeepsSlotButClearsPopularity)
{
	CKadAICHHashList list;
	const CKadAICHHash hash = MakeHash(7);
	list.AddReference(hash);
	list.AddReference(hash);

	list.DropReferenceAt(0);
	ASSERT_EQUALS(1u, (unsigned)list.GetPopularityAt(0));
	ASSERT_EQUALS(1u, (unsigned)list.GetReferencedCount());

	list.DropReferenceAt(0);
	ASSERT_EQUALS(0u, (unsigned)list.GetPopularityAt(0));
	ASSERT_EQUALS(0u, (unsigned)list.GetReferencedCount());
	// The slot survives so that publisher-held indexes stay valid; only
	// BuildCompactionMap() renumbers.
	ASSERT_EQUALS(1u, (unsigned)list.GetSlotCount());
	ASSERT_TRUE(list.IsEmpty());

	// Dropping below zero must not underflow the popularity counter.
	list.DropReferenceAt(0);
	ASSERT_EQUALS(0u, (unsigned)list.GetPopularityAt(0));

	// An out-of-range index is ignored rather than corrupting the list.
	list.DropReferenceAt(CKadAICHHashList::INVALID_INDEX);
	ASSERT_EQUALS(1u, (unsigned)list.GetSlotCount());
}

TEST(KadAICHHashList, BuildCompactionMapRenumbersReferencedSlotsOnly)
{
	CKadAICHHashList list;
	list.AddReference(MakeHash(1)); // slot 0, popularity 1
	list.AddReference(MakeHash(2)); // slot 1, popularity 1
	list.AddReference(MakeHash(3)); // slot 2, popularity 1
	list.DropReferenceAt(1);        // slot 1 now unreferenced

	std::vector<uint16_t> map = list.BuildCompactionMap();
	ASSERT_EQUALS(3u, (unsigned)map.size());
	ASSERT_EQUALS(0u, (unsigned)map[0]);
	ASSERT_EQUALS((unsigned)CKadAICHHashList::INVALID_INDEX, (unsigned)map[1]);
	ASSERT_EQUALS(1u, (unsigned)map[2]);
	ASSERT_EQUALS(2u, (unsigned)list.GetReferencedCount());
}

// Wire format, pinned byte for byte:
//   TAG_KADAICHHASHRESULT payload = <Count 1>{<Publishers 1><AICH Hash 20>} Count
TEST(KadAICHHashList, EncodeResultTagPinsTheWireLayout)
{
	CKadAICHHashList list;
	const CKadAICHHash hash = MakeHash(0x10);
	list.AddReference(hash);
	list.AddReference(hash);
	list.AddReference(MakeHash(0x40));
	list.DropReferenceAt(1); // unreferenced slots are not carried

	std::vector<uint8_t> encoded = list.EncodeResultTag();
	ASSERT_EQUALS(1u + 1u + 20u, (unsigned)encoded.size());
	ASSERT_EQUALS(1u, (unsigned)encoded[0]); // hash count
	ASSERT_EQUALS(2u, (unsigned)encoded[1]); // publishers of the first hash
	for (size_t i = 0; i < CKadAICHHash().size(); ++i) {
		ASSERT_EQUALS((unsigned)hash[i], (unsigned)encoded[2 + i]);
	}
}

TEST(KadAICHHashList, EncodeResultTagTruncatesToTheBsobBudget)
{
	CKadAICHHashList list;
	for (unsigned i = 0; i < 30; ++i) {
		list.AddReference(MakeHash((uint8_t)(i * 3 + 1)));
	}
	ASSERT_EQUALS(30u, (unsigned)list.GetReferencedCount());

	std::vector<uint8_t> encoded = list.EncodeResultTag();
	ASSERT_EQUALS((unsigned)CKadAICHHashList::MAX_RESULT_HASHES, (unsigned)encoded[0]);
	ASSERT_EQUALS(
		1u + (1u + 20u) * (unsigned)CKadAICHHashList::MAX_RESULT_HASHES, (unsigned)encoded.size());
	// Kad BSOB tags carry a uint8 length, so the payload must fit in 255
	// bytes; the class holds itself to the tighter 250-byte budget.
	ASSERT_TRUE(encoded.size() <= CKadAICHHashList::MAX_RESULT_TAG_SIZE);
}

TEST(KadAICHHashList, DecodeResultTagRoundTripsEncode)
{
	CKadAICHHashList list;
	const CKadAICHHash popular = MakeHash(0x21);
	const CKadAICHHash rare = MakeHash(0x81);
	list.AddReference(popular);
	list.AddReference(popular);
	list.AddReference(popular);
	list.AddReference(rare);

	std::vector<uint8_t> encoded = list.EncodeResultTag();
	std::vector<CKadAICHHashList::SResultHash> decoded;
	ASSERT_TRUE(CKadAICHHashList::DecodeResultTag(&encoded[0], encoded.size(), decoded));

	ASSERT_EQUALS(2u, (unsigned)decoded.size());
	ASSERT_EQUALS(3u, (unsigned)decoded[0].m_popularity);
	ASSERT_TRUE(decoded[0].m_hash == popular);
	ASSERT_EQUALS(1u, (unsigned)decoded[1].m_popularity);
	ASSERT_TRUE(decoded[1].m_hash == rare);
}

TEST(KadAICHHashList, DecodeResultTagRejectsMalformedPayloads)
{
	std::vector<CKadAICHHashList::SResultHash> decoded;

	// No payload at all.
	ASSERT_FALSE(CKadAICHHashList::DecodeResultTag(NULL, 0, decoded));

	// Announces one hash but carries a truncated one.
	std::vector<uint8_t> truncated(1 + 1 + 19, 0);
	truncated[0] = 1;
	truncated[1] = 1;
	ASSERT_FALSE(CKadAICHHashList::DecodeResultTag(&truncated[0], truncated.size(), decoded));
	ASSERT_TRUE(decoded.empty());

	// Announces more hashes than the wire budget allows.
	std::vector<uint8_t> overlong(1 + (1 + 20) * 12, 0);
	overlong[0] = 12;
	ASSERT_FALSE(CKadAICHHashList::DecodeResultTag(&overlong[0], overlong.size(), decoded));
	ASSERT_TRUE(decoded.empty());
}

TEST(KadAICHHashList, DecodeResultTagDropsZeroPopularityEntries)
{
	// A zero publisher count carries no information and is what a peer that
	// has evicted its publishers would emit; it must not reach the caller.
	std::vector<uint8_t> payload(1 + (1 + 20) * 2, 0);
	payload[0] = 2;
	payload[1] = 0; // first hash: no publishers
	payload[1 + 1 + 20] = 5;
	payload[1 + 1 + 20 + 1] = 0xAB;

	std::vector<CKadAICHHashList::SResultHash> decoded;
	ASSERT_TRUE(CKadAICHHashList::DecodeResultTag(&payload[0], payload.size(), decoded));
	ASSERT_EQUALS(1u, (unsigned)decoded.size());
	ASSERT_EQUALS(5u, (unsigned)decoded[0].m_popularity);
	ASSERT_EQUALS(0xABu, (unsigned)decoded[0].m_hash[0]);
}

TEST(KadAICHHashList, DecodeResultTagIgnoresTrailingGarbage)
{
	// A well-formed prefix followed by junk is accepted for the announced
	// count only: peers may pad, and an honest count must still be usable.
	std::vector<uint8_t> payload(1 + (1 + 20) + 7, 0xFF);
	payload[0] = 1;
	payload[1] = 3;

	std::vector<CKadAICHHashList::SResultHash> decoded;
	ASSERT_TRUE(CKadAICHHashList::DecodeResultTag(&payload[0], payload.size(), decoded));
	ASSERT_EQUALS(1u, (unsigned)decoded.size());
	ASSERT_EQUALS(3u, (unsigned)decoded[0].m_popularity);
}

TEST(KadAICHHashList, GetMostPopularPicksTheHighestPublisherCount)
{
	std::vector<CKadAICHHashList::SResultHash> decoded;
	CKadAICHHashList::SResultHash a = { 2, MakeHash(1) };
	CKadAICHHashList::SResultHash b = { 9, MakeHash(2) };
	CKadAICHHashList::SResultHash c = { 4, MakeHash(3) };
	decoded.push_back(a);
	decoded.push_back(b);
	decoded.push_back(c);

	const CKadAICHHashList::SResultHash *best = CKadAICHHashList::GetMostPopular(decoded);
	ASSERT_TRUE(best != NULL);
	ASSERT_EQUALS(9u, (unsigned)best->m_popularity);
	ASSERT_TRUE(best->m_hash == MakeHash(2));

	decoded.clear();
	ASSERT_TRUE(CKadAICHHashList::GetMostPopular(decoded) == NULL);
}

// Task 1.5: mixed-version publish and search. Both the publish gate
// (TAG_KADAICHHASHPUB) and the result gate (TAG_KADAICHHASHRESULT) consult this
// one predicate, so pinning it pins the version behaviour of both directions.
TEST(KadAICHHashList, AICHKeywordStorageIsGatedOnKadVersion0x09)
{
	// A peer we could not identify gets no credit.
	ASSERT_FALSE(CKadAICHHashList::PeerSupportsAICHKeywordStorage(0x00));
	// Every version below 0x09 is unaffected: it is neither sent the publish
	// tag nor believed if it claims to send the result tag.
	ASSERT_FALSE(CKadAICHHashList::PeerSupportsAICHKeywordStorage(KADEMLIA_VERSION6_49aBETA));
	ASSERT_FALSE(CKadAICHHashList::PeerSupportsAICHKeywordStorage(KADEMLIA_VERSION7_49a));
	ASSERT_FALSE(CKadAICHHashList::PeerSupportsAICHKeywordStorage(KADEMLIA_VERSION8_49b));
	// 0x09 introduced AICH hashes on keyword storage; 0x0a and later keep it.
	ASSERT_TRUE(CKadAICHHashList::PeerSupportsAICHKeywordStorage(KADEMLIA_VERSION9_50a));
	ASSERT_TRUE(CKadAICHHashList::PeerSupportsAICHKeywordStorage(0x0a));
	ASSERT_TRUE(CKadAICHHashList::PeerSupportsAICHKeywordStorage(0xFF));
}

// The version byte we advertise is what makes this change visible on the wire,
// so both states of the ENABLE_KAD_PROTOCOL_10 switch are pinned here: with the
// switch off aMule must still announce 0x08, exactly as upstream does, and it
// must not claim AICH keyword storage it does not use.
TEST(KadAICHHashList, AdvertisedKadVersionFollowsTheBuildSwitch)
{
#ifdef ENABLE_KAD_PROTOCOL_10
	ASSERT_EQUALS(0x0au, (unsigned)KADEMLIA_VERSION);
	ASSERT_TRUE(CKadAICHHashList::PeerSupportsAICHKeywordStorage(KADEMLIA_VERSION));
#else
	ASSERT_EQUALS(0x08u, (unsigned)KADEMLIA_VERSION);
	ASSERT_FALSE(CKadAICHHashList::PeerSupportsAICHKeywordStorage(KADEMLIA_VERSION));
#endif
	// The eD2k CT_EMULE_MISCOPTIONS2 capability field reserves four bits for
	// the Kad version (BaseClient.cpp, uKadVersion << 0), so a bump past
	// 0x0F needs that field changed first.
	ASSERT_TRUE(KADEMLIA_VERSION <= 0x0F);
}

TEST(KadAICHHashList, PopularitySaturatesInsteadOfWrapping)
{
	CKadAICHHashList list;
	const CKadAICHHash hash = MakeHash(0x55);
	for (unsigned i = 0; i < 300; ++i) {
		list.AddReference(hash);
	}
	// Popularity travels the wire as a uint8, so it must clamp at 255
	// rather than wrap back through zero and lose the hash entirely.
	ASSERT_EQUALS(255u, (unsigned)list.GetPopularityAt(0));
	ASSERT_EQUALS(1u, (unsigned)list.GetSlotCount());
}
