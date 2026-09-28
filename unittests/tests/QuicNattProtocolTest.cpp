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
