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
#include "PCPClient.h"
#include "PCPGateway.h"
#include <chrono>
#include <functional>
#include <memory>
#include <optional>

namespace pcp
{
class MappingClient
{
public:
	virtual ~MappingClient() = default;
	virtual void SetMapping(
		std::uint16_t port, boost::asio::ip::address_v6 address, std::uint16_t externalPort) = 0;
	virtual void Stop() = 0;
	// Current lease state of the underlying client, for callers that must know whether
	// the pinhole is currently owned by PCP.
	virtual LeaseSnapshot Snapshot() const = 0;
	// False when the underlying client never became usable (for example bind() failed);
	// the manager's due pass replaces such a client.
	virtual bool Ready() const = 0;
};

class PCPManager
{
public:
	using Discovery = std::function<std::optional<Gateway>(boost::asio::ip::address_v6)>;
	using Factory = std::function<std::unique_ptr<MappingClient>(const Gateway &)>;
	PCPManager(Discovery discovery = {},
		Factory factory = {},
		std::chrono::milliseconds retryInterval = std::chrono::seconds(30));
	~PCPManager();
	PCPManager(const PCPManager &) = delete;
	PCPManager &operator=(const PCPManager &) = delete;
	void SetTarget(const boost::asio::ip::address_v6 &address, std::uint16_t port);
	void ClearTarget();
	void Stop();
	// Whether a lease for the current target is currently held. While this returns true
	// the pinhole has exactly one owner (PCP); the IGD2 fallback must stand down.
	LeaseSnapshot LeaseState() const;

private:
	struct State;
	std::shared_ptr<State> m_state;
};
} // namespace pcp
