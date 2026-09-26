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

#ifndef VERIFYLOCALDATARESULT_H
#define VERIFYLOCALDATARESULT_H

#include "Types.h"                   // Needed for uint16, uint64, ...
#include <protocol/ed2k/Constants.h> // Needed for PARTSIZE, EMBLOCKSIZE
#include <common/Format.h>           // Needed for CFormat

#include <wx/string.h>
#include <wx/tokenzr.h> // Needed for wxStringTokenizer

#include <algorithm>
#include <utility>
#include <vector>

/**
 * Outcome of the last completed "Verify Local Data" check on a complete file, persisted in
 * known.met (FT_VERIFY_*, see FileTags.h). date == 0 means never verified; a date with both lists
 * empty means the file verified clean then.
 *
 * Kept apart from CKnownFile, with its known.met string encoding, so the codec can be unit tested:
 * CKnownFile reaches theApp and cannot be linked into a test.
 */
struct CVerifyLocalDataResult
{
	typedef std::vector<uint16> PartList;
	// Per corrupt part: its number and the corrupt AICH blocks (EMBLOCKSIZE) within it.
	typedef std::vector<std::pair<uint16, std::vector<uint8>>> BlockList;

	uint32 date = 0;
	PartList corruptedMD4;
	BlockList corruptedAICH;

	bool IsCorrupt() const { return !corruptedMD4.empty() || !corruptedAICH.empty(); }

	// FT_VERIFY_CORRUPTMD4: "p,p,p", the FT_CORRUPTEDPARTS format. Empty when no part is corrupt.
	wxString EncodeCorruptedMD4() const
	{
		wxString str;
		for (uint16 part : corruptedMD4) {
			if (!str.IsEmpty()) {
				str += ",";
			}
			str += CFormat("%u") % part;
		}
		return str;
	}

	// FT_VERIFY_CORRUPTAICH: "p:b.b;p:b". Empty when no block is corrupt.
	wxString EncodeCorruptedAICH() const
	{
		wxString str;
		for (const auto &part : corruptedAICH) {
			if (!str.IsEmpty()) {
				str += ";";
			}
			str += CFormat("%u:") % part.first;
			for (size_t i = 0; i < part.second.size(); ++i) {
				if (i) {
					str += ".";
				}
				str += CFormat("%u") % part.second[i];
			}
		}
		return str;
	}

	/**
	 * Replaces both lists with the ones parsed from their known.met strings. Validated like
	 * FT_CORRUPTEDPARTS in a .part.met: a part or block outside a file of fileSize bytes, a
	 * repeat, or a malformed entry is dropped rather than trusted.
	 */
	void DecodeCorrupted(const wxString &md4, const wxString &aich, uint64 fileSize)
	{
		corruptedMD4.clear();
		corruptedAICH.clear();
		const uint64 partCount = (fileSize + PARTSIZE - 1) / PARTSIZE;

		wxStringTokenizer parts(md4, ",");
		while (parts.HasMoreTokens()) {
			unsigned long part;
			if (parts.GetNextToken().ToULong(&part) && part < partCount &&
				std::find(corruptedMD4.begin(), corruptedMD4.end(), part) ==
					corruptedMD4.end()) {
				corruptedMD4.push_back(part);
			}
		}

		wxStringTokenizer aichParts(aich, ";");
		while (aichParts.HasMoreTokens()) {
			const wxString token = aichParts.GetNextToken();
			unsigned long part;
			if (token.Find(':') == wxNOT_FOUND || !token.BeforeFirst(':').ToULong(&part) ||
				part >= partCount ||
				std::any_of(corruptedAICH.begin(),
					corruptedAICH.end(),
					[part](const BlockList::value_type &e) { return e.first == part; })) {
				continue;
			}
			const uint64 partSize = std::min<uint64>(PARTSIZE, fileSize - part * PARTSIZE);
			const uint64 blockCount = (partSize + EMBLOCKSIZE - 1) / EMBLOCKSIZE;
			std::vector<uint8> blocks;
			wxStringTokenizer blockTokens(token.AfterFirst(':'), ".");
			while (blockTokens.HasMoreTokens()) {
				unsigned long block;
				if (blockTokens.GetNextToken().ToULong(&block) && block < blockCount &&
					std::find(blocks.begin(), blocks.end(), block) == blocks.end()) {
					blocks.push_back(block);
				}
			}
			if (!blocks.empty()) {
				corruptedAICH.emplace_back(part, blocks);
			}
		}
	}
};

#endif // VERIFYLOCALDATARESULT_H
// File_checked_for_headers
