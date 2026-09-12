//								-*- C++ -*-
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

// A second translation unit that includes NetworkAddress.h, so the test beside it can ask whether
// the exclusion table is one entity or a copy per unit. That is the whole content: without
// `inline` on the table, `constexpr` at namespace scope implies `const` and therefore internal
// linkage, and the address taken here differs from the one taken there.

#include <NetworkAddress.h>

const void *ExcludedPrefixTableAddressFromOtherUnit() noexcept
{
	return &NetworkAddressPolicy::kIPv6ExcludedPrefixes[0];
}
