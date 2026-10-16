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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
//

#include "FreeSpaceSamples.h" // Interface declarations

#include <protocol/ed2k/Constants.h> // Needed for PARTSIZE

#include <algorithm>

bool FreeSpaceAllows(sint64 free, uint64 needed, bool checkEnabled, uint64 minFree)
{
	// Below one part, a write risks failing on a full disk whatever the user set.
	if (free < 0 || static_cast<uint64>(free) < PARTSIZE) {
		return false;
	}
	// Sparse files mean a write within the file can still allocate new blocks, so the only
	// safe rule is to stop the moment the limit would be crossed.
	return !checkEnabled || static_cast<uint64>(free) >= needed + minFree;
}

void CFreeSpaceSamples::Publish(const CPath &dir, sint64 free, uint64 tick)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_samples[dir.GetRaw()] = Sample{ free, tick };
}

sint64 CFreeSpaceSamples::Get(const CPath &dir, uint64 now, uint64 maxAgeMs, uint64 notBefore) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const auto it = m_samples.find(dir.GetRaw());
	if (it == m_samples.end() || it->second.free < 0 || now - it->second.tick > maxAgeMs ||
		it->second.tick < notBefore) {
		return -1;
	}
	return it->second.free;
}

void CFreeSpaceSamples::Retain(const std::vector<CPath> &dirs)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	for (auto it = m_samples.begin(); it != m_samples.end();) {
		const bool kept = std::any_of(dirs.begin(), dirs.end(), [&it](const CPath &dir) {
			return dir.GetRaw() == it->first;
		});
		it = kept ? std::next(it) : m_samples.erase(it);
	}
}
// File_checked_for_headers
