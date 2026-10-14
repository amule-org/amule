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
#ifndef KADEMLIA_PUBLICIPVOTE_H
#define KADEMLIA_PUBLICIPVOTE_H

#include <cstdint>

namespace Kademlia
{

/**
 * Our public IP as Kad peers report it. Header-only so a test can drive it.
 *
 * The first report after connecting is taken as is. After that a new value needs a second, equal
 * report from another /24: a mismatch means either that our IP changed or that a peer answered
 * with a bad one, and one party must not be able to settle which.
 */
class CPublicIPVote
{
public:
	uint32_t Get() const noexcept { return m_ip; }

	//! Restores the IP saved with the Kad preferences. It counts as nobody's report.
	void Load(uint32_t ip) noexcept { m_ip = ip; }

	//! @a reporter is the peer that told us @a ip, in host order like @a ip.
	void Report(uint32_t ip, uint32_t reporter) noexcept
	{
		if (!ip || !m_last) {
			m_ip = ip;
			m_last = ip;
			m_lastReporter = reporter;
		} else if (ip != m_last) {
			m_last = ip;
			m_lastReporter = reporter;
		} else if ((reporter ^ m_lastReporter) & 0xFFFFFF00) {
			m_ip = ip;
		}
	}

private:
	uint32_t m_ip = 0;
	uint32_t m_last = 0;
	uint32_t m_lastReporter = 0;
};

} // namespace Kademlia

#endif // KADEMLIA_PUBLICIPVOTE_H
// File_checked_for_headers
