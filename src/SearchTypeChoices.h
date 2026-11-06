//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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

#ifndef SEARCHTYPECHOICES_H
#define SEARCHTYPECHOICES_H

#include "SearchList.h"
#include <algorithm>
#include <vector>

// The dialog renders these choices directly. Keep capability filtering and
// saved-selection restoration together so an unavailable mode cannot be restored.
struct CSearchTypeChoices
{
	std::vector<SearchType> types;
	int selection = -1;
};

inline CSearchTypeChoices BuildSearchTypeChoices(
	bool ed2kEnabled, bool kadEnabled, bool supportsAll, long savedType)
{
	CSearchTypeChoices choices;
	if (ed2kEnabled) {
		choices.types.push_back(LocalSearch);
		choices.types.push_back(GlobalSearch);
	}
	if (kadEnabled) {
		choices.types.push_back(KadSearch);
	}
	if (ed2kEnabled && kadEnabled && supportsAll) {
		choices.types.push_back(AllSearch);
	}
	// Older GUI preferences stored All's dropdown index rather than its code.
	if (savedType == 3) {
		savedType = AllSearch;
	}
	if (!choices.types.empty()) {
		const auto found = std::find(choices.types.begin(), choices.types.end(), savedType);
		// There are at most four choices; wxWidgets takes an int index.
		choices.selection =
			found == choices.types.end() ? 0 : static_cast<int>(found - choices.types.begin());
	}
	return choices;
}

#endif
