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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
//

#include "config.h"
#include "UploadStorageMap.h"
#include <cstdio>
#include <cstdlib>
#include <cwchar>

#if defined(_WIN32)
#include <wx/string.h>
#include <windows.h>
#include <winnetwk.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <sys/param.h>
#include <sys/mount.h>
#endif

bool UploadStorageMap::Refresh([[maybe_unused]] bool includeNetwork)
{
	UploadStorageMap next;
#if defined(__linux__)
	// mountinfo contains device IDs and mount points without probing any volume.
	if (FILE *file = std::fopen("/proc/self/mountinfo", "r")) {
		char *line = nullptr;
		size_t size = 0;
		while (::getline(&line, &size, file) >= 0) {
			next.AddLinuxMount(line);
		}
		std::free(line);
		std::fclose(file);
	}
#elif defined(_WIN32)
	wchar_t volume[MAX_PATH];
	HANDLE search = FindFirstVolumeW(volume, MAX_PATH);
	if (search != INVALID_HANDLE_VALUE) {
		do {
			DWORD size = 0;
			GetVolumePathNamesForVolumeNameW(volume, nullptr, 0, &size);
			if (size == 0) {
				continue;
			}
			std::vector<wchar_t> paths(size);
			if (GetVolumePathNamesForVolumeNameW(volume, paths.data(), size, &size)) {
				for (const wchar_t *path = paths.data(); *path;
					path += std::wcslen(path) + 1) {
					wxString normalized(path);
					normalized.Replace("\\", "/");
					next.Add(normalized.Lower().ToStdString(wxConvUTF8),
						wxString(volume).ToStdString(wxConvUTF8));
				}
			}
		} while (FindNextVolumeW(search, volume, MAX_PATH));
		FindVolumeClose(search);
	}
	const DWORD drives = GetLogicalDrives();
	for (unsigned index = 0; index < 26; ++index) {
		if (!includeNetwork || (drives & (1UL << index)) == 0) {
			continue;
		}
		const wchar_t drive[] = { static_cast<wchar_t>(L'A' + index), L':', L'\0' };
		const auto local = wxString(drive).Lower().ToStdString(wxConvUTF8);
		if (next.Lookup(local + "/", true) != "unknown") {
			continue; // already covered by the local volume GUID table
		}
		std::vector<wchar_t> remote(MAX_PATH);
		DWORD size = static_cast<DWORD>(remote.size());
		DWORD result = WNetGetConnectionW(drive, remote.data(), &size);
		if (result == ERROR_MORE_DATA && size > 0) {
			remote.resize(size);
			result = WNetGetConnectionW(drive, remote.data(), &size);
		}
		if (result == NO_ERROR) {
			next.AddWindowsNetworkDrive(
				local, wxString(remote.data()).Lower().ToStdString(wxConvUTF8));
		}
	}
#elif defined(__APPLE__) || defined(__FreeBSD__)
	struct statfs *mounts = nullptr;
	const int count = getmntinfo(&mounts, MNT_NOWAIT);
	for (int i = 0; i < count; ++i) {
		next.Add(mounts[i].f_mntonname,
			"device:" + std::to_string(mounts[i].f_fsid.val[0]) + ":" +
				std::to_string(mounts[i].f_fsid.val[1]));
	}
#ifdef __APPLE__
	next.MergeMacDataVolume();
#endif
#endif
	// Keep the previous snapshot on a transient discovery failure. Unsupported OSes
	// use the conservative unknown group rather than inventing per-directory limits.
	if (!next.m_mounts.empty()) {
		m_mounts = std::move(next.m_mounts);
		return true;
	}
	return false;
}
