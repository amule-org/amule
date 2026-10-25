// Copyright (c) 2026 aMule Team
// SPDX-License-Identifier: GPL-2.0-or-later
#include <muleunit/test.h>
#include "UploadStorageMap.h"

using namespace muleunit;
DECLARE_SIMPLE(UploadStorageMap)

TEST(UploadStorageMap, LongestMountAndDirectoryBoundaries)
{
	UploadStorageMap map;
	map.Add("/", "root");
	map.Add("/media/disk", "disk");
	map.Add("/media/disk/nested", "nested");
	ASSERT_EQUALS(std::string("disk"), map.Lookup("/media/disk/a.bin"));
	ASSERT_EQUALS(std::string("nested"), map.Lookup("/media/disk/nested/a.bin"));
	ASSERT_EQUALS(std::string("root"), map.Lookup("/media/diskette/a.bin"));
	ASSERT_EQUALS(std::string("root"), map.Lookup("/media/disk/../other/a.bin"));
}

TEST(UploadStorageMap, BindMountsShareTheDeviceBudgetAndMountEscapesDecode)
{
	UploadStorageMap map;
	map.AddLinuxMount("1 0 8:1 / /media/drive\\040one rw - ext4 /dev/example rw");
	map.AddLinuxMount("2 0 8:1 /subdir /alias rw - ext4 /dev/example rw");
	map.AddLinuxMount("3 0 8:2 / /other rw - ext4 /dev/other rw");
	ASSERT_EQUALS(map.Lookup("/media/drive one/a.bin"), map.Lookup("/alias/b.bin"));
	ASSERT_FALSE(map.Lookup("/alias/b.bin") == map.Lookup("/other/a.bin"));
}

TEST(UploadStorageMap, UnmappedPathsAreConservativeAndUncSharesAreGrouped)
{
	UploadStorageMap map;
	ASSERT_EQUALS(std::string("unknown"), map.Lookup("/unmapped/a.bin"));
	ASSERT_EQUALS(map.Lookup("//server/share/a/b.bin", true), map.Lookup("//server/share/c.bin", true));
	ASSERT_FALSE(map.Lookup("//server/share/a.bin", true) == map.Lookup("//server/other/a.bin", true));
}

TEST(UploadStorageMap, MacFirmlinksAndDataSpellingsShareOneBudget)
{
	UploadStorageMap map;
	map.Add("/", "system");
	map.Add("/System/Volumes/Data", "data");
	map.Add("/Volumes/RAM", "ram");
	map.MergeMacDataVolume();
	for (const std::string path :
		{ "/Users/a/file", "/private/tmp/file", "/tmp/file", "/opt/homebrew/file" }) {
		ASSERT_EQUALS(std::string("data"), map.Lookup(path));
		ASSERT_EQUALS(map.Lookup(path), map.Lookup("/System/Volumes/Data" + path));
	}
	ASSERT_EQUALS(std::string("ram"), map.Lookup("/Volumes/RAM/file"));
	UploadStorageMap legacy;
	legacy.Add("/", "system");
	legacy.MergeMacDataVolume();
	ASSERT_EQUALS(std::string("system"), legacy.Lookup("/Users/a/file"));
}

TEST(UploadStorageMap, WindowsMappedDrivesShareTheirUncBudget)
{
	UploadStorageMap map;
	map.Add("c:", "local-guid");
	map.AddWindowsNetworkDrive("Z:", "\\\\NAS\\Share");
	map.AddWindowsNetworkDrive("Y:", "//nas/share/subdirectory");
	map.AddWindowsNetworkDrive("X:", "//nas/other");
	map.AddWindowsNetworkDrive("W:", "not-a-unc-path");
	ASSERT_EQUALS(std::string("local-guid"), map.Lookup("C:\\local\\file", true));
	ASSERT_EQUALS(map.Lookup("//nas/share/file", true), map.Lookup("Z:\\folder\\file", true));
	ASSERT_EQUALS(map.Lookup("z:/file", true), map.Lookup("y:/file", true));
	ASSERT_FALSE(map.Lookup("z:/file", true) == map.Lookup("x:/file", true));
	ASSERT_EQUALS(std::string("unknown"), map.Lookup("w:/file", true));
	ASSERT_EQUALS(std::string("unknown"), map.Lookup("//nas/", true));
}

TEST(UploadStorageMap, SymlinkedDirectoriesUseTheirTargetVolume)
{
	UploadStorageMap map;
	map.Add("/", "root");
	map.Add("/media/disk", "disk");
	map.AddResolvedDirectory("/home/user/shared/", "/media/disk/shared");
	ASSERT_EQUALS(std::string("disk"), map.Lookup("/home/user/shared/a.bin"));
	ASSERT_EQUALS(std::string("disk"), map.Lookup("/home/user/./shared/a.bin"));
	// Only files directly in a resolved directory follow it.
	ASSERT_EQUALS(std::string("root"), map.Lookup("/home/user/shared/sub/a.bin"));
	ASSERT_EQUALS(std::string("root"), map.Lookup("/home/user/other/a.bin"));
	// Windows spellings are never resolved.
	map.AddResolvedDirectory("c:/shared", "/media/disk");
	ASSERT_EQUALS(std::string("unknown"), map.Lookup("C:\\shared\\a.bin", true));
}
