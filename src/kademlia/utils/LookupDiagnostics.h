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

#ifndef AMULE_LOOKUPDIAGNOSTICS_H
#define AMULE_LOOKUPDIAGNOSTICS_H
#include "LookupTrace.h"
#include <wx/string.h>
#include <vector>
namespace Kademlia
{
struct LookupSnapshot
{
	uint32_t type;
	wxString target;
	wxString keyword;
	uint64_t started;
	uint64_t finished;
	CLookupTrace trace;
};
constexpr size_t MaxLookupHistory = 16;
// Local GUI text; remote clients receive values and format in their own locale.
wxString FormatLookupDiagnostics(
	const std::vector<LookupSnapshot> &active, const std::deque<LookupSnapshot> &recent, uint64_t now);
} // namespace Kademlia
#endif
