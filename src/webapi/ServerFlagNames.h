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

#ifndef WEBAPI_SERVERFLAGNAMES_H
#define WEBAPI_SERVERFLAGNAMES_H

#include <protocol/ed2k/Client2Server/TCP.h>
#include <protocol/ed2k/Client2Server/UDP.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace webapi
{

/**
 * The eD2k server capability bitmasks, decoded to stable JSON keys.
 *
 * A server announces what it supports as two bitmasks; the API decodes them
 * server-side so a consumer never has to carry a copy of the aMule protocol
 * headers, the same rule /clients already follows for its numeric enums (see
 * ClientTagNames.h).
 *
 * Header-only, and deliberately built around a string-returning helper rather
 * than a per-writer emit loop: the REST object (Api.cpp, via CJsonWriter) and
 * the SSE payload (EventDiff.cpp, via ostringstream) use different writers but
 * are documented as carrying the identical object, so they call the same
 * function and emit the same bytes. Only the two tables below decide what a flags
 * object contains; adding a bit is a one-line change that both paths pick up.
 *
 * EventDiff.cpp is compiled into EventDiffTest, which is stdlib-only by
 * design, so nothing here may reach for wxWidgets or EC.
 *
 * These are protocol tokens, not display text: untranslated and stable. The
 * desktop's TCP/UDP Flags columns render the same bits as single letters
 * (ServerListCtrl.cpp), which is a separate presentation of the same data.
 */

struct ServerFlagBit
{
	std::uint32_t mask;
	const char *key;
};

/**
 * One flags object: `{"bitmask":N,"compression":true,...}`.
 *
 * Every key in the table is always present, false when its bit is clear, so a
 * consumer never branches on key existence. `bitmask` rides along for the
 * diagnostic case and for any bit a future server announces that this build
 * does not name yet -- decoding it to booleans alone would silently drop it.
 *
 * All keys are ASCII literals from the tables, so no JSON escaping is needed.
 */
inline std::string ServerFlagsJson(std::uint32_t bits, const ServerFlagBit *table, std::size_t count)
{
	std::string out = "{\"bitmask\":";
	out += std::to_string(bits);
	for (std::size_t i = 0; i < count; ++i) {
		out += ",\"";
		out += table[i].key;
		out += "\":";
		out += (bits & table[i].mask) != 0 ? "true" : "false";
	}
	out += "}";
	return out;
}

/**
 * The TCP capability object for `tcp_flags`.
 *
 * Bits from include/protocol/ed2k/Client2Server/TCP.h, in wire-bit order.
 * The table is a function-local static rather than a namespace-scope one so
 * every translation unit including this header shares a single instance.
 */
inline std::string ServerTcpFlagsJson(std::uint32_t bits)
{
	static const ServerFlagBit kBits[] = {
		{ SRV_TCPFLG_COMPRESSION, "compression" },
		{ SRV_TCPFLG_NEWTAGS, "new_tags" },
		{ SRV_TCPFLG_UNICODE, "unicode" },
		{ SRV_TCPFLG_RELATEDSEARCH, "related_search" },
		{ SRV_TCPFLG_TYPETAGINTEGER, "type_tag_integer" },
		{ SRV_TCPFLG_LARGEFILES, "large_files" },
		// Spelled out rather than plain "obfuscation" because the UDP
		// object below carries both a UDP- and a TCP-obfuscation bit;
		// one key means one wire constant in either object.
		{ SRV_TCPFLG_TCPOBFUSCATION, "tcp_obfuscation" },
	};
	return ServerFlagsJson(bits, kBits, sizeof(kBits) / sizeof(kBits[0]));
}

//! The UDP capability object for `udp_flags`. Bits from
//! include/protocol/ed2k/Client2Server/UDP.h, in wire-bit order.
inline std::string ServerUdpFlagsJson(std::uint32_t bits)
{
	static const ServerFlagBit kBits[] = {
		{ SRV_UDPFLG_EXT_GETSOURCES, "get_sources" },
		{ SRV_UDPFLG_EXT_GETFILES, "get_files" },
		{ SRV_UDPFLG_NEWTAGS, "new_tags" },
		{ SRV_UDPFLG_UNICODE, "unicode" },
		// "_v2" not "2": a bare digit glued to the name reads like a count.
		{ SRV_UDPFLG_EXT_GETSOURCES2, "get_sources_v2" },
		{ SRV_UDPFLG_LARGEFILES, "large_files" },
		{ SRV_UDPFLG_UDPOBFUSCATION, "udp_obfuscation" },
		{ SRV_UDPFLG_TCPOBFUSCATION, "tcp_obfuscation" },
	};
	return ServerFlagsJson(bits, kBits, sizeof(kBits) / sizeof(kBits[0]));
}

} // namespace webapi

#endif // WEBAPI_SERVERFLAGNAMES_H
// File_checked_for_headers
