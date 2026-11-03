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
#include "NetworkPreferences.h"

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

// A by-value flag, such as accelerated publication, keeps its value in the diff.
TEST(ECPrefsDiff, ValueFlagSwitchedOffCarriesFalse)
{
	auto prefs = [](bool accelerated) {
		CECPacket packet(EC_OP_SET_PREFERENCES, EC_DETAIL_UPDATE);
		CECEmptyTag servers(EC_TAG_PREFS_SERVERS);
		servers.AddTag(CECTag(EC_TAG_SERVERS_ED2K_ACCELERATED_PUBLICATION, accelerated));
		packet.AddTag(servers);
		return packet;
	};
	const std::unique_ptr<CECPacket> diff = MakePrefsDiffPacket(prefs(true), prefs(false));
	const CECTag *flag = Find(*diff, EC_TAG_PREFS_SERVERS, EC_TAG_SERVERS_ED2K_ACCELERATED_PUBLICATION);
	ASSERT_TRUE(flag != nullptr);
	ASSERT_EQUALS(0u, flag->GetInt());
	ASSERT_EQUALS(0u, MakePrefsDiffPacket(prefs(true), prefs(true))->GetTagCount());
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

TEST(ECPrefsDiff, NetworkPollKeepsReverseToggleWritable)
{
	CECPacket base = MakePrefs(100, true, true, true);
	UpdateNetworkPrefsSnapshot(base, true, false);
	CECPacket current(base);
	UpdateNetworkPrefsSnapshot(current, false, true);
	const auto diff = MakePrefsDiffPacket(base, current);
	ASSERT_EQUALS(0u, Find(*diff, EC_TAG_PREFS_CONNECTIONS, EC_TAG_NETWORK_ED2K)->GetInt());
	ASSERT_EQUALS(1u, Find(*diff, EC_TAG_PREFS_CONNECTIONS, EC_TAG_NETWORK_KADEMLIA)->GetInt());
	ASSERT_TRUE(Find(*diff, EC_TAG_PREFS_CONNECTIONS, EC_TAG_CONN_MAX_UL) == nullptr);
	ASSERT_TRUE(Find(*diff, EC_TAG_PREFS_MESSAGEFILTER, EC_TAG_MSGFILTER_COMMENT_KEYWORDS) == nullptr);
}

TEST(ECPrefsDiff, NetworkPollPreservesOtherPendingEdits)
{
	CECPacket base = MakePrefs(100, true, true, true);
	CECPacket current = MakePrefs(200, true, true, true);
	for (bool ed2k : { false, true }) {
		for (bool kad : { false, true }) {
			UpdateNetworkPrefsSnapshot(base, ed2k, kad);
			UpdateNetworkPrefsSnapshot(current, ed2k, kad);
			const auto diff = MakePrefsDiffPacket(base, current);
			ASSERT_EQUALS(
				200u, Find(*diff, EC_TAG_PREFS_CONNECTIONS, EC_TAG_CONN_MAX_UL)->GetInt());
			ASSERT_TRUE(Find(*diff, EC_TAG_PREFS_CONNECTIONS, EC_TAG_NETWORK_ED2K) == nullptr);
			ASSERT_TRUE(
				Find(*diff, EC_TAG_PREFS_CONNECTIONS, EC_TAG_NETWORK_KADEMLIA) == nullptr);
		}
	}
}

TEST(ECPrefsDiff, DebugCategoryChangeSendsTheWholeSet)
{
	const auto prefs = [](std::initializer_list<const char *> enabled) {
		CECPacket packet(EC_OP_SET_PREFERENCES, EC_DETAIL_UPDATE);
		CECEmptyTag tweaks(EC_TAG_PREFS_CORETWEAKS);
		CECTag all(EC_TAG_CORETW_DEBUG_CATEGORIES, static_cast<uint32>(2));
		all.AddTag(CECTag(EC_TAG_STRING, wxString("General")));
		all.AddTag(CECTag(EC_TAG_STRING, wxString("Hasher")));
		tweaks.AddTag(all);
		CECTag list(EC_TAG_CORETW_DEBUG_CATEGORIES_ENABLED, static_cast<uint32>(enabled.size()));
		for (const char *name : enabled) {
			list.AddTag(CECTag(EC_TAG_STRING, wxString(name)));
		}
		tweaks.AddTag(list);
		packet.AddTag(tweaks);
		return packet;
	};

	ASSERT_EQUALS(0u, MakePrefsDiffPacket(prefs({ "General" }), prefs({ "General" }))->GetTagCount());

	const auto grown = MakePrefsDiffPacket(prefs({ "General" }), prefs({ "General", "Hasher" }));
	ASSERT_EQUALS(2u,
		Find(*grown, EC_TAG_PREFS_CORETWEAKS, EC_TAG_CORETW_DEBUG_CATEGORIES_ENABLED)->GetTagCount());
	ASSERT_TRUE(Find(*grown, EC_TAG_PREFS_CORETWEAKS, EC_TAG_CORETW_DEBUG_CATEGORIES) == nullptr);

	// Disabling every category must still reach the core.
	const auto emptied = MakePrefsDiffPacket(prefs({ "General" }), prefs({}));
	const CECTag *list = Find(*emptied, EC_TAG_PREFS_CORETWEAKS, EC_TAG_CORETW_DEBUG_CATEGORIES_ENABLED);
	ASSERT_TRUE(list != nullptr);
	ASSERT_EQUALS(0u, list->GetTagCount());
}

TEST(ECPrefsDiff, RepeatedNetworkPollIsIdempotent)
{
	CECPacket base = MakePrefs(100, true, true, true);
	UpdateNetworkPrefsSnapshot(base, true, true);
	CECPacket repeated(base);
	UpdateNetworkPrefsSnapshot(repeated, true, true);
	ASSERT_EQUALS(0u, MakePrefsDiffPacket(base, repeated)->GetTagCount());
	UpdateNetworkPrefsSnapshot(repeated, false, false);
	const auto diff = MakePrefsDiffPacket(base, repeated);
	ASSERT_EQUALS(0u, Find(*diff, EC_TAG_PREFS_CONNECTIONS, EC_TAG_NETWORK_ED2K)->GetInt());
	ASSERT_EQUALS(0u, Find(*diff, EC_TAG_PREFS_CONNECTIONS, EC_TAG_NETWORK_KADEMLIA)->GetInt());
}

TEST(ECPrefsDiff, OpenNetworkCheckboxFollowsRemoteChanges)
{
	CNetworkPreferenceEdit edit;
	edit.Reset(false);
	bool displayed = edit.Merge(false, true);
	ASSERT_TRUE(displayed);
	displayed = edit.Merge(displayed, false);
	ASSERT_FALSE(displayed);
}

TEST(ECPrefsDiff, OpenNetworkCheckboxKeepsStagedEdit)
{
	CNetworkPreferenceEdit edit;
	edit.Reset(false);
	// The user enabled the checkbox, but has not saved it yet.
	ASSERT_TRUE(edit.Merge(true, false));
	// An external client subsequently makes the same change.
	ASSERT_TRUE(edit.Merge(true, true));
	// The user can still change back, even as a fresh poll arrives.
	ASSERT_FALSE(edit.Merge(false, true));
	// Reopening the dialog discards the cancelled edit.
	edit.Reset(true);
	ASSERT_FALSE(edit.Merge(true, false));
}

TEST(ECPrefsDiff, OpenNetworkCheckboxKeepsEditAcrossRemoteRoundTrip)
{
	for (const bool initial : { false, true }) {
		CNetworkPreferenceEdit edit;
		edit.Reset(initial);
		bool displayed = edit.Merge(!initial, initial);
		ASSERT_EQUALS(!initial, displayed);
		displayed = edit.Merge(displayed, !initial);
		ASSERT_EQUALS(!initial, displayed);
		displayed = edit.Merge(displayed, initial);
		ASSERT_EQUALS(!initial, displayed);
	}
}

TEST(ECPrefsDiff, RevertingStagedNetworkEditResumesRemoteUpdates)
{
	CNetworkPreferenceEdit edit;
	edit.Reset(false);
	ASSERT_TRUE(edit.Merge(true, false));
	// The user returns to the core's current value before saving.
	ASSERT_FALSE(edit.Merge(false, false));
	ASSERT_TRUE(edit.Merge(false, true));
}

TEST(ECPrefsDiff, ServerSourcesRemainAListWhenEditedOrCleared)
{
	for (bool clear : { false, true }) {
		CECPacket base(EC_OP_SET_PREFERENCES, EC_DETAIL_UPDATE);
		CECEmptyTag before(EC_TAG_PREFS_SERVERS);
		CECEmptyTag oldSources(EC_TAG_SERVERS_URL_LIST);
		oldSources.AddTag(CECTag(EC_TAG_STRING, wxString("https://old.example/list")));
		before.AddTag(oldSources);
		base.AddTag(before);

		CECPacket current(EC_OP_SET_PREFERENCES, EC_DETAIL_UPDATE);
		CECEmptyTag after(EC_TAG_PREFS_SERVERS);
		CECEmptyTag sources(EC_TAG_SERVERS_URL_LIST);
		if (!clear) {
			sources.AddTag(CECTag(EC_TAG_STRING, wxString("https://new.example/list")));
		}
		after.AddTag(sources);
		current.AddTag(after);

		const auto diff = MakePrefsDiffPacket(base, current);
		const CECTag *list = Find(*diff, EC_TAG_PREFS_SERVERS, EC_TAG_SERVERS_URL_LIST);
		ASSERT_TRUE(list != nullptr);
		ASSERT_TRUE(list->IsCustom());
		ASSERT_EQUALS(0u, list->GetTagDataLen());
		ASSERT_EQUALS(clear ? 0u : 1u, list->GetTagCount());
	}
}
