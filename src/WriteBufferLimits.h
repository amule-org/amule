// Copyright (c) 2026 aMule Team
// SPDX-License-Identifier: GPL-2.0-or-later
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
