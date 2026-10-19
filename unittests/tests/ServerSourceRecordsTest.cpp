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

#include <muleunit/test.h>
#include "ServerSourceRecords.h"

#include <vector>

using namespace muleunit;

DECLARE_SIMPLE(ServerSourceRecords)

namespace
{
void PushUInt32(std::vector<std::uint8_t> &buf, std::uint32_t value)
{
	buf.push_back(static_cast<std::uint8_t>(value & 0xFF));
	buf.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
	buf.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
	buf.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

void PushUInt16(std::vector<std::uint8_t> &buf, std::uint16_t value)
{
	buf.push_back(static_cast<std::uint8_t>(value & 0xFF));
	buf.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

CNetworkAddress::Octets SomeIPv6()
{
	return { 0x20, 0x01, 0x48, 0x60, 0x48, 0x60, 0, 0, 0, 0, 0, 0, 0, 0, 0x88, 0x88 };
}
} // namespace

TEST(ServerSourceRecords, APlainIPv4RecordParses)
{
	std::vector<std::uint8_t> buf;
	PushUInt32(buf, 0xC0A80001u);
	PushUInt16(buf, 4662);

	const SServerSourceRecordParse parsed = ParseServerSourceRecord(buf.data(), buf.size(), false, false);
	ASSERT_TRUE(parsed.ok);
	ASSERT_EQUALS(buf.size(), parsed.bytesConsumed);
	ASSERT_FALSE(parsed.record.isIPv6);
	ASSERT_EQUALS(static_cast<std::uint32_t>(0xC0A80001u), parsed.record.userid);
	ASSERT_EQUALS(static_cast<std::uint16_t>(4662), parsed.record.port);
	ASSERT_FALSE(parsed.record.hasUserHash);
}

// 0xFFFFFFFF is not special without the obfuscation extension at all: the sentinel always rides
// alongside at least the crypt-options byte in practice, but the parser itself only keys off
// weAdvertisedIPv6, so this also covers "a plain, non-obfuscated record".
TEST(ServerSourceRecords, AnIPv4RecordAmongSentinelCapablePacketsStillParsesAsIPv4)
{
	std::vector<std::uint8_t> buf;
	PushUInt32(buf, 0xC0A80001u);
	PushUInt16(buf, 4662);

	const SServerSourceRecordParse parsed = ParseServerSourceRecord(buf.data(), buf.size(), false, true);
	ASSERT_TRUE(parsed.ok);
	ASSERT_FALSE(parsed.record.isIPv6);
	ASSERT_EQUALS(static_cast<std::uint32_t>(0xC0A80001u), parsed.record.userid);
}

TEST(ServerSourceRecords, AnObfuscatedIPv4RecordWithAHashParses)
{
	std::vector<std::uint8_t> buf;
	PushUInt32(buf, 0xC0A80001u);
	PushUInt16(buf, 4662);
	buf.push_back(0x80); // crypt options: hash present
	for (int i = 0; i < 16; ++i) {
		buf.push_back(static_cast<std::uint8_t>(i));
	}

	const SServerSourceRecordParse parsed = ParseServerSourceRecord(buf.data(), buf.size(), true, false);
	ASSERT_TRUE(parsed.ok);
	ASSERT_EQUALS(buf.size(), parsed.bytesConsumed);
	ASSERT_FALSE(parsed.record.isIPv6);
	ASSERT_EQUALS(static_cast<std::uint8_t>(0x80), parsed.record.cryptOptions);
	ASSERT_TRUE(parsed.record.hasUserHash);
}

TEST(ServerSourceRecords, AnObfuscatedRecordWithoutTheHashBitCarriesNoHash)
{
	std::vector<std::uint8_t> buf;
	PushUInt32(buf, 0xC0A80001u);
	PushUInt16(buf, 4662);
	buf.push_back(0x03); // crypt options: supported + requested, no hash bit

	const SServerSourceRecordParse parsed = ParseServerSourceRecord(buf.data(), buf.size(), true, false);
	ASSERT_TRUE(parsed.ok);
	ASSERT_EQUALS(buf.size(), parsed.bytesConsumed);
	ASSERT_FALSE(parsed.record.hasUserHash);
}

TEST(ServerSourceRecords, ASentinelRecordParsesAsIPv6WhenWeAdvertised)
{
	std::vector<std::uint8_t> buf;
	PushUInt32(buf, SERVER_SOURCE_IPV6_SENTINEL_ID);
	PushUInt16(buf, 4662);
	const CNetworkAddress::Octets octets = SomeIPv6();
	for (std::uint8_t b : octets) {
		buf.push_back(b);
	}

	const SServerSourceRecordParse parsed = ParseServerSourceRecord(buf.data(), buf.size(), false, true);
	ASSERT_TRUE(parsed.ok);
	ASSERT_EQUALS(buf.size(), parsed.bytesConsumed);
	ASSERT_TRUE(parsed.record.isIPv6);
	ASSERT_TRUE(parsed.record.ipv6Address == CNetworkAddress::IPv6FromOctets(octets));
	ASSERT_EQUALS(static_cast<std::uint16_t>(4662), parsed.record.port);
}

// The ed2k-server reference puts the sixteen address bytes ALWAYS LAST, after any obfuscation
// fields -- that position is what makes the record self-delimiting on the wire.
TEST(ServerSourceRecords, ASentinelRecordWithObfuscationFieldsPutsTheAddressBytesLast)
{
	std::vector<std::uint8_t> buf;
	PushUInt32(buf, SERVER_SOURCE_IPV6_SENTINEL_ID);
	PushUInt16(buf, 4662);
	buf.push_back(0x80);
	for (int i = 0; i < 16; ++i) {
		buf.push_back(static_cast<std::uint8_t>(0xA0 + i));
	}
	const CNetworkAddress::Octets octets = SomeIPv6();
	for (std::uint8_t b : octets) {
		buf.push_back(b);
	}

	const SServerSourceRecordParse parsed = ParseServerSourceRecord(buf.data(), buf.size(), true, true);
	ASSERT_TRUE(parsed.ok);
	ASSERT_EQUALS(buf.size(), parsed.bytesConsumed);
	ASSERT_TRUE(parsed.record.isIPv6);
	ASSERT_TRUE(parsed.record.hasUserHash);
	ASSERT_TRUE(parsed.record.ipv6Address == CNetworkAddress::IPv6FromOctets(octets));
}

// Without CT_MOD_IP_V6 in our login, a server honouring the wire contract never
// sends the sentinel id -- but if it arrived anyway, it must parse exactly as it did before this
// feature existed: an ordinary (unusable) userid.
TEST(ServerSourceRecords, ASentinelIsAnOrdinaryUseridWhenWeDidNotAdvertise)
{
	std::vector<std::uint8_t> buf;
	PushUInt32(buf, SERVER_SOURCE_IPV6_SENTINEL_ID);
	PushUInt16(buf, 4662);

	const SServerSourceRecordParse parsed = ParseServerSourceRecord(buf.data(), buf.size(), false, false);
	ASSERT_TRUE(parsed.ok);
	ASSERT_EQUALS(buf.size(), parsed.bytesConsumed);
	ASSERT_FALSE(parsed.record.isIPv6);
	ASSERT_EQUALS(SERVER_SOURCE_IPV6_SENTINEL_ID, parsed.record.userid);
}

TEST(ServerSourceRecords, ATruncatedSentinelRecordFailsToParse)
{
	std::vector<std::uint8_t> buf;
	PushUInt32(buf, SERVER_SOURCE_IPV6_SENTINEL_ID);
	PushUInt16(buf, 4662);
	// Only 8 of the 16 trailing address bytes are present.
	for (int i = 0; i < 8; ++i) {
		buf.push_back(0xAA);
	}

	const SServerSourceRecordParse parsed = ParseServerSourceRecord(buf.data(), buf.size(), false, true);
	ASSERT_FALSE(parsed.ok);
}

TEST(ServerSourceRecords, ATruncatedPlainRecordFailsToParse)
{
	std::vector<std::uint8_t> buf;
	PushUInt32(buf, 0xC0A80001u);
	// The port is missing entirely.

	const SServerSourceRecordParse parsed = ParseServerSourceRecord(buf.data(), buf.size(), false, false);
	ASSERT_FALSE(parsed.ok);
}

TEST(ServerSourceRecords, ATruncatedObfuscationHashFailsToParse)
{
	std::vector<std::uint8_t> buf;
	PushUInt32(buf, 0xC0A80001u);
	PushUInt16(buf, 4662);
	buf.push_back(0x80); // hash bit set, but no hash bytes follow

	const SServerSourceRecordParse parsed = ParseServerSourceRecord(buf.data(), buf.size(), true, false);
	ASSERT_FALSE(parsed.ok);
}
