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
#include "Packet.h"
#include "MemFile.h"
#include <protocol/Protocols.h>
#include <string>
#include <zlib.h>

using namespace muleunit;

namespace
{

// A received OP_PACKEDPROT packet whose payload inflates to @a data, built the way the socket
// builds one: a header plus a payload buffer the packet takes ownership of.
CPacket *Received(const std::string &data)
{
	uLongf packedSize = compressBound(data.size());
	uint8_t *packed = new uint8_t[packedSize];
	compress2(packed,
		&packedSize,
		reinterpret_cast<const Bytef *>(data.data()),
		data.size(),
		Z_BEST_COMPRESSION);

	const uint32_t length = packedSize + 1; // the opcode byte counts
	uint8_t header[6] = { OP_PACKEDPROT,
		static_cast<uint8_t>(length),
		static_cast<uint8_t>(length >> 8),
		static_cast<uint8_t>(length >> 16),
		static_cast<uint8_t>(length >> 24),
		0 };
	return new CPacket(header, packed);
}

std::string Payload(const CPacket &packet)
{
	return std::string(reinterpret_cast<const char *>(packet.GetDataBuffer()), packet.GetPacketSize());
}

// Packs to a few times smaller, like a real file list.
std::string FileList(size_t n)
{
	std::string data;
	for (unsigned i = 0; data.size() < n; ++i) {
		data += "file_" + std::to_string(i * 7919 % 100000) + ".mp3|";
	}
	data.resize(n);
	return data;
}

} // namespace

DECLARE_SIMPLE(Packet)

TEST(Packet, UnpacksWithinTheFirstEstimate)
{
	const std::string data = FileList(20000);
	CPacket *packet = Received(data);
	ASSERT_TRUE(packet->GetPacketSize() * 10 + 300 >= data.size());

	ASSERT_TRUE(packet->UnPackPacket());
	ASSERT_EQUALS(static_cast<int>(OP_EMULEPROT), static_cast<int>(packet->GetProtocol()));
	ASSERT_TRUE(Payload(*packet) == data);
	delete packet;
}

TEST(Packet, UnpacksBeyondTheTenfoldEstimate)
{
	// Packs to well under a tenth of its size, yet fits the 50000-byte default limit.
	const std::string data(40000, 'A');
	CPacket *packet = Received(data);
	ASSERT_TRUE(packet->GetPacketSize() * 10 + 300 < data.size());

	ASSERT_TRUE(packet->UnPackPacket());
	ASSERT_TRUE(Payload(*packet) == data);
	delete packet;
}

TEST(Packet, RejectsPayloadOverTheLimit)
{
	CPacket *packet = Received(std::string(60000, 'A'));
	ASSERT_FALSE(packet->UnPackPacket());
	delete packet;
}
