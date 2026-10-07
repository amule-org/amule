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

#ifndef QUIC_NATT_PROTOCOL_H
#define QUIC_NATT_PROTOCOL_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace QuicNatt
{

static constexpr char QUIC_NATT_ALPN[] = "ed2k-ai-natt-quic-v1";
static constexpr size_t EAQN1_PROOF_SIZE = 37;
static constexpr size_t EAQC_FRAME_SIZE = 54;

struct EaqcFrame
{
	uint8_t options = 0;
	std::array<uint8_t, 16> senderUserHash{};
	std::array<uint8_t, 16> expectedPeerHash{};
	std::array<uint8_t, 16> fileHash{};
};

inline std::array<uint8_t, EAQC_FRAME_SIZE> EncodeEaqcFrame(uint8_t options,
	const std::array<uint8_t, 16> &senderUserHash,
	const std::array<uint8_t, 16> &expectedPeerHash,
	const std::array<uint8_t, 16> &fileHash)
{
	std::array<uint8_t, EAQC_FRAME_SIZE> frame{};
	frame[0] = 0x45;
	frame[1] = 0x41;
	frame[2] = 0x51;
	frame[3] = 0x43;
	frame[4] = 1;
	frame[5] = options;
	std::copy(senderUserHash.begin(), senderUserHash.end(), frame.begin() + 6);
	std::copy(expectedPeerHash.begin(), expectedPeerHash.end(), frame.begin() + 22);
	std::copy(fileHash.begin(), fileHash.end(), frame.begin() + 38);
	return frame;
}

inline std::array<uint8_t, EAQC_FRAME_SIZE> BuildEaqcCapsAck(const EaqcFrame &request,
	const std::array<uint8_t, 16> &ourUserHash,
	bool directCallback,
	bool utpAvailable)
{
	const auto options =
		static_cast<uint8_t>(0x40 | (directCallback ? 0x08 : 0x00) | (utpAvailable ? 0x80 : 0x00));
	return EncodeEaqcFrame(options, ourUserHash, request.senderUserHash, request.fileHash);
}

inline bool DecodeEaqcFrame(
	const uint8_t *data, size_t length, const std::array<uint8_t, 16> &ourUserHash, EaqcFrame &frame)
{
	if (data == nullptr || length != EAQC_FRAME_SIZE || data[0] != 0x45 || data[1] != 0x41 ||
		data[2] != 0x51 || data[3] != 0x43 || data[4] != 1) {
		return false;
	}
	const uint8_t *expected = data + 22;
	const bool wildcard = std::all_of(expected, expected + 16, [](uint8_t byte) { return byte == 0; });
	if (!wildcard && !std::equal(expected, expected + 16, ourUserHash.begin())) {
		return false;
	}
	frame.options = data[5];
	std::copy(data + 6, data + 22, frame.senderUserHash.begin());
	std::copy(expected, expected + 16, frame.expectedPeerHash.begin());
	std::copy(data + 38, data + 54, frame.fileHash.begin());
	return true;
}

inline bool IsQuicNattAlpn(const uint8_t *value, size_t length)
{
	constexpr size_t alpnLength = sizeof(QUIC_NATT_ALPN) - 1;
	return value != nullptr && length == alpnLength &&
	       std::equal(value, value + alpnLength, QUIC_NATT_ALPN);
}

inline std::array<uint8_t, EAQN1_PROOF_SIZE> BuildEaqn1Proof(
	const std::array<uint8_t, 16> &localHash, const std::array<uint8_t, 16> *peerHash)
{
	std::array<uint8_t, EAQN1_PROOF_SIZE> proof{};
	proof[0] = 'E';
	proof[1] = 'A';
	proof[2] = 'Q';
	proof[3] = 'N';
	proof[4] = '1';
	std::copy(localHash.begin(), localHash.end(), proof.begin() + 5);
	if (peerHash != nullptr) {
		std::copy(peerHash->begin(), peerHash->end(), proof.begin() + 21);
	}
	return proof;
}

inline bool ValidateEaqn1Proof(const uint8_t *proof,
	size_t length,
	const std::array<uint8_t, 16> &localHash,
	const std::array<uint8_t, 16> *expectedPeerHash,
	bool allowDirectNatHashRefresh = false)
{
	if (proof == nullptr || length < EAQN1_PROOF_SIZE || proof[0] != 'E' || proof[1] != 'A' ||
		proof[2] != 'Q' || proof[3] != 'N' || proof[4] != '1') {
		return false;
	}

	const uint8_t *target = proof + 21;
	const bool targetIsZero = std::all_of(target, target + 16, [](uint8_t byte) { return byte == 0; });
	if (!targetIsZero && !std::equal(target, target + 16, localHash.begin())) {
		return false;
	}

	return expectedPeerHash == nullptr || allowDirectNatHashRefresh ||
	       std::equal(proof + 5, proof + 21, expectedPeerHash->begin());
}

} // namespace QuicNatt

#endif
