// This file is part of the aMule Project.
// Copyright (c) 2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU GPL version 2 or later.

#ifndef UTP_DIAL_POLICY_H
#define UTP_DIAL_POLICY_H

#include <cstdint>

// PreserveLegacy means keep the existing TCP/callback/buddy/refusal decision,
// not "force TCP". This policy neither bypasses contact security checks nor
// authorizes NAT rendezvous. It is not wired into client selection yet.
enum class EUtpDialDecision
{
	PreserveLegacy,
	TryUtp
};

struct SUtpDialFacts
{
	bool peerSupportsUtp = false;
	bool localOutboundService = false;
	bool directHighId = false;
	bool proxyEnabled = false;
	bool ipv4Endpoint = false;
	uint32_t ip = 0; // aMule low-byte-first IPv4 representation
	uint16_t udpPort = 0;
};

constexpr bool IsUsableUtpEndpoint(uint32_t ip, uint16_t port)
{
	// Unspecified, multicast and reserved/broadcast addresses cannot be dialed.
	// Private and loopback endpoints remain usable; this is not a WAN policy.
	const uint32_t firstOctet = ip & 0xff;
	return port != 0 && firstOctet != 0 && firstOctet < 224;
}

constexpr EUtpDialDecision DecideUtpDial(const SUtpDialFacts &facts)
{
	return facts.peerSupportsUtp && facts.localOutboundService && facts.directHighId &&
			       !facts.proxyEnabled && facts.ipv4Endpoint &&
			       IsUsableUtpEndpoint(facts.ip, facts.udpPort)
		       ? EUtpDialDecision::TryUtp
		       : EUtpDialDecision::PreserveLegacy;
}

#endif // UTP_DIAL_POLICY_H
