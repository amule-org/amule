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

TEST(KadReceiverKeyPolicy, CountsValidAndMissingKeysSeparatelyPerHandler)
{
	CKadReceiverKeyCounts counts;
	counts.Record(EKadReceiverKeyHandler::KeywordPublish, true);
	counts.Record(EKadReceiverKeyHandler::KeywordPublish, false);

	ASSERT_EQUALS(1, counts.GetValid(EKadReceiverKeyHandler::KeywordPublish));
	ASSERT_EQUALS(1, counts.GetMissing(EKadReceiverKeyHandler::KeywordPublish));
	ASSERT_EQUALS(0, counts.GetValid(EKadReceiverKeyHandler::SourcePublish));
	ASSERT_EQUALS(0, counts.GetMissing(EKadReceiverKeyHandler::SourcePublish));
}

TEST(KadReceiverKeyPolicy, HandlersDoNotShareBuckets)
{
	CKadReceiverKeyCounts counts;
	counts.Record(EKadReceiverKeyHandler::KeywordPublish, true);
	counts.Record(EKadReceiverKeyHandler::SourcePublish, false);

	ASSERT_EQUALS(1, counts.GetValid(EKadReceiverKeyHandler::KeywordPublish));
	ASSERT_EQUALS(0, counts.GetMissing(EKadReceiverKeyHandler::KeywordPublish));
	ASSERT_EQUALS(0, counts.GetValid(EKadReceiverKeyHandler::SourcePublish));
	ASSERT_EQUALS(1, counts.GetMissing(EKadReceiverKeyHandler::SourcePublish));
}

TEST(KadReceiverKeyPolicy, TheKad2FirewallCheckNeedsAProvenSender)
{
	ASSERT_TRUE(DropsWithoutReceiverKey(EKadReceiverKeyHandler::Firewall2Check, false));
	ASSERT_FALSE(DropsWithoutReceiverKey(EKadReceiverKeyHandler::Firewall2Check, true));
}

TEST(KadReceiverKeyPolicy, NoOtherHandlerDropsAWholeRequestWithoutAKey)
{
	for (std::size_t i = 0; i < static_cast<std::size_t>(EKadReceiverKeyHandler::Count); ++i) {
		const auto handler = static_cast<EKadReceiverKeyHandler>(i);
		if (handler == EKadReceiverKeyHandler::Firewall2Check) {
			continue;
		}
		ASSERT_FALSE(DropsWithoutReceiverKey(handler, false));
		ASSERT_FALSE(DropsWithoutReceiverKey(handler, true));
	}
}

TEST(KadReceiverKeyPolicy, AnUnkeyedTypeSevenPublishIsDropped)
{
	ASSERT_TRUE(DropsUnkeyedSourcePublish(7, false, false, false));
	ASSERT_FALSE(DropsUnkeyedSourcePublish(7, false, false, true));
}

// The pointer route travels on a classic record, and its own record is a type 3 at a derived key.
TEST(KadReceiverKeyPolicy, AnUnkeyedPointerTagOrPointerRecordIsDropped)
{
	ASSERT_TRUE(DropsUnkeyedSourcePublish(3, true, false, false));
	ASSERT_TRUE(DropsUnkeyedSourcePublish(5, false, true, false));
	ASSERT_FALSE(DropsUnkeyedSourcePublish(3, true, false, true));
	ASSERT_FALSE(DropsUnkeyedSourcePublish(5, false, true, true));
}

TEST(KadReceiverKeyPolicy, ClassicSourcePublishesAreKeptWithoutAKey)
{
	for (uint8_t sourceType : { 0, 1, 2, 3, 4, 5, 6, 8 }) {
		ASSERT_FALSE(DropsUnkeyedSourcePublish(sourceType, false, false, false));
		ASSERT_FALSE(DropsUnkeyedSourcePublish(sourceType, false, false, true));
	}
}
