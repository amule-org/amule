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

#ifndef CLIENTNAMECELL_H
#define CLIENTNAMECELL_H

#include <map>
#include <tuple>
#include <vector>

#include <wx/bmpbndl.h>
#include <wx/gdicmn.h> // Needed for wxRect
#include <wx/string.h>

#include "Types.h"

class CUpDownClient;
class wxDC;
class wxBitmap;
class wxColour;

//! The client status icons, indexed by ClientSkinEnum.
class CClientImages
{
public:
	void Add(const wxBitmapBundle &bundle) { m_bundles.push_back(bundle); }
	void Clear()
	{
		m_bundles.clear();
		m_bitmaps.clear();
		m_tinted.clear();
	}
	const std::vector<wxBitmapBundle> &GetBundles() const { return m_bundles; }
	wxBitmapBundle GetBundle(size_t index) const
	{
		return index < m_bundles.size() ? m_bundles[index] : wxBitmapBundle();
	}

	/**
	 * The icon as drawn on a DC: logicalSize in its drawing coordinates, contentScale its
	 * backing scale. Cached, because setting a bitmap's scale factor may copy its pixels.
	 */
	wxBitmap GetBitmap(size_t index, const wxSize &logicalSize, double contentScale) const;
	//! The same, every pixel recoloured to `tint` with its alpha kept: for one-colour glyphs.
	wxBitmap GetTintedBitmap(
		size_t index, const wxSize &logicalSize, double contentScale, const wxColour &tint) const;

private:
	std::vector<wxBitmapBundle> m_bundles;
	mutable std::map<std::tuple<size_t, int, int, double>, wxBitmap> m_bitmaps;
	mutable std::map<std::tuple<size_t, int, int, double, wxUint32>, wxBitmap> m_tinted;
};

/**
 * Everything the User Name cell draws, as values.
 *
 * A snapshot rather than a client pointer because the two lists drawing this cell reach their peers
 * differently: the per-file lists hold an owning CClientRef and could be asked at paint time, while
 * the global clients list copies each peer's values once per sweep precisely so nothing it paints
 * can be freed underneath it. A value type is the only input both can produce, and it keeps the
 * drawing free of any lifetime question.
 */
struct ClientNameCell
{
	wxString name;
	//! Two-letter code, or empty to draw no flag. Already through GetDisplayCountryCode(), so
	//! empty means "draw nothing" whether the feature is off or the country is unknown.
	wxString countryCode;
	uint8 downloadState = 0; //!< DS_*
	uint8 clientSoft = 0;    //!< SO_*
	uint8 obfuscation = 0;   //!< OBST_*
	//! An A4AF source is drawn grey whatever its download state says.
	bool a4af = false;
	/**
	 * Draw the download-state badge. False for a row describing a peer we are not talking to --
	 * the history list -- where there is no live state and a badge could only invent one.
	 */
	bool showState = true;
	//! False when the software is simply not recorded, so it draws as unknown
	//! rather than as whatever SO_* value zero happens to mean (eMule).
	bool knownSoftware = true;
	bool remoteQueueFull = false;
	bool isFriend = false;
	bool identified = false;
	//! Failed signature verification.
	bool identFailed = false;
	//! Identity bound to a different peer address.
	bool badGuy = false;
	bool extProtocol = true;
	//! Score ratio above 1, i.e. we owe this peer.
	bool highCredits = false;

	/**
	 * Whole-struct comparison, so a caller deciding "has this cell changed" cannot test a
	 * subset by accident.
	 *
	 * The badges move independently of the name: secure identification completes after the
	 * hello, the credit ratio crosses 1 mid-session, obfuscation and bad-guy state can change.
	 * A test written against name and address alone freezes all of them.
	 */
	bool operator==(const ClientNameCell &other) const
	{
		return name == other.name && countryCode == other.countryCode &&
		       downloadState == other.downloadState && clientSoft == other.clientSoft &&
		       obfuscation == other.obfuscation && a4af == other.a4af &&
		       showState == other.showState && knownSoftware == other.knownSoftware &&
		       remoteQueueFull == other.remoteQueueFull && isFriend == other.isFriend &&
		       identified == other.identified && identFailed == other.identFailed &&
		       badGuy == other.badGuy && extProtocol == other.extProtocol &&
		       highCredits == other.highCredits;
	}
	bool operator!=(const ClientNameCell &other) const { return !(*this == other); }
};

//! Snapshot a live peer. Safe to call only while `client` is known alive.
ClientNameCell MakeClientNameCell(const CUpDownClient *client, bool a4af = false);

//! Width of the same badge cluster, flag and text, in the DC's logical coordinates.
int MeasureClientNameCell(const ClientNameCell &cell, wxDC &dc);

/**
 * Draw the badge cluster, the optional country flag and the name.
 *
 * Left to right: the download-state icon, the client icon, three badge positions (secure
 * identification, obfuscation, credits), the flag and the name. Empty badge positions stay
 * reserved, so names line up and each badge kind keeps its own column. "No extended protocol"
 * has no badge: only ClientNameCellToolTip() reports it.
 */
void DrawClientNameCell(const ClientNameCell &cell, const wxRect &rect, wxDC *dc);

//! The cell's badges in words, one per line; empty when it shows none.
wxString ClientNameCellToolTip(const ClientNameCell &cell);

// Resource-explicit forms share the production layout and drawing without
// needing a running application or live client. Flag is already DPI-scaled.
int MeasureClientNameCell(
	const ClientNameCell &cell, wxDC &dc, const CClientImages &images, const wxBitmap &flag);
void DrawClientNameCell(const ClientNameCell &cell,
	const wxRect &rect,
	wxDC &dc,
	const CClientImages &images,
	const wxBitmap &flag);

#endif // CLIENTNAMECELL_H
// File_checked_for_headers
