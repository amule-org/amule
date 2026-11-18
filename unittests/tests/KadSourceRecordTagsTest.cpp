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
#include "KadSourceRecordTags.h"

#include "KadBuddyIdLayout.h"
#include "MD4Hash.h"
#include <tags/FileTags.h>

using namespace muleunit;

DECLARE_SIMPLE(KadSourceRecordTags)

namespace
{
SKadBuddyRoute Route(uint16_t kadPort = 4672)
{
	SKadBuddyRoute route;
	route.buddyIP = 0x0A000001;
	route.buddyUDPPort = 4673;
	for (unsigned i = 0; i < 16; ++i) {
		route.buddyID[i] = static_cast<uint8_t>(i + 1);
	}
	route.tcpPort = 4662;
	route.kadPort = kadPort;
	return route;
}

const CTag *Find(const TagPtrList &tags, const wxString &name)
{
	for (const CTag *tag : tags) {
		if (tag->GetName() == name) {
			return tag;
		}
	}
	return nullptr;
}

struct STags
{
	TagPtrList list;
	~STags() { deleteTagPtrListEntries(&list); }
};
} // namespace

// The fields a receiver needs for a KADEMLIA_CALLBACK_REQ, with the buddy ID in eMule's layout.
TEST(KadSourceRecordTags, ThePointerRecordIsAType3WithTheBuddyRoute)
{
	STags tags;
	AddKadPointerRecordTags(tags.list, Route(), 0x0B);
	ASSERT_EQUALS(static_cast<uint64>(3), Find(tags.list, TAG_SOURCETYPE)->GetInt());
	ASSERT_EQUALS(static_cast<uint64>(0x0A000001), Find(tags.list, TAG_SERVERIP)->GetInt());
	ASSERT_EQUALS(static_cast<uint64>(4673), Find(tags.list, TAG_SERVERPORT)->GetInt());
	uint8_t emuleLayout[16];
	Kademlia::ConvertBuddyIdLayout(Route().buddyID, emuleLayout);
	ASSERT_EQUALS(CMD4Hash(emuleLayout).Encode(), Find(tags.list, TAG_BUDDYHASH)->GetStr());
	ASSERT_EQUALS(static_cast<uint64>(4662), Find(tags.list, TAG_SOURCEPORT)->GetInt());
	ASSERT_EQUALS(static_cast<uint64>(4672), Find(tags.list, TAG_SOURCEUPORT)->GetInt());
	ASSERT_EQUALS(static_cast<uint64>(1), Find(tags.list, TAG_BUDDYPOINTER)->GetInt());
	ASSERT_EQUALS(static_cast<uint64>(0x0B), Find(tags.list, TAG_ENCRYPTION)->GetInt());
	ASSERT_EQUALS(static_cast<size_t>(8), tags.list.size());
}

// It names no file, so it carries no size.
TEST(KadSourceRecordTags, ThePointerRecordHasNoSize)
{
	STags tags;
	AddKadPointerRecordTags(tags.list, Route(), 0);
	ASSERT_TRUE(Find(tags.list, TAG_FILESIZE) == nullptr);
}

// With the external Kad port, storers take the port the publish came from.
TEST(KadSourceRecordTags, TheKadPortIsLeftOutWhenTheExternalOneIsUsed)
{
	STags tags;
	AddKadPointerRecordTags(tags.list, Route(0), 0);
	ASSERT_TRUE(Find(tags.list, TAG_SOURCEUPORT) == nullptr);
}

TEST(KadSourceRecordTags, AMarkedFileRecordCarriesThePointerVersion)
{
	STags tags;
	AddKadBuddyRecordTags(tags.list, Route(), 5, 9728000, true);
	ASSERT_EQUALS(static_cast<uint64>(5), Find(tags.list, TAG_SOURCETYPE)->GetInt());
	ASSERT_EQUALS(static_cast<uint64>(0x0A000001), Find(tags.list, TAG_SERVERIP)->GetInt());
	ASSERT_EQUALS(static_cast<uint64>(9728000), Find(tags.list, TAG_FILESIZE)->GetInt());
	ASSERT_EQUALS(static_cast<uint64>(1), Find(tags.list, TAG_BUDDYPOINTER)->GetInt());
}

// An unkeyed storer gets the record it always got: a storer following §5.6 would drop it marked.
TEST(KadSourceRecordTags, AnUnmarkedFileRecordIsTheClassicOne)
{
	STags tags;
	AddKadBuddyRecordTags(tags.list, Route(), 3, 9728000, false);
	ASSERT_TRUE(Find(tags.list, TAG_BUDDYPOINTER) == nullptr);
	ASSERT_EQUALS(static_cast<size_t>(7), tags.list.size());
}

// A Kad 1 storer does not read the size.
TEST(KadSourceRecordTags, ASizeOfZeroIsLeftOut)
{
	STags tags;
	AddKadBuddyRecordTags(tags.list, Route(), 3, 0, false);
	ASSERT_TRUE(Find(tags.list, TAG_FILESIZE) == nullptr);
}
