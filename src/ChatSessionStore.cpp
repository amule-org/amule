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

#include "ChatSessionStore.h"

#include "OtherFunctions.h" // IP_FROM_GUI_ID / PORT_FROM_GUI_ID

#include <algorithm>
#include <ctime>

CChatSessionStore::Session &CChatSessionStore::Touch(uint64 gui_id, const wxString &name)
{
	const uint32 now = static_cast<uint32>(time(NULL));

	for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
		if (it->gui_id != gui_id) {
			continue;
		}
		// Only overwrite the name when the caller actually has one. An
		// outbound message carries none, and a peer that has not sent its
		// nick yet reports empty -- neither should erase a name we already
		// learnt from an earlier inbound message.
		if (!name.IsEmpty()) {
			it->name = name;
		}
		it->last_activity = now;
		// Move to front: activity order is what Sessions() returns and what
		// eviction walks backwards through.
		if (it != m_sessions.begin()) {
			m_sessions.splice(m_sessions.begin(), m_sessions, it);
		}
		return m_sessions.front();
	}

	Session s;
	s.gui_id = gui_id;
	s.name = name;
	s.ip = static_cast<uint32>(IP_FROM_GUI_ID(gui_id));
	s.port = static_cast<uint16>(PORT_FROM_GUI_ID(gui_id));
	s.last_activity = now;
	m_sessions.push_front(std::move(s));
	EvictSessionsIfNeeded();
	return m_sessions.front();
}

uint32 CChatSessionStore::Append(Session &s, uint8 direction, const wxString &text)
{
	Message m;
	// Pre-increment: id 0 is reserved as the "no cursor / nothing yet"
	// sentinel every reader uses, so the first real message is 1.
	m.id = ++m_lastMsgId;
	m.direction = direction;
	m.timestamp = static_cast<uint32>(time(NULL));
	m.text = text;
	s.messages.push_back(std::move(m));
	while (s.messages.size() > MAX_MESSAGES_PER_SESSION) {
		s.messages.pop_front();
	}
	return m_lastMsgId;
}

uint32 CChatSessionStore::AddIncoming(uint64 gui_id, const wxString &name, const wxString &text)
{
	return Append(Touch(gui_id, name), DIR_IN, text);
}

uint32 CChatSessionStore::AddOutgoing(uint64 gui_id, const wxString &text)
{
	return Append(Touch(gui_id, wxEmptyString), DIR_OUT, text);
}

bool CChatSessionStore::CloseSession(uint64 gui_id)
{
	for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
		if (it->gui_id == gui_id) {
			m_sessions.erase(it);
			return true;
		}
	}
	return false;
}

const CChatSessionStore::Session *CChatSessionStore::Find(uint64 gui_id) const
{
	for (const Session &s : m_sessions) {
		if (s.gui_id == gui_id) {
			return &s;
		}
	}
	return NULL;
}

std::vector<const CChatSessionStore::Session *> CChatSessionStore::Sessions() const
{
	std::vector<const Session *> out;
	out.reserve(m_sessions.size());
	for (const Session &s : m_sessions) {
		out.push_back(&s);
	}
	return out;
}

void CChatSessionStore::EvictSessionsIfNeeded()
{
	// Least recently active first, i.e. from the back. Note this drops a
	// conversation whole rather than trimming it: a client tracking that
	// GUI_ID sees it vanish from the session list and closes its tab, which
	// is the same rule a session closed by another client follows.
	while (m_sessions.size() > MAX_SESSIONS) {
		m_sessions.pop_back();
	}
}

// File_checked_for_headers
