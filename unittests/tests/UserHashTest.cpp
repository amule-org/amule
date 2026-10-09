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

#include <muleunit/test.h>

#include "UserHash.h"

#include <set>

using namespace muleunit;

DECLARE_SIMPLE(UserHash)

TEST(UserHash, BadMeansZeroApartFromTheMarkers)
{
	CMD4Hash hash;
	ASSERT_TRUE(IsBadUserHash(hash));

	// The markers alone do not make a hash good, as in eMule's isbadhash().
	hash[USERHASH_MARKER_POS_1] = 14;
	hash[USERHASH_MARKER_POS_2] = 111;
	ASSERT_TRUE(IsBadUserHash(hash));

	for (size_t i = 0; i < MD4HASH_LENGTH; ++i) {
		if (i == USERHASH_MARKER_POS_1 || i == USERHASH_MARKER_POS_2) {
			continue;
		}
		CMD4Hash one;
		one[i] = 1;
		ASSERT_FALSE(IsBadUserHash(one));
	}
}

TEST(UserHash, ReplacesABadHash)
{
	CMD4Hash hash;
	hash[USERHASH_MARKER_POS_1] = 14;
	CreateUserHash(hash);
	ASSERT_FALSE(IsBadUserHash(hash));
}

TEST(UserHash, KeepsAGoodHash)
{
	// A hash loaded from preferences.dat must survive: peers key credits on it.
	CMD4Hash loaded;
	loaded[0] = 0x42;
	const CMD4Hash before = loaded;
	CreateUserHash(loaded);
	ASSERT_TRUE(loaded == before);
}

TEST(UserHash, NewHashesDiffer)
{
	// rand() seeded from the clock gave daemons started together the same hash.
	std::set<CMD4Hash> seen;
	for (int i = 0; i < 64; ++i) {
		CMD4Hash hash;
		CreateUserHash(hash);
		ASSERT_TRUE(seen.insert(hash).second);
	}
}
