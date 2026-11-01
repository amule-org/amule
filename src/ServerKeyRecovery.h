//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// Any parts of this program contributed by third-party developers are copyrighted
// by their respective authors.
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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
//
#ifndef AMULE_SERVER_KEY_RECOVERY_H
#define AMULE_SERVER_KEY_RECOVERY_H

#include "Types.h"                   // Needed for the uint32 that Constants.h uses
#include <protocol/ed2k/Constants.h> // Needed for UDPSERVSTATREASKTIME and UDPSERVSTATMINREASKTIME

#include <cstdint>
#include <ctime>
#include <optional>

// A server's UDP key comes only from the answer to its obfuscated status ping. When that ping is
// lost the next one is due 3.5 to 4.5 h later, so a user of the key brings it forward instead.
namespace ServerKeyRecovery
{
struct Facts
{
	bool cryptLayerUdpEnabled = false;
	// Without it ServerStats() sends only a plain status ping, which brings no key.
	bool publicIpKnown = false;
	bool serverSupportsObfuscationUdp = false;
	// CServer::GetServerKeyUDP(): also 0 when the key was given to an earlier public IP.
	std::uint32_t serverKeyUdp = 0;
	bool cryptPingReplyPending = false;
	time_t lastPingedTime = 0;
	time_t realLastPingedTime = 0;
};

// The LastPingedTime that makes CServerList::ServerStats() send the server its next status ping,
// obfuscated first, as soon as UDPSERVSTATMINREASKTIME allows, as CheckForExpiredUDPKeys() does. No
// value when the key is there, cannot be had, is already being asked for, or the ping is no later.
inline std::optional<time_t> RecoveryLastPingedTime(const Facts &facts, time_t now)
{
	if (!facts.cryptLayerUdpEnabled || !facts.publicIpKnown || !facts.serverSupportsObfuscationUdp ||
		facts.serverKeyUdp != 0 || facts.cryptPingReplyPending || facts.lastPingedTime == 0) {
		return std::nullopt;
	}
	time_t earliest = now;
	if (facts.realLastPingedTime != 0 && facts.realLastPingedTime + UDPSERVSTATMINREASKTIME > now) {
		earliest = facts.realLastPingedTime + UDPSERVSTATMINREASKTIME;
	}
	if (facts.lastPingedTime + UDPSERVSTATREASKTIME <= earliest) {
		return std::nullopt;
	}
	return earliest - UDPSERVSTATREASKTIME;
}
} // namespace ServerKeyRecovery

#endif // AMULE_SERVER_KEY_RECOVERY_H
