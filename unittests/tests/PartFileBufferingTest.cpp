// Copyright (c) 2026 aMule Team
// SPDX-License-Identifier: GPL-2.0-or-later
#include <muleunit/test.h>
#include "PartFileBuffering.h"

#include <array>
#include <cstdint>

using namespace muleunit;
DECLARE_SIMPLE(PartFileBuffering)

namespace
{
struct Write
{
	uint64_t end;
};
} // namespace

TEST(PartFileBuffering, EveryArrivalOrderProducesAscendingWrites)
{
	std::array<Write, 4> entries{ { { 199 }, { 399 }, { 299 }, { 99 } } };
	std::array<unsigned, 4> order{ 0, 1, 2, 3 };
	do {
		std::multimap<uint64_t, Write *> buffers;
		for (unsigned index : order) {
			InsertPartFileBufferedWrite(buffers, &entries[index]);
		}
		uint64_t expected = 99;
		for (const auto &entry : buffers) {
			ASSERT_EQUALS(expected, entry.second->end);
			expected += 100;
		}
		ASSERT_EQUALS(entries.size(), buffers.size());
	} while (std::next_permutation(order.begin(), order.end()));
}

TEST(PartFileBuffering, EqualEndsKeepTheirArrivalOrder)
{
	Write first{ 199 }, second{ 199 }, lower{ 99 }, higher{ 299 };
	std::multimap<uint64_t, Write *> buffers;
	for (Write *entry : { &first, &higher, &second, &lower }) {
		InsertPartFileBufferedWrite(buffers, entry);
	}
	const std::array<Write *, 4> expected{ &lower, &first, &second, &higher };
	auto actual = buffers.begin();
	for (const Write *entry : expected) {
		ASSERT_TRUE((actual++)->second == entry);
	}
}
