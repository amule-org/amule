// This file is part of the aMule Project.
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#include <muleunit/test.h>
#include "PCPProtocol.h"

using namespace muleunit;
using namespace pcp;

DECLARE_SIMPLE(PCPProtocol)

namespace
{
Request ExampleRequest()
{
	Request request{};
	for (std::size_t i = 0; i < request.nonce.size(); ++i)
		request.nonce[i] = static_cast<std::uint8_t>(i + 1);
	request.clientAddress = { 0x20, 1, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
	request.suggestedExternalAddress = { 0x20, 1, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2 };
	request.internalPort = 4662;
	request.suggestedExternalPort = 4662;
	request.lifetime = 3600;
	request.preferFailure = true;
	return request;
}

std::vector<std::uint8_t> SuccessResponse()
{
	return { 2,
		0x81,
		0,
		0,
		0,
		0,
		0x0e,
		0x10,
		0x01,
		0x02,
		0x03,
		0x04,
		0xff,
		0xee,
		0xdd,
		0xcc,
		0xbb,
		0xaa,
		0x99,
		0x88,
		0x77,
		0x66,
		0x55,
		0x44,
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
		6,
		0xaa,
		0xbb,
		0xcc,
		0x12,
		0x36,
		0x12,
		0x36,
		0x20,
		1,
		0x0d,
		0xb8,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		2,
		2,
		0,
		0,
		0 };
}
} // namespace

TEST(PCPProtocol, EncodesRequestUsingRFCByteOffsets)
{
	const auto packet = EncodeMapRequest(ExampleRequest());
	const std::vector<std::uint8_t> expected = { 2,
		1,
		0,
		0,
		0,
		0,
		0x0e,
		0x10,
		0x20,
		1,
		0x0d,
		0xb8,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		1,
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
		6,
		0,
		0,
		0,
		0x12,
		0x36,
		0x12,
		0x36,
		0x20,
		1,
		0x0d,
		0xb8,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		2,
		2,
		0,
		0,
		0 };
	ASSERT_TRUE(packet.has_value());
	ASSERT_TRUE(expected == *packet);
}

TEST(PCPProtocol, EncodesRenewalAndDeletion)
{
	auto request = ExampleRequest();
	request.preferFailure = false;
	request.lifetime = 1200;
	auto renewal = EncodeMapRequest(request);
	ASSERT_TRUE(renewal.has_value());
	ASSERT_EQUALS(1200u, ReadNetwork32(*renewal, 4));
	request.lifetime = 0;
	auto deletion = EncodeMapRequest(request);
	ASSERT_TRUE(deletion.has_value());
	ASSERT_EQUALS(0u, ReadNetwork32(*deletion, 4));
}

TEST(PCPProtocol, DecodesResponseLifetimeEpochEndpointAndReservedFields)
{
	const auto response = DecodeMapResponse(SuccessResponse(), ExampleRequest());
	ASSERT_TRUE(response.has_value());
	ASSERT_EQUALS(0u, response->result);
	ASSERT_EQUALS(3600u, response->lifetime);
	ASSERT_EQUALS(0x01020304u, response->epoch);
	ASSERT_TRUE(response->mapping.has_value());
	ASSERT_EQUALS(3600u, response->mapping->lifetime);
	ASSERT_EQUALS(4662u, response->mapping->externalPort);
	ASSERT_EQUALS(0x20u, response->mapping->externalAddress[0]);
	ASSERT_EQUALS(2u, response->mapping->externalAddress[15]);
}

TEST(PCPProtocol, RejectsMismatchedHeaderAndMapBindingWithoutMutation)
{
	const auto request = ExampleRequest();
	const auto valid = SuccessResponse();
	for (const auto index : { 0u, 1u, 24u, 35u, 36u, 40u, 41u }) {
		auto bad = valid;
		bad[index] ^= 1;
		const auto original = bad;
		ASSERT_FALSE(DecodeMapResponse(bad, request).has_value());
		ASSERT_TRUE(original == bad);
	}
	for (std::size_t length = 0; length < valid.size(); ++length) {
		std::vector<std::uint8_t> truncated(valid.begin(), valid.begin() + length);
		if (length == 60) {
			ASSERT_TRUE(DecodeMapResponse(truncated, request).has_value());
		} else {
			ASSERT_FALSE(DecodeMapResponse(truncated, request).has_value());
		}
	}
	auto trailing = valid;
	trailing.push_back(0);
	ASSERT_FALSE(DecodeMapResponse(trailing, request).has_value());
}

TEST(PCPProtocol, ErrorHasNoMappingAndPreferFailureIsNotEchoed)
{
	auto wire = SuccessResponse();
	wire[3] = 2;
	wire[4] = 0;
	wire[5] = 0;
	wire[6] = 0;
	wire[7] = 30;
	wire.erase(wire.begin() + 60, wire.end());
	const auto response = DecodeMapResponse(wire, ExampleRequest());
	ASSERT_TRUE(response.has_value());
	ASSERT_EQUALS(2u, response->result);
	ASSERT_EQUALS(30u, response->lifetime);
	ASSERT_EQUALS(0x01020304u, response->epoch);
	ASSERT_FALSE(response->mapping.has_value());
}
