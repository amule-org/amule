//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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

#include "FriendList.h" // Interface declarations.

#include "BrowseManager.h"

#include <common/DataFileVersion.h>

#include "amule.h"        // Needed for theApp: let it first or fail under win32
#include "ClientList.h"   // Needed for CClientList
#include "updownclient.h" // Needed for CUpDownClient
#include "Friend.h"       // Needed for CFriend
#include "CFile.h"
#include "Logger.h"
#include "GuiEvents.h"
#include "Preferences.h" // Needed for thePrefs

CFriendList::CFriendList() {}

CFriendList::~CFriendList()
{
	SaveList();

	DeleteContents(m_FriendList);
}

void CFriendList::AddFriend(CFriend *toadd, bool notify)
{
	m_FriendList.push_back(toadd);
	SaveList();
	if (notify) {
		Notify_ChatUpdateFriend(toadd);
	}
}

void CFriendList::AddFriend(const CMD4Hash &userhash,
	uint32 lastUsedIP,
	uint32 lastUsedPort,
	const wxString &name,
	uint32 lastSeen,
	uint32 lastChatted)
{
	CFriend *NewFriend = new CFriend(userhash, lastSeen, lastUsedIP, lastUsedPort, lastChatted, name);

	AddFriend(NewFriend);
}

void CFriendList::AddFriend(const CClientRef &toadd)
{
	if (toadd.IsFriend()) {
		return;
	}

	CFriend *NewFriend = new CFriend(toadd);
	toadd.SetFriend(NewFriend);

	AddFriend(NewFriend, false); // has already notified
}

void CFriendList::RemoveFriend(CFriend *toremove)
{
	if (toremove) {
		CClientRef client = toremove->GetLinkedClient();
		if (client.IsLinked()) {
			client.SetFriendSlot(false);
			client.SetFriend(NULL);
			toremove->UnLinkClient();
		}

		m_FriendList.remove(toremove);

		SaveList();

		Notify_ChatRemoveFriend(toremove); // this deletes the friend
	}
}

void CFriendList::LoadList()
{
	CPath metfile = CPath(thePrefs::GetConfigDir() + "emfriends.met");

	if (!metfile.FileExists()) {
		return;
	}

	CFile file;
	try {
		if (file.Open(metfile)) {
			if (file.ReadUInt8() /*header*/ == MET_HEADER) {
				uint32 nRecordsNumber = file.ReadUInt32();
				for (uint32 i = 0; i < nRecordsNumber; i++) {
					CFriend *Record = new CFriend();
					Record->LoadFromFile(&file);
					m_FriendList.push_back(Record);
					Notify_ChatUpdateFriend(Record);
				}
			}
		} else {
			AddLogLineN(_("Failed to open friend list file 'emfriends.met' for reading!"));
		}
	} catch (const CInvalidPacket &e) {
		AddDebugLogLineC(
			logGeneral, "Invalid entry in friend list, file may be corrupt: " + e.what());
	} catch (const CSafeIOException &e) {
		AddDebugLogLineC(logGeneral, "IO error while reading 'emfriends.met': " + e.what());
	}
}

void CFriendList::SaveList()
{
	CFile file;
	if (file.Create(thePrefs::GetConfigDir() + "emfriends.met", true)) {
		try {
			file.WriteUInt8(MET_HEADER);
			file.WriteUInt32(m_FriendList.size());

			for (FriendList::iterator it = m_FriendList.begin(); it != m_FriendList.end(); ++it) {
				(*it)->WriteToFile(&file);
			}
		} catch (const CIOFailureException &e) {
			AddDebugLogLineC(logGeneral, "IO failure while saving 'emfriends.met': " + e.what());
		}
	} else {
		AddLogLineN(_("Failed to open friend list file 'emfriends.met' for writing!"));
	}
}

CFriend *CFriendList::LookupFriend(const CMD4Hash &userhash, uint32 dwIP, uint16 nPort) const
{
	for (CFriend *cur_friend : m_FriendList) {
		// to avoid that unwanted clients become a friend, we have to distinguish between friends with
		// a userhash and of friends which are identified by IP+port only.
		if (!userhash.IsEmpty() && cur_friend->HasHash()) {
			// check for a friend which has the same userhash as the specified one
			if (cur_friend->GetUserHash() == userhash) {
				return cur_friend;
			}
		} else if (dwIP != 0 && cur_friend->GetIP() == dwIP && cur_friend->GetPort() == nPort) {
			// A zero address is the absence of one, not a value to match on.
			// Friends can be added from a record that never carried an IP, and
			// without this every such record answers to the same query: a
			// client would link to whichever was stored first and inherit its
			// persistent friend slot.
			return cur_friend;
		}
	}

	return nullptr;
}

CFriend *CFriendList::FindFriend(const CMD4Hash &userhash, uint32 dwIP, uint16 nPort)
{
	CFriend *found = LookupFriend(userhash, dwIP, nPort);
	// A match can only lack a hash when it was matched on address, so this
	// is the "friend entered as IP:port, now seen connecting" case.
	if (found != nullptr && !userhash.IsEmpty() && !found->HasHash()) {
		found->SetUserHash(userhash);
		SaveList();
	}
	return found;
}

CFriend *CFriendList::FindFriend(uint32 ecid)
{
	for (FriendList::iterator it = m_FriendList.begin(); it != m_FriendList.end(); ++it) {
		CFriend *cur_friend = *it;
		if (cur_friend->ECID() == ecid) {
			return cur_friend;
		}
	}

	return NULL;
}

bool CFriendList::IsAlreadyFriend(uint32 dwLastUsedIP, uint32 nLastUsedPort)
{
	return (FindFriend(CMD4Hash(), dwLastUsedIP, nLastUsedPort) != NULL);
}

void CFriendList::RemoveAllFriendSlots()
{
	for (FriendList::iterator it = m_FriendList.begin(); it != m_FriendList.end(); ++it) {
		CFriend *cur_friend = *it;
		if (cur_friend->GetLinkedClient().IsLinked()) {
			cur_friend->GetLinkedClient().SetFriendSlot(false);
		}
	}
}

void CFriendList::RequestSharedFileList(CFriend *cur_friend, uint32 browseSearchId)
{
	if (cur_friend) {
		CUpDownClient *client = cur_friend->GetLinkedClient().GetClient();
		if (!client) {
			CClientRef ref = theApp->clientlist->CreateForAddress(cur_friend->GetUserHash(),
				cur_friend->GetIP(),
				cur_friend->GetPort(),
				cur_friend->GetName());
			cur_friend->LinkClient(ref);
			client = ref.GetClient();
		}
		// Pin the EC-allocated browse ID before firing the request, so
		// ProcessSharedFileList files the listing under it -- but only when
		// this peer has no browse running. RequestSharedFileList returns
		// early for one that has, and pinning first would have left the live
		// browse's ID overwritten: with 0 for a local "View Files", which is
		// what the rest of its listing would then be filed under.
		if (theApp->browsemanager->SearchIdFor(client) == 0) {
			client->PinBrowseSearchId(browseSearchId);
		}
		client->RequestSharedFileList();
	}
}

void CFriendList::SetFriendSlot(CFriend *Friend, bool new_state)
{
	if (!Friend) {
		return;
	}
	// Persist the flag on the friend record so it survives the friend
	// going offline (the live CUpDownClient::m_bFriendSlot is per-session
	// state that dies with the client object on disconnect).
	Friend->SetPersistentFriendSlot(new_state);
	if (new_state) {
		// Only one friend can hold the slot at a time. Clear any other
		// friend's persistent flag too so the on-disk state matches.
		for (FriendList::iterator it = m_FriendList.begin(); it != m_FriendList.end(); ++it) {
			if (*it != Friend) {
				(*it)->SetPersistentFriendSlot(false);
			}
		}
		RemoveAllFriendSlots();
	}
	if (Friend->GetLinkedClient().IsLinked()) {
		Friend->GetLinkedClient().SetFriendSlot(new_state);
		CoreNotify_Upload_Resort_Queue();
	}
	SaveList();
}

void CFriendList::StartChatSession(CFriend *Friend)
{
	if (Friend) {
		if (!Friend->GetLinkedClient().GetClient()) {
			Friend->LinkClient(theApp->clientlist->CreateForAddress(Friend->GetUserHash(),
				Friend->GetIP(),
				Friend->GetPort(),
				Friend->GetName()));
		}
	} else {
		AddLogLineC(_("CRITICAL - no client on StartChatSession"));
	}
}

// File_checked_for_headers
