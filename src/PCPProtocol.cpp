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

#include "PCPProtocol.h"

#include <algorithm>

namespace pcp
{
namespace
{

constexpr std::size_t HeaderSize = 24;
constexpr std::size_t MapSize = 36;
constexpr std::uint8_t TcpProtocol = 6;
constexpr std::uint8_t PreferFailureOption = 2;

void Put16(std::vector<std::uint8_t> &out, std::uint16_t value)
{
	out.push_back(static_cast<std::uint8_t>(value >> 8));
	out.push_back(static_cast<std::uint8_t>(value));
}

void Put32(std::vector<std::uint8_t> &out, std::uint32_t value)
{
	out.push_back(static_cast<std::uint8_t>(value >> 24));
	out.push_back(static_cast<std::uint8_t>(value >> 16));
	out.push_back(static_cast<std::uint8_t>(value >> 8));
	out.push_back(static_cast<std::uint8_t>(value));
}

std::uint16_t Get16(const std::vector<std::uint8_t> &in, std::size_t offset)
{
	return static_cast<std::uint16_t>((static_cast<unsigned>(in[offset]) << 8) | in[offset + 1]);
}

bool IsZero(const std::array<std::uint8_t, 16> &address)
{
	return std::all_of(address.begin(), address.end(), [](std::uint8_t byte) { return byte == 0; });
}

bool IsIPv4Mapped(const std::array<std::uint8_t, 16> &address)
{
	return std::all_of(
		       address.begin(), address.begin() + 10, [](std::uint8_t byte) { return byte == 0; }) &&
	       address[10] == 0xff && address[11] == 0xff;
}

bool ValidOptions(
	const std::vector<std::uint8_t> &packet, std::size_t offset, const Request &request, bool response)
{
	bool sawPreferFailure = false;
	while (offset < packet.size()) {
		if (packet.size() - offset < 4)
			return false;
		const auto code = packet[offset];
		const auto length = Get16(packet, offset + 2);
		const auto paddedLength = (static_cast<std::size_t>(length) + 3) & ~std::size_t{ 3 };
		if (paddedLength > packet.size() - offset - 4)
			return false;
		if (code == PreferFailureOption) {
			if (length != 0 || sawPreferFailure || (response && !request.preferFailure))
				return false;
			sawPreferFailure = true;
		} else if ((code & 0x80) == 0) {
			return false;
		}
		offset += 4 + paddedLength;
	}
	return true;
}

} // namespace

std::uint32_t ReadNetwork32(const std::vector<std::uint8_t> &packet, std::size_t offset)
{
	if (offset > packet.size() || packet.size() - offset < 4)
		return 0;
	return (static_cast<std::uint32_t>(packet[offset]) << 24) |
	       (static_cast<std::uint32_t>(packet[offset + 1]) << 16) |
	       (static_cast<std::uint32_t>(packet[offset + 2]) << 8) | packet[offset + 3];
}

std::optional<std::vector<std::uint8_t>> EncodeMapRequest(const Request &request)
{
	if (request.internalPort == 0 || (request.preferFailure && request.suggestedExternalPort == 0) ||
		IsIPv4Mapped(request.clientAddress))
		return std::nullopt;
	std::vector<std::uint8_t> packet;
	packet.reserve(HeaderSize + MapSize + (request.preferFailure ? 4 : 0));
	packet.insert(packet.end(), { 2, 1, 0, 0 });
	Put32(packet, request.lifetime);
	packet.insert(packet.end(), request.clientAddress.begin(), request.clientAddress.end());
	packet.insert(packet.end(), request.nonce.begin(), request.nonce.end());
	packet.push_back(TcpProtocol);
	packet.insert(packet.end(), 3, 0);
	Put16(packet, request.internalPort);
	Put16(packet, request.suggestedExternalPort);
	packet.insert(packet.end(),
		request.suggestedExternalAddress.begin(),
		request.suggestedExternalAddress.end());
	if (request.preferFailure)
		packet.insert(packet.end(), { PreferFailureOption, 0, 0, 0 });
	return packet;
}

std::optional<Response> DecodeMapResponse(const std::vector<std::uint8_t> &packet, const Request &request)
{
	if (packet.size() < HeaderSize + MapSize || packet[0] != 2 || packet[1] != 0x81 ||
		!std::equal(request.nonce.begin(), request.nonce.end(), packet.begin() + HeaderSize) ||
		packet[HeaderSize + 12] != TcpProtocol ||
		Get16(packet, HeaderSize + 16) != request.internalPort ||
		!ValidOptions(packet, HeaderSize + MapSize, request, true))
		return std::nullopt;

	Response response;
	response.result = packet[3];
	response.lifetime = ReadNetwork32(packet, 4);
	response.epoch = ReadNetwork32(packet, 8);
	if (response.result == 0) {
		Mapping mapping;
		mapping.lifetime = ReadNetwork32(packet, 4);
		mapping.externalPort = Get16(packet, HeaderSize + 18);
		std::copy_n(packet.begin() + HeaderSize + 20, 16, mapping.externalAddress.begin());
		if (request.preferFailure &&
			(mapping.externalPort != request.suggestedExternalPort ||
				mapping.externalAddress != request.suggestedExternalAddress))
			return std::nullopt;
		response.mapping = mapping;
	}
	return response;
}

} // namespace pcp
