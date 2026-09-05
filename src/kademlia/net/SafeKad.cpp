//
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

#include "SafeKad.h"

namespace Kademlia
{

CSafeKad safeKad;

CSafeKad::CSafeKad()
: m_lastCleanup(0)
{
}

void CSafeKad::Clear()
{
	m_trackedNodes.Clear();
	m_problematicNodes.Clear();
	m_bannedAddresses.Clear();
	m_lastCleanup = 0;
}

bool CSafeKad::TrackNode(uint32_t ip, uint16_t port, const CUInt128 &id, bool idVerified, time_t now)
{
	if (IsBanned(ip, now)) {
		return false;
	}

	const SKadNodeAddress address(ip, port);
	CKadAgedMap<SKadNodeAddress, sTracked>::iterator it = m_trackedNodes.Find(address);
	if (it == m_trackedNodes.End()) {
		Cleanup(now); // make room for a new node to track
		if (m_trackedNodes.Size() >= MAX_TRACKED_NODES) {
			// Still full of entries too recent to drop: evict the
			// least recently referenced one anyway, because refusing
			// to track is worse than forgetting the oldest node --
			// an untracked node gets no identity checks at all.
			m_trackedNodes.Erase(m_trackedNodes.OldestKey());
		}
		sTracked tracked;
		tracked.m_lastID = id;
		tracked.m_lastIDChange = now;
		tracked.m_lastReferenced = now;
		tracked.m_idVerified = idVerified;
		m_trackedNodes.Set(address, tracked);
		return true;
	}

	sTracked tracked = it->second;
	bool accepted = true;
	if (id != tracked.m_lastID) {
		// A verified identity is not replaced by an unverified claim:
		// otherwise anyone able to spoof a source address could rewrite
		// our view of a node that has actually proved who it is.
		if (tracked.m_idVerified && !idVerified) {
			accepted = false;
		} else if (now - tracked.m_lastIDChange < MIN_ID_CHANGE_INTERVAL) {
			// Rotating identity faster than once an hour. Mark the
			// address problematic, and escalate to a ban if it was
			// problematic already -- one rejected change is a
			// plausible accident, two inside 300 s is not.
			accepted = false;
			Escalate(ip, port, now);
		} else {
			tracked.m_lastID = id;
			tracked.m_lastIDChange = now;
		}
	}

	tracked.m_lastReferenced = now;
	// Verification is sticky: a node that once proved its identity is not
	// downgraded by a later unverified packet.
	if (!tracked.m_idVerified) {
		tracked.m_idVerified = idVerified;
	}
	// The ban above may have dropped this entry, so only write it back if
	// the address is still trackable.
	if (!IsBanned(ip, now)) {
		m_trackedNodes.Set(address, tracked);
	} else {
		m_trackedNodes.Erase(address);
	}
	return accepted;
}

void CSafeKad::Escalate(uint32_t ip, uint16_t port, time_t now)
{
	if (IsProblematic(ip, port, now)) {
		BanAddress(ip, now);
	} else {
		TrackProblematicNode(ip, port, now);
	}
}

void CSafeKad::TrackProblematicNode(uint32_t ip, uint16_t port, time_t now)
{
	if (IsBanned(ip, now)) {
		return; // already covered by the stronger measure
	}

	const SKadNodeAddress address(ip, port);
	CKadAgedMap<SKadNodeAddress, sProblematic>::iterator it = m_problematicNodes.Find(address);
	sProblematic problematic;
	if (it == m_problematicNodes.End()) {
		Cleanup(now);
		if (m_problematicNodes.Size() >= MAX_PROBLEMATIC_NODES) {
			m_problematicNodes.Erase(m_problematicNodes.OldestKey());
		}
		problematic.m_failed = now;
	} else {
		problematic = it->second;
	}
	problematic.m_lastReferenced = now;
	m_problematicNodes.Set(address, problematic);
}

void CSafeKad::BanAddress(uint32_t ip, time_t now)
{
	Cleanup(now);

	CKadAgedMap<uint32_t, sBanned>::iterator it = m_bannedAddresses.Find(ip);
	sBanned banned;
	if (it == m_bannedAddresses.End() && m_bannedAddresses.Size() >= MAX_BANNED_ADDRESSES) {
		// A thousand simultaneously banned addresses means something much
		// larger is going on than one bad node; drop the oldest ban
		// rather than growing without bound.
		m_bannedAddresses.Erase(m_bannedAddresses.OldestKey());
	}
	banned.m_banned = now;
	banned.m_lastReferenced = now;
	m_bannedAddresses.Set(ip, banned);

	// A banned address is not worth tracking an identity for, and its
	// problematic entries have been superseded by the stronger measure.
	// The ban covers the address, so every port on it goes.
	DropAllPortsOf(ip);
}

void CSafeKad::DropAllPortsOf(uint32_t ip)
{
	// Collect first, erase after: erasing invalidates the iterator, and the
	// aged map has to see each removal individually to keep its age index in
	// step.
	std::vector<uint16_t> ports;
	for (CKadAgedMap<SKadNodeAddress, sTracked>::iterator it =
			m_trackedNodes.LowerBound(SKadNodeAddress(ip, 0));
		it != m_trackedNodes.End() && it->first.m_ip == ip;
		++it) {
		ports.push_back(it->first.m_port);
	}
	for (const uint16_t port : ports) {
		m_trackedNodes.Erase(SKadNodeAddress(ip, port));
	}

	ports.clear();
	for (CKadAgedMap<SKadNodeAddress, sProblematic>::iterator it =
			m_problematicNodes.LowerBound(SKadNodeAddress(ip, 0));
		it != m_problematicNodes.End() && it->first.m_ip == ip;
		++it) {
		ports.push_back(it->first.m_port);
	}
	for (const uint16_t port : ports) {
		m_problematicNodes.Erase(SKadNodeAddress(ip, port));
	}
}

bool CSafeKad::IsBanned(uint32_t ip, time_t now)
{
	CKadAgedMap<uint32_t, sBanned>::iterator it = m_bannedAddresses.Find(ip);
	if (it == m_bannedAddresses.End()) {
		return false;
	}
	if (now - it->second.m_banned > MAX_BAN_TIME) {
		// The ban has lapsed; from here the address is judged on its
		// current behaviour alone.
		m_bannedAddresses.Erase(ip);
		return false;
	}
	sBanned banned = it->second;
	banned.m_lastReferenced = now;
	m_bannedAddresses.Set(ip, banned);
	return true;
}

bool CSafeKad::IsProblematic(uint32_t ip, uint16_t port, time_t now)
{
	const SKadNodeAddress address(ip, port);
	CKadAgedMap<SKadNodeAddress, sProblematic>::iterator it = m_problematicNodes.Find(address);
	if (it != m_problematicNodes.End()) {
		if (now - it->second.m_failed > MAX_PROBLEMATIC_TIME) {
			m_problematicNodes.Erase(address);
		} else {
			sProblematic problematic = it->second;
			problematic.m_lastReferenced = now;
			m_problematicNodes.Set(address, problematic);
			return true;
		}
	}
	// A banned address is problematic by construction.
	return IsBanned(ip, now);
}

bool CSafeKad::HasOtherTrackedPort(uint32_t ip, uint16_t port)
{
	CKadAgedMap<SKadNodeAddress, sTracked>::iterator it =
		m_trackedNodes.LowerBound(SKadNodeAddress(ip, 0));
	for (; it != m_trackedNodes.End() && it->first.m_ip == ip; ++it) {
		if (it->first.m_port != port) {
			return true;
		}
	}
	return false;
}

bool CSafeKad::IsBadNode(uint32_t ip,
	uint16_t port,
	const CUInt128 &id,
	uint8_t kadVersion,
	bool idVerified,
	bool onlyOneNodePerIP,
	time_t now)
{
	if (now - m_lastCleanup > CLEANUP_INTERVAL) {
		Cleanup(now);
	}

	if (IsBanned(ip, now)) {
		m_trackedNodes.Erase(SKadNodeAddress(ip, port));
		return true;
	}

	const SKadNodeAddress address(ip, port);
	CKadAgedMap<SKadNodeAddress, sTracked>::iterator it = m_trackedNodes.Find(address);
	if (it != m_trackedNodes.End()) {
		if (it->second.m_lastID == id) {
			// Same identity as before: just refresh the reference.
			sTracked tracked = it->second;
			tracked.m_lastReferenced = now;
			if (!tracked.m_idVerified) {
				tracked.m_idVerified = idVerified;
			}
			m_trackedNodes.Set(address, tracked);
			return false;
		}
		// A different identity. A node below 0x08 could not prove which
		// port it listens on, so an unverified identity change from one
		// is refused outright rather than rate-limited.
		if ((it->second.m_idVerified || kadVersion < MIN_PORT_VERIFIABLE_VERSION) && !idVerified) {
			// Refusing alone costs the sender nothing, so a rotation
			// inside the one-hour interval still climbs the ladder:
			// otherwise a sybil that always arrives unverified against
			// a verified entry could retry the same rotation forever
			// for free, which is exactly what the ladder exists to
			// stop. Past the interval the change is not rotation, only
			// unverifiable, and a legacy client that reinstalled once
			// is not worth a four-hour ban. Escalate() may ban and so
			// invalidate `it`; nothing reads it after this.
			if (now - it->second.m_lastIDChange < MIN_ID_CHANGE_INTERVAL) {
				Escalate(ip, port, now);
			}
			return true;
		}
		// TrackNode applies the one-hour interval and the escalation.
		return !TrackNode(ip, port, id, idVerified, now) || IsBanned(ip, now);
	}

	if (onlyOneNodePerIP && HasOtherTrackedPort(ip, port)) {
		// A second Kad port on one address is either a NAT hiding several
		// clients or one client pretending to be several. Both are bad
		// for the routing table, and the honest case still has its first
		// port tracked and usable.
		return true;
	}
	TrackNode(ip, port, id, idVerified, now);
	return false;
}

void CSafeKad::Cleanup(time_t now)
{
	m_lastCleanup = now;

	// Each table is walked oldest-first and stops at the first entry inside
	// its horizon, so a clean table costs one comparison.
	while (!m_trackedNodes.Empty() && now - m_trackedNodes.OldestReference() > NODE_MAX_REFERENCE_AGE) {
		m_trackedNodes.Erase(m_trackedNodes.OldestKey());
	}
	while (!m_bannedAddresses.Empty() &&
		now - m_bannedAddresses.OldestReference() > BAN_MAX_REFERENCE_AGE) {
		m_bannedAddresses.Erase(m_bannedAddresses.OldestKey());
	}
	while (!m_problematicNodes.Empty() &&
		now - m_problematicNodes.OldestReference() > PROBLEMATIC_MAX_REFERENCE_AGE) {
		m_problematicNodes.Erase(m_problematicNodes.OldestKey());
	}
}

} // namespace Kademlia
