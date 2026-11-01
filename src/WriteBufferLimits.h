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

#ifndef WRITEBUFFERLIMITS_H
#define WRITEBUFFERLIMITS_H

#include <cstdint>

namespace WriteBufferLimits
{
constexpr std::uint32_t minMiB = 1;
constexpr std::uint32_t defaultMiB = 128;
constexpr std::uint32_t maxMiB = 1024;
constexpr std::uint64_t bytesPerMiB = 1024 * 1024;
} // namespace WriteBufferLimits
#endif
