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
 * Last Verify Local Data result (FT_VERIFY_* in known.met). date == 0: never verified. Kept apart
 * from CKnownFile so the encoding can be unit tested.
 */
class CVerifyLocalDataResult
{
public:
	typedef std::vector<uint16> PartList;
	// Per corrupt part: its number and the corrupt AICH blocks (EMBLOCKSIZE) within it.
	typedef std::vector<std::pair<uint16, std::vector<uint8>>> BlockList;

	uint32 date = 0;

	const PartList &CorruptedMD4() const { return m_corruptedMD4; }
	const BlockList &CorruptedAICH() const { return m_corruptedAICH; }
	bool IsCorrupt() const { return !m_corruptedMD4.empty() || !m_corruptedAICH.empty(); }

	// FT_VERIFY_CORRUPTMD4: "p,p,p", the FT_CORRUPTEDPARTS format. Empty when no part is corrupt.
	const wxString &EncodedMD4() const { return m_encodedMD4; }
	// FT_VERIFY_CORRUPTAICH: "p:b.b;p:b". Empty when no block is corrupt.
	const wxString &EncodedAICH() const { return m_encodedAICH; }

	void SetCorrupted(const PartList &md4, const BlockList &aich)
	{
		m_corruptedMD4 = md4;
		m_corruptedAICH = aich;
		Encode();
	}

	// The notation of the log report: "p: (b,b), p: (b)".
	wxString FormatCorruptedAICH() const
	{
		wxString str;
		for (const auto &part : m_corruptedAICH) {
			wxString blocks;
			for (uint8 block : part.second) {
				blocks += CFormat("%s%u") % (blocks.IsEmpty() ? "" : ",") % (unsigned)block;
			}
			str += CFormat("%s%u: (%s)") % (str.IsEmpty() ? "" : ", ") % part.first % blocks;
		}
		return str;
	}

	// Merges an EC update, which carries only the tags that changed (nullptr: not in it).
	// Returns whether it carried any.
	bool ApplyUpdate(const uint32 *newDate, const wxString *md4, const wxString *aich, uint64 fileSize)
	{
		if (!newDate && !md4 && !aich) {
			return false;
		}
		if (newDate) {
			date = *newDate;
		}
		// Copies: DecodeCorrupted() rewrites the members these would otherwise alias.
		const wxString keptMD4 = m_encodedMD4;
		const wxString keptAICH = m_encodedAICH;
		DecodeCorrupted(md4 ? *md4 : keptMD4, aich ? *aich : keptAICH, fileSize);
		return true;
	}

	// Replaces both lists. Out-of-range, repeated or malformed entries are dropped.
	void DecodeCorrupted(const wxString &md4, const wxString &aich, uint64 fileSize)
	{
		m_corruptedMD4.clear();
		m_corruptedAICH.clear();
		const uint64 partCount = (fileSize + PARTSIZE - 1) / PARTSIZE;

		wxStringTokenizer parts(md4, ",");
		while (parts.HasMoreTokens()) {
			unsigned long part;
			if (parts.GetNextToken().ToULong(&part) && part < partCount &&
				std::find(m_corruptedMD4.begin(), m_corruptedMD4.end(), part) ==
					m_corruptedMD4.end()) {
				m_corruptedMD4.push_back(part);
			}
		}

		wxStringTokenizer aichParts(aich, ";");
		while (aichParts.HasMoreTokens()) {
			const wxString token = aichParts.GetNextToken();
			unsigned long part;
			if (token.Find(':') == wxNOT_FOUND || !token.BeforeFirst(':').ToULong(&part) ||
				part >= partCount ||
				std::any_of(m_corruptedAICH.begin(),
					m_corruptedAICH.end(),
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
				m_corruptedAICH.emplace_back(part, blocks);
			}
		}
		Encode();
	}

private:
	// Encoded once per change, not per use: EC sends the strings on every update of a
	// verified file. Private with the lists, so nothing can change one without the other.
	void Encode()
	{
		m_encodedMD4.clear();
		for (uint16 part : m_corruptedMD4) {
			if (!m_encodedMD4.IsEmpty()) {
				m_encodedMD4 += ",";
			}
			m_encodedMD4 += CFormat("%u") % part;
		}
		m_encodedAICH.clear();
		for (const auto &part : m_corruptedAICH) {
			if (!m_encodedAICH.IsEmpty()) {
				m_encodedAICH += ";";
			}
			m_encodedAICH += CFormat("%u:") % part.first;
			for (size_t i = 0; i < part.second.size(); ++i) {
				if (i) {
					m_encodedAICH += ".";
				}
				m_encodedAICH += CFormat("%u") % part.second[i];
			}
		}
	}

	PartList m_corruptedMD4;
	BlockList m_corruptedAICH;
	wxString m_encodedMD4;
	wxString m_encodedAICH;
};

#endif // VERIFYLOCALDATARESULT_H
// File_checked_for_headers
