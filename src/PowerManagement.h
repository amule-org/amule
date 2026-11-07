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

#ifndef POWERMANAGEMENT_H
#define POWERMANAGEMENT_H

#include <cstdint>
#include <wx/config.h>

namespace PowerManagement
{
enum class SleepMode
{
	Off = 0,
	WhileDownloading = 1,
	WhileRunning = 2
};

inline SleepMode NormalizeMode(long value)
{
	return value >= 0 && value <= 2 ? static_cast<SleepMode>(value) : SleepMode::Off;
}

inline SleepMode LoadMode(wxConfigBase &config)
{
	if (config.HasEntry("/PowerManagement/PreventSleep")) {
		long mode = 0;
		config.Read("/PowerManagement/PreventSleep", &mode, 0);
		return NormalizeMode(mode);
	}
	bool legacy = false;
	config.Read("/PowerManagement/PreventSleepWhileDownloading", &legacy, false);
	return legacy ? SleepMode::WhileDownloading : SleepMode::Off;
}

inline void SaveMode(wxConfigBase &config, SleepMode mode)
{
	mode = NormalizeMode(static_cast<long>(mode));
	config.Write("/PowerManagement/PreventSleep", static_cast<long>(mode));
	// Older versions have no always-awake mode. Keep their closest supported policy
	// in sync, including when the user turns inhibition off in a newer version.
	config.Write("/PowerManagement/PreventSleepWhileDownloading", mode != SleepMode::Off);
}

inline bool ShouldPreventSleep(
	SleepMode mode, bool pendingDownload, std::uint64_t sessionSent, std::uint64_t sessionReceived)
{
	return mode == SleepMode::WhileRunning ||
	       (mode == SleepMode::WhileDownloading && (pendingDownload || sessionSent < sessionReceived));
}
} // namespace PowerManagement

#endif
