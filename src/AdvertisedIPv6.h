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

#ifndef ADVERTISED_IPV6_H
#define ADVERTISED_IPV6_H

#include "NetworkAddress.h"
#include "NetworkInterfaces.h"
#include "PeerAddressing.h" // Needed for IsUsableTagIdentity

#include <cstdint>
#include <optional>
#include <tuple>
#include <vector>

/**
 * The IPv6 address to give peers as ours, or none.
 *
 * Peers dial whatever is advertised, so a candidate must be an address our own tag parser would
 * accept, and never a deprecated one, which is kept for existing connections only. With an
 * interface pin, inbound traffic arrives only through that interface, so only its addresses count.
 * A stable address is preferred over an RFC 4941 temporary one, which the system rotates away from
 * peers that stored it. The lowest address wins a tie, so every refresh agrees whatever order the
 * platform enumerates in.
 */
inline std::optional<CNetworkAddress> ChooseAdvertisedIPv6(
	const std::vector<NetworkInterface> &interfaces, const wxString &pinnedInterface)
{
	const NetworkInterfaceIPv6 *best = nullptr;
	for (const NetworkInterface &iface : interfaces) {
		if (!pinnedInterface.IsEmpty() && iface.name != pinnedInterface) {
			continue;
		}
		for (const NetworkInterfaceIPv6 &address : iface.ipv6) {
			if (address.deprecated || !PeerAddressing::IsUsableTagIdentity(
							  CNetworkAddress::IPv6FromOctets(address.bytes))) {
				continue;
			}
			if (!best || std::tie(address.temporary, address.bytes) <
					     std::tie(best->temporary, best->bytes)) {
				best = &address;
			}
		}
	}
	if (!best) {
		return std::nullopt;
	}
	return CNetworkAddress::IPv6FromOctets(best->bytes);
}

// Verified advertises when reachability has been verified; Always declares inbound IPv6 reachable;
// Never suppresses advertising regardless of evidence.
enum class EIPv6AdvertisingMode
{
	Verified,
	Always,
	Never
};

//! Convert the stored legacy checkbox value to its equivalent advertising mode.
inline EIPv6AdvertisingMode LegacyAdvertiseIPv6Mode(bool stored)
{
	return stored ? EIPv6AdvertisingMode::Always : EIPv6AdvertisingMode::Verified;
}

//! Whether a stored integer is one of the three modes.
inline bool IsValidIPv6AdvertisingMode(int stored)
{
	return stored >= static_cast<int>(EIPv6AdvertisingMode::Verified) &&
	       stored <= static_cast<int>(EIPv6AdvertisingMode::Never);
}

//! Map a stored integer to a mode; anything unrecognised falls back to the default.
inline EIPv6AdvertisingMode NormalizeIPv6AdvertisingMode(int stored)
{
	return IsValidIPv6AdvertisingMode(stored) ? static_cast<EIPv6AdvertisingMode>(stored)
						  : EIPv6AdvertisingMode::Verified;
}

//! Report the legacy EC boolean for an advertising mode.
inline bool IPv6AdvertisingModeAsECBool(EIPv6AdvertisingMode mode)
{
	return mode == EIPv6AdvertisingMode::Always;
}

//! Apply the legacy EC boolean without allowing false to replace an explicit Never mode.
inline EIPv6AdvertisingMode IPv6AdvertisingModeFromECBool(EIPv6AdvertisingMode current, bool received)
{
	if (received) {
		return EIPv6AdvertisingMode::Always;
	}
	return current == EIPv6AdvertisingMode::Always ? EIPv6AdvertisingMode::Verified : current;
}

//! The mode an EC preferences packet carries.
//!
//! The mode tag wins when its value is one of the three; an invalid value leaves the current mode
//! alone rather than letting a malformed packet move it. The legacy boolean is the fallback for a
//! client or daemon that predates the tag, and an absent pair changes nothing.
inline EIPv6AdvertisingMode ResolveAdvertiseIPv6ModeFromEC(EIPv6AdvertisingMode current,
	bool hasMode,
	std::uint64_t modeValue,
	bool hasBoolean,
	bool booleanValue)
{
	if (hasMode) {
		return modeValue <= static_cast<std::uint64_t>(EIPv6AdvertisingMode::Never)
			       ? static_cast<EIPv6AdvertisingMode>(modeValue)
			       : current;
	}
	return hasBoolean ? IPv6AdvertisingModeFromECBool(current, booleanValue) : current;
}

struct SAdvertisedIPv6Facts
{
	std::optional<CNetworkAddress> choice; // ChooseAdvertisedIPv6() at the last interface refresh
	bool contradicted = false;             // Peers see us arrive from a /64 we do not hold
	EIPv6AdvertisingMode mode = EIPv6AdvertisingMode::Verified; // User's IPv6 advertising preference
	bool serverVerified = false;    // Server's dial-back probe reached us and reported us reachable
	bool serverProbeFailed = false; // Server probed and could not reach us
	// A completed hello reached the chosen address from outside our /48.
	bool provedByInboundHello = false;
	bool ipv6Built = false;
	bool familyPermitted = false;
	bool proxyEnabled = false;
	bool listenerAcceptsIPv6 = false;
};

/**
 * Whether the user's declaration or current evidence says inbound IPv6 is reachable.
 *
 * A failed server probe outranks positive inbound proof, failing closed until the server re-probes.
 * Always outranks the verdicts because it is the user's own declaration.
 */
inline bool IsIPv6DeclaredReachable(const SAdvertisedIPv6Facts &facts)
{
	if (facts.mode == EIPv6AdvertisingMode::Never) {
		return false;
	}
	if (facts.mode == EIPv6AdvertisingMode::Always) {
		return true;
	}
	return !facts.serverProbeFailed && (facts.serverVerified || facts.provedByInboundHello);
}

/**
 * The IPv6 address to advertise as ours, or none.
 *
 * eMuleAI treats a peer's CT_MOD_IP_V6 as reachable: it dials that address for every later
 * connection with no IPv4 fallback, skips LowID callbacks, keeps it for the whole session and passes
 * it on in source exchange. An address a firewall drops would therefore cost connections we have
 * today, so it is advertised only when evidence says it is reachable or the user declares it, and
 * never against a contradiction. A proxy carries IPv4 only and advertising around it would leak our
 * address; without an IPv6 listener there is nothing to reach.
 *
 * Peer corroboration is a veto, not a precondition. Requiring it first deadlocks: votes arrive only
 * over IPv6 connections, which peers open only to an address we advertise. Behind IPv6 NAT, the case
 * it guards against, peers report a /64 we do not hold, and that contradiction withdraws the address.
 *
 * The inbound-hello proof has the same shape as a peer vote, which is worth naming: a peer can only
 * dial our IPv6 once it has learned the address, and it learns it from our hello, Kad, the server's
 * source lists or source exchange, all of which wait for this decision. So in practice
 * `When verified` means "advertise once a server that probes has checked us", with two ways out: a
 * proof carried over from a probing server for 24 h, and an address the server itself asks for.
 */
inline std::optional<CNetworkAddress> DecideAdvertisedIPv6(const SAdvertisedIPv6Facts &facts)
{
	const bool reachable = IsIPv6DeclaredReachable(facts) && !facts.contradicted;
	const bool permitted = facts.ipv6Built && facts.familyPermitted && !facts.proxyEnabled;
	if (!facts.choice || !reachable || !permitted || !facts.listenerAcceptsIPv6) {
		return std::nullopt;
	}
	return facts.choice;
}

/**
 * Whether Kad source records need a prompt republish after the IPv6 advertising state changes.
 *
 * Every reason to stop advertising now makes the address in existing records wrong: the
 * configuration or local address changed, or peers contradict it. So any change to what would be
 * published, including withdrawing it, republishes.
 */
inline bool ShouldRepublishKadSources(
	const std::optional<CNetworkAddress> &lastPublished, const SAdvertisedIPv6Facts &facts)
{
	return DecideAdvertisedIPv6(facts) != lastPublished;
}

/**
 * The address to send a server: the one it will test, or what the peers decision allows.
 *
 * A server that probes needs an address it has not published yet, so reachability is not required
 * here; everything else that protects the address is. Any other server receives exactly what peers
 * would, which may be nothing until the address is proved.
 */
inline std::optional<CNetworkAddress> ServerAddressToSend(
	const SAdvertisedIPv6Facts &facts, bool serverProbes)
{
	if (!serverProbes) {
		return DecideAdvertisedIPv6(facts);
	}
	const bool permitted = facts.ipv6Built && facts.familyPermitted && !facts.proxyEnabled;
	if (!facts.choice || !permitted || !facts.listenerAcceptsIPv6 || facts.contradicted ||
		facts.mode == EIPv6AdvertisingMode::Never) {
		return std::nullopt;
	}
	return facts.choice;
}

#ifdef ENABLE_IPV6
/**
 * The IPv6 address our hellos carry right now, or none.
 *
 * Defined in amule.cpp, beside the interface refresh that feeds it, so this header stays clear of
 * the app and its sockets and the decision above can be unit-tested on its own. It also hands a
 * server its address on every call, through ServerAddressToSend(); duplicates are dropped there,
 * so calling it as often as the interface refresh, a hello or Kad does is safe.
 */
std::optional<CNetworkAddress> AdvertisedIPv6ForPeers();

//! The facts the decision is made from, defined beside the interface refresh.
SAdvertisedIPv6Facts CurrentAdvertisedIPv6Facts();

/**
 * The stable globally routable local IPv6 selected from the current interfaces, or none.
 *
 * Unlike AdvertisedIPv6ForPeers(), this does not assert inbound reachability or opt the address
 * into hello advertisement. It is only evidence that an outbound IPv6 source route exists.
 */
std::optional<CNetworkAddress> SelectedLocalIPv6();
#endif

#endif // ADVERTISED_IPV6_H
