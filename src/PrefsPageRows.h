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

#ifndef AMULE_PREFSPAGEROWS_H
#define AMULE_PREFSPAGEROWS_H

#include <algorithm>
#include <vector>

// The preferences sidebar lists pages in pages[] order, but a page can be hidden, so a page's
// row is its position among the pages shown, not its pages[] index. @p shown holds the
// pages[] index of each sidebar row, in row order.
namespace PrefsPageRows
{

//! Row showing @p page, or -1 if it is hidden.
inline int Find(const std::vector<unsigned> &shown, unsigned page)
{
	const auto it = std::find(shown.begin(), shown.end(), page);
	return it == shown.end() ? -1 : static_cast<int>(it - shown.begin());
}

//! Row at which @p page goes to keep the sidebar in pages[] order.
inline unsigned InsertionRow(const std::vector<unsigned> &shown, unsigned page)
{
	return static_cast<unsigned>(
		std::count_if(shown.begin(), shown.end(), [page](unsigned other) { return other < page; }));
}

} // namespace PrefsPageRows

#endif // AMULE_PREFSPAGEROWS_H
