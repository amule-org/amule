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

#ifndef UPLOADSTORAGEMAP_H
#define UPLOADSTORAGEMAP_H

#include <filesystem>
#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

// Snapshot of the OS mount table. Refresh runs on a worker; lookups never contact
// a filesystem/network provider. Bind mounts / volume aliases share one key.
class UploadStorageMap
{
public:
	bool Refresh(bool includeNetwork = true); // false leaves the previous snapshot intact
	void Add(std::string path, std::string key)
	{
		while (path.size() > 1 && path.back() == '/') {
			path.pop_back();
		}
		m_mounts.push_back({ std::move(path), std::move(key) });
	}
	void MergeMacDataVolume()
	{
		const auto data = std::find_if(m_mounts.begin(), m_mounts.end(), [](const Mount &mount) {
			return mount.path == "/System/Volumes/Data";
		});
		if (data != m_mounts.end()) {
			for (auto &mount : m_mounts) {
				if (mount.path == "/") {
					// Firmlinks expose Data through /Users, /private, /opt, etc.
					// Conservatively share its budget with the system-volume fallback.
					mount.key = data->key;
				}
			}
		}
	}
	void AddWindowsNetworkDrive(const std::string &drive, const std::string &remote)
	{
		const auto key = Lookup(remote, true);
		if (key.compare(0, 6, "unc://") == 0) {
			Add(WindowsPath(drive) + "/", key);
		}
	}
	void AddLinuxMount(const std::string &line)
	{
		std::istringstream input(line);
		std::string id, parent, device, root, path;
		if (!(input >> id >> parent >> device >> root >> path) ||
			device.find(':') == std::string::npos) {
			return;
		}
		std::string decoded;
		for (size_t i = 0; i < path.size(); ++i) {
			if (path[i] == '\\' && i + 3 < path.size() &&
				(path.compare(i, 4, "\\040") == 0 || path.compare(i, 4, "\\011") == 0 ||
					path.compare(i, 4, "\\012") == 0 ||
					path.compare(i, 4, "\\134") == 0)) {
				decoded += static_cast<char>((path[i + 1] - '0') * 64 +
							     (path[i + 2] - '0') * 8 + path[i + 3] - '0');
				i += 3;
			} else {
				decoded += path[i];
			}
		}
		Add(std::move(decoded), "device:" + device);
	}
	std::string Lookup(const std::string &input, bool windows = false) const
	{
		const std::string normalized = windows ? WindowsPath(input) : input;
		std::string path = std::filesystem::u8path(normalized).lexically_normal().generic_u8string();
		if (windows && normalized.compare(0, 2, "//") == 0 && path.compare(0, 2, "//") != 0) {
			path.insert(0, "/");
		}
		const Mount *best = nullptr;
		for (const auto &mount : m_mounts) {
			if (path.compare(0, mount.path.size(), mount.path) == 0 &&
				(mount.path == "/" || path.size() == mount.path.size() ||
					path[mount.path.size()] == '/') &&
				(best == nullptr || mount.path.size() > best->path.size())) {
				best = &mount;
			}
		}
		if (best != nullptr) {
			return best->key;
		}
		// UNC shares are not in Windows' local volume table. Group by server/share,
		// not by directory. Unknown paths conservatively share one worker permit.
		if (windows && path.compare(0, 2, "//") == 0) {
			const auto server = path.find('/', 2);
			if (server != std::string::npos && server > 2 && server + 1 < path.size()) {
				return "unc:" + path.substr(0, path.find('/', server + 1));
			}
		}
		return "unknown";
	}

private:
	static std::string WindowsPath(std::string path)
	{
		for (char &character : path) {
			if (character == '\\') {
				character = '/';
			} else if (character >= 'A' && character <= 'Z') {
				character = static_cast<char>(character + ('a' - 'A'));
			}
		}
		return path;
	}
	struct Mount
	{
		std::string path;
		std::string key;
	};
	std::vector<Mount> m_mounts;
};
#endif
