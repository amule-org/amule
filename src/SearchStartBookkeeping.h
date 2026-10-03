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

#ifndef SEARCHSTARTBOOKKEEPING_H
#define SEARCHSTARTBOOKKEEPING_H

#include "SearchList.h"

#include <ctime>

// Hold the proposed scalar search state until startup has completed.
class CSearchStartBookkeeping
{
public:
	CSearchStartBookkeeping(
		SearchType &currentType, time_t &currentStart, SearchType requestedType, bool preserveAnchor)
	: m_currentType(currentType)
	, m_currentStart(currentStart)
	, m_requestedType(requestedType)
	, m_preserveAnchor(preserveAnchor)
	{
	}

	void Commit(time_t start)
	{
		if (!m_preserveAnchor) {
			m_currentType = m_requestedType;
			m_currentStart = start;
		}
	}

private:
	SearchType &m_currentType;
	time_t &m_currentStart;
	SearchType m_requestedType;
	bool m_preserveAnchor;
};

#endif
