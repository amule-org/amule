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

#ifndef COUNTRYDISPLAY_H
#define COUNTRYDISPLAY_H

//
// Which country code a list should display for one entry -- the rule shared by
// the peer list and the server list, so the two cannot drift apart.
//
// This is a header, and the body below is deliberately NOT in a .cpp: the
// answer depends on ENABLE_IP2COUNTRY / CLIENT_GUI, and those are per-target
// compile definitions. CountryFlags.cpp -- the obvious neighbour -- lives in
// muleappgui, one static library built once and linked into both aMule and
// amulegui, so it is compiled with neither define and could only ever produce
// the amulegui answer. Compiling per includer is what makes the monolithic
// build take the local-resolver path at all.
//
// The corollary: only include this from sources that are built once per
// variant (the list controls are), never from a shared library like
// muleappgui, or one binary would end up linking two different definitions.
//

#include "amule.h" // Needed for theApp and GEOIP_GUI
#if defined(GEOIP_GUI) && !defined(CLIENT_GUI)
#include "IP2Country.h" // Needed for CIP2Country (monolithic local lookup)
#endif

#include <wx/string.h>

/**
 * The country code to display for one list entry.
 *
 * The core emits its country tag whenever its resolver is enabled -- even as
 * an empty string for an address that resolved to nothing -- so a tag that
 * arrived over EC is authoritative: @a fromCore true means the core knows, and
 * an empty @a coreCode then means "unknown", not "GeoIP is off". Only a build
 * with no such feed (monolithic aMule) falls back to its own resolver, which
 * carries its enabled state itself.
 *
 * Deliberately no thePrefs::IsGeoIPEnabled() on top: on amulegui that pref is
 * only a mirror of the core's, refreshed at prefs-sync time, so consulting it
 * could never do more than disagree with the tag we were handed.
 *
 * @param fromCore Whether the core sent a country tag for this entry.
 * @param coreCode The code it sent (meaningful only when @a fromCore).
 * @param ip       Dotted address, for the monolithic local lookup.
 * @param code     Receives the ISO 3166-1 alpha-2 code; may come back empty
 *                 for an address that did not resolve.
 * @return false when no country is known at all -- render neither icon nor
 *         text.
 */
inline bool GetDisplayCountryCode(bool fromCore, const wxString &coreCode, const wxString &ip, wxString &code)
{
	if (fromCore) {
		code = coreCode;
		return true;
	}
#if defined(GEOIP_GUI) && !defined(CLIENT_GUI)
	// Monolithic aMule resolves locally. amulegui only ever takes the EC path
	// above, and compiling this branch out of it keeps it link-clean of
	// CIP2Country.
	if (theApp->GetIP2Country() && theApp->GetIP2Country()->IsEnabled()) {
		code = theApp->GetIP2Country()->GetCountryCode(ip);
		return true;
	}
#else
	wxUnusedVar(ip);
#endif
	code.Clear();
	return false;
}

#endif // COUNTRYDISPLAY_H
