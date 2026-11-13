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

#ifndef KAD_BUDDY_MERGE_POLICY_H
#define KAD_BUDDY_MERGE_POLICY_H

struct SKadBuddyMergeFacts
{
	bool incomingHasID = false;
	bool existingHasID = false;
	// The ID came from TAG_BUDDYHASH rather than from source exchange.
	bool incomingIDFromKad = false;
	bool existingIDFromKad = false;
	// Both the buddy IP and port are set.
	bool incomingHasAddress = false;
	bool existingHasAddress = false;
	bool sameAddress = false;
	bool incomingFromKad = false;
};

struct SKadBuddyMerge
{
	bool takeID = false;
	bool takeAddress = false;
	// Kept as the fallback route, used only once the stored one stops answering.
	bool storeCandidate = false;
};

/**
 * What a duplicate copy of a known source hands to the copy we keep before it is deleted.
 *
 * Only a Kad source result carries a buddy ID, so a copy from a server or source exchange that
 * arrived first would otherwise lose it (#2455). Missing fields are filled. A different buddy
 * address from Kad is not applied at once: the buddy ID stays the same across a buddy change, and
 * nodes the republish missed keep the old address for up to 5 hours, so one search can return
 * either in any order. Applying the last one could move a working route to a dead buddy.
 *
 * An ID from Kad replaces one from source exchange: only the source publishes it, and only an ID
 * from Kad is passed on to other peers.
 */
inline SKadBuddyMerge DecideKadBuddyMerge(const SKadBuddyMergeFacts &facts)
{
	SKadBuddyMerge merge;
	if (!facts.incomingHasID) {
		return merge;
	}
	merge.takeID = !facts.existingHasID || (facts.incomingIDFromKad && !facts.existingIDFromKad);
	if (!facts.incomingHasAddress || facts.sameAddress) {
		return merge;
	}
	if (!facts.existingHasAddress) {
		merge.takeAddress = true;
	} else if (facts.incomingFromKad) {
		merge.storeCandidate = true;
	}
	return merge;
}

/**
 * A callback through the stored buddy failed: move to the fallback.
 *
 * The callback is the signal because the source answers it with a TCP connection. A UDP reask
 * answer is matched by the exact IP and UDP port, so a source behind a NAT that changes the port
 * looks silent through a working buddy.
 */
inline bool ShouldSwitchToBuddyCandidate(bool hasCandidate, bool callbackUnanswered)
{
	return hasCandidate && callbackUnanswered;
}

#endif // KAD_BUDDY_MERGE_POLICY_H
