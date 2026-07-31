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
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
//

#ifndef LIBWEBCOMMON_ATOMICFILE_H
#define LIBWEBCOMMON_ATOMICFILE_H

#include <string>

namespace webcommon
{

// Writes `body` to `path` owner-only, without ever exposing a partial or
// world-readable version of it.
//
// POSIX: create a sibling temp with mode 0600, write it in full, fsync,
// then rename(2) onto the target. The file is never observable in a
// half-written state, and never exists with looser permissions than the
// ones it is created with -- creating at 0600 rather than chmod-ing after
// the fact closes the window where another user could open it. fchmod is
// applied anyway as belt and braces against an odd umask.
//
// Windows: best-effort. There is no equivalent permission story here --
// see RestrictToOwner() in FileFunctions.cpp, which is a no-op on Windows
// for the same reason -- so callers holding a secret must not rely on this
// for confidentiality on that platform.
//
// Returns false on any failure, leaving the previous contents of `path`
// intact. Reports nothing: this lives below the logger for the same reason
// the file helpers in mulecommon do, so the caller decides what to say.
bool WriteFileAtomic0600(const std::string &path, const std::string &body);

} // namespace webcommon

#endif // LIBWEBCOMMON_ATOMICFILE_H
