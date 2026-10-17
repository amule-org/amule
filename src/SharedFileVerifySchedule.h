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

#ifndef SHAREDFILEVERIFYSCHEDULE_H
#define SHAREDFILEVERIFYSCHEDULE_H

#include "Types.h"

// Main-thread state for request-triggered verification. Only completed results are persisted;
// an in-memory attempt stamp also throttles failed checks and delayed completion events.
class CSharedFileVerifySchedule
{
public:
	bool IsDue(int days, uint32 verifiedDate, uint64 now, uint64 uptimeMs) const
	{
		if (days <= 0) {
			return false;
		}
		const uint64 interval = static_cast<uint64>(days) * 86400;
		if (m_attempted && uptimeMs >= m_lastAttemptMs &&
			uptimeMs - m_lastAttemptMs < interval * 1000) {
			return false;
		}
		// A future persisted date can follow a wall-clock correction. Recheck it once,
		// with the monotonic attempt stamp preventing repeated work during the rollback.
		return !verifiedDate || now < verifiedDate || now - verifiedDate >= interval;
	}

	void OnQueued(uint64 uptimeMs)
	{
		m_attempted = true;
		m_lastAttemptMs = uptimeMs;
	}

private:
	bool m_attempted = false;
	uint64 m_lastAttemptMs = 0;
};

#endif // SHAREDFILEVERIFYSCHEDULE_H
// File_checked_for_headers
