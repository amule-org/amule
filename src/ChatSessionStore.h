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

#ifndef CHATSESSIONSTORE_H
#define CHATSESSIONSTORE_H

#include "Types.h" // uint64 / uint32 / uint8

#include <deque>
#include <list>
#include <vector>

#include <wx/string.h>

// The core's record of who we are chatting with and what was said.
//
// Chat used to exist only inside the monolithic GUI's notebook tabs, which
// left nothing for EC to serve and no way for two clients to agree on the
// transcript. This store moves the model into the core so the local GUI,
// amulegui and amuleapi are three views of one conversation rather than three
// private ones.
//
// Compiled into both `amule` and `amuled` -- no GUI dependency. Owned by
// CamuleApp, fed at the two existing choke points (CUpDownClient::
// ProcessChatMessage inbound, CClientList::SendChatMessage outbound).
//
// In-memory only: an amuled restart empties it, which is the behaviour the
// monolithic GUI already has (the transcript dies with the notebook page).
// Persisting it is a self-contained follow-up that needs no EC change.
//
// **Threading:** every mutator runs on the main thread, from the packet
// handlers and the EC request handlers, exactly like CClientList. No locking,
// deliberately -- adding a mutex here would imply off-thread callers that do
// not exist and would have to be audited into existence.
class CChatSessionStore
{
public:
	enum Direction : uint8
	{
		DIR_IN = 0,  //!< received from the peer
		DIR_OUT = 1, //!< sent by us, from any client
	};

	struct Message
	{
		uint32 id = 0; //!< monotonic across the whole store, never reused
		uint8 direction = DIR_IN;
		uint32 timestamp = 0; //!< unix seconds, stamped by the core
		wxString text;
	};

	struct Session
	{
		uint64 gui_id = 0; //!< GUI_ID(ip, port) -- the key
		wxString name;     //!< peer display name; may be empty
		uint32 ip = 0;
		uint16 port = 0;
		uint32 last_activity = 0; //!< unix seconds, drives session eviction
		std::deque<Message> messages;

		//! Highest message id in this session, 0 when it holds none.
		uint32 LastMsgId() const { return messages.empty() ? 0 : messages.back().id; }
	};

	// Bounded so a chatty peer cannot grow the daemon without limit. Plain
	// constants rather than preferences: nobody has asked to tune them, and a
	// pref would need EC prefs plumbing to be reachable from a remote client.
	static const size_t MAX_MESSAGES_PER_SESSION = 200;
	static const size_t MAX_SESSIONS = 50;

	// Record one message, creating the session when it is the first. `name`
	// updates the stored display name when non-empty, so a peer that only
	// reveals its nick later still ends up named. Returns the id assigned.
	uint32 AddIncoming(uint64 gui_id, const wxString &name, const wxString &text);
	uint32 AddOutgoing(uint64 gui_id, const wxString &text);

	// Drop one session. Returns false when there was none, so the EC handler
	// can answer 404-equivalent rather than silently succeeding.
	bool CloseSession(uint64 gui_id);

	const Session *Find(uint64 gui_id) const;

	// Sessions in most-recently-active-first order -- the order a client wants
	// to render, and the order eviction walks backwards through.
	std::vector<const Session *> Sessions() const;

	//! Store-wide highest id, so a client resumes with one cursor rather than one per session.
	uint32 LastMsgId() const { return m_lastMsgId; }

	size_t SessionCount() const { return m_sessions.size(); }

private:
	Session &Touch(uint64 gui_id, const wxString &name);
	uint32 Append(Session &s, uint8 direction, const wxString &text);
	void EvictSessionsIfNeeded();

	// A list, not a map: the working set is at most MAX_SESSIONS, and the
	// dominant operations are "walk in activity order" and "move to front",
	// both O(1) here and both awkward on a map keyed by GUI_ID. Front is the
	// most recently active session.
	std::list<Session> m_sessions;
	uint32 m_lastMsgId = 0;
};

#endif // CHATSESSIONSTORE_H

// File_checked_for_headers
