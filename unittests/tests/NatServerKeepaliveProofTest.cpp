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

#include "NatServerKeepaliveProof.h"

#include <algorithm>

using namespace muleunit;
using namespace NatServerHolePunch;

DECLARE_SIMPLE(NatServerKeepaliveProof)

namespace
{
const Hash kToken{
	{ 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f }
};
const Hash kUserHash{
	{ 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f }
};
} // namespace

// The server's published test vector.
TEST(NatServerKeepaliveProof, MatchesTheServerTestVector)
{
	const std::vector<std::uint8_t> proof = BuildKeepaliveProof(kUserHash, kToken, 1);
	const std::vector<std::uint8_t> mac{
		0x57, 0xf6, 0x31, 0xc9, 0x01, 0xf6, 0x52, 0xe6, 0x92, 0x30, 0x7d, 0xb3, 0xea, 0x25, 0xbe, 0x83
	};
	ASSERT_EQUALS(kKeepaliveProofSize, proof.size());
	ASSERT_TRUE(std::vector<std::uint8_t>(proof.begin() + 20, proof.end()) == mac);
}

TEST(NatServerKeepaliveProof, CarriesTheUserHashThenTheCounterLittleEndian)
{
	const std::vector<std::uint8_t> proof = BuildKeepaliveProof(kUserHash, kToken, 0x04030201);
	ASSERT_TRUE(std::equal(kUserHash.begin(), kUserHash.end(), proof.begin()));
	ASSERT_EQUALS(0x01, proof[16]);
	ASSERT_EQUALS(0x02, proof[17]);
	ASSERT_EQUALS(0x03, proof[18]);
	ASSERT_EQUALS(0x04, proof[19]);
	// A server without the proof still reads the user hash.
	ASSERT_TRUE(ParseKeepalive(proof.data(), proof.size()).value() == kUserHash);
}

TEST(NatServerKeepaliveProof, TheMacDependsOnTheCounterAndTheToken)
{
	const std::vector<std::uint8_t> first = BuildKeepaliveProof(kUserHash, kToken, 1);
	ASSERT_FALSE(first == BuildKeepaliveProof(kUserHash, kToken, 2));
	Hash otherToken = kToken;
	otherToken[0] ^= 0xff;
	ASSERT_FALSE(first == BuildKeepaliveProof(kUserHash, otherToken, 1));
}
