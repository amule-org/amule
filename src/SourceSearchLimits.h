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

#ifndef SOURCESEARCHLIMITS_H
#define SOURCESEARCHLIMITS_H

#include <algorithm>
#include <cstdint>

// The only definition of these limits. The core, the preferences dialog and amuleapi all read
// them, and a WebUI test checks preferences.js against amuleapi's schema.
struct CPrefRange
{
	std::uint32_t min;
	std::uint32_t def;
	std::uint32_t max;

	constexpr std::uint64_t Clamp(std::uint64_t value) const
	{
		return std::min<std::uint64_t>(std::max<std::uint64_t>(value, min), max);
	}
};

// Concurrent files that may search Kad for sources at once.
constexpr CPrefRange KAD_MAX_SOURCE_SEARCHES{ 5, 30, 50 };
// Per-file Kad re-search interval, in minutes.
constexpr CPrefRange KAD_SOURCE_REASK_MINUTES{ 30, 30, 60 };
// Interval before re-asking a known source, in minutes. The UDP reask goes out 20 s earlier, and
// peers ban a client that reasks within about 10 minutes.
constexpr CPrefRange SOURCE_REASK_MINUTES{ 15, 15, 60 };

#endif // SOURCESEARCHLIMITS_H
