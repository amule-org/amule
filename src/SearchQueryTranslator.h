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

#ifndef SEARCHQUERYTRANSLATOR_H
#define SEARCHQUERYTRANSLATOR_H

#include <ec/cpp/ECSpecialTags.h>

struct CSearchQueryPreview
{
	wxString keywords;
	CSearchMetadata metadata;
};

// Explicit labels only: no artist/title inference and no network access.
// On failure, leave the previous preview untouched.
bool TranslateSearchQuery(const wxString &input, CSearchQueryPreview &preview, wxString &error);

#endif // SEARCHQUERYTRANSLATOR_H
