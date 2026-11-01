//								-*- C++ -*-
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

#include "ECReplaySocket.h"

#include <ec/cpp/ECCodes.h>
#include <ec/cpp/ECPacket.h>
#include <ec/cpp/ECSocket.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace muleunit;

namespace
{

void Put16(std::vector<unsigned char> &v, uint32_t x)
{
	v.push_back(static_cast<unsigned char>(x >> 8));
	v.push_back(static_cast<unsigned char>(x));
}

void Put32(std::vector<unsigned char> &v, uint32_t x)
{
	Put16(v, x >> 16);
	Put16(v, x & 0xFFFF);
}

// One packet holding one tag of @a type with exactly @a data, written by hand because
// CECTag cannot build a tag whose length disagrees with its type.
std::vector<unsigned char> OneTag(uint8_t type, const std::vector<unsigned char> &data)
{
	std::vector<unsigned char> body;
	body.push_back(EC_OP_NOOP);
	Put16(body, 1);                  // tag count
	Put16(body, EC_TAG_STRING << 1); // tag name, no children
	body.push_back(type);
	Put32(body, static_cast<uint32_t>(data.size()));
	body.insert(body.end(), data.begin(), data.end());

	std::vector<unsigned char> packet;
	Put32(packet, 0x20); // flags: no zlib, plain numbers
	Put32(packet, static_cast<uint32_t>(body.size()));
	packet.insert(packet.end(), body.begin(), body.end());
	return packet;
}

std::unique_ptr<const CECPacket> Receive(const std::vector<unsigned char> &bytes)
{
	CReplaySocket socket(bytes);
	const CECPacket request(EC_OP_NOOP);
	return std::unique_ptr<const CECPacket>(socket.SendRecvPacket(&request));
}

bool Parses(uint8_t type, size_t len)
{
	return Receive(OneTag(type, std::vector<unsigned char>(len, 'A'))) != nullptr;
}

} // namespace

DECLARE_SIMPLE(ECTagLength)

TEST(ECTagLength, FixedWidthTypesNeedTheirWidth)
{
	const struct
	{
		uint8_t type;
		size_t width;
	} types[] = {
		{ EC_TAGTYPE_UINT8, 1 },
		{ EC_TAGTYPE_UINT16, 2 },
		{ EC_TAGTYPE_UINT32, 4 },
		{ EC_TAGTYPE_UINT64, 8 },
		{ EC_TAGTYPE_IPV4, 6 },
		{ EC_TAGTYPE_HASH16, 16 },
		{ EC_TAGTYPE_UINT128, 16 },
	};
	for (const auto &t : types) {
		ASSERT_TRUE(Parses(t.type, t.width));
		ASSERT_FALSE(Parses(t.type, 1 == t.width ? 2 : 1));
		ASSERT_FALSE(Parses(t.type, t.width + 1));
	}
}

// A tag with children and no value of its own is sent with its type and no data.
TEST(ECTagLength, EmptyTagOfAnyTypeParses)
{
	ASSERT_TRUE(Parses(EC_TAGTYPE_UINT64, 0));
	ASSERT_TRUE(Parses(EC_TAGTYPE_HASH16, 0));
}

// The GHSA-6qxf case: a one-byte client name without its NUL used to be read past the end.
TEST(ECTagLength, StringWithoutNulStopsAtItsLength)
{
	std::unique_ptr<const CECPacket> reply = Receive(OneTag(EC_TAGTYPE_STRING, { 'A' }));
	ASSERT_TRUE(reply != nullptr);
	const CECTag *tag = reply->GetTagByName(EC_TAG_STRING);
	ASSERT_TRUE(tag != nullptr);
	ASSERT_EQUALS(std::string("A"), tag->GetStringDataSTL());
}

TEST(ECTagLength, WellFormedTagsStillRoundTrip)
{
	std::unique_ptr<const CECPacket> reply =
		Receive(OneTag(EC_TAGTYPE_UINT32, { 0x01, 0x02, 0x03, 0x04 }));
	ASSERT_TRUE(reply != nullptr);
	ASSERT_EQUALS(0x01020304u, reply->GetTagByName(EC_TAG_STRING)->GetInt());

	reply = Receive(OneTag(EC_TAGTYPE_STRING, { 'h', 'i', 0 }));
	ASSERT_TRUE(reply != nullptr);
	ASSERT_EQUALS(std::string("hi"), reply->GetTagByName(EC_TAG_STRING)->GetStringDataSTL());
}

// The accessors used to assert on an empty tag or an unexpected type, so in a build without
// NDEBUG one pre-auth packet aborted the daemon. Both cases now read as the empty value.
TEST(ECTagLength, EmptyOrUnexpectedTypeReadsAsEmpty)
{
	std::unique_ptr<const CECPacket> reply = Receive(OneTag(EC_TAGTYPE_UINT64, {}));
	ASSERT_TRUE(reply != nullptr);
	ASSERT_EQUALS(0u, reply->GetTagByName(EC_TAG_STRING)->GetInt());

	reply = Receive(OneTag(EC_TAGTYPE_STRING, { 'x', 0 }));
	ASSERT_TRUE(reply != nullptr);
	const CECTag *tag = reply->GetTagByName(EC_TAG_STRING);
	ASSERT_EQUALS(0u, tag->GetInt());
	ASSERT_TRUE(tag->GetMD4Data().IsEmpty());
	ASSERT_EQUALS(0u, tag->GetIPv4Data().IP());
	ASSERT_EQUALS(0.0, tag->GetDoubleData());
	uint8_t byte = 7;
	ASSERT_TRUE(reply->AssignIfExist(EC_TAG_STRING, byte));
	ASSERT_EQUALS(0, byte);

	reply = Receive(OneTag(EC_TAGTYPE_UINT8, { 1 }));
	ASSERT_TRUE(reply != nullptr);
	ASSERT_EQUALS(std::string(), reply->GetTagByName(EC_TAG_STRING)->GetStringDataSTL());
}
