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

#include "kademlia/net/KadReceiverKeyPolicy.h"

using namespace muleunit;

DECLARE_SIMPLE(KadReceiverKeyPolicy)

TEST(KadReceiverKeyPolicy, AValidKeyIsProcessedEitherWay)
{
	// The preference exists to govern requests that carry no proof; a proved sender is never
	// affected by it.
	ASSERT_EQUALS(static_cast<int>(EKadReceiverKeyDecision::Process),
		static_cast<int>(DecideKadReceiverKeyRequest(true, false)));
	ASSERT_EQUALS(static_cast<int>(EKadReceiverKeyDecision::Process),
		static_cast<int>(DecideKadReceiverKeyRequest(true, true)));
}

TEST(KadReceiverKeyPolicy, WithoutAKeyAndEnforcementOffItIsProcessedAndCounted)
{
	// eMule official sends no key: turning enforcement off has to keep those requests working, and
	// the count is how the user gets the evidence to decide whether to turn it on.
	ASSERT_EQUALS(static_cast<int>(EKadReceiverKeyDecision::ProcessAndCount),
		static_cast<int>(DecideKadReceiverKeyRequest(false, false)));
}

TEST(KadReceiverKeyPolicy, WithoutAKeyAndEnforcementOnItIsRejected)
{
	ASSERT_EQUALS(static_cast<int>(EKadReceiverKeyDecision::Reject),
		static_cast<int>(DecideKadReceiverKeyRequest(false, true)));
}
