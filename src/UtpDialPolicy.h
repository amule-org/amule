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

#ifndef UTP_DIAL_POLICY_H
#define UTP_DIAL_POLICY_H

#include "NetworkFunctions.h"

#include <cstdint>

// Preserve the TCP/callback/buddy/refusal path unless direct uTP is eligible.
// This policy does not replace security checks or authorize NAT rendezvous.
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
	uint32_t ip = 0; // aMule low-byte-first IPv4 representation
	uint16_t udpPort = 0;
};

inline bool IsUsableUtpEndpoint(uint32_t ip, uint16_t port)
{
	return IsGoodIP(ip, false) && port != 0;
}

inline EUtpDialDecision DecideUtpDial(const SUtpDialFacts &facts)
{
	return facts.peerSupportsUtp && facts.localOutboundService && facts.directHighId &&
			       !facts.proxyEnabled && IsUsableUtpEndpoint(facts.ip, facts.udpPort)
		       ? EUtpDialDecision::TryUtp
		       : EUtpDialDecision::PreserveLegacy;
}

#endif // UTP_DIAL_POLICY_H
