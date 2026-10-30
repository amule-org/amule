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

#include <muleunit/test.h>
#include <Preferences.h>

using namespace muleunit;

uint16 CPreferences::s_kadMaxSourceSearches;
uint64 CPreferences::s_kadSourceReaskMins;
uint64 CPreferences::s_sourceReaskMins;

DECLARE_SIMPLE(SourceSearchLimits)

// EC writes go straight to these setters, so they must keep every value inside the range.
TEST(SourceSearchLimits, SourceReaskSetterClamps)
{
	thePrefs::SetSourceReaskTime(0);
	ASSERT_EQUALS(uint64{ SOURCE_REASK_MINUTES.min } * 60000, thePrefs::GetSourceReaskTime());
	thePrefs::SetSourceReaskTime(7 * 60000);
	ASSERT_EQUALS(uint64{ SOURCE_REASK_MINUTES.min } * 60000, thePrefs::GetSourceReaskTime());
	thePrefs::SetSourceReaskTime(20 * 60000);
	ASSERT_EQUALS(uint64{ 20 * 60000 }, thePrefs::GetSourceReaskTime());
	thePrefs::SetSourceReaskTime(UINT64_MAX);
	ASSERT_EQUALS(uint64{ SOURCE_REASK_MINUTES.max } * 60000, thePrefs::GetSourceReaskTime());
}

TEST(SourceSearchLimits, KadSourceReaskSetterClamps)
{
	thePrefs::SetKadSourceReaskTime(60000);
	ASSERT_EQUALS(uint64{ KAD_SOURCE_REASK_MINUTES.min } * 60000, thePrefs::GetKadSourceReaskTime());
	thePrefs::SetKadSourceReaskTime(45 * 60000);
	ASSERT_EQUALS(uint64{ 45 * 60000 }, thePrefs::GetKadSourceReaskTime());
	thePrefs::SetKadSourceReaskTime(UINT64_MAX);
	ASSERT_EQUALS(uint64{ KAD_SOURCE_REASK_MINUTES.max } * 60000, thePrefs::GetKadSourceReaskTime());
}

TEST(SourceSearchLimits, KadMaxSourceSearchesSetterClampsWithoutWrapping)
{
	thePrefs::SetKadMaxSourceSearches(1);
	ASSERT_EQUALS(KAD_MAX_SOURCE_SEARCHES.min, thePrefs::GetKadMaxSourceSearches());
	thePrefs::SetKadMaxSourceSearches(40);
	ASSERT_EQUALS(40u, thePrefs::GetKadMaxSourceSearches());
	// 65541 would wrap to 5 through a uint16.
	thePrefs::SetKadMaxSourceSearches(65541);
	ASSERT_EQUALS(KAD_MAX_SOURCE_SEARCHES.max, thePrefs::GetKadMaxSourceSearches());
}
