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
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
//

#ifndef REFILL_H
#define REFILL_H

#include <algorithm>
#include <cstdint>

// A budget that refills one unit every msPer, up to capacity.
class CRefill
{
public:
	CRefill(uint32_t capacity, uint64_t msPer)
	: m_capacity(capacity)
	, m_msPer(msPer)
	{
	}

	uint32_t Available(uint64_t now) const
	{
		if (m_fullAt <= now) {
			return m_capacity;
		}
		const uint64_t owed = (m_fullAt - now + m_msPer - 1) / m_msPer;
		return owed >= m_capacity ? 0 : m_capacity - static_cast<uint32_t>(owed);
	}

	void Spend(uint64_t now, uint32_t units) { m_fullAt = std::max(m_fullAt, now) + units * m_msPer; }

private:
	uint32_t m_capacity;
	uint64_t m_msPer;
	// When it is back to capacity.
	uint64_t m_fullAt = 0;
};

#endif // REFILL_H
// File_checked_for_headers
