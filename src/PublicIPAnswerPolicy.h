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

#ifndef PUBLIC_IP_ANSWER_POLICY_H
#define PUBLIC_IP_ANSWER_POLICY_H

#include "NetworkAddress.h"
#include "NetworkFunctions.h" // Needed for IsLowID
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * Our public IPv4 as peers report it in OP_PUBLICIP_ANSWER (0x98).
 *
 * Only a client with no IP from its ID, its server or Kad asks. One peer must not decide the
 * answer: once our IP is set we stop asking, and a wrong one stays in our UDP obfuscation keys
 * until we leave the server.
 *
 * Votes are keyed on the /16 of the socket the answer came on, never on anything the peer
 * declares, and each /16 holds only its latest answer. A disagreeing answer is a vote of its own:
 * one wrong peer can neither set our IP nor stop two others that agree.
 *
 * A peer at a non-public address is on our side of the NAT and sees our LAN address, so it does
 * not vote. Nor does a non-public or LowID-shaped value.
 */

//! How many answering /16s must agree.
constexpr std::size_t PUBLIC_IP_ANSWER_QUORUM = 2;

//! How many /16s are remembered. Above the quorum, so a few disagreeing answers do not push out an
//! agreeing one; when full, the oldest vote goes.
constexpr std::size_t PUBLIC_IP_ANSWER_MAX_VOTES = 8;

class CPublicIPAnswerQuorum
{
public:
	//! Both IPs are in ed2k order. Returns the IP to adopt, or 0 while there is no quorum.
	std::uint32_t Add(std::uint32_t answerer, std::uint32_t claimed)
	{
		if (!CountsAsVote(answerer, claimed)) {
			return 0;
		}
		const std::uint16_t net = NetworkOf(answerer);
		m_votes.erase(std::remove_if(m_votes.begin(),
				      m_votes.end(),
				      [net](const SVote &vote) { return vote.net == net; }),
			m_votes.end());
		if (m_votes.size() >= PUBLIC_IP_ANSWER_MAX_VOTES) {
			m_votes.erase(m_votes.begin());
		}
		m_votes.push_back({ net, claimed });

		const auto agreeing = std::count_if(m_votes.begin(),
			m_votes.end(),
			[claimed](const SVote &vote) { return vote.claimed == claimed; });
		if (static_cast<std::size_t>(agreeing) < PUBLIC_IP_ANSWER_QUORUM) {
			return 0;
		}
		m_votes.clear();
		return claimed;
	}

	void Reset() noexcept { m_votes.clear(); }

	std::size_t VoteCount() const noexcept { return m_votes.size(); }

	static bool CountsAsVote(std::uint32_t answerer, std::uint32_t claimed)
	{
		return CNetworkAddress::FromIPv4NetworkOrder(answerer).IsGloballyRoutableIPv4() &&
		       !IsLowID(claimed) &&
		       CNetworkAddress::FromIPv4NetworkOrder(claimed).IsGloballyRoutableIPv4();
	}

private:
	struct SVote
	{
		std::uint16_t net;
		std::uint32_t claimed;
	};

	//! Ed2k order keeps the first two octets in the low half.
	static std::uint16_t NetworkOf(std::uint32_t ip) { return static_cast<std::uint16_t>(ip & 0xFFFFu); }

	std::vector<SVote> m_votes; // Oldest first
};

//! The votes for this session. Function-local so this header and its tests stay clear of the app.
inline CPublicIPAnswerQuorum &PublicIPAnswerVotes()
{
	static CPublicIPAnswerQuorum instance;
	return instance;
}

#endif // PUBLIC_IP_ANSWER_POLICY_H
