// This file is part of the aMule Project.
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#include <muleunit/test.h>
#include "SourceExchangePacket.h"
#include "MD4Hash.h"
#include <algorithm>
#include <array>
#include <memory>
#include <vector>

using namespace muleunit;

namespace
{
const std::array<uint8, 16> FileHash = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
const std::array<uint8, 16> UserHash = { 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };

void AddSource(CSourceExchangePacket &builder)
{
	builder.AddSource(0x12345678, 0x9abc, 0x87654321, 0xdef0, CMD4Hash(UserHash.data()), 0x07);
}

// Outgoing packets own a complete send buffer; unpacking is a receive-side operation.
std::unique_ptr<CPacket> Receive(CPacket &sent)
{
	auto data = std::make_unique<uint8[]>(sent.GetPacketSize());
	std::copy_n(sent.GetDataBuffer(), sent.GetPacketSize(), data.get());
	return std::make_unique<CPacket>(sent.GetHeader(), data.release());
}

// Fixed wire bytes: ID, port, server address, server port, hash and crypt flags. Each version
// below consumes an explicit prefix length, so field order and version boundaries are checked.
const std::vector<uint8> Source = { 0x78,
	0x56,
	0x34,
	0x12,
	0xbc,
	0x9a,
	0x21,
	0x43,
	0x65,
	0x87,
	0xf0,
	0xde,
	16,
	17,
	18,
	19,
	20,
	21,
	22,
	23,
	24,
	25,
	26,
	27,
	28,
	29,
	30,
	31,
	0x07 };

void CheckReply(bool supportsSX2,
	uint8 sx1Version,
	uint8 requestedVersion,
	uint8 expectedVersion,
	bool expectedSX2,
	size_t sourceSize)
{
	CSourceExchangePacket builder(
		CMD4Hash(FileHash.data()), supportsSX2, sx1Version, requestedVersion, 0xffff);
	ASSERT_EQUALS(expectedVersion, builder.GetVersion());
	AddSource(builder);
	std::unique_ptr<CPacket> packet(builder.CreatePacket());
	ASSERT_TRUE(packet != nullptr);
	ASSERT_EQUALS(static_cast<uint8>(OP_EMULEPROT), packet->GetProtocol());
	ASSERT_EQUALS(
		static_cast<uint8>(expectedSX2 ? OP_ANSWERSOURCES2 : OP_ANSWERSOURCES), packet->GetOpCode());

	std::vector<uint8> expected;
	if (expectedSX2) {
		expected.push_back(expectedVersion);
	}
	expected.insert(expected.end(), FileHash.begin(), FileHash.end());
	expected.push_back(1);
	expected.push_back(0);
	expected.insert(expected.end(), Source.data(), Source.data() + sourceSize);
	const std::vector<uint8> actual(
		packet->GetDataBuffer(), packet->GetDataBuffer() + packet->GetPacketSize());
	ASSERT_TRUE(expected == actual);
}
} // namespace

DECLARE_SIMPLE(SourceExchangePacket)

TEST(SourceExchangePacket, LegacyVersionsKeepTheirWireLayout)
{
	CheckReply(false, 1, 0, 1, false, 12);
	CheckReply(false, 2, 0, 2, false, 28);
	CheckReply(false, 3, 0, 3, false, 28);
	CheckReply(false, 4, 0, 4, false, 29);
}

TEST(SourceExchangePacket, NegotiatesSX2WithoutUsingTheSX1Version)
{
	CheckReply(true, 4, 1, 1, true, 12);
	CheckReply(true, 1, 2, 2, true, 28);
	CheckReply(true, 1, 3, 3, true, 28);
	CheckReply(true, 1, 4, 4, true, 29);
	CheckReply(true, 1, 255, 4, true, 29);
}

TEST(SourceExchangePacket, UsesSX1WhenSX2IsUnavailableOrNotRequested)
{
	CheckReply(false, 2, 4, 2, false, 28);
	CheckReply(true, 3, 0, 3, false, 28);
}

TEST(SourceExchangePacket, DoesNotSendEmptyReplies)
{
	CSourceExchangePacket sx1(CMD4Hash(), false, 2, 0, 0);
	CSourceExchangePacket sx2(CMD4Hash(), true, 2, 4, 0);
	ASSERT_TRUE(sx1.CreatePacket() == nullptr);
	ASSERT_TRUE(sx2.CreatePacket() == nullptr);
}

TEST(SourceExchangePacket, PreservesThe501SourceLimitAndCountOffset)
{
	for (const bool sx2 : { false, true }) {
		CSourceExchangePacket builder(CMD4Hash(FileHash.data()), sx2, 4, 4, 0);
		for (unsigned i = 0; i < 501; ++i) {
			ASSERT_FALSE(builder.IsFull());
			AddSource(builder);
		}
		ASSERT_TRUE(builder.IsFull());
		std::unique_ptr<CPacket> packet(builder.CreatePacket());
		ASSERT_EQUALS(static_cast<uint8>(OP_PACKEDPROT), packet->GetProtocol());
		packet = Receive(*packet);
		ASSERT_TRUE(packet->UnPackPacket());
		CMemFile data(packet->GetDataBuffer(), packet->GetPacketSize());
		if (sx2) {
			ASSERT_EQUALS(uint8{ 4 }, data.ReadUInt8());
		}
		ASSERT_TRUE(data.ReadHash() == CMD4Hash(FileHash.data()));
		ASSERT_EQUALS(uint16{ 501 }, data.ReadUInt16());
		ASSERT_EQUALS(sint64{ 501 * 29 }, data.GetAvailable());
	}
}

TEST(SourceExchangePacket, CompressesOnlyAbove354Bytes)
{
	// SX1 v1: 18 + 28*12 = 354 bytes. SX2 adds one version byte, crossing the threshold.
	for (const bool sx2 : { false, true }) {
		CSourceExchangePacket builder(CMD4Hash(), sx2, 1, 1, 0);
		for (unsigned i = 0; i < 28; ++i) {
			AddSource(builder);
		}
		std::unique_ptr<CPacket> packet(builder.CreatePacket());
		ASSERT_EQUALS(static_cast<uint8>(sx2 ? OP_PACKEDPROT : OP_EMULEPROT), packet->GetProtocol());
		if (sx2) {
			packet = Receive(*packet);
			ASSERT_TRUE(packet->UnPackPacket());
		}
		ASSERT_EQUALS(sx2 ? uint32{ 355 } : uint32{ 354 }, packet->GetPacketSize());
	}
}
