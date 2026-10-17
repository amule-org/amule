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

#ifndef SERVER_SOURCE_RECORDS_H
#define SERVER_SOURCE_RECORDS_H

#include "ArchSpecific.h" // Needed for PeekUInt16/PeekUInt32
#include "MD4Hash.h"      // Needed for CMD4Hash
#include "NetworkAddress.h"

#include <cstddef>
#include <cstdint>

// The userid that marks an IPv6 source record (eMuleQt, eNode-go, ed2k-server).
const std::uint32_t SERVER_SOURCE_IPV6_SENTINEL_ID = 0xFFFFFFFFu;

struct SServerSourceRecord
{
	bool isIPv6 = false;
	std::uint32_t userid = 0;    // Valid when !isIPv6.
	CNetworkAddress ipv6Address; // Valid when isIPv6; Absent on a malformed address.
	std::uint16_t port = 0;
	std::uint8_t cryptOptions = 0; // Meaningful only when the record carried obfuscation fields.
	bool hasUserHash = false;
	CMD4Hash userHash;
};

struct SServerSourceRecordParse
{
	bool ok = false; // false: length was too short for the record the flags describe.
	std::size_t bytesConsumed = 0;
	SServerSourceRecord record;
};

/**
 * Parses one OP_FOUNDSOURCES(_OBFU) record: `<id 4><port 2>`, plus `<crypt options 1>[<user hash 16>]`
 * when @a obfuscated. After we advertised IPv6 in the login, a sentinel id is followed by 16 address
 * bytes placed last, after the obfuscation fields; that position is what delimits the record.
 * Otherwise the sentinel is an ordinary userid, as before the extension.
 */
inline SServerSourceRecordParse ParseServerSourceRecord(
	const std::uint8_t *data, std::size_t length, bool obfuscated, bool weAdvertisedIPv6)
{
	SServerSourceRecordParse result;
	std::size_t offset = 0;

	if (length < offset + 4 + 2) {
		return result;
	}
	const std::uint32_t userid = PeekUInt32(data + offset);
	offset += 4;
	const std::uint16_t port = PeekUInt16(data + offset);
	offset += 2;

	std::uint8_t cryptOptions = 0;
	bool hasUserHash = false;
	CMD4Hash userHash;
	if (obfuscated) {
		if (length < offset + 1) {
			return result;
		}
		cryptOptions = data[offset];
		offset += 1;
		if ((cryptOptions & 0x80) != 0) {
			if (length < offset + MD4HASH_LENGTH) {
				return result;
			}
			userHash = CMD4Hash(data + offset);
			offset += MD4HASH_LENGTH;
			hasUserHash = true;
		}
	}

	SServerSourceRecord record;
	record.port = port;
	record.cryptOptions = cryptOptions;
	record.hasUserHash = hasUserHash;
	record.userHash = userHash;

	if (weAdvertisedIPv6 && userid == SERVER_SOURCE_IPV6_SENTINEL_ID) {
		if (length < offset + CNetworkAddress::OCTET_COUNT) {
			return result;
		}
		record.isIPv6 = true;
		record.ipv6Address = CNetworkAddress::FromIPv6Bytes(data + offset);
		offset += CNetworkAddress::OCTET_COUNT;
	} else {
		record.isIPv6 = false;
		record.userid = userid;
	}

	result.ok = true;
	result.bytesConsumed = offset;
	result.record = record;
	return result;
}

#endif // SERVER_SOURCE_RECORDS_H
