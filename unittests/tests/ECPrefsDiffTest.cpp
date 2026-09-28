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

#include "ECPrefsDiff.h"

using namespace muleunit;

// amulegui sends the core only what the user changed, so a preference another EC client set
// since the last refresh is not written back. The core reads the result at EC_DETAIL_FULL, where
// an absent tag means "leave alone", so every boolean in it must carry a value.

DECLARE_SIMPLE(ECPrefsDiff)

namespace
{
// Shaped like CEC_Prefs_Packet at EC_DETAIL_UPDATE: booleans are bare tags.
CECPacket MakePrefs(uint32 maxUpload, bool filterComments, bool guest, bool webPassword)
{
	CECPacket prefs(EC_OP_SET_PREFERENCES, EC_DETAIL_UPDATE);

	CECEmptyTag connection(EC_TAG_PREFS_CONNECTIONS);
	connection.AddTag(CECTag(EC_TAG_CONN_MAX_UL, maxUpload));
	connection.AddTag(CECTag(EC_TAG_CONN_MAX_DL, static_cast<uint32>(500)));
	prefs.AddTag(connection);

	CECEmptyTag filter(EC_TAG_PREFS_MESSAGEFILTER);
	if (filterComments) {
		filter.AddTag(CECEmptyTag(EC_TAG_MSGFILTER_FILTER_COMMENTS));
	}
	filter.AddTag(CECTag(EC_TAG_MSGFILTER_COMMENT_KEYWORDS, wxString("spam")));
	prefs.AddTag(filter);

	CECEmptyTag remote(EC_TAG_PREFS_REMOTECTRL);
	if (webPassword) {
		remote.AddTag(CECTag(EC_TAG_PASSWD_HASH, CMD4Hash()));
	}
	if (guest) {
		CECEmptyTag guestTag(EC_TAG_WEBSERVER_GUEST);
		guestTag.AddTag(CECTag(EC_TAG_PASSWD_HASH, CMD4Hash()));
		remote.AddTag(guestTag);
	}
	remote.AddTag(CECEmptyTag(EC_TAG_AMULEAPI_PASSWD));
	prefs.AddTag(remote);

	return prefs;
}

const CECTag *Find(const CECPacket &packet, ec_tagname_t category, ec_tagname_t name)
{
	const CECTag *group = packet.GetTagByName(category);
	return group != nullptr ? group->GetTagByName(name) : nullptr;
}
} // namespace

TEST(ECPrefsDiff, NothingChangedSendsNothing)
{
	const CECPacket prefs = MakePrefs(100, true, true, true);
	ASSERT_EQUALS(0u, MakePrefsDiffPacket(prefs, prefs)->GetTagCount());
}

TEST(ECPrefsDiff, IsReadAtFullDetail)
{
	const std::unique_ptr<CECPacket> diff =
		MakePrefsDiffPacket(MakePrefs(100, true, true, true), MakePrefs(200, true, true, true));
	ASSERT_EQUALS(EC_OP_SET_PREFERENCES, diff->GetOpCode());
	ASSERT_EQUALS(EC_DETAIL_FULL, diff->GetDetailLevel());
}

// The #1659 case: another client turned the comment filter off, amulegui changed a limit.
TEST(ECPrefsDiff, SendsOnlyTheChangedValue)
{
	const std::unique_ptr<CECPacket> diff =
		MakePrefsDiffPacket(MakePrefs(100, false, true, true), MakePrefs(200, false, true, true));
	ASSERT_EQUALS(1u, diff->GetTagCount());
	const CECTag *connection = diff->GetTagByName(EC_TAG_PREFS_CONNECTIONS);
	ASSERT_TRUE(connection != nullptr);
	ASSERT_EQUALS(1u, connection->GetTagCount());
	ASSERT_EQUALS(200u, Find(*diff, EC_TAG_PREFS_CONNECTIONS, EC_TAG_CONN_MAX_UL)->GetInt());
}

TEST(ECPrefsDiff, FlagSwitchedOnCarriesTrue)
{
	const std::unique_ptr<CECPacket> diff =
		MakePrefsDiffPacket(MakePrefs(100, false, true, true), MakePrefs(100, true, true, true));
	const CECTag *flag = Find(*diff, EC_TAG_PREFS_MESSAGEFILTER, EC_TAG_MSGFILTER_FILTER_COMMENTS);
	ASSERT_TRUE(flag != nullptr);
	ASSERT_EQUALS(1u, flag->GetInt());
}

TEST(ECPrefsDiff, FlagSwitchedOffCarriesFalse)
{
	const std::unique_ptr<CECPacket> diff =
		MakePrefsDiffPacket(MakePrefs(100, true, true, true), MakePrefs(100, false, true, true));
	const CECTag *flag = Find(*diff, EC_TAG_PREFS_MESSAGEFILTER, EC_TAG_MSGFILTER_FILTER_COMMENTS);
	ASSERT_TRUE(flag != nullptr);
	ASSERT_EQUALS(0u, flag->GetInt());
}

TEST(ECPrefsDiff, GuestSwitchedOnKeepsItsPassword)
{
	const std::unique_ptr<CECPacket> diff =
		MakePrefsDiffPacket(MakePrefs(100, true, false, true), MakePrefs(100, true, true, true));
	const CECTag *guest = Find(*diff, EC_TAG_PREFS_REMOTECTRL, EC_TAG_WEBSERVER_GUEST);
	ASSERT_TRUE(guest != nullptr);
	ASSERT_EQUALS(1u, guest->GetInt());
	ASSERT_TRUE(guest->GetTagByName(EC_TAG_PASSWD_HASH) != nullptr);
}

TEST(ECPrefsDiff, GuestSwitchedOffCarriesFalse)
{
	const std::unique_ptr<CECPacket> diff =
		MakePrefsDiffPacket(MakePrefs(100, true, true, true), MakePrefs(100, true, false, true));
	const CECTag *guest = Find(*diff, EC_TAG_PREFS_REMOTECTRL, EC_TAG_WEBSERVER_GUEST);
	ASSERT_TRUE(guest != nullptr);
	ASSERT_EQUALS(0u, guest->GetInt());
}

// A missing hash means "unchanged", never "clear": it must not be sent at all.
TEST(ECPrefsDiff, DroppedPasswordHashIsNotSent)
{
	const std::unique_ptr<CECPacket> diff =
		MakePrefsDiffPacket(MakePrefs(100, true, true, true), MakePrefs(100, true, true, false));
	ASSERT_EQUALS(0u, diff->GetTagCount());
}

TEST(ECPrefsDiff, AmuleApiAdminPasswordHasNoOffState)
{
	CECPacket current = MakePrefs(100, true, true, true);
	CECTag *remote = current.GetTagByName(EC_TAG_PREFS_REMOTECTRL);
	CECEmptyTag rebuilt(EC_TAG_PREFS_REMOTECTRL);
	for (const CECTag &tag : *remote) {
		if (tag.GetTagName() != EC_TAG_AMULEAPI_PASSWD) {
			rebuilt.AddTag(CECTag(tag));
		}
	}
	CECPacket withoutAdmin(EC_OP_SET_PREFERENCES, EC_DETAIL_UPDATE);
	for (const CECTag &category : current) {
		if (category.GetTagName() == EC_TAG_PREFS_REMOTECTRL) {
			withoutAdmin.AddTag(CECTag(rebuilt));
		} else if (category.GetTagName() != EC_TAG_DETAIL_LEVEL) {
			withoutAdmin.AddTag(CECTag(category));
		}
	}
	ASSERT_EQUALS(0u, MakePrefsDiffPacket(current, withoutAdmin)->GetTagCount());
}
