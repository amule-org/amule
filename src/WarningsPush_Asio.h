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

// Silences the deprecation diagnostics Boost.Asio's headers trip, across the
// includes that follow and nothing else. Close with WarningsPop.h.
//
// Asio gives several exception types a user-provided destructor alongside an
// implicit copy constructor, and its execution headers define constexpr
// statics out of line, which C++17 makes redundant. Both are deprecated and
// the -Werror=deprecated gate reaches them. Named exactly rather than a
// blanket suppression, for the reason in WarningsPush_CryptoPP.h.
//
// Measured with AppleClang 21 and Boost 1.92: without both suppressions
// ip/tcp.hpp produces 29 errors under -Werror=deprecated, 27 redundant
// constexpr static definitions and 2 deprecated copies with a user-provided
// destructor. Each suppression is independently necessary.
//
// Only the wider closures need this: <boost/asio/ip/address.hpp> alone trips
// neither, measured at zero, which is why NetworkAddressAsio.h has no wrap.
//
// Deliberately unguarded: see WarningsPop.h.

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-copy-with-user-provided-dtor"
#pragma clang diagnostic ignored "-Wdeprecated-redundant-constexpr-static-def"
#endif
