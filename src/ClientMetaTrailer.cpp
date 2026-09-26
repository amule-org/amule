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

#include "ClientMetaTrailer.h" // Interface declarations

#include <common/Format.h> // Needed for CFormat

#include "Logger.h"   // Needed for AddDebugLogLineN
#include "SafeFile.h" // Needed for CFileDataIO

#include <cstring>

namespace
{
/**
 * Appended after the fixed 119-byte credit records rather than folded into them, and the file
 * version stays at CREDITFILE_VERSION on purpose. Bumping that version is what would break
 * compatibility, not adding data: an older aMule meeting a version it does not know logs
 * "Creditfile is outdated and will be replaced" and returns without loading a single record, and
 * the next save then overwrites the file -- every credit gone, on nothing worse than running a
 * previous build once. The same byte is also the format shared with the eMule lineage.
 *
 * The loader reads exactly `count` records and stops; it never checks the file length and never
 * looks further. So anything after the last record is invisible to every reader that predates this,
 * and they keep loading the credits they always did. The cost is that such a reader's *save* drops
 * the trailer, losing the metadata but never the credits.
 *
 * The magic makes presence unambiguous, since absence is the normal state for a file last written
 * by an older build.
 */
const char kMetaMagic[8] = { 'A', 'M', 'U', 'L', 'E', 'M', 'D', '1' };
const uint8 kMetaVersion = 1;
//! Names are attacker-supplied; cap what we store rather than what we read. Counted in characters,
//! because that is what wxString::Left() takes -- a multi-byte name can occupy more bytes on disk,
//! which the uint16 length field has ample room for. Storage sanity, not a bound the format depends
//! on.
const size_t kMetaMaxNameChars = 64;
} // namespace

namespace ClientMetaTrailer
{
void Write(CFileDataIO &file, const std::vector<Entry> &entries)
{
	if (entries.empty()) {
		return;
	}

	file.Write(kMetaMagic, sizeof(kMetaMagic));
	file.WriteUInt8(kMetaVersion);
	file.WriteUInt32(static_cast<uint32>(entries.size()));

	for (const Entry &entry : entries) {
		const ClientMetaStruct &meta = entry.meta;
		file.WriteHash(entry.key);
		file.WriteUInt32(meta.firstSeen);
		file.WriteUInt32(meta.sessions);
		file.WriteUInt32(meta.lastIP);
		file.WriteUInt16(meta.lastPort);
		file.WriteUInt16(meta.kadPort);
		file.WriteUInt32(meta.version);
		file.WriteUInt8(meta.clientSoft);
		file.WriteUInt8(meta.sourceFrom);
		file.WriteUInt8(meta.obfuscation);
		// Two length bytes because CFileDataIO supports 0, 2 or 4 and rejects
		// anything else: a 1-byte prefix throws "Invalid length for string-length
		// field" from inside the write, which the caller's handler would swallow,
		// leaving a trailer truncated mid-entry. The cap is enforced here, not by
		// the field width.
		file.WriteString(meta.name.Left(kMetaMaxNameChars), utf8strRaw, sizeof(uint16));
	}
}

ReadResult Read(CFileDataIO &file, std::vector<Entry> &entries)
{
	// Optional by construction: a file written by any older build simply ends after the last
	// record. Everything here is best-effort -- the credits are already loaded and must survive
	// whatever this finds, so a trailer that is absent, truncated or unrecognised is dropped
	// rather than treated as corruption.
	entries.clear();
	try {
		const uint64 remaining = file.GetLength() - file.GetPosition();
		if (remaining < sizeof(kMetaMagic) + 1 + 4) {
			return READ_ABSENT;
		}

		char magic[sizeof(kMetaMagic)];
		file.Read(magic, sizeof(magic));
		if (memcmp(magic, kMetaMagic, sizeof(magic)) != 0) {
			AddDebugLogLineN(logCredits, "Trailing data in clients.met is not a metadata block");
			return READ_FOREIGN;
		}
		const uint8 version = file.ReadUInt8();
		if (version != kMetaVersion) {
			// A newer aMule wrote it. Same reasoning as the file version: do not guess
			// at a layout we do not know, just leave the metadata behind. The credits
			// are unaffected either way.
			AddDebugLogLineN(logCredits,
				CFormat("clients.met metadata is version %u, expected %u -- ignoring") %
					version % kMetaVersion);
			return READ_OTHER_VERSION;
		}

		const uint32 count = file.ReadUInt32();
		for (uint32 i = 0; i < count; i++) {
			Entry entry;
			ClientMetaStruct &meta = entry.meta;
			entry.key = file.ReadHash();
			meta.firstSeen = file.ReadUInt32();
			meta.sessions = file.ReadUInt32();
			meta.lastIP = file.ReadUInt32();
			meta.lastPort = file.ReadUInt16();
			meta.kadPort = file.ReadUInt16();
			meta.version = file.ReadUInt32();
			meta.clientSoft = file.ReadUInt8();
			meta.sourceFrom = file.ReadUInt8();
			meta.obfuscation = file.ReadUInt8();
			meta.name = file.ReadString(true, sizeof(uint16));
			entries.push_back(entry);
		}
		return READ_OK;
	} catch (const CSafeIOException &e) {
		// Truncated or malformed: whatever was read before stays usable.
		AddDebugLogLineN(logCredits, "Could not read clients.met metadata: " + e.what());
		return READ_DAMAGED;
	}
}
} // namespace ClientMetaTrailer
// File_checked_for_headers
