//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
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
// along with this program; if not, write to the Free Software Foundation,
// Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA.
// If not, see <https://www.gnu.org/licenses/>.
//

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
