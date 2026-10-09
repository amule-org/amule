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

#include "UserHash.h"

#include "RandomFunctions.h" // Needed for GetRandomBlock

bool IsBadUserHash(const CMD4Hash &hash)
{
	for (size_t i = 0; i < MD4HASH_LENGTH; ++i) {
		if (i != USERHASH_MARKER_POS_1 && i != USERHASH_MARKER_POS_2 && hash[i] != 0) {
			return false;
		}
	}
	return true;
}

void CreateUserHash(CMD4Hash &hash)
{
	while (IsBadUserHash(hash)) {
		GetRandomBlock(hash.GetHash(), MD4HASH_LENGTH);
	}
}
