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

#include <muleunit/test.h>

#include <QuicNattProtocol.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

using namespace muleunit;
using namespace QuicNatt;

DECLARE_SIMPLE(QuicNattProtocol)

namespace
{
std::array<uint8_t, 16> Hash(uint8_t first)
{
	std::array<uint8_t, 16> hash{};
	for (size_t i = 0; i < hash.size(); ++i) {
		hash[i] = static_cast<uint8_t>(first + i);
	}
	return hash;
}
} // namespace

TEST(QuicNattProtocol, EaqcCodecMatchesWireFixture)
{
	const std::array<uint8_t, 16> sender{ { 0x10,
		0x11,
		0x12,
		0x13,
		0x14,
		0x15,
		0x16,
		0x17,
		0x18,
		0x19,
		0x1A,
		0x1B,
		0x1C,
		0x1D,
		0x1E,
		0x1F } };
	const std::array<uint8_t, 16> expected{ { 0x20,
		0x21,
		0x22,
		0x23,
		0x24,
		0x25,
		0x26,
		0x27,
		0x28,
		0x29,
		0x2A,
		0x2B,
		0x2C,
		0x2D,
		0x2E,
		0x2F } };
	const std::array<uint8_t, 16> file{ { 0x30,
		0x31,
		0x32,
		0x33,
		0x34,
		0x35,
		0x36,
		0x37,
		0x38,
		0x39,
		0x3A,
		0x3B,
		0x3C,
		0x3D,
		0x3E,
		0x3F } };
	const std::array<uint8_t, EAQC_FRAME_SIZE> fixture{ { 0x45,
		0x41,
		0x51,
		0x43,
		0x01,
		0x40,
		0x10,
		0x11,
		0x12,
		0x13,
		0x14,
		0x15,
		0x16,
		0x17,
		0x18,
		0x19,
		0x1A,
		0x1B,
		0x1C,
		0x1D,
		0x1E,
		0x1F,
		0x20,
		0x21,
		0x22,
		0x23,
		0x24,
		0x25,
		0x26,
		0x27,
		0x28,
		0x29,
		0x2A,
		0x2B,
		0x2C,
		0x2D,
		0x2E,
		0x2F,
		0x30,
		0x31,
		0x32,
		0x33,
		0x34,
		0x35,
		0x36,
		0x37,
		0x38,
		0x39,
		0x3A,
		0x3B,
		0x3C,
		0x3D,
		0x3E,
		0x3F } };
	const auto encoded = EncodeEaqcFrame(0x40, sender, expected, file);
	ASSERT_TRUE(encoded == fixture);
	EaqcFrame decoded{};
	ASSERT_TRUE(DecodeEaqcFrame(fixture.data(), fixture.size(), expected, decoded));
	ASSERT_EQUALS(0x40, decoded.options);
	ASSERT_TRUE(decoded.senderUserHash == sender);
	ASSERT_TRUE(decoded.expectedPeerHash == expected);
	ASSERT_TRUE(decoded.fileHash == file);
}

TEST(QuicNattProtocol, EaqcCapsAckEchoesRequestAndAdvertisesQuic)
{
	const auto local = Hash(1);
	const auto sender = Hash(33);
	const auto file = Hash(65);
	EaqcFrame request{ 0, sender, local, file };
	const auto ack = BuildEaqcCapsAck(request, local, true, false);
	EaqcFrame decoded{};
	ASSERT_TRUE(DecodeEaqcFrame(ack.data(), ack.size(), sender, decoded));
	ASSERT_EQUALS(0x48, ack[5]);
	ASSERT_TRUE(decoded.senderUserHash == local);
	ASSERT_TRUE(decoded.expectedPeerHash == sender);
	ASSERT_TRUE(decoded.fileHash == file);
	const auto noCallback = BuildEaqcCapsAck(request, local, false, false);
	ASSERT_EQUALS(0x40, noCallback[5]);
	const auto withUtp = BuildEaqcCapsAck(request, local, false, true);
	ASSERT_EQUALS(0xC0, withUtp[5]);
	const auto everything = BuildEaqcCapsAck(request, local, true, true);
	ASSERT_EQUALS(0xC8, everything[5]);
}

TEST(QuicNattProtocol, EaqcCodecValidatesLengthAndIdentity)
{
	const auto local = Hash(1);
	const auto sender = Hash(33);
	const auto file = Hash(65);
	const auto wildcard = std::array<uint8_t, 16>{};
	const auto frame = EncodeEaqcFrame(0x40, sender, wildcard, file);
	EaqcFrame decoded{};
	ASSERT_TRUE(DecodeEaqcFrame(frame.data(), frame.size(), local, decoded));
	ASSERT_FALSE(DecodeEaqcFrame(frame.data(), frame.size() - 1, local, decoded));
	std::vector<uint8_t> longFrame(frame.begin(), frame.end());
	longFrame.push_back(0);
	ASSERT_FALSE(DecodeEaqcFrame(longFrame.data(), longFrame.size(), local, decoded));
	auto bad = frame;
	bad[0] = 0;
	ASSERT_FALSE(DecodeEaqcFrame(bad.data(), bad.size(), local, decoded));
	bad = frame;
	bad[4] = 2;
	ASSERT_FALSE(DecodeEaqcFrame(bad.data(), bad.size(), local, decoded));
	const auto mismatch = EncodeEaqcFrame(0, sender, Hash(90), file);
	ASSERT_FALSE(DecodeEaqcFrame(mismatch.data(), mismatch.size(), local, decoded));
}

TEST(QuicNattProtocol, AlpnRequiresExactBytesAndLength)
{
	const char valid[] = "ed2k-ai-natt-quic-v1";
	ASSERT_TRUE(IsQuicNattAlpn(reinterpret_cast<const uint8_t *>(valid), sizeof(valid) - 1));
	ASSERT_FALSE(IsQuicNattAlpn(reinterpret_cast<const uint8_t *>("ed2k-ai-natt-quic-v2"), 20));
	ASSERT_FALSE(IsQuicNattAlpn(reinterpret_cast<const uint8_t *>("ed2k-ai-natt-quic-v1x"), 21));
	ASSERT_FALSE(IsQuicNattAlpn(nullptr, 0));
}

TEST(QuicNattProtocol, ProofConstructionUsesOrderedHashesAndZeroUnknownTarget)
{
	const auto local = Hash(1);
	const auto peer = Hash(33);
	const auto proof = BuildEaqn1Proof(local, &peer);
	ASSERT_EQUALS(37u, proof.size());
	ASSERT_EQUALS(0x45, proof[0]);
	ASSERT_EQUALS(0x41, proof[1]);
	ASSERT_EQUALS(0x51, proof[2]);
	ASSERT_EQUALS(0x4E, proof[3]);
	ASSERT_EQUALS(0x31, proof[4]);
	ASSERT_EQUALS(0, std::memcmp(proof.data() + 5, local.data(), local.size()));
	ASSERT_EQUALS(0, std::memcmp(proof.data() + 21, peer.data(), peer.size()));

	const auto unknown = BuildEaqn1Proof(local, nullptr);
	for (size_t i = 21; i < unknown.size(); ++i) {
		ASSERT_EQUALS(0, (int)unknown[i]);
	}
}

TEST(QuicNattProtocol, ValidationRequiresCompleteMagicAndAcceptsLocalOrZeroTarget)
{
	const auto local = Hash(1);
	const auto peer = Hash(33);
	const auto valid = BuildEaqn1Proof(peer, &local);
	ASSERT_TRUE(ValidateEaqn1Proof(valid.data(), valid.size(), local, &peer));
	const auto unknownTarget = BuildEaqn1Proof(peer, nullptr);
	ASSERT_TRUE(ValidateEaqn1Proof(unknownTarget.data(), unknownTarget.size(), local, &peer));
	ASSERT_FALSE(ValidateEaqn1Proof(nullptr, 0, local, &peer));
	ASSERT_FALSE(ValidateEaqn1Proof(valid.data(), 36, local, &peer));
	std::array<uint8_t, 37> bad = valid;
	bad[0] = 'X';
	ASSERT_FALSE(ValidateEaqn1Proof(bad.data(), bad.size(), local, &peer));
}

TEST(QuicNattProtocol, ValidationRejectsWrongTargetAndSenderMismatch)
{
	const auto local = Hash(1);
	const auto peer = Hash(33);
	const auto other = Hash(65);
	const auto wrongTarget = BuildEaqn1Proof(peer, &other);
	ASSERT_FALSE(ValidateEaqn1Proof(wrongTarget.data(), wrongTarget.size(), local, &peer));
	const auto wrongSender = BuildEaqn1Proof(other, &local);
	ASSERT_FALSE(ValidateEaqn1Proof(wrongSender.data(), wrongSender.size(), local, &peer));
}

TEST(QuicNattProtocol, DirectNatRefreshExceptionIsExplicit)
{
	const auto local = Hash(1);
	const auto expected = Hash(33);
	const auto refreshed = Hash(65);
	const auto proof = BuildEaqn1Proof(refreshed, &local);
	ASSERT_FALSE(ValidateEaqn1Proof(proof.data(), proof.size(), local, &expected));
	ASSERT_TRUE(ValidateEaqn1Proof(proof.data(), proof.size(), local, &expected, true));
}

TEST(QuicNattProtocol, UnknownExpectedPeerDoesNotInventIdentity)
{
	const auto local = Hash(1);
	const auto sender = Hash(65);
	const auto proof = BuildEaqn1Proof(sender, &local);
	ASSERT_TRUE(ValidateEaqn1Proof(proof.data(), proof.size(), local, nullptr));
}

TEST(QuicNattProtocol, ProofIsAStreamPrefix)
{
	const auto local = Hash(1);
	const auto peer = Hash(33);
	const auto proof = BuildEaqn1Proof(peer, &local);
	std::array<uint8_t, 40> stream{};
	std::copy(proof.begin(), proof.end(), stream.begin());
	stream[37] = 0xAA;
	stream[38] = 0xBB;
	stream[39] = 0xCC;
	ASSERT_TRUE(ValidateEaqn1Proof(stream.data(), stream.size(), local, &peer));
}
