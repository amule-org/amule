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
	// The one accepting combination: every fact set except the proxy.
	for (unsigned bits = 0; bits < 64; ++bits) {
		const SUtpDialFacts facts{ (bits % 2) != 0,
			((bits / 2) % 2) != 0,
			((bits / 4) % 2) != 0,
			((bits / 8) % 2) != 0,
			((bits / 16) % 2) != 0,
			((bits / 32) % 2) != 0 };
		ASSERT_EQUALS(bits == 55, DecideUtpDial(facts) == EUtpDialDecision::TryUtp);
	}
}

TEST(UtpDialPolicy, EndpointRoutability)
{
	SUtpDialFacts facts{ true, true, true, false, true, true };
	ASSERT_TRUE(DecideUtpDial(facts) == EUtpDialDecision::TryUtp);
	facts.routableEndpoint = false;
	ASSERT_TRUE(DecideUtpDial(facts) == EUtpDialDecision::PreserveLegacy);
}

TEST(UtpDialPolicy, SimultaneousConnectionsUseOppositeDirectionDecisions)
{
	const uint8_t lower[16] = { 0 };
	const uint8_t higher[16] = { 1 };

	ASSERT_TRUE(ShouldKeepFoundUtp(false, lower, higher));
	ASSERT_TRUE(ShouldKeepFoundUtp(true, higher, lower));
	ASSERT_FALSE(ShouldKeepFoundUtp(true, lower, higher));
	ASSERT_FALSE(ShouldKeepFoundUtp(false, higher, lower));
}

// A peer owed obfuscation is kept on TCP rather than dialled in the clear: the
// stream handshake does not run over a transport, so the frames are the only
// thing that could carry it.
TEST(UtpDialPolicy, ObfuscationOwedButUnavailableStaysOnTcp)
{
	SUtpDialFacts facts{ true, true, true, false, true, true };
	ASSERT_TRUE(DecideUtpDial(facts) == EUtpDialDecision::TryUtp);
	facts.obfuscationSatisfied = false;
	ASSERT_TRUE(DecideUtpDial(facts) == EUtpDialDecision::PreserveLegacy);
}
