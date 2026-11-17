#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace pcp
{

struct Request
{
	std::array<std::uint8_t, 12> nonce{};
	std::array<std::uint8_t, 16> clientAddress{};
	std::array<std::uint8_t, 16> suggestedExternalAddress{};
	std::uint16_t internalPort = 0;
	std::uint16_t suggestedExternalPort = 0;
	std::uint32_t lifetime = 0;
	bool preferFailure = false;
};

struct Mapping
{
	std::uint32_t lifetime = 0;
	std::uint16_t externalPort = 0;
	std::array<std::uint8_t, 16> externalAddress{};
};

struct Response
{
	std::uint8_t result = 0;
	std::uint32_t lifetime = 0;
	std::uint32_t epoch = 0;
	std::optional<Mapping> mapping;
};

std::optional<std::vector<std::uint8_t>> EncodeMapRequest(const Request &request);
std::optional<Response> DecodeMapResponse(const std::vector<std::uint8_t> &packet, const Request &request);
std::uint32_t ReadNetwork32(const std::vector<std::uint8_t> &packet, std::size_t offset);

} // namespace pcp
