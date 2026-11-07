// Copyright (c) 2026 aMule Team
// SPDX-License-Identifier: GPL-2.0-or-later

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
	for (const bool pending : { false, true }) {
		ASSERT_TRUE(PowerManagement::ShouldPreventSleep(SleepMode::WhileRunning, pending, 0, 0));
		ASSERT_TRUE(PowerManagement::ShouldPreventSleep(SleepMode::WhileRunning, pending, 100, 1));
		ASSERT_FALSE(PowerManagement::ShouldPreventSleep(SleepMode::Off, pending, 0, 100));
		ASSERT_FALSE(
			PowerManagement::ShouldPreventSleep(static_cast<SleepMode>(99), pending, 0, 100));
	}
}
