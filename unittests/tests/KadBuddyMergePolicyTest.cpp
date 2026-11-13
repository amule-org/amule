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

#include "KadBuddyMergePolicy.h"

using namespace muleunit;

DECLARE_SIMPLE(KadBuddyMergePolicy)

namespace
{
SKadBuddyMergeFacts KadCopyFor(bool existingHasID, bool existingHasAddress, bool sameAddress)
{
	SKadBuddyMergeFacts facts;
	facts.incomingHasID = true;
	facts.incomingHasAddress = true;
	facts.incomingFromKad = true;
	facts.existingHasID = existingHasID;
	facts.existingHasAddress = existingHasAddress;
	facts.sameAddress = sameAddress;
	return facts;
}
} // namespace

TEST(KadBuddyMergePolicy, KadCopyFillsAMissingBuddy)
{
	const SKadBuddyMerge merge = DecideKadBuddyMerge(KadCopyFor(false, false, false));
	ASSERT_TRUE(merge.takeID);
	ASSERT_TRUE(merge.takeAddress);
	ASSERT_FALSE(merge.storeCandidate);
}

TEST(KadBuddyMergePolicy, KadCopyFillsOnlyTheMissingAddress)
{
	const SKadBuddyMerge merge = DecideKadBuddyMerge(KadCopyFor(true, false, false));
	ASSERT_FALSE(merge.takeID);
	ASSERT_TRUE(merge.takeAddress);
}

TEST(KadBuddyMergePolicy, DifferentAddressIsStoredNotApplied)
{
	const SKadBuddyMerge merge = DecideKadBuddyMerge(KadCopyFor(true, true, false));
	ASSERT_FALSE(merge.takeID);
	ASSERT_FALSE(merge.takeAddress);
	ASSERT_TRUE(merge.storeCandidate);
}

TEST(KadBuddyMergePolicy, UnchangedBuddyChangesNothing)
{
	const SKadBuddyMerge merge = DecideKadBuddyMerge(KadCopyFor(true, true, true));
	ASSERT_FALSE(merge.takeID || merge.takeAddress || merge.storeCandidate);
}

TEST(KadBuddyMergePolicy, OtherCopyNeverStoresACandidate)
{
	SKadBuddyMergeFacts facts = KadCopyFor(true, true, false);
	facts.incomingFromKad = false;
	const SKadBuddyMerge merge = DecideKadBuddyMerge(facts);
	ASSERT_FALSE(merge.takeAddress || merge.storeCandidate);
}

TEST(KadBuddyMergePolicy, CopyWithoutBuddyIDChangesNothing)
{
	SKadBuddyMergeFacts facts = KadCopyFor(false, false, false);
	facts.incomingHasID = false;
	const SKadBuddyMerge merge = DecideKadBuddyMerge(facts);
	ASSERT_FALSE(merge.takeID || merge.takeAddress || merge.storeCandidate);
}

// Only an ID from Kad is passed on, so it replaces one learned from source exchange.
TEST(KadBuddyMergePolicy, KadIDReplacesASourceExchangeID)
{
	SKadBuddyMergeFacts facts = KadCopyFor(true, true, true);
	facts.incomingIDFromKad = true;
	facts.existingIDFromKad = false;
	ASSERT_TRUE(DecideKadBuddyMerge(facts).takeID);

	facts.existingIDFromKad = true;
	ASSERT_FALSE(DecideKadBuddyMerge(facts).takeID);
}

TEST(KadBuddyMergePolicy, SourceExchangeIDNeverReplacesAnID)
{
	SKadBuddyMergeFacts facts = KadCopyFor(true, true, true);
	facts.incomingFromKad = false;
	facts.incomingIDFromKad = false;
	facts.existingIDFromKad = false;
	ASSERT_FALSE(DecideKadBuddyMerge(facts).takeID);
	facts.existingHasID = false;
	ASSERT_TRUE(DecideKadBuddyMerge(facts).takeID);
}

TEST(KadBuddyMergePolicy, SwitchesOnlyAfterAFailedCallback)
{
	ASSERT_TRUE(ShouldSwitchToBuddyCandidate(true, true));
	ASSERT_FALSE(ShouldSwitchToBuddyCandidate(true, false));
}

TEST(KadBuddyMergePolicy, NoSwitchWithoutACandidate)
{
	ASSERT_FALSE(ShouldSwitchToBuddyCandidate(false, true));
}
