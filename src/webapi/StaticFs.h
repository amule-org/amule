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

// Small filesystem helpers shared between ServeStaticFile (Api.cpp) and
// its security-critical unit tests (StaticFsTest). Lifted to its own
// TU so the test target can link the helper without dragging the rest
// of the dispatcher (and its wx/Boost.Beast/EC web).

#ifndef AMULE_WEBAPI_STATICFS_H
#define AMULE_WEBAPI_STATICFS_H

#include <string>

namespace webapi
{

bool IsDir(const std::string &path);

// Resolve `rel` under `root` and reject if the result escapes `root`
// (symlink containment + belt-and-suspenders against any traversal
// that slips past the upstream path-pattern filter). Writes the
// canonical absolute path into `fs_out` on success. Any failure
// (missing file, escape, OS error) returns false — the caller emits
// an opaque 404 either way to keep the directory layout non-
// enumerable from outside.
bool ResolveWithinRoot(const std::string &root, const std::string &rel, std::string &fs_out);

} // namespace webapi

#endif // AMULE_WEBAPI_STATICFS_H
