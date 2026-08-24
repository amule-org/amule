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

#ifndef CLIENTVERSIONSTRING_H
#define CLIENTVERSIONSTRING_H

#include "Types.h"

class wxString;

/**
 * Render a client's version the way that client's own project writes it.
 *
 * The conventions differ per software and are not cosmetic: eMule spells its
 * update component as a letter (0.70b, not 0.70.1), eMule Plus omits zero
 * components, lPhant counts its major from one higher than the wire does, and
 * the mule/mldonkey family really is three numbers.
 *
 * This exists because the rendering was reachable from two directions that had
 * drifted apart. The handshake built the string from locals and got it right;
 * the client-history rows re-derived it from the stored numeric with a fixed
 * `v%u.%u.%u`, which silently dropped every convention above. The same peer
 * could therefore be listed twice in one window under two different versions,
 * depending only on whether it happened to be online (amule-org/amule#1127).
 *
 * `major`, `minor` and `update` are the decoded components, not the packed
 * value MAKE_CLIENT_VERSION produces.
 */
wxString FormatClientVersion(uint32 clientSoft, uint32 major, uint32 minor, uint32 update);

/**
 * As above, from the packed form MAKE_CLIENT_VERSION produces and
 * CreditStruct/ClientMetaStruct stores, which is a decimal composite rather
 * than a bitfield.
 */
wxString FormatPackedClientVersion(uint32 clientSoft, uint32 packedVersion);

#endif // CLIENTVERSIONSTRING_H
