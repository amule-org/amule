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

#include <muleunit/test.h>

#include <common/ProcessUsage.h>

#include <chrono>
#include <cstddef>
#include <numeric>
#include <vector>

using namespace muleunit;

DECLARE_SIMPLE(ProcessUsage)

TEST(ProcessUsage, CpuTimeRisesWhileBusy)
{
	const auto before = GetProcessCpuMilliseconds();
	ASSERT_TRUE(before.has_value());

	// Windows updates process times about every 16 ms.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	auto after = before;
	volatile std::uint64_t spin = 0;
	while (*after == *before && std::chrono::steady_clock::now() < deadline) {
		for (int i = 0; i < 100000; ++i) {
			spin = spin + 1;
		}
		after = GetProcessCpuMilliseconds();
		ASSERT_TRUE(after.has_value());
	}
	ASSERT_TRUE(*after > *before);
}

TEST(ProcessUsage, ResidentMemoryFollowsTouchedPages)
{
	const auto before = GetProcessResidentBytes();
#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
	ASSERT_TRUE(before.has_value());
#endif
	if (!before) {
		return;
	}
	ASSERT_TRUE(*before > 0);

	const std::size_t size = 64 * 1024 * 1024;
	std::vector<unsigned char> pages(size);
	for (std::size_t i = 0; i < size; ++i) {
		pages[i] = static_cast<unsigned char>(i * 31);
	}
	const auto after = GetProcessResidentBytes();
	ASSERT_TRUE(after.has_value());
	// Half, so a page or two reclaimed under memory pressure cannot fail the test.
	ASSERT_TRUE(*after >= *before + size / 2);
	// Keeps the writes observable.
	ASSERT_TRUE(std::accumulate(pages.begin(), pages.end(), std::uint64_t{ 0 }) > 0);
}
