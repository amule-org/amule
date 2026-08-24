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

#include "ClientVersionString.h"

#include <common/Format.h>
#include <protocol/ed2k/ClientSoftware.h>

#include <wx/string.h>

wxString FormatClientVersion(uint32 clientSoft, uint32 major, uint32 minor, uint32 update)
{
	switch (clientSoft) {
	case SO_AMULE:
	case SO_LXMULE:
	case SO_HYDRANODE:
	case SO_MLDONKEY:
	case SO_NEW_MLDONKEY:
	case SO_NEW2_MLDONKEY:
		// The mule/mldonkey family versions itself with three numbers.
		return CFormat(wxT("v%u.%u.%u")) % major % minor % update;

	case SO_EDONKEYHYBRID:
		// Three numbers as well, but it drops a zero update rather than
		// printing it.
		if (update == 0) {
			return CFormat(wxT("v%u.%u")) % major % minor;
		}
		return CFormat(wxT("v%u.%u.%u")) % major % minor % update;

	case SO_LPHANT:
		// Counts its major one higher than the wire does, pads the minor to
		// two digits, and letters the update from 'a'.
		return CFormat(wxT(" v%u.%.2u%c")) % (major - 1) % minor % ('a' + update);

	case SO_EMULEPLUS: {
		// Omits whichever trailing components are zero, and letters the
		// update from 'a' meaning 1 -- unlike eMule, where 'a' means 0.
		wxString out = CFormat(wxT("v%u")) % major;
		if (minor != 0) {
			out += CFormat(wxT(".%u")) % minor;
		}
		if (update != 0) {
			out += CFormat(wxT("%c")) % ('a' + update - 1);
		}
		return out;
	}

	default:
		// eMule and everything modelled on it: the update is a letter, 'a'
		// meaning 0. This is the case the history rows used to render as a
		// digit.
		return CFormat(wxT("v%u.%u%c")) % major % minor % ('a' + update);
	}
}

wxString FormatPackedClientVersion(uint32 clientSoft, uint32 packedVersion)
{
	// MAKE_CLIENT_VERSION is a decimal composite, not a bitfield:
	// major * 100000 + minor * 1000 + update * 100.
	return FormatClientVersion(clientSoft,
		packedVersion / 100000,
		(packedVersion % 100000) / 1000,
		(packedVersion % 1000) / 100);
}
