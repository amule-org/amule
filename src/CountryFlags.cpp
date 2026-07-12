//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
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

// Country flags are libmaxminddb-free: they map an ISO code to an embedded PNG
// via the art provider, so this is compiled unconditionally into every GUI
// (both amulegui, which gets codes over EC, and monolithic amule). Only the
// resolver that produces the codes is gated on ENABLE_IP2COUNTRY.
#include "CountryFlags.h"
#include "Logger.h"          // For AddLogLine*
#include <common/Format.h>   // For CFormat()
#include "icons/icon_data.h" // For amule_get_all_icons()

#include <wx/artprov.h> // For wxArtProvider::GetBitmap
#include <wx/intl.h>

#include <cstring> // For strncmp

CCountryFlags::CCountryFlags() = default;

void CCountryFlags::LoadFlags()
{
	// Walk the embedded icon table and pick out anything named
	// "flag_<code>". The table is built by src/icons/embed_icons.py
	// from src/icons/flags/<code>.png at compile time; CamuleArtProvider
	// (registered in CamuleGuiApp::OnInit) hands us back a decoded
	// wxBitmap for each.
	int icon_count = 0;
	const struct AMuleIconEntry *icons = amule_get_all_icons(&icon_count);
	const char flag_prefix[] = "flag_";
	const size_t flag_prefix_len = sizeof(flag_prefix) - 1;

	for (int i = 0; i < icon_count; ++i) {
		const char *name = icons[i].name;
		if (strncmp(name, flag_prefix, flag_prefix_len) != 0) {
			continue;
		}
		const wxString code = wxString(name + flag_prefix_len, wxConvISO8859_1);
		const wxString art_id = wxString::Format("amule:%s", name);
		const wxImage flag = wxArtProvider::GetBitmap(art_id).ConvertToImage();

		if (!flag.IsOk()) {
			// Reuse the existing catalog string (avoid a new msgid).
			AddLogLineC(CFormat(_("Failed to load country data for '%s'.")) % code);
			continue;
		}
		if (code == "unknown") {
			m_unknown = flag;
		}
		m_flags[code] = flag;
	}

	AddDebugLogLineN(logGeneral,
		CFormat("Loaded %d flag bitmaps.") %
			m_flags.size()); // there's never just one - no plural needed
}

const wxImage &CCountryFlags::GetFlag(const wxString &code)
{
	if (!m_loaded) {
		// First call happens during list drawing, well after the app's
		// OnInit pushed CamuleArtProvider — so the flag art resolves now.
		LoadFlags();
		m_loaded = true;
	}
	std::map<wxString, wxImage>::const_iterator it = m_flags.find(code);
	if (it != m_flags.end()) {
		return it->second;
	}
	// Empty or unrecognised code -> the "??" placeholder flag.
	return m_unknown;
}
