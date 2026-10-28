//
// This file is part of aMule Project
//
// Copyright (c) 2004-2011 Angel Vidal ( kry@amule.org )
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2003-2011 Barry Dunne ( http://www.emule-project.net )
// Copyright (C)2007-2011 Merkur ( strEmail.Format("%s@%s", "devteam", "emule-project.net") /
// http://www.emule-project.net )

// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either
// version 2 of the License, or (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA

#include "SearchExpression.h"

#include "../kademlia/Indexed.h"
#include "../kademlia/SearchManager.h"
#include "../../Logger.h"
#include "../../MemFile.h"
#include "../../Tag.h"
#include <common/Format.h>

#include <wx/tokenzr.h>

namespace Kademlia
{

static void Free(SSearchTerm *pSearchTerms)
{
	if (pSearchTerms) {
		Free(pSearchTerms->left);
		Free(pSearchTerms->right);
		delete pSearchTerms;
	}
}

static SSearchTerm *CreateSearchExpressionTree(CMemFile &bio, int iLevel, unsigned &nodes)
{
	// the max. depth has to match our own limit for creating the search expression
	// (see also 'ParsedSearchExpression' and 'GetSearchPacket')
	if (iLevel >= 24) {
		AddDebugLogLineN(logKadSearch, "***NOTE: Search expression tree exceeds depth limit!");
		return nullptr;
	}
	iLevel++;
	// eMule and aMule send at most 10 operators plus one per filter, about 45 nodes. Words
	// ANDed together travel in one String node, so long searches stay small.
	if (++nodes > 256) {
		AddDebugLogLineN(logKadSearch, "***NOTE: Search expression tree has too many nodes!");
		return nullptr;
	}

	uint8_t op = bio.ReadUInt8();
	if (op == 0x00) {
		uint8_t boolop = bio.ReadUInt8();
		// The recursive children below read from `bio` and can throw CEOFException on a
		// truncated packet. Without a guard the parent node and the already-built left
		// subtree leak during unwinding: ~SSearchTerm does not recurse into left/right,
		// only the explicit Free() walk does, and that is only reached on the success and
		// NULL-return paths (#884).
		if (boolop == 0x00) { // AND
			SSearchTerm *pSearchTerm = new SSearchTerm;
			pSearchTerm->type = SSearchTerm::AND;
			try {
				if ((pSearchTerm->left = CreateSearchExpressionTree(bio, iLevel, nodes)) ==
					nullptr) {
					delete pSearchTerm;
					return nullptr;
				}
				if ((pSearchTerm->right = CreateSearchExpressionTree(bio, iLevel, nodes)) ==
					nullptr) {
					Free(pSearchTerm->left);
					delete pSearchTerm;
					return nullptr;
				}
			} catch (...) {
				Free(pSearchTerm);
				throw;
			}
			return pSearchTerm;
		} else if (boolop == 0x01) { // OR
			SSearchTerm *pSearchTerm = new SSearchTerm;
			pSearchTerm->type = SSearchTerm::OR;
			try {
				if ((pSearchTerm->left = CreateSearchExpressionTree(bio, iLevel, nodes)) ==
					nullptr) {
					delete pSearchTerm;
					return nullptr;
				}
				if ((pSearchTerm->right = CreateSearchExpressionTree(bio, iLevel, nodes)) ==
					nullptr) {
					Free(pSearchTerm->left);
					delete pSearchTerm;
					return nullptr;
				}
			} catch (...) {
				Free(pSearchTerm);
				throw;
			}
			return pSearchTerm;
		} else if (boolop == 0x02) { // NOT
			SSearchTerm *pSearchTerm = new SSearchTerm;
			pSearchTerm->type = SSearchTerm::NOT;
			try {
				if ((pSearchTerm->left = CreateSearchExpressionTree(bio, iLevel, nodes)) ==
					nullptr) {
					delete pSearchTerm;
					return nullptr;
				}
				if ((pSearchTerm->right = CreateSearchExpressionTree(bio, iLevel, nodes)) ==
					nullptr) {
					Free(pSearchTerm->left);
					delete pSearchTerm;
					return nullptr;
				}
			} catch (...) {
				Free(pSearchTerm);
				throw;
			}
			return pSearchTerm;
		} else {
			AddDebugLogLineN(logKadSearch,
				CFormat("*** Unknown boolean search operator 0x%02x "
					"(CreateSearchExpressionTree)") %
					boolop);
			return nullptr;
		}
	} else if (op == 0x01) { // String
		wxString str(bio.ReadString(true));

		// Make lowercase, the search code expects lower case strings!
		str.MakeLower();

		SSearchTerm *pSearchTerm = new SSearchTerm;
		pSearchTerm->type = SSearchTerm::String;
		pSearchTerm->astr = new wxArrayString;

		// pre-tokenize the string term
		// #warning TODO: TokenizeOptQuotedSearchTerm
		wxStringTokenizer token(str, CSearchManager::GetInvalidKeywordChars(), wxTOKEN_DEFAULT);
		while (token.HasMoreTokens()) {
			wxString strTok(token.GetNextToken());
			if (!strTok.IsEmpty()) {
				pSearchTerm->astr->Add(strTok);
			}
		}

		return pSearchTerm;
	} else if (op == 0x02) { // Meta tag
		wxString strValue(bio.ReadString(true));
		// Honest values are short ("avi", "Video"); a long one only adds compare work.
		if (strValue.length() > 256) {
			return nullptr;
		}
		// Make lowercase, the search code expects lower case strings!
		strValue.MakeLower();

		wxString strTagName = bio.ReadString(false);

		SSearchTerm *pSearchTerm = new SSearchTerm;
		pSearchTerm->type = SSearchTerm::MetaTag;
		pSearchTerm->tag = new CTagString(strTagName, strValue);
		return pSearchTerm;
	} else if (op == 0x03 || op == 0x08) { // Numeric relation (0x03=32-bit or 0x08=64-bit)
		static const struct
		{
			SSearchTerm::ESearchTermType eSearchTermOp;
			wxString pszOp;
		} _aOps[] = {
			{ SSearchTerm::OpEqual, "=" },         // mmop=0x00
			{ SSearchTerm::OpGreater, ">" },       // mmop=0x01
			{ SSearchTerm::OpLess, "<" },          // mmop=0x02
			{ SSearchTerm::OpGreaterEqual, ">=" }, // mmop=0x03
			{ SSearchTerm::OpLessEqual, "<=" },    // mmop=0x04
			{ SSearchTerm::OpNotEqual, "<>" }      // mmop=0x05
		};

		uint64_t ullValue = (op == 0x03) ? bio.ReadUInt32() : bio.ReadUInt64();

		uint8_t mmop = bio.ReadUInt8();
		if (mmop >= itemsof(_aOps)) {
			AddDebugLogLineN(logKadSearch,
				CFormat("*** Unknown integer search op=0x%02x (CreateSearchExpressionTree)") %
					mmop);
			return nullptr;
		}

		wxString strTagName = bio.ReadString(false);

		SSearchTerm *pSearchTerm = new SSearchTerm;
		pSearchTerm->type = _aOps[mmop].eSearchTermOp;
		pSearchTerm->tag = new CTagVarInt(strTagName, ullValue);

		return pSearchTerm;
	} else {
		AddDebugLogLineN(logKadSearch,
			CFormat("*** Unknown search op=0x%02x (CreateSearchExpressionTree)") % op);
		return nullptr;
	}
}

SSearchTerm *ParseSearchExpression(CMemFile &bio)
{
	unsigned nodes = 0;
	return CreateSearchExpressionTree(bio, 0, nodes);
}

void FreeSearchExpression(SSearchTerm *term)
{
	Free(term);
}

} // namespace Kademlia
