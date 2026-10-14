//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// Any parts of this program derived from the xMule, lMule or eMule project,
// or contributed by third-party developers are copyrighted by their
// respective authors.
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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
//

#ifndef KADEMLIA_FIREWALLCHECKREQUESTS_H
#define KADEMLIA_FIREWALLCHECKREQUESTS_H

#include <cstdint>
#include <list>

namespace Kademlia
{

/**
 * The peers we asked to check our firewall, for 180 seconds each. Header-only, with the clock
 * passed in, like CFirewallRecheck.
 *
 * Each peer we asked may give each kind of reply once while its request lives. Without that, one
 * peer could repeat its answer: two replies settle our public IP, and two confirmations mark us
 * reachable.
 */
class CFirewallCheckRequests
{
public:
	enum class Reply
	{
		PublicIP,  //!< KADEMLIA_FIREWALLED_RES
		Reachable, //!< KADEMLIA_FIREWALLED_ACK_RES or OP_KAD_FWTCPCHECK_ACK
	};

	static const uint64_t kLifetimeMs = 180000;

	//! A peer already asked keeps its request: asking it again must not buy it more replies.
	void Add(uint32_t ip, uint64_t now)
	{
		while (!m_requests.empty() && now - m_requests.back().sent > kLifetimeMs) {
			m_requests.pop_back();
		}
		if (!Contains(ip, now)) {
			m_requests.push_front(Request{ ip, now });
		}
	}

	bool Contains(uint32_t ip, uint64_t now) const
	{
		for (const Request &request : m_requests) {
			if (request.ip == ip && now - request.sent < kLifetimeMs) {
				return true;
			}
		}
		return false;
	}

	//! Accepts @a reply from @a ip if a live request to it has not had that reply yet.
	bool Take(uint32_t ip, Reply reply, uint64_t now)
	{
		for (Request &request : m_requests) {
			if (request.ip != ip || now - request.sent >= kLifetimeMs) {
				continue;
			}
			bool &taken =
				reply == Reply::PublicIP ? request.publicIPTaken : request.reachableTaken;
			if (!taken) {
				taken = true;
				return true;
			}
		}
		return false;
	}

private:
	struct Request
	{
		uint32_t ip;
		uint64_t sent;
		bool publicIPTaken = false;
		bool reachableTaken = false;
	};
	std::list<Request> m_requests;
};

} // namespace Kademlia

#endif // KADEMLIA_FIREWALLCHECKREQUESTS_H
// File_checked_for_headers
