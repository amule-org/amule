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
#include <common/MuleDebug.h>
#include <array>
#include <limits>
#include <memory>

using namespace muleunit;

namespace
{

class OversizedMemFile : public CMemFile
{
public:
	uint64 GetLength() const override { return uint64{ 1 } << 32; }
};

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

TEST(Packet, RejectsUnrepresentablePayloadBeforeAllocating)
{
	ASSERT_RAISES(CInvalidParamsEx, CPacket(int8{ 0 }, std::numeric_limits<uint32>::max(), OP_EMULEPROT));
	OversizedMemFile file;
	ASSERT_RAISES(CInvalidParamsEx, CPacket(file, OP_EMULEPROT, 0));
	ASSERT_TRUE(file.GetPosition() == 0);
}

TEST(Packet, NumericConstructorRejectsWideLengthsBeforeNarrowing)
{
	uint64 length = std::numeric_limits<uint32>::max();
	++length; // formerly truncated to zero at the constructor boundary
	ASSERT_RAISES(CInvalidParamsEx, CPacket(int8{ 0 }, length, OP_EMULEPROT));
}

TEST(Packet, SplitConstructorReleasesBufferOnInvalidLength)
{
	uint64 length = std::numeric_limits<uint32>::max();
	length += 7; // formerly truncated to a valid six-byte split frame
	ASSERT_RAISES(CInvalidParamsEx, CPacket(new uint8_t[6], length, true));
}

TEST(Packet, RejectsHeaderLengthUnderflow)
{
	uint8_t header[6] = { OP_EMULEPROT, 0, 0, 0, 0, 0 };
	ASSERT_RAISES(CInvalidPacket, CPacket(header, nullptr));
	ASSERT_RAISES(CInvalidParamsEx, CPacket(new uint8_t[5], 5, true));
}

TEST(Packet, HeaderValidationDistinguishesEmptyAndInvalidPackets)
{
	for (uint32 length : { 0u, 0x7ffffff0u, 0xffffffffu }) {
		uint8_t header[6] = { OP_EMULEPROT,
			static_cast<uint8_t>(length),
			static_cast<uint8_t>(length >> 8),
			static_cast<uint8_t>(length >> 16),
			static_cast<uint8_t>(length >> 24),
			0 };
		ASSERT_TRUE(CPacket::GetPacketSizeFromHeader(header) == CPacket::InvalidPacketSize);
		ASSERT_RAISES(CInvalidPacket, CPacket(header, nullptr));
	}
	uint8_t empty[6] = { OP_EMULEPROT, 1, 0, 0, 0, 0 };
	ASSERT_TRUE(CPacket::GetPacketSizeFromHeader(empty) == 0);
	CPacket packet(empty, nullptr);
	ASSERT_TRUE(packet.GetPacketSize() == 0);
}

TEST(Packet, CopyBoundsRejectUnsignedWrapAndOneByteOverrun)
{
	// Exercise wxCHECK_RET returning after diagnostics, as in a release send path.
	CAssertOff assertionsOff;
	CPacket packet(int8{ 0 }, 4, OP_EMULEPROT);
	const std::array<uint8_t, 4> bytes = { 1, 2, 3, 4 };
	packet.CopyToDataBuffer(0, bytes.data(), bytes.size());
	packet.CopyToDataBuffer(4, bytes.data(), 1);
	packet.CopyToDataBuffer(std::numeric_limits<unsigned>::max(), bytes.data(), 2);
	packet.CopyToDataBuffer(1, bytes.data(), std::numeric_limits<unsigned>::max());
	for (size_t i = 0; i < bytes.size(); ++i) {
		ASSERT_TRUE(packet.GetDataBuffer()[i] == bytes[i]);
	}
	packet.CopyToDataBuffer(4, nullptr, 0); // empty span at the valid endpoint
}

TEST(Packet, FixedSizeCopiesRejectShortPayloads)
{
	// Exercise wxCHECK_RET returning after diagnostics, as in a release send path.
	CAssertOff assertionsOff;
	CPacket packet(int8{ 0 }, 3, OP_EMULEPROT);
	const std::array<uint8_t, 3> original = { 7, 8, 9 };
	packet.CopyToDataBuffer(0, original.data(), original.size());
	packet.CopyUInt32ToDataBuffer(0);
	const std::array<uint8_t, 16> hash{};
	packet.Copy16ToDataBuffer(hash.data());
	for (size_t i = 0; i < original.size(); ++i) {
		ASSERT_TRUE(packet.GetDataBuffer()[i] == original[i]);
	}
	CPacket full(int8{ 0 }, 4, OP_EMULEPROT);
	full.CopyUInt32ToDataBuffer(0x04030201);
	ASSERT_TRUE(full.GetDataBuffer()[0] == 1);
	ASSERT_TRUE(full.GetDataBuffer()[3] == 4);
	full.CopyUInt32ToDataBuffer(0, 1);
	ASSERT_TRUE(full.GetDataBuffer()[0] == 1);
	ASSERT_TRUE(full.GetDataBuffer()[3] == 4);
}

TEST(Packet, ValidPacketCopyAndDetachKeepTheSerializedBytes)
{
	CMemFile file;
	file.WriteUInt32(0x04030201);
	file.Seek(2, wxFromStart);
	CPacket packet(file, OP_EMULEPROT, 0x53);
	ASSERT_TRUE(file.GetPosition() == 2);
	CPacket copy(packet);
	std::unique_ptr<uint8_t[]> serialized(copy.DetachPacket());
	ASSERT_TRUE(serialized[0] == OP_EMULEPROT);
	ASSERT_TRUE(serialized[1] == 5); // four payload bytes plus the opcode
	ASSERT_TRUE(serialized[5] == 0x53);
	ASSERT_TRUE(serialized[6] == 1);
	ASSERT_TRUE(serialized[9] == 4);
	ASSERT_TRUE(Payload(packet) == std::string("\x01\x02\x03\x04", 4));
}

TEST(Packet, ZeroDecompressionLimitLeavesPackedPacketIntact)
{
	std::unique_ptr<CPacket> packet(Received(std::string(100, 'A')));
	const std::string before = Payload(*packet);
	ASSERT_FALSE(packet->UnPackPacket(0));
	ASSERT_TRUE(packet->GetProtocol() == OP_PACKEDPROT);
	ASSERT_TRUE(Payload(*packet) == before);
	ASSERT_TRUE(packet->UnPackPacket(100));
	ASSERT_TRUE(Payload(*packet) == std::string(100, 'A'));
}

TEST(Packet, MemoryFilePositionBeyond32BitsIsPreserved)
{
	CMemFile file;
	file.WriteUInt32(0x04030201);
	const sint64 position = (sint64{ 1 } << 32) + 2;
	// CMemFile keeps its position in a size_t, so a 32-bit host cannot seek past 4 GiB.
	if (std::numeric_limits<size_t>::max() < static_cast<uint64>(position)) {
		return;
	}
	file.Seek(position, wxFromStart);
	CPacket packet(file, OP_EMULEPROT, 0x53);
	ASSERT_TRUE(file.GetPosition() == static_cast<uint64>(position));
	ASSERT_TRUE(Payload(packet) == std::string("\x01\x02\x03\x04", 4));
}
