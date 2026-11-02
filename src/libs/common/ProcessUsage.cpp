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

#include "ProcessUsage.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <fstream>
#include <unistd.h>
#endif

std::optional<std::uint64_t> GetProcessCpuMilliseconds()
{
#ifdef _WIN32
	FILETIME creation, exit, kernel, user;
	if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
		return std::nullopt;
	}
	const auto ticks = [](const FILETIME &time) {
		return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
	};
	return (ticks(kernel) + ticks(user)) / 10000; // FILETIME counts 100 ns units.
#else
	rusage usage{};
	if (getrusage(RUSAGE_SELF, &usage) != 0) {
		return std::nullopt;
	}
	const auto milliseconds = [](const timeval &time) {
		return static_cast<std::uint64_t>(time.tv_sec) * 1000 +
		       static_cast<std::uint64_t>(time.tv_usec) / 1000;
	};
	return milliseconds(usage.ru_utime) + milliseconds(usage.ru_stime);
#endif
}

std::optional<std::uint64_t> GetProcessResidentBytes()
{
#if defined(_WIN32)
	PROCESS_MEMORY_COUNTERS counters{};
	if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
		return std::nullopt;
	}
	return static_cast<std::uint64_t>(counters.WorkingSetSize);
#elif defined(__APPLE__)
	mach_task_basic_info info{};
	mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
	if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
		KERN_SUCCESS) {
		return std::nullopt;
	}
	return static_cast<std::uint64_t>(info.resident_size);
#elif defined(__linux__)
	std::ifstream statm("/proc/self/statm");
	std::uint64_t total = 0;
	std::uint64_t residentPages = 0;
	const long pageSize = sysconf(_SC_PAGESIZE);
	if (!(statm >> total >> residentPages) || pageSize <= 0) {
		return std::nullopt;
	}
	return residentPages * static_cast<std::uint64_t>(pageSize);
#else
	// getrusage's ru_maxrss is only the peak.
	return std::nullopt;
#endif
}
