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
#include "IPv6ReachabilityProof.h"

using namespace muleunit;

DECLARE_SIMPLE(IPv6ReachabilityProof)

TEST(IPv6ReachabilityProof, UnconfirmedIsNotProven)
{
	CIPv6ReachabilityProof proof;
	ASSERT_FALSE(proof.IsProven(100));
}

TEST(IPv6ReachabilityProof, FreshConfirmationIsProven)
{
	CIPv6ReachabilityProof proof;
	proof.NoteHello(100);
	ASSERT_TRUE(proof.IsProven(100));
}

TEST(IPv6ReachabilityProof, ExpiresAtTheLifetimeBoundary)
{
	CIPv6ReachabilityProof proof;
	proof.NoteHello(100);
	ASSERT_TRUE(proof.IsProven(100 + kIPv6ReachabilityProofLifetimeMs - 1));
	ASSERT_FALSE(proof.IsProven(100 + kIPv6ReachabilityProofLifetimeMs));
}

TEST(IPv6ReachabilityProof, ClearRemovesConfirmation)
{
	CIPv6ReachabilityProof proof;
	proof.NoteHello(100);
	proof.Clear();
	ASSERT_FALSE(proof.IsProven(101));
}

TEST(IPv6ReachabilityProof, ConfirmationAfterExpiryProvesAgain)
{
	CIPv6ReachabilityProof proof;
	proof.NoteHello(100);
	proof.NoteHello(100 + kIPv6ReachabilityProofLifetimeMs);
	ASSERT_TRUE(proof.IsProven(100 + kIPv6ReachabilityProofLifetimeMs));
}

TEST(IPv6ReachabilityProof, BackwardsClockDoesNotProve)
{
	CIPv6ReachabilityProof proof;
	proof.NoteHello(101);
	ASSERT_FALSE(proof.IsProven(100));
}
