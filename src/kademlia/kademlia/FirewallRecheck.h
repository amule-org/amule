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

#ifndef KADEMLIA_FIREWALLRECHECK_H
#define KADEMLIA_FIREWALLRECHECK_H

#include <protocol/kad/Constants.h> // Needed for KADEMLIAFIREWALLCHECKS

#include <cstdint>
#include <ctime>

namespace Kademlia
{

/**
 * Our TCP firewall state across a recheck. Header-only, with the clock passed in, so a test can
 * drive both orders in which the answers arrive.
 *
 * A recheck asks peers for our IP over UDP, and they confirm we are reachable by connecting back
 * over TCP, which takes longer. The IP answers alone do not end the recheck: if they did, the
 * last one arriving before the confirmations reported us firewalled for a moment every hour.
 */
class CFirewallRecheck
{
public:
	//! Peers that must reach us over TCP before we count as open.
	static const uint32_t kConfirmations = 2;
	//! How long the confirmations may take after the last IP answer.
	static const time_t kGraceSeconds = 60;

	//! Starts a recheck, keeping the current state until it ends.
	void Start(time_t now)
	{
		m_lastState = IsFirewalled(now);
		m_answers = 0;
		m_confirmations = 0;
		m_answeredAt = 0;
	}

	void AddAnswer(time_t now)
	{
		if (++m_answers == KADEMLIAFIREWALLCHECKS) {
			m_answeredAt = now;
		}
	}

	void AddConfirmation() { ++m_confirmations; }

	//! Whether more IP answers are wanted; the UDP listener asks peers while this is true.
	bool AwaitingAnswers() const { return m_answers < KADEMLIAFIREWALLCHECKS; }

	bool IsFirewalled(time_t now) const
	{
		if (m_confirmations >= kConfirmations) {
			return false;
		}
		if (AwaitingAnswers() || now < m_answeredAt + kGraceSeconds) {
			return m_lastState;
		}
		return true;
	}

private:
	uint32_t m_answers = 0;
	uint32_t m_confirmations = 0;
	time_t m_answeredAt = 0;
	bool m_lastState = true;
};

} // namespace Kademlia

#endif // KADEMLIA_FIREWALLRECHECK_H
// File_checked_for_headers
