// Copyright (c) 2026 aMule Team
// SPDX-License-Identifier: GPL-2.0-or-later

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
