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

#include "NatServerHolePunch.h"

#include <algorithm>

using namespace muleunit;
using namespace NatServerHolePunch;

DECLARE_SIMPLE(NatServerHolePunch)

TEST(NatServerHolePunch, BuildsExactPayloadsAndParsesFields)
{
	const Request request{ 0x12345678, 0xABCD };
	const std::array<std::uint8_t, 6> requestBytes{ { 0x78, 0x56, 0x34, 0x12, 0xCD, 0xAB } };
	ASSERT_TRUE(BuildRequest(request) == requestBytes);
	const auto parsedRequest = ParseRequest(requestBytes.data(), requestBytes.size());
	ASSERT_TRUE(parsedRequest.has_value());
	ASSERT_EQUALS(request.targetId, parsedRequest->targetId);
	ASSERT_EQUALS(request.udpPort, parsedRequest->udpPort);

	const Hash hash{ { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 } };
	const Info info{ { { 192, 0, 2, 1 } }, 0x1236, 0x5678, hash, 0xFE };
	const std::array<std::uint8_t, 25> infoBytes{ { 192,
		0,
		2,
		1,
		0x36,
		0x12,
		0x78,
		0x56,
		0,
		1,
		2,
		3,
		4,
		5,
		6,
		7,
		8,
		9,
		10,
		11,
		12,
		13,
		14,
		15,
		0xFE } };
	ASSERT_TRUE(BuildInfo(info) == infoBytes);
	const auto parsedInfo = ParseInfo(infoBytes.data(), infoBytes.size());
	ASSERT_TRUE(parsedInfo.has_value());
	ASSERT_TRUE(parsedInfo->peerIP == info.peerIP && parsedInfo->userHash == hash);
	ASSERT_EQUALS(info.tcpPort, parsedInfo->tcpPort);
	ASSERT_EQUALS(info.udpPort, parsedInfo->udpPort);
	ASSERT_EQUALS(info.role, parsedInfo->role);

	const Failure failure{ 0x89ABCDEF, 0xFF };
	const std::array<std::uint8_t, 5> failureBytes{ { 0xEF, 0xCD, 0xAB, 0x89, 0xFF } };
	ASSERT_TRUE(BuildFailure(failure) == failureBytes);
	const auto parsedFailure = ParseFailure(failureBytes.data(), failureBytes.size());
	ASSERT_TRUE(parsedFailure.has_value());
	ASSERT_EQUALS(failure.targetId, parsedFailure->targetId);
	ASSERT_EQUALS(failure.reason, parsedFailure->reason);
	ASSERT_TRUE(BuildKeepalive(hash) == hash);
	ASSERT_TRUE(ParseKeepalive(hash.data(), hash.size()).value() == hash);
}

TEST(NatServerHolePunch, AcceptsMinimumPayloadAndIgnoresTrailingBytes)
{
	const Request request{ 1, 2 };
	const auto requestBytes = BuildRequest(request);
	std::array<std::uint8_t, 7> requestExtended{};
	std::copy(requestBytes.begin(), requestBytes.end(), requestExtended.begin());
	ASSERT_TRUE(ParseRequest(requestExtended.data(), requestExtended.size()).has_value());
	ASSERT_FALSE(ParseRequest(requestBytes.data(), requestBytes.size() - 1).has_value());
	ASSERT_FALSE(ParseRequest(nullptr, requestBytes.size()).has_value());

	const Info info{ { { 192, 0, 2, 1 } }, 3, 4, {}, 0 };
	const auto infoBytes = BuildInfo(info);
	std::array<std::uint8_t, 26> infoExtended{};
	std::copy(infoBytes.begin(), infoBytes.end(), infoExtended.begin());
	ASSERT_TRUE(ParseInfo(infoExtended.data(), infoExtended.size()).has_value());
	ASSERT_FALSE(ParseInfo(infoBytes.data(), infoBytes.size() - 1).has_value());
	ASSERT_FALSE(ParseInfo(nullptr, infoBytes.size()).has_value());

	const auto failureBytes = BuildFailure({ 5, 0xA7 });
	std::array<std::uint8_t, 6> failureExtended{};
	std::copy(failureBytes.begin(), failureBytes.end(), failureExtended.begin());
	ASSERT_TRUE(ParseFailure(failureExtended.data(), failureExtended.size()).has_value());
	ASSERT_FALSE(ParseFailure(failureBytes.data(), failureBytes.size() - 1).has_value());
	ASSERT_FALSE(ParseFailure(nullptr, failureBytes.size()).has_value());

	const Hash hash{ { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 } };
	std::array<std::uint8_t, 17> keepaliveExtended{};
	std::copy(hash.begin(), hash.end(), keepaliveExtended.begin());
	ASSERT_TRUE(ParseKeepalive(keepaliveExtended.data(), keepaliveExtended.size()).has_value());
	ASSERT_FALSE(ParseKeepalive(hash.data(), hash.size() - 1).has_value());
	ASSERT_FALSE(ParseKeepalive(nullptr, hash.size()).has_value());
}

TEST(NatServerHolePunch, PreservesUnknownReasonsAndOpcodeNamespaces)
{
	for (unsigned reason = 0; reason < 256; ++reason) {
		const auto bytes = BuildFailure({ 0, static_cast<std::uint8_t>(reason) });
		const auto parsed = ParseFailure(bytes.data(), bytes.size());
		ASSERT_TRUE(parsed.has_value());
		ASSERT_EQUALS(reason, parsed->reason);
	}
	ASSERT_TRUE(IsServerTcpOpcode(0x60));
	ASSERT_TRUE(IsServerUdpOpcode(0x9F));
	ASSERT_TRUE(IsPeerUdpOpcode(0xB3));
	ASSERT_FALSE(IsServerUdpOpcode(0xE3));
	ASSERT_FALSE(IsServerTcpOpcode(0xB3));
}
