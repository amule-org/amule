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
#include "UtpDialPolicy.h"

using namespace muleunit;

DECLARE_SIMPLE(UtpDialPolicy)

TEST(UtpDialPolicy, DecisionMatrix)
{
	constexpr uint32_t peer = 0x04030201; // 1.2.3.4, low byte first
	for (unsigned bits = 0; bits < 64; ++bits) {
		const SUtpDialFacts facts{ (bits % 2) != 0,
			((bits / 2) % 2) != 0,
			((bits / 4) % 2) != 0,
			((bits / 8) % 2) != 0,
			((bits / 16) % 2) != 0 ? peer : 0,
			static_cast<uint16_t>((bits / 32) != 0 ? 4672 : 0) };
		ASSERT_EQUALS(bits == 55, DecideUtpDial(facts) == EUtpDialDecision::TryUtp);
	}
}

TEST(UtpDialPolicy, EndpointBoundaries)
{
	for (uint32_t ip : { 0u, 0x01020300u, 0x010000E0u, 0x010000F0u, 0xffffffffu }) {
		ASSERT_FALSE(IsUsableUtpEndpoint(ip, 4672));
	}
	ASSERT_FALSE(IsUsableUtpEndpoint(0x04030201, 0));
	ASSERT_TRUE(IsUsableUtpEndpoint(0x04030201, 1));
	ASSERT_TRUE(IsUsableUtpEndpoint(0x04030201, 65535));
	ASSERT_TRUE(IsUsableUtpEndpoint(0x0100007f, 4672));
	ASSERT_TRUE(IsUsableUtpEndpoint(0x0100000a, 4672));
	ASSERT_TRUE(DecideUtpDial({}) == EUtpDialDecision::PreserveLegacy);
}
