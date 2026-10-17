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

//! How we publish ourselves as a Kad source, in order of preference.
enum class EKadSourcePublish
{
	HighID,         // Types 1/4: peers dial our IPv4 address
	DirectCallback, // Type 6: firewalled, but peers can ask for a UDP callback directly
	Buddy,          // Types 3/5: firewalled, peers ask our buddy to relay a callback
	IPv6Only,       // Types 3/5 without a buddy: peers can only dial the IPv6 we advertise
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
 * IPv4 with neither callback, yet reachable at the IPv6 address we advertise. It is published as a
 * firewalled source with no buddy tags, which aMule (#1817) and eMuleAI add by its "ip6" tag alone.
 *
 * Neither eMuleAI nor emule-qt publishes in the IPv6Only case: eMuleAI's StorePacket() stops when
 * firewalled with no direct callback and no serving buddy, and emule-qt holds that a published source
 * nobody can reach is worse than none. Older receivers (eMule 0.70b, aMule up to 3.1) ignore the
 * "ip6" tag and accept a type 3/5 source without buddy tags, store a zero buddy ID and spend a Kad
 * buddy lookup on it. We publish anyway because the case needs an ENABLE_IPV6 build and the
 * AdvertiseIPv6 opt-in, and IPv6-capable eMuleAI, emule-qt and aMule peers can dial the source.
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

//! Whether Kad publishing runs at all; the same decision StorePacket() takes per source.
inline bool PublishesToKad(const SKadPublishFacts &facts)
{
	return DecideKadSourcePublish(facts) != EKadSourcePublish::None;
}

#endif // KAD_PUBLISH_POLICY_H
