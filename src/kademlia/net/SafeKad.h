//								-*- C++ -*-
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2026 eMule AI
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

#ifndef __KAD_SAFEKAD_H__
#define __KAD_SAFEKAD_H__

#include <cstddef>
#include <map>
#include <set>
#include <time.h>
#include <utility>
#include <vector>

#include "../utils/UInt128.h"
#include "../../Types.h"

////////////////////////////////////////
namespace Kademlia
{
////////////////////////////////////////

// One Kad node address. Kad keys on IPv4 throughout -- routing, the UDP key and
// the publish tracking all take a uint32 -- so this does too.
struct SKadNodeAddress
{
	SKadNodeAddress()
	: m_ip(0)
	, m_port(0)
	{
	}
	SKadNodeAddress(uint32_t ip, uint16_t port)
	: m_ip(ip)
	, m_port(port)
	{
	}
	bool operator<(const SKadNodeAddress &value) const
	{
		return (m_ip < value.m_ip) || (m_ip == value.m_ip && m_port < value.m_port);
	}
	bool operator==(const SKadNodeAddress &value) const
	{
		return m_ip == value.m_ip && m_port == value.m_port;
	}

	uint32_t m_ip;
	uint16_t m_port;
};

// A bounded map whose entries are evicted by last-reference age.
//
// The plain map alone would need an O(n) scan to find the least recently
// referenced entry, which is exactly the wrong complexity for a table that only
// evicts while under a flood. The parallel set, keyed on (last reference, key),
// makes both the oldest-first walk in Cleanup() and the make-room eviction
// O(log n).
//
// `Entry` must expose a `time_t m_lastReferenced`; every mutation goes through
// Set() so the two containers cannot drift apart.
template <class Key, class Entry> class CKadAgedMap
{
public:
	typedef std::map<Key, Entry> Map;
	typedef typename Map::iterator iterator;
	typedef typename Map::const_iterator const_iterator;

	iterator Find(const Key &key) { return m_map.find(key); }
	iterator LowerBound(const Key &key) { return m_map.lower_bound(key); }
	iterator Begin() { return m_map.begin(); }
	iterator End() { return m_map.end(); }
	size_t Size() const { return m_map.size(); }
	bool Empty() const { return m_map.empty(); }

	void Set(const Key &key, const Entry &entry)
	{
		iterator it = m_map.find(key);
		if (it != m_map.end()) {
			m_age.erase(AgeKey(it->second.m_lastReferenced, key));
			it->second = entry;
		} else {
			m_map.insert(std::make_pair(key, entry));
		}
		m_age.insert(AgeKey(entry.m_lastReferenced, key));
	}

	void Erase(const Key &key)
	{
		iterator it = m_map.find(key);
		if (it == m_map.end()) {
			return;
		}
		m_age.erase(AgeKey(it->second.m_lastReferenced, key));
		m_map.erase(it);
	}

	// Only valid while !Empty().
	const Key &OldestKey() const { return m_age.begin()->second; }
	time_t OldestReference() const { return m_age.begin()->first; }

	void Clear()
	{
		m_map.clear();
		m_age.clear();
	}

private:
	typedef std::pair<time_t, Key> AgeKey;

	Map m_map;
	std::set<AgeKey> m_age;
};

// Identity and address protections layered on top of the standard 0.49b Kad
// defences, which stay exactly as they are.
//
// What this adds over CPacketTracking and the UDP key challenge:
//
//  - CPacketTracking rate-limits *packets* per IP and opcode. It says nothing
//    about a node that behaves politely while presenting a new Kad ID every
//    few minutes, which is how a routing table gets flooded with sybils that
//    each look individually reasonable. That is what the tracked-node table
//    and the one-hour minimum identity-change interval cover.
//  - The UDP key challenge proves an address controls its own traffic. It does
//    not remember that the same address failed us five minutes ago, which is
//    what the problematic list is for.
//
// The escalation ladder mirrors CPacketTracking's own drop-then-ban shape:
// first identity rotation marks the address problematic (300 s), a rotation
// while already problematic bans it (4 h). The ban is Kad-routing-local and
// deliberately separate from CClientList's eD2k-wide ban, which has its own
// lifetime and its own triggers.
//
// Every table is bounded and evicts by last-reference age, so sustained inbound
// traffic costs a fixed amount of memory. All entry points take `now` so that
// the whole ladder is testable without waiting on a real clock.
class CSafeKad
{
public:
	static const size_t MAX_TRACKED_NODES = 10000;
	static const size_t MAX_PROBLEMATIC_NODES = 10000;
	static const size_t MAX_BANNED_ADDRESSES = 1000;

	// A node may change its Kad ID at most once an hour. Legitimate reasons
	// exist (a fresh install, a wiped preferencesKad.dat), and an hour is
	// far longer than any of them need.
	static const time_t MIN_ID_CHANGE_INTERVAL = 3600;
	// A ban lapses after four hours; past that the address is judged on its
	// current behaviour alone.
	static const time_t MAX_BAN_TIME = 4 * 3600;
	// A problematic address is ignored for 300 s.
	static const time_t MAX_PROBLEMATIC_TIME = 300;

	// Lowest advertised Kad version whose three-way handshake can prove
	// which UDP port a node listens on (eMule 0.49b). Below it an
	// unverified identity change cannot be told from a spoof, so it is
	// refused outright instead of merely rate-limited.
	//
	// Written out here rather than taken from the protocol version table:
	// this class adds nothing to the wire, so it has no business depending
	// on the header that defines what we advertise.
	static const uint8_t MIN_PORT_VERIFIABLE_VERSION = 0x08;

	// Eviction horizons for Cleanup(): an entry nothing has referenced for
	// this long carries no information worth its memory.
	static const time_t NODE_MAX_REFERENCE_AGE = 3600;
	static const time_t BAN_MAX_REFERENCE_AGE = 3600;
	static const time_t PROBLEMATIC_MAX_REFERENCE_AGE = 300;
	// Cleanup() also runs from IsBadNode() at most this often.
	static const time_t CLEANUP_INTERVAL = 600;

	CSafeKad();

	// Records the identity `id` at this address. A changed ID inside
	// MIN_ID_CHANGE_INTERVAL marks the address problematic (and bans it if
	// it was problematic already) and leaves the tracked ID untouched.
	// Returns false if the identity change was rejected.
	bool TrackNode(uint32_t ip, uint16_t port, const CUInt128 &id, bool idVerified, time_t now);

	// Marks an address as having misbehaved (a timeout, an inconsistent
	// answer, a rejected identity change).
	void TrackProblematicNode(uint32_t ip, uint16_t port, time_t now);

	// Bans an address for MAX_BAN_TIME. Use only where the address is
	// demonstrably at fault: a node can be *reported* bad by a third party,
	// and acting on that would make this a remote-controlled blocklist.
	void BanAddress(uint32_t ip, time_t now);

	// The one call the Kad packet paths need: true when this contact must
	// not be used or inserted into the routing table.
	//
	// `onlyOneNodePerIP` rejects a second port on an address that already
	// has a tracked node -- a real client uses one Kad port. It is gated on
	// `kadVersion` by the caller's choice because the pre-0x08 handshake
	// could not prove which port a node really listens on.
	bool IsBadNode(uint32_t ip,
		uint16_t port,
		const CUInt128 &id,
		uint8_t kadVersion,
		bool idVerified,
		bool onlyOneNodePerIP,
		time_t now);

	bool IsBanned(uint32_t ip, time_t now);
	bool IsProblematic(uint32_t ip, uint16_t port, time_t now);

	// Drops entries nothing has referenced within their horizon.
	void Cleanup(time_t now);
	void Clear();

	size_t GetTrackedNodeCount() const { return m_trackedNodes.Size(); }
	size_t GetProblematicNodeCount() const { return m_problematicNodes.Size(); }
	size_t GetBannedAddressCount() const { return m_bannedAddresses.Size(); }

private:
	struct sTracked
	{
		CUInt128 m_lastID;
		time_t m_lastIDChange;
		time_t m_lastReferenced;
		bool m_idVerified;
	};
	struct sProblematic
	{
		time_t m_failed;
		time_t m_lastReferenced;
	};
	struct sBanned
	{
		time_t m_banned;
		time_t m_lastReferenced;
	};

	// One step up the escalation ladder for a rejected identity change:
	// problematic the first time, banned if the address was problematic
	// already. Both refusal paths share this so that one rejection is
	// always exactly one step.
	void Escalate(uint32_t ip, uint16_t port, time_t now);
	// True when this address already has a tracked node on another port.
	bool HasOtherTrackedPort(uint32_t ip, uint16_t port);
	// Forgets every tracked and problematic entry for `ip`, on every port.
	void DropAllPortsOf(uint32_t ip);

	CKadAgedMap<SKadNodeAddress, sTracked> m_trackedNodes;
	CKadAgedMap<SKadNodeAddress, sProblematic> m_problematicNodes;
	CKadAgedMap<uint32_t, sBanned> m_bannedAddresses;
	time_t m_lastCleanup;
};

// The single instance shared by the Kad packet, search and routing paths, like
// the other Kad singletons reached through CKademlia.
extern CSafeKad safeKad;

} // namespace Kademlia

#endif // __KAD_SAFEKAD_H__
