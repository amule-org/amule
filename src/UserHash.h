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

#ifndef USERHASH_H
#define USERHASH_H

#include "MD4Hash.h"

// Byte positions of the eMule client markers (14 and 111), which every userhash carries.
const size_t USERHASH_MARKER_POS_1 = 5;
const size_t USERHASH_MARKER_POS_2 = 14;

// True if the hash is zero apart from the marker bytes, as eMule's isbadhash().
bool IsBadUserHash(const CMD4Hash &hash);

// eMule's CPreferences::CreateUserHash(): while the hash is bad, fill it from the crypto RNG.
// A good hash is kept, so a loaded one survives. The caller applies the markers.
void CreateUserHash(CMD4Hash &hash);

#endif // USERHASH_H
