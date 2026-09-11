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

// Silences the deprecation diagnostics Crypto++'s headers trip, across the includes that follow and
// nothing else. Close with WarningsPop.h.
//
// Crypto++ gives several classes a user-provided copy or destructor alongside an implicit
// counterpart, and still carries dynamic exception specifications. All three are deprecated in
// C++17 and the -Werror=deprecated gate in src/CMakeLists.txt reaches them, because the headers are
// discovered without -isystem. A deprecation-warning audit build surfaces roughly 200 warnings from
// cryptopp headers alone. Named exactly, rather than a blanket -Wno-deprecated, so a genuine
// deprecation in aMule's own code still fails the build.
//
// Clang-only by necessity as well as by choice: GCC's -Wdeprecated-copy does not decompose into the
// -with-user-provided-* sub-cases, so there is nothing for it to suppress.
//
// Deliberately unguarded: see WarningsPop.h.

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-copy-with-user-provided-copy"
#pragma clang diagnostic ignored "-Wdeprecated-copy-with-user-provided-dtor"
#pragma clang diagnostic ignored "-Wdeprecated-dynamic-exception-spec"
#endif
