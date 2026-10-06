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

#ifndef SEARCHMODEICONS_H
#define SEARCHMODEICONS_H

#include "SearchList.h"
#include "MuleNotebook.h"
#include <wx/artprov.h>
#include <wx/image.h>
#include <wx/imaglist.h>
#include <wx/intl.h>
#include <array>
#include <memory>
#include <cstring>

inline wxString SearchModeLabel(SearchType type)
{
	switch (type) {
	case LocalSearch:
		return _("eD2k: Current server");
	case GlobalSearch:
		return _("eD2k: All servers");
	case KadSearch:
		return _("Kad");
	case AllSearch:
		return _("All networks");
	default:
		return wxEmptyString;
	}
}

inline wxString SearchModeHelp(SearchType type)
{
	switch (type) {
	case LocalSearch:
		return _("Search the connected eD2k server.");
	case GlobalSearch:
		return _("Search the connected eD2k server and other servers in the server list.");
	case KadSearch:
		return _("Search the Kad network.");
	case AllSearch:
		return _("Search eD2k servers and Kad, using whichever networks are available.");
	default:
		return wxEmptyString;
	}
}

inline wxArtID SearchModeArtId(SearchType type)
{
	switch (type) {
	case LocalSearch:
		return "amule:search_current_server";
	case GlobalSearch:
		return "amule:search_all_servers";
	case KadSearch:
		return "amule:search_kad";
	case AllSearch:
		return "amule:search_all_networks";
	default:
		return wxEmptyString;
	}
}

// Entries 0/1 retain the notebook's existing close-only convention for browse tabs.
inline int SearchModeImage(SearchType type)
{
	switch (type) {
	case LocalSearch:
		return 2;
	case GlobalSearch:
		return 3;
	case KadSearch:
		return 4;
	case AllSearch:
		return 5;
	default:
#ifdef __WXMAC__
		return -1;
#else
		return 0;
#endif
	}
}

inline int SearchModeCloseWidth(const wxWindow *window)
{
#ifdef __WXMAC__
	// macOS previously had no close images: retain that behavior while showing mode icons.
	(void)window;
	return -1;
#else
	return window->FromDIP(16);
#endif
}

inline std::unique_ptr<wxImageList> CreateSearchModeImages(const wxWindow *window)
{
	const wxSize size = window->FromDIP(wxSize(16, 16));
	const int closeWidth = SearchModeCloseWidth(window);
	const int modeOffset = closeWidth > 0 ? closeWidth + window->FromDIP(4) : 0;
	const wxBitmap closeBitmap = closeWidth > 0 ? ThemedCloseIcon(size) : wxNullBitmap;
	const wxImage closeImage = closeBitmap.IsOk() ? closeBitmap.ConvertToImage() : wxImage();
	constexpr std::array<SearchType, 4> types{ LocalSearch, GlobalSearch, KadSearch, AllSearch };
	constexpr int firstModeImage = 2;
	auto images = std::make_unique<wxImageList>(modeOffset + size.x, size.y);
	for (int index = 0; index < firstModeImage + static_cast<int>(types.size()); ++index) {
		wxImage image(modeOffset + size.x, size.y);
		image.InitAlpha();
		std::memset(image.GetAlpha(), 0, static_cast<size_t>(image.GetWidth()) * image.GetHeight());
		if (closeImage.IsOk()) {
			image.Paste(closeImage, 0, 0);
		}
		if (index >= firstModeImage) {
			const wxBitmap icon = wxArtProvider::GetBitmap(
				SearchModeArtId(types[index - firstModeImage]), wxART_OTHER, size);
			if (icon.IsOk()) {
				image.Paste(icon.ConvertToImage(), modeOffset, 0);
			}
		}
		images->Add(wxBitmap(image));
	}
	return images;
}

#endif
