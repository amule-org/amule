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

#ifndef AMULE_PREFSSEARCH_H
#define AMULE_PREFSSEARCH_H

#include <wx/string.h>
#include <wx/tokenzr.h>

#include <vector>

// Text matching for the search box in the preferences sidebar. The labels it searches are
// translated, so matching ignores case and the common Latin accents: "conexion" finds
// "Conexión" just as "CONNECTION" finds "Connection".
namespace PrefsSearch
{

//! The lowercase base letter of an accented Latin letter, or @p c itself. Covers Latin-1 and the
//! letters of Latin Extended-A that the shipped translations use. Both cases are listed because
//! wxString::Lower() follows the C library's locale, which may only know ASCII.
inline wxUniChar BaseLetter(wxUniChar c)
{
	if (c.IsAscii()) {
		return c;
	}
	// Escapes rather than the letters themselves: the sources are not all compiled as UTF-8.
	static const struct
	{
		const wchar_t *accented;
		char base;
	} letters[] = {
		{ L"\u00e0\u00e1\u00e2\u00e3\u00e4\u00e5\u0101\u0103\u0105"
		  L"\u00c0\u00c1\u00c2\u00c3\u00c4\u00c5\u0100\u0102\u0104",
			'a' },
		{ L"\u00e7\u0107\u0109\u010b\u010d\u00c7\u0106\u0108\u010a\u010c", 'c' },
		{ L"\u010f\u0111\u010e\u0110", 'd' },
		{ L"\u00e8\u00e9\u00ea\u00eb\u0113\u0115\u0117\u0119\u011b"
		  L"\u00c8\u00c9\u00ca\u00cb\u0112\u0114\u0116\u0118\u011a",
			'e' },
		{ L"\u011f\u0121\u0123\u011e\u0120\u0122", 'g' },
		{ L"\u00ec\u00ed\u00ee\u00ef\u0129\u012b\u012d\u012f\u0131"
		  L"\u00cc\u00cd\u00ce\u00cf\u0128\u012a\u012c\u012e\u0130",
			'i' },
		{ L"\u013a\u013c\u013e\u0142\u0139\u013b\u013d\u0141", 'l' },
		{ L"\u00f1\u0144\u0146\u0148\u00d1\u0143\u0145\u0147", 'n' },
		{ L"\u00f2\u00f3\u00f4\u00f5\u00f6\u00f8\u014d\u014f\u0151"
		  L"\u00d2\u00d3\u00d4\u00d5\u00d6\u00d8\u014c\u014e\u0150",
			'o' },
		{ L"\u0155\u0157\u0159\u0154\u0156\u0158", 'r' },
		{ L"\u015b\u015d\u015f\u0161\u015a\u015c\u015e\u0160", 's' },
		{ L"\u0163\u0165\u0162\u0164", 't' },
		{ L"\u00f9\u00fa\u00fb\u00fc\u0169\u016b\u016d\u016f\u0171\u0173"
		  L"\u00d9\u00da\u00db\u00dc\u0168\u016a\u016c\u016e\u0170\u0172",
			'u' },
		{ L"\u00fd\u00ff\u00dd\u0178", 'y' },
		{ L"\u017a\u017c\u017e\u0179\u017b\u017d", 'z' },
	};
	for (const auto &letter : letters) {
		for (const wchar_t *p = letter.accented; *p; ++p) {
			if (c == wxUniChar(*p)) {
				return letter.base;
			}
		}
	}
	return c;
}

//! @p text lowercased, with accented Latin letters reduced to their base letter, and every run of
//! whitespace collapsed to one space.
inline wxString Fold(const wxString &text)
{
	const wxString lower = text.Lower();
	wxString folded;
	folded.reserve(lower.length());
	bool pendingSpace = false;
	for (wxString::const_iterator it = lower.begin(); it != lower.end(); ++it) {
		const wxUniChar c = *it;
		if (wxIsspace(c)) {
			pendingSpace = !folded.empty();
			continue;
		}
		if (pendingSpace) {
			folded += ' ';
			pendingSpace = false;
		}
		folded += BaseLetter(c);
	}
	return folded;
}

//! The words of @p query, folded. Empty when the query is blank.
inline std::vector<wxString> Terms(const wxString &query)
{
	std::vector<wxString> terms;
	wxStringTokenizer tokens(Fold(query), " ", wxTOKEN_STRTOK);
	while (tokens.HasMoreTokens()) {
		terms.push_back(tokens.GetNextToken());
	}
	return terms;
}

//! Whether @p c belongs to a word in a script written with spaces between words, where a search
//! term has to start a word to match: "port" finds "Port" and "ports" but not "supported". Scripts
//! written without them, such as CJK and Thai, have no word starts to anchor to, so a term may
//! match anywhere in them. Decided by code point rather than the C library's classification,
//! which follows a locale that may only know ASCII.
inline bool IsWordLetter(wxUniChar c)
{
	const wxUint32 v = c.GetValue();
	if (v < 0x80) {
		return wxIsalnum(c);
	}
	if (v < 0xC0 || v == 0xD7 || v == 0xF7) {
		return false; // Latin-1 punctuation and symbols
	}
	if ((v >= 0x0E00 && v < 0x0F00) || (v >= 0x2000 && v < 0x2070)) {
		return false; // Thai and Lao; general punctuation
	}
	return v < 0x2E80; // CJK onwards
}

//! Whether folded @p text has @p term (already folded) at the start of a word.
inline bool HasWordPrefix(const wxString &text, const wxString &term)
{
	for (size_t pos = text.find(term); pos != wxString::npos; pos = text.find(term, pos + 1)) {
		if (pos == 0 || !IsWordLetter(text[pos - 1]) || !IsWordLetter(term[0])) {
			return true;
		}
	}
	return false;
}

//! Whether @p text has every one of @p terms, in any order, each at the start of a word. Mnemonic
//! ampersands are part of a label's markup, not its text, so the caller passes GetLabelText(),
//! not GetLabel().
inline bool Matches(const wxString &text, const std::vector<wxString> &terms)
{
	if (terms.empty()) {
		return false;
	}
	const wxString folded = Fold(text);
	for (const wxString &term : terms) {
		if (!HasWordPrefix(folded, term)) {
			return false;
		}
	}
	return true;
}

} // namespace PrefsSearch

#endif // AMULE_PREFSSEARCH_H
