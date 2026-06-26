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

#include "StaticFs.h"

#include <cstdlib>
#include <cstring>
#include <limits.h>
#include <sys/stat.h>

namespace webapi
{

bool IsDir(const std::string &path)
{
	if (path.empty())
		return false;
	// Windows MSVCRT stat() rejects a trailing slash on a directory
	// path; POSIX accepts it. Trim consistently.
	std::string p = path;
	while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) {
		p.pop_back();
	}
	struct stat st
	{
	};
	if (::stat(p.c_str(), &st) != 0)
		return false;
	return S_ISDIR(st.st_mode);
}

bool ResolveWithinRoot(const std::string &root, const std::string &rel, std::string &fs_out)
{
	char root_real[PATH_MAX];
	char fs_real[PATH_MAX];
#ifdef _WIN32
	// _fullpath() is lexical-only (no reparse-point resolution). On
	// Windows the symlink containment threat model is weaker — symlinks
	// require elevation and are functionally exotic, so lexical
	// containment is sufficient for the operator-misconfig case the
	// rejection step targets.
	if (!_fullpath(root_real, root.c_str(), PATH_MAX))
		return false;
	const std::string joined = std::string(root_real) + "\\" + rel;
	if (!_fullpath(fs_real, joined.c_str(), PATH_MAX))
		return false;
	struct stat st;
	if (::stat(fs_real, &st) != 0)
		return false;
#else
	if (!realpath(root.c_str(), root_real))
		return false;
	const std::string joined = std::string(root_real) + "/" + rel;
	if (!realpath(joined.c_str(), fs_real))
		return false;
#endif
	// _fullpath() on Windows preserves a trailing path separator from
	// its input, which then breaks the prefix comparison below. POSIX
	// realpath() strips them. Normalise here so the containment check
	// is platform-agnostic.
	std::size_t root_len = std::strlen(root_real);
	while (root_len > 1 && (root_real[root_len - 1] == '/' || root_real[root_len - 1] == '\\')) {
		root_real[--root_len] = '\0';
	}
	if (std::strncmp(fs_real, root_real, root_len) != 0)
		return false;
	const char sep = fs_real[root_len];
	if (sep != '/' && sep != '\\' && sep != '\0')
		return false;
	fs_out.assign(fs_real);
	return true;
}

} // namespace webapi
