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

#ifndef CRYPTOPP_INC_H
#define CRYPTOPP_INC_H

#define CRYPTOPP_ENABLE_NAMESPACE_WEAK 1

#ifndef CRYPTOPP_INCLUDE_PREFIX
#include "config.h" // Needed for CRYPTOPP_INCLUDE_PREFIX
#endif

#define noinline noinline

#define CRYPTO_HEADER(hdr) <CRYPTOPP_INCLUDE_PREFIX/hdr>

// cryptopp's headers are heavy on C++03 patterns: virtual dtors that
// bring in the deprecated implicit copy ctor (P0806 rule), and
// throw(...) dynamic exception specs that were formally removed in
// C++17. On Homebrew macOS the cryptopp include directory reaches
// consumers via CPATH (an `-I` alias), not as `-isystem`, so a strict
// deprecation-warning audit build (`-Wdeprecated-declarations
// -Wdeprecated-copy -Wdeprecated`) surfaces ~200 warnings from
// cryptopp headers alone. GCC's -Wdeprecated-copy doesn't decompose
// into the -with-user-provided-dtor sub-case, so this only bites
// Clang builds today; the pragma is scoped to Clang-known sub-flags
// only for that reason. Local to cryptopp includes; nothing else on
// the translation unit is affected.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-copy-with-user-provided-dtor"
#pragma clang diagnostic ignored "-Wdeprecated-copy-with-user-provided-copy"
#pragma clang diagnostic ignored "-Wdeprecated-dynamic-exception-spec"
#endif

#include CRYPTO_HEADER(config.h)
#include CRYPTO_HEADER(md4.h)
#include CRYPTO_HEADER(md5.h)
#include CRYPTO_HEADER(rsa.h)
#include CRYPTO_HEADER(sha.h)
#include CRYPTO_HEADER(base64.h)
#include CRYPTO_HEADER(osrng.h)
#include CRYPTO_HEADER(files.h)
#include CRYPTO_HEADER(sha.h)
#include CRYPTO_HEADER(des.h)

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#endif /* CRYPTOPP_INC_H */
