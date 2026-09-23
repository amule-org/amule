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
#include "MD4Hash.h"
#include "NetworkAddress.h"
#include "OtherFunctions.h" // Needed for CFormat, Uint32toStringIP
#include "PeerAddressing.h"

#include <memory>

#include <wx/string.h>

// Hashes identify peers; hashless targets temporarily share a normalized route.
// Promotion requires an explicit handshake, never an endpoint-only identity match.
class CChatPeer
{
public:
	CChatPeer() = default;
	CChatPeer(const CMD4Hash &hash)
	: CChatPeer(hash, CNetworkAddress(), 0)
	{
	}
	CChatPeer(const CMD4Hash &hash, const CNetworkAddress &address, uint16 port)
	: m_state(std::make_shared<State>(State{ hash, address, port }))
	{
	}

	const CMD4Hash &Hash() const
	{
		static const CMD4Hash empty;
		return m_state ? m_state->hash : empty;
	}
	CNetworkAddress Address() const { return m_state ? m_state->address : CNetworkAddress(); }
	uint16 Port() const { return m_state ? m_state->port : 0; }
	bool IsEmpty() const
	{
		return Hash().IsEmpty() &&
		       (Address().IsAbsent() || !Port() ||
			       (Address().IsIPv4() && !Address().ToIPv4NetworkOrderOrZero()));
	}
	wxString Encode() const { return Hash().Encode(); }
	friend bool operator==(const CChatPeer &a, const CChatPeer &b)
	{
		if (!a.Hash().IsEmpty() || !b.Hash().IsEmpty()) {
			return !a.Hash().IsEmpty() && a.Hash() == b.Hash();
		}
		return (a.IsEmpty() && b.IsEmpty()) ||
		       (!a.IsEmpty() && !b.IsEmpty() && a.Port() == b.Port() &&
			       PeerAddressing::IndexKey(a.Address()) ==
				       PeerAddressing::IndexKey(b.Address()));
	}
	friend bool operator!=(const CChatPeer &a, const CChatPeer &b) { return !(a == b); }

private:
	friend class CChatSessionStore;
	struct State
	{
		CMD4Hash hash;
		CNetworkAddress address;
		uint16 port;
	};
	std::shared_ptr<State> m_state;
};

// One target type for both builds. The remote GUI decodes it from EC_TAG_CHAT_PEER_HASH or
// the legacy GUI_ID; everything downstream (CChatSelector, CChatWnd) is shared code that only
// ever compares CChatTarget values, so unifying the type is what keeps that code build-agnostic.
using CChatTarget = CChatPeer;
inline bool ChatTargetValid(const CChatTarget &id)
{
	return !id.IsEmpty();
}

// Fallback label for a peer the core sent no nickname for. Deliberately NOT translated: the
// same label is rendered by the monolithic chat selector, amulegui and amuleapi's /chats, and
// the API contract fixes it as English. Inline: needed in both GUI_SOURCES (ChatSelector.cpp,
// amule-remote-gui.cpp) and CORE_SOURCES, and ChatSessionStore.cpp itself is core-only.
inline wxString ChatPeerFallbackName(const CChatPeer &peer)
{
	if (!peer.Hash().IsEmpty()) {
		return peer.Hash().Encode();
	}
	return CFormat(wxT("IP: %s Port: %u")) % Uint32toStringIP(peer.Address().ToIPv4NetworkOrderOrZero()) %
	       peer.Port();
}

#include <deque>
#include <list>
#include <vector>

// The core's record of who we are chatting with and what was said.
//
// Chat used to exist only inside the monolithic GUI's notebook tabs, which left nothing for EC to
// serve and no way for two clients to agree on the transcript. This store moves the model into the
// core so the local GUI, amulegui and amuleapi are three views of one conversation rather than
// three private ones.
//
// Compiled into both `amule` and `amuled` -- no GUI dependency. Owned by CamuleApp, fed at the two
// existing choke points (CUpDownClient::ProcessChatMessage inbound, CClientList::SendChatMessage
// outbound).
//
// In-memory only: an amuled restart empties it, which is what the monolithic GUI already does (the
// transcript dies with the notebook page). Persisting it is a self-contained follow-up that needs
// no EC change.
//
// **Threading:** every mutator runs on the main thread, from the packet handlers and the EC request
// handlers, exactly like CClientList. No locking, deliberately -- a mutex here would imply off-
// thread callers that do not exist.
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
		CChatPeer peer;          //!< shared provisional identity, then stable handshake hash
		wxString name;           //!< peer display name; may be empty
		CNetworkAddress address; //!< mutable route, may be absent
		uint16 port = 0;
		uint32 last_activity = 0; //!< unix seconds, drives session eviction
		std::deque<Message> messages;

		// Legacy EC only: zero means this route cannot be represented as IPv4.
		uint64 LegacyGuiId() const;

		//! Highest message id in this session, 0 when it holds none.
		uint32 LastMsgId() const { return messages.empty() ? 0 : messages.back().id; }
	};

	// Bounded so a chatty peer cannot grow the daemon without limit. Plain constants rather
	// than preferences: nobody has asked to tune them, and a pref would need EC prefs plumbing
	// to be reachable from a remote client.
	static const size_t MAX_MESSAGES_PER_SESSION = 200;
	static const size_t MAX_SESSIONS = 50;

	// Record one message, creating the session when it is the first. `name` updates the stored
	// display name when non-empty, so a peer that only reveals its nick later still ends up
	// named. Returns the id assigned.
	// Bare empty hashes are unavailable. Route-bound targets must be opened explicitly.
	// A hash is a protocol identity, not proof of authentication.
	uint32 AddIncoming(const CChatPeer &peer,
		const wxString &name,
		const wxString &text,
		const CNetworkAddress &address = CNetworkAddress(),
		uint16 port = 0);
	uint32 AddOutgoing(const CChatPeer &peer,
		const wxString &text,
		const CNetworkAddress &address = CNetworkAddress(),
		uint16 port = 0);

	// Drop one session. Returns false when there was none, so the EC handler
	// can answer 404-equivalent rather than silently succeeding.
	bool CloseSession(const CChatPeer &peer);

	const Session *Find(const CChatPeer &peer) const;

	// Reuse a known hash or a normalized provisional route. A route-only transcript
	// is temporary: the first successful handshake claims it. Other identified
	// occupants remain separate even when they share that route.
	CChatPeer Open(
		const CMD4Hash &hash, const CNetworkAddress &address, uint16 port, const wxString &name);
	bool Promote(const CChatPeer &peer, const CMD4Hash &hash);
	// Ambiguous endpoints are not projected: EC cannot distinguish their peers.
	const Session *FindLegacy(uint64 gui_id) const;

	// Sessions in most-recently-active-first order -- the order a client wants
	// to render, and the order eviction walks backwards through.
	std::vector<const Session *> Sessions() const;

	//! Store-wide highest id, so a client resumes with one cursor rather than one per session.
	uint32 LastMsgId() const { return m_lastMsgId; }

	size_t SessionCount() const { return m_sessions.size(); }

private:
	Session &Touch(
		const CChatPeer &peer, const wxString &name, const CNetworkAddress &address, uint16 port);
	uint32 Append(Session &s, uint8 direction, const wxString &text);
	void EvictSessionsIfNeeded();

	// A list, not a map: the working set is at most MAX_SESSIONS, and the dominant operations
	// are "walk in activity order" and "move to front", both O(1) here and both awkward on a
	// map keyed by peer hash. Front is the most recently active session.
	std::list<Session> m_sessions;
	uint32 m_lastMsgId = 0;
};

#endif // CHATSESSIONSTORE_H

// File_checked_for_headers
