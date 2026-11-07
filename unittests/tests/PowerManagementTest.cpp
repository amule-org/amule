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
#include "PowerManagement.h"
#include <wx/fileconf.h>
#include <wx/sstream.h>
#include <limits>

using namespace muleunit;
using PowerManagement::SleepMode;

DECLARE_SIMPLE(PowerManagement)

TEST(PowerManagement, MigratesLegacySettingOnlyWhenNewKeyIsAbsent)
{
	wxStringInputStream input("[PowerManagement]\nPreventSleepWhileDownloading=1\n");
	wxFileConfig config(input);
	ASSERT_TRUE(PowerManagement::LoadMode(config) == SleepMode::WhileDownloading);
	config.Write("/PowerManagement/PreventSleep", 0L);
	ASSERT_TRUE(PowerManagement::LoadMode(config) == SleepMode::Off);
	config.Write("/PowerManagement/PreventSleep", 2L);
	ASSERT_TRUE(PowerManagement::LoadMode(config) == SleepMode::WhileRunning);
	config.DeleteEntry("/PowerManagement/PreventSleep");
	config.Write("/PowerManagement/PreventSleepWhileDownloading", false);
	ASSERT_TRUE(PowerManagement::LoadMode(config) == SleepMode::Off);
	config.DeleteEntry("/PowerManagement/PreventSleepWhileDownloading");
	ASSERT_TRUE(PowerManagement::LoadMode(config) == SleepMode::Off);
}

TEST(PowerManagement, InvalidNewValuesStayOffDespiteEnabledLegacySetting)
{
	wxStringInputStream input("[PowerManagement]\nPreventSleepWhileDownloading=1\n");
	wxFileConfig config(input);
	for (const auto *value : { "-1", "3", "4294967297", "999999999999999999999", "invalid" }) {
		config.Write("/PowerManagement/PreventSleep", wxString::FromUTF8(value));
		ASSERT_TRUE(PowerManagement::LoadMode(config) == SleepMode::Off);
	}
}

TEST(PowerManagement, SavesModesAndKeepsDowngradeSettingInSync)
{
	for (const auto mode : { SleepMode::Off, SleepMode::WhileDownloading, SleepMode::WhileRunning }) {
		wxStringInputStream input("");
		wxFileConfig config(input);
		PowerManagement::SaveMode(config, mode);
		wxStringOutputStream output;
		config.Save(output);
		wxStringInputStream saved(output.GetString());
		wxFileConfig reloaded(saved);
		ASSERT_TRUE(PowerManagement::LoadMode(reloaded) == mode);
		bool legacy = false;
		ASSERT_TRUE(reloaded.Read("/PowerManagement/PreventSleepWhileDownloading", &legacy));
		ASSERT_EQUALS(mode != SleepMode::Off, legacy);
		PowerManagement::SaveMode(reloaded, SleepMode::Off);
		ASSERT_TRUE(PowerManagement::LoadMode(reloaded) == SleepMode::Off);
		ASSERT_TRUE(reloaded.Read("/PowerManagement/PreventSleepWhileDownloading", &legacy));
		ASSERT_FALSE(legacy);
	}
}

TEST(PowerManagement, PreservesQueuedDownloadsAndSessionBalancePolicy)
{
	ASSERT_TRUE(PowerManagement::ShouldPreventSleep(SleepMode::WhileDownloading, true, 100, 0));
	ASSERT_TRUE(PowerManagement::ShouldPreventSleep(SleepMode::WhileDownloading, false, 0, 1));
	ASSERT_FALSE(PowerManagement::ShouldPreventSleep(SleepMode::WhileDownloading, false, 1, 1));
	ASSERT_FALSE(PowerManagement::ShouldPreventSleep(SleepMode::WhileDownloading, false, 2, 1));
	ASSERT_FALSE(PowerManagement::ShouldPreventSleep(SleepMode::WhileDownloading, false, 0, 0));
	ASSERT_TRUE(PowerManagement::ShouldPreventSleep(
		SleepMode::WhileDownloading, false, 1, std::numeric_limits<std::uint64_t>::max()));
}

TEST(PowerManagement, RunningModeIgnoresQueueAndSessionBalanceWhileOffNeverInhibits)
{
	// Intentionally exercise a value outside the supported preference modes.
	// NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
	const auto unknownMode = static_cast<SleepMode>(99);
	for (const bool pending : { false, true }) {
		ASSERT_TRUE(PowerManagement::ShouldPreventSleep(SleepMode::WhileRunning, pending, 0, 0));
		ASSERT_TRUE(PowerManagement::ShouldPreventSleep(SleepMode::WhileRunning, pending, 100, 1));
		ASSERT_FALSE(PowerManagement::ShouldPreventSleep(SleepMode::Off, pending, 0, 100));
		ASSERT_FALSE(PowerManagement::ShouldPreventSleep(unknownMode, pending, 0, 100));
	}
}
