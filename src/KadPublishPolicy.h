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

#ifndef KAD_PUBLISH_POLICY_H
#define KAD_PUBLISH_POLICY_H

#include <cstdint>

//! How we publish ourselves as a Kad source, in order of preference.
enum class EKadSourcePublish
{
	HighID,         // Types 1/4: peers dial our IPv4 address
	DirectCallback, // Type 6: firewalled, but peers can ask for a UDP callback directly
	Buddy,          // Types 3/5: firewalled, peers ask our buddy to relay a callback
	IPv6Only,       // Type 7: no IPv4 route left, peers reach us by a tag, here the IPv6 one
	None            // No route back to us; publishing would only waste peers' attempts
};

struct SKadPublishFacts
{
	bool firewalled = false;
	bool directCallback = false;
	bool hasBuddy = false;
	bool advertisesIPv6 = false;
};

/**
 * Which kind of Kad source to publish, or None.
 *
 * The IPv4 callback routes come first because they also reach peers without an IPv6 route, and the
 * "ip6" tag rides along with every kind. IPv6Only is the case those routes leave out: firewalled on
 * IPv4 with neither callback, yet reachable at the IPv6 address we advertise.
 *
 * It goes out as source type 7, "reach me by one of the routes in my tags". Sent as a type 3/5 with
 * no buddy tags it was worse than useless to a receiver that cannot read "ip6": it read the absent
 * buddy hash as a zero buddy ID and spent a Kad lookup on it (#2521). A type that receiver does not
 * know costs it nothing instead.
 */
inline EKadSourcePublish DecideKadSourcePublish(const SKadPublishFacts &facts)
{
	if (!facts.firewalled) {
		return EKadSourcePublish::HighID;
	}
	if (facts.directCallback) {
		return EKadSourcePublish::DirectCallback;
	}
	if (facts.hasBuddy) {
		return EKadSourcePublish::Buddy;
	}
	if (facts.advertisesIPv6) {
		return EKadSourcePublish::IPv6Only;
	}
	return EKadSourcePublish::None;
}

//! The FT_SOURCETYPE value a publish of this kind carries. 2 is unusable: older clients misread it.
inline uint8_t KadSourceTypeFor(EKadSourcePublish mode, bool largeFile) noexcept
{
	switch (mode) {
	case EKadSourcePublish::HighID:
		return largeFile ? 4 : 1;
	case EKadSourcePublish::DirectCallback:
		return 6;
	case EKadSourcePublish::Buddy:
		return largeFile ? 5 : 3;
	case EKadSourcePublish::IPv6Only:
		return 7; // One value for every size; the size travels in FT_FILESIZE.
	case EKadSourcePublish::None:
		break;
	}
	return 0;
}

//! Whether Kad publishing runs at all; the same decision StorePacket() takes per source.
inline bool PublishesToKad(const SKadPublishFacts &facts)
{
	return DecideKadSourcePublish(facts) != EKadSourcePublish::None;
}

/**
 * Whether a file's Kad source entries still lead to us: a node acknowledged one less than `lifetime`
 * seconds ago, the time nodes keep it, and it names the buddy we would publish with now. Only a
 * buddy publish names one, so `buddyIP`/`buddyPort` are 0 for every other mode.
 */
inline bool KadSourceEntriesLive(EKadSourcePublish mode,
	uint32_t ackTime,
	uint32_t ackBuddyIP,
	uint16_t ackBuddyPort,
	uint32_t now,
	uint32_t lifetime,
	uint32_t buddyIP,
	uint16_t buddyPort)
{
	return mode != EKadSourcePublish::None && ackTime != 0 && ackTime <= now &&
	       now - ackTime < lifetime && ackBuddyIP == buddyIP && ackBuddyPort == buddyPort;
}

/**
 * Whether a lookup answer's sender key is kept on the contact that answered, so that what we publish
 * to it later proves we receive at our IP.
 *
 * Only for a contact the search owns. A routing table contact with a key refuses updates that lack
 * it, so a spoofed answer must not be able to set one. Only a missing key: the first answer wins.
 * A contact below Kad version 6 is sent no key at all.
 */
inline bool KeepsLookupResponderKey(
	bool ownedBySearch, uint8_t contactVersion, uint32_t currentKey, uint32_t answerKey)
{
	return ownedBySearch && contactVersion >= 6 && currentKey == 0 && answerKey != 0;
}

#endif // KAD_PUBLISH_POLICY_H
