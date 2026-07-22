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

#ifndef PICOJSON_INC_H
#define PICOJSON_INC_H

// Single include site for picojson. Include this wrapper rather than
// picojson.h directly.
//
// PICOJSON_USE_INT64 switches picojson::value's number storage from the
// default double to int64_t, so large integers (file sizes, ECIDs, ...)
// survive exactly. It changes the layout of picojson::value, so it is
// ABI-affecting: every translation unit that includes picojson.h must
// define it identically, or the TUs disagree on the type (an ODR
// violation the compiler cannot catch). Defining it here -- once -- keeps
// the toggle from drifting between use sites. It is set at the use site
// rather than patched into picojson.h so the vendored header stays
// byte-identical to upstream (see docs/THIRDPARTY.md).
#define PICOJSON_USE_INT64

// USE_SYSTEM_PICOJSON (src/libwebcommon/CMakeLists.txt) sets
// AMULE_PICOJSON_HEADER to the absolute path of the selected header --
// the bundled copy by default, or a system-installed one for distro
// packaging. The path is absolute so include-directory ordering cannot
// substitute the other copy. The <picojson.h> fallback is defensive
// only; the macro is always defined for the targets that link
// picojson_header.
#ifdef AMULE_PICOJSON_HEADER
#include AMULE_PICOJSON_HEADER
#else
#include <picojson.h>
#endif

#endif // PICOJSON_INC_H
