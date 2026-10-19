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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
//

// Shared logging for a parsed OP_NAT_CALLBACKREQUESTED{,_UDP}: ServerSocket.cpp (TCP 0x37) and
// ClientUDPSocket.cpp (UDP 0xA7) both route through this one handler, mirroring NeoMule's
// ServerSocket.cpp/ClientUDPSocket.cpp split. Separate from LugdunumNatTraversal.h because it
// pulls in Logger.h/Format.h, which that header deliberately stays free of for cheap unit testing.
#ifndef AMULE_LUGDUNUM_NAT_CALLBACK_LOG_H
#define AMULE_LUGDUNUM_NAT_CALLBACK_LOG_H

#ifdef ENABLE_NATT_SERVER_COORDINATION

#include "LugdunumNatTraversal.h"
#include "Logger.h"
#include "NetworkFunctions.h" // Needed for Uint32toStringIP

#include <common/Format.h> // Needed for CFormat

namespace LugdunumNatTraversal
{

// Logs the three forwarded-callback fields and nothing else: there is no reply and no punch.
inline void LogForwardedCallback(DebugType logType, const char *context, const ForwardedCallback &callback)
{
	AddDebugLogLineN(logType,
		CFormat("%s: seen IP %s, seen UDP port %u, requester ID %u") % context %
			Uint32toStringIP(callback.seenIp) % callback.seenUdpPort % callback.requesterId);
}

} // namespace LugdunumNatTraversal

#endif // ENABLE_NATT_SERVER_COORDINATION

#endif // AMULE_LUGDUNUM_NAT_CALLBACK_LOG_H
