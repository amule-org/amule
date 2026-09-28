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

#ifndef ECPREFSDIFF_H
#define ECPREFSDIFF_H

#include <ec/cpp/ECPacket.h> // Needed for CECPacket

#include <memory>

/**
 * Builds an EC_OP_SET_PREFERENCES packet holding only what `current` changes against `base`.
 *
 * Both are CEC_Prefs_Packet snapshots at EC_DETAIL_UPDATE, where a boolean is a bare tag that
 * means "on" by being there. The result is EC_DETAIL_FULL, where the core leaves an absent tag
 * alone, so every boolean in it carries an explicit value. The result has no tags when nothing
 * changed.
 */
std::unique_ptr<CECPacket> MakePrefsDiffPacket(const CECPacket &base, const CECPacket &current);

#endif // ECPREFSDIFF_H
