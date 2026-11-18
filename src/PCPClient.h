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
#include <memory>
#include <optional>

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/udp.hpp>

namespace pcp
{

struct LeaseSnapshot
{
	bool active = false;
	std::uint16_t internalPort = 0;
	std::uint32_t grantedLifetime = 0;
	std::uint64_t generation = 0;
};

class Client
{
public:
	// The client runs on a caller-owned io_context: the PCP manager owns one event loop
	// serviced by a single thread for every client it creates.
	Client(const boost::asio::ip::udp::endpoint &gateway,
		const boost::asio::ip::address_v6 &localAddress,
		boost::asio::io_context &ioContext);
	~Client();
	Client(const Client &) = delete;
	Client &operator=(const Client &) = delete;

	// Replace the desired TCP mapping. A zero target port disables mapping.
	void SetMapping(std::uint16_t internalPort,
		const boost::asio::ip::address_v6 &externalAddress,
		std::uint16_t externalPort);
	void ClearMapping();
	LeaseSnapshot Snapshot() const;
	// False when the socket never became usable (for example bind() failed while the
	// address was still tentative); the manager recreates such a client on its due pass.
	bool Ready() const;
	void Stop();

private:
	struct State;
	std::shared_ptr<State> m_state;
};

} // namespace pcp
