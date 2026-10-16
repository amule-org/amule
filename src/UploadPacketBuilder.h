
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

#ifndef UPLOADPACKETBUILDER_H
#define UPLOADPACKETBUILDER_H

#include "Types.h"
#include <list>
#include <utility>

class CPacket;

// Packet + payload-size pair, used by the static packet-creation helpers. Mirrors eMule's
// CPacketList + Packet::uStatsPayLoad approach; aMule's CPacket has no uStatsPayLoad member, so we
// carry the value alongside the pointer.
typedef std::list<std::pair<CPacket *, uint32>> CPacketList;

namespace UploadPacketBuilder
{
// Called once per packet, including the standard fallback, to preserve packet counters.
using OverheadRecorder = void (*)(uint32);

// Append owning packets, report per-packet overhead, and return the overhead byte total.
// uploadDatarate (bytes/s) scales per-packet chunk size (10 KiB floor, EMBLOCKSIZE ceiling).
uint32 Standard(const uint8_t *buffer,
	uint64 startOffset,
	uint64 endOffset,
	CPacketList &packets,
	const uint8_t *fileHash,
	uint32 uploadDatarate,
	OverheadRecorder recordOverhead = nullptr);
uint32 Packed(const uint8_t *buffer,
	uint64 startOffset,
	uint64 endOffset,
	CPacketList &packets,
	const uint8_t *fileHash,
	uint32 uploadDatarate,
	OverheadRecorder recordOverhead = nullptr);
} // namespace UploadPacketBuilder

#endif
