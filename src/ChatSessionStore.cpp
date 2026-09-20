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

#include "OtherFunctions.h" // GUI_ID: legacy EC projection only

#include <algorithm>
#include <ctime>

uint64 CChatSessionStore::Session::LegacyGuiId() const
{
	if (!address.IsIPv4()) {
		return 0;
	}
	const uint32 ip = address.ToIPv4NetworkOrderOrZero();
	return ip && port ? GUI_ID(ip, port) : 0;
}

CChatSessionStore::Session &CChatSessionStore::Touch(
	const CMD4Hash &peer, const wxString &name, const CNetworkAddress &address, uint16 port)
{
	const uint32 now = static_cast<uint32>(time(nullptr));

	for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
		if (it->peer != peer) {
			continue;
		}
		// Only overwrite the name when the caller actually has one. An outbound message
		// carries none, and a peer that has not sent its nick yet reports empty -- neither
		// should erase a name we already learnt from an earlier inbound message.
		if (!name.IsEmpty()) {
			it->name = name;
		}
		it->address = address;
		it->port = port;
		it->last_activity = now;
		// Move to front: activity order is what Sessions() returns and what
		// eviction walks backwards through.
		if (it != m_sessions.begin()) {
			m_sessions.splice(m_sessions.begin(), m_sessions, it);
		}
		return m_sessions.front();
	}

	Session s;
	s.peer = peer;
	s.name = name;
	s.address = address;
	s.port = port;
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
	m.timestamp = static_cast<uint32>(time(nullptr));
	m.text = text;
	s.messages.push_back(std::move(m));
	while (s.messages.size() > MAX_MESSAGES_PER_SESSION) {
		s.messages.pop_front();
	}
	return m_lastMsgId;
}

uint32 CChatSessionStore::AddIncoming(const CMD4Hash &peer,
	const wxString &name,
	const wxString &text,
	const CNetworkAddress &address,
	uint16 port)
{
	return peer.IsEmpty() ? 0 : Append(Touch(peer, name, address, port), DIR_IN, text);
}

uint32 CChatSessionStore::AddOutgoing(
	const CMD4Hash &peer, const wxString &text, const CNetworkAddress &address, uint16 port)
{
	return peer.IsEmpty() ? 0 : Append(Touch(peer, wxEmptyString, address, port), DIR_OUT, text);
}

bool CChatSessionStore::CloseSession(const CMD4Hash &peer)
{
	for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
		if (it->peer == peer) {
			m_sessions.erase(it);
			return true;
		}
	}
	return false;
}

const CChatSessionStore::Session *CChatSessionStore::Find(const CMD4Hash &peer) const
{
	for (const Session &s : m_sessions) {
		if (s.peer == peer) {
			return &s;
		}
	}
	return nullptr;
}

const CChatSessionStore::Session *CChatSessionStore::FindLegacy(uint64 gui_id) const
{
	if (!gui_id) {
		return nullptr;
	}
	const Session *found = nullptr;
	for (const Session &s : m_sessions) {
		if (s.LegacyGuiId() == gui_id) {
			if (found) {
				return nullptr; // Never choose an arbitrary peer sharing an endpoint.
			}
			found = &s;
		}
	}
	return found;
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
	// Least recently active first, i.e. from the back. Note this drops a conversation whole
	// rather than trimming it: an EC client tracking its projection sees it vanish from the session
	// list and closes its tab, the same rule a session closed by another client follows.
	while (m_sessions.size() > MAX_SESSIONS) {
		m_sessions.pop_back();
	}
}

// File_checked_for_headers
