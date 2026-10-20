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

// Shared logging for a parsed OP_LOWID_HOLEPUNCH_INFO/FAIL: ServerSocket.cpp only (S4 -- no
// punch, no reply). Separate from NatServerHolePunch.h because it pulls in Logger.h/Format.h,
// which that header deliberately stays free of for cheap unit testing.
#ifndef AMULE_NAT_SERVER_HOLE_PUNCH_LOG_H
#define AMULE_NAT_SERVER_HOLE_PUNCH_LOG_H

#ifdef ENABLE_NATT_SERVER_COORDINATION

#include "NatServerHolePunch.h"
#include "Logger.h"
#include "NetworkFunctions.h" // Needed for Uint32toStringIP

#include <common/Format.h> // Needed for CFormat

namespace NatServerHolePunch
{

inline void LogInfo(DebugType logType, const Info &info)
{
	const uint32 ip = PeekUInt32(info.peerIP.data());
	AddDebugLogLineN(logType,
		CFormat("OP_LOWID_HOLEPUNCH_INFO: peer %s, TCP %u, UDP %u, role %u") % Uint32toStringIP(ip) %
			info.tcpPort % info.udpPort % info.role);
}

inline void LogFailure(DebugType logType, const Failure &failure)
{
	AddDebugLogLineN(logType,
		CFormat("OP_LOWID_HOLEPUNCH_FAIL: target %u, reason %u") % failure.targetId % failure.reason);
}

} // namespace NatServerHolePunch

#endif // ENABLE_NATT_SERVER_COORDINATION

#endif // AMULE_NAT_SERVER_HOLE_PUNCH_LOG_H
