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

#ifndef KAD_BUDDY_LIVENESS_H
#define KAD_BUDDY_LIVENESS_H

#include <cstdint>

namespace Kademlia
{

// Keep received activity independent of outgoing traffic, using a monotonic millisecond clock.
class CBuddyLiveness
{
public:
	void Reset(uint64_t now)
	{
		m_lastReceived = now;
		m_nextPing = now + PingInterval;
		m_lastIncomingPing = 0;
		m_hasIncomingPing = false;
	}

	void ReceivedPacket(uint64_t now) { m_lastReceived = now; }
	bool PingDue(uint64_t now) const { return now >= m_nextPing; }
	void PingSent(uint64_t now) { m_nextPing = now + PingInterval; }
	void PongReceived(uint64_t now)
	{
		ReceivedPacket(now);
		m_nextPing = now + PingInterval;
	}
	bool AllowIncomingPing(uint64_t now) const
	{
		return !m_hasIncomingPing || now - m_lastIncomingPing >= IncomingPingInterval;
	}
	void PingReceived(uint64_t now)
	{
		m_lastIncomingPing = now;
		m_hasIncomingPing = true;
		PongReceived(now);
	}
	bool Expired(uint64_t now) const { return now - m_lastReceived >= ReceiveTimeout; }
	// Counted from Reset(): a request that has not connected receives nothing to renew it.
	bool RequestExpired(uint64_t now) const { return now - m_lastReceived >= RequestTimeout; }

	// A requester dials its candidates in turn: two failed 40 s attempts, then its attempt to reach us.
	static constexpr uint64_t RequestTimeout = 2 * 60 * 1000;
	static constexpr uint64_t PingInterval = 10 * 60 * 1000;
	static constexpr uint64_t IncomingPingInterval = 3 * 60 * 1000;
	// Stock peers can answer only every second ten-minute ping because their incoming
	// throttle shares the next-ping timestamp. Allow twenty minutes plus a grace period.
	static constexpr uint64_t ReceiveTimeout = 25 * 60 * 1000;

private:
	uint64_t m_lastReceived = 0;
	uint64_t m_nextPing = 0;
	uint64_t m_lastIncomingPing = 0;
	bool m_hasIncomingPing = false;
};

} // namespace Kademlia

#endif // KAD_BUDDY_LIVENESS_H
