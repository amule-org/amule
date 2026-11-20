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

#ifndef SERVER_ID_CHANGE_H
#define SERVER_ID_CHANGE_H

#include "ArchSpecific.h" // Needed for PeekUInt32
#include <cstddef>
#include <cstdint>

struct IdChangeFields
{
	bool ok = false;
	std::uint32_t id = 0;
	bool hasTcpFlags = false;
	std::uint32_t tcpFlags = 0;
	bool hasAuxPort = false;
	std::uint32_t auxPort = 0;
	bool hasReportedIp = false;
	std::uint32_t reportedIp = 0;
	bool hasObfuscationPortTcp = false;
	std::uint32_t obfuscationPortTcp = 0;
	bool hasYourPort = false;
	std::uint32_t yourPort = 0;
};

// OP_IDCHANGE field layout: [ID][flags][aux port][reported IP][obfuscation TCP port][your port],
// each a 4-byte little-endian eD2k uint32, every field after ID optional and gated on packet
// size. As in eMule, a 16-byte packet carries the reported IP without the obfuscation port.
inline IdChangeFields ParseIdChangeFields(const std::uint8_t *data, std::size_t size)
{
	IdChangeFields fields;
	if (data == nullptr || size < 4) {
		return fields;
	}
	fields.ok = true;
	fields.id = PeekUInt32(data);
	if (size >= 8) {
		fields.hasTcpFlags = true;
		fields.tcpFlags = PeekUInt32(data + 4);
	}
	if (size >= 12) {
		fields.hasAuxPort = true;
		fields.auxPort = PeekUInt32(data + 8);
	}
	if (size >= 16) {
		fields.hasReportedIp = true;
		fields.reportedIp = PeekUInt32(data + 12);
	}
	if (size >= 20) {
		fields.hasObfuscationPortTcp = true;
		fields.obfuscationPortTcp = PeekUInt32(data + 16);
	}
	if (size >= 24) {
		fields.hasYourPort = true;
		fields.yourPort = PeekUInt32(data + 20);
	}
	return fields;
}

#endif // SERVER_ID_CHANGE_H
