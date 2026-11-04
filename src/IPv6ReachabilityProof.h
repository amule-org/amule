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

#ifndef IPV6_REACHABILITY_PROOF_H
#define IPV6_REACHABILITY_PROOF_H

#include <cstdint>

constexpr std::uint64_t kIPv6ReachabilityProofLifetimeMs = 24ULL * 60 * 60 * 1000;

/**
 * Tracks a recent inbound confirmation. It knows nothing about servers, so a server disconnect
 * cannot clear the proof.
 */
class CIPv6ReachabilityProof
{
public:
	//! Record a fresh inbound confirmation.
	void NoteHello(std::uint64_t nowMs)
	{
		m_confirmedAt = nowMs;
		m_confirmed = true;
	}

	//! Drop the current confirmation when the chosen address changes.
	void Clear() { m_confirmed = false; }

	//! Whether the confirmation is still fresh at the supplied monotonic time.
	bool IsProven(std::uint64_t nowMs) const
	{
		return m_confirmed && nowMs >= m_confirmedAt &&
		       nowMs - m_confirmedAt < kIPv6ReachabilityProofLifetimeMs;
	}

private:
	std::uint64_t m_confirmedAt = 0;
	bool m_confirmed = false;
};

#endif // IPV6_REACHABILITY_PROOF_H
