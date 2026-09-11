//
// This file is part of the aMule Project.
//
// Copyright (c) 2004-2011 Angel Vidal ( kry@amule.org )
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

#ifndef STRING_FUNCTIONS_H
#define STRING_FUNCTIONS_H

#include "../../Types.h" // Needed for uint16 and uint32

// UTF8 types: No UTF8, BOM prefix, or Raw UTF8
enum EUtf8Str
{
	utf8strNone,
	utf8strOptBOM,
	utf8strRaw
};

/**
 * Unicode <-> (char *) and UTF-8 conversion.
 *
 * Do NOT store pointers returned by unicode2char(): they are freed as soon as
 * cWX2MB's return value goes out of scope. Hold a wxCharBuffer instead, which
 * frees on scope exit:
 *
 *   const wxCharBuffer buf(unicode2char(aWxString));
 *   printf("%s", (const char *)buf);
 *
 * The cast is needed because varargs do not apply the implicit conversion.
 *
 * Use Unicode2CharBuf / Char2UnicodeBuf, and do not declare them const or the
 * compiler complains about a double const.
 */
typedef const wxCharBuffer Unicode2CharBuf;
typedef const wxWCharBuffer Char2UnicodeBuf;

Unicode2CharBuf unicode2char(const wxChar *x);
Unicode2CharBuf unicode2char(const Char2UnicodeBuf &x);
inline Unicode2CharBuf unicode2char(const wxString &x)
{
	return unicode2char(x.wc_str());
}
inline Char2UnicodeBuf char2unicode(const char *x)
{
	return wxConvLibc.cMB2WX(x);
}

inline Unicode2CharBuf unicode2UTF8(const wxChar *x)
{
	return wxConvUTF8.cWX2MB(x);
}
inline Unicode2CharBuf unicode2UTF8(const Char2UnicodeBuf &x)
{
	return wxConvUTF8.cWX2MB(x);
}
inline Unicode2CharBuf unicode2UTF8(const wxString &x)
{
	return x.utf8_str();
}
inline Char2UnicodeBuf UTF82unicode(const char *x)
{
	return wxConvUTF8.cMB2WX(x);
}

inline const wxCharBuffer char2UTF8(const char *x)
{
	return unicode2UTF8(char2unicode(x));
}
inline const wxCharBuffer UTF82char(const char *x)
{
	return unicode2char(UTF82unicode(x));
}

inline Unicode2CharBuf filename2char(const wxChar *x)
{
	return wxConvFileName->cWC2MB(x);
}
inline Unicode2CharBuf filename2char(const wxString &x)
{
	return x.mb_str(*wxConvFileName);
}
inline Char2UnicodeBuf char2filename(const char *x)
{
	return wxConvFileName->cMB2WC(x);
}

// Replaces "&" with "&&" in 'in' for use with text-labels
inline wxString MakeStringEscaped(wxString in)
{
	in.Replace("&", "&&");
	return in;
}

// Render a string suitable for one log line.
//
// Control characters (< 0x20) and DEL become \xHH escapes, so hostile input
// cannot break log framing or inject entries into a downstream collector.
// Printable ASCII and anything >= 0x80 passes through untouched.
inline wxString EscapeForLog(const wxString &in)
{
	wxString out;
	out.reserve(in.length());
	for (size_t i = 0; i < in.length(); ++i) {
		wxChar c = in[i];
		if ((c >= 0 && c < 0x20) || c == 0x7f) {
			out += wxString::Format("\\x%02x", static_cast<unsigned>(c));
		} else {
			out += c;
		}
	}
	return out;
}

// Make a string be a folder
inline wxString MakeFoldername(wxString path)
{

	if (!path.IsEmpty() && (path.Right(1) == '/')) {
		path.RemoveLast();
	}

	return path;
}

// Duplicates a string
inline char *nstrdup(const char *src)
{
	size_t len = (src ? strlen(src) : 0) + 1;
	char *res = new char[len];
	if (src)
		strcpy(res, src);
	res[len - 1] = 0;
	return res;
}

// atoi/atol without converting through unicode2char. Returns the value in
// the string, or 0 if conversion failed.
inline long StrToLong(const wxString &str)
{
	long value = 0;
	if (!str.ToLong(&value)) { // value may be changed even if it fails according to wx docu
		value = 0;
	}
	return value;
}

inline unsigned long StrToULong(const wxString &str)
{
	unsigned long value = 0;
	if (!str.ToULong(&value)) {
		value = 0;
	}
	return value;
}

inline unsigned long long StrToULongLong(const wxString &str)
{
	unsigned long long value = 0;
	if (!str.ToULongLong(&value)) {
		value = 0;
	}
	return value;
}

inline size_t GetRawSize(const wxString &rstr, EUtf8Str eEncode)
{
	size_t RealLen = 0;
	switch (eEncode) {
	case utf8strOptBOM:
		RealLen = 3;
	/* fall through */
	case utf8strRaw: {
		Unicode2CharBuf s(unicode2UTF8(rstr));
		if (s) {
			RealLen += strlen(s);
			break;
		} else {
			RealLen = 0;
		}
	}
	/* fall through */
	default: {
		Unicode2CharBuf s(unicode2char(rstr));
		if (s) {
			RealLen = strlen(s);
		}
	}
	}

	return RealLen;
}

// Makes sIn suitable for inclusion in an URL, by escaping all chars that could cause trouble.
wxString URLEncode(const wxString &sIn);

/**
 * Converts a hexadecimal number of at most 2 digits to a char.
 *
 * @return The char, or \0 if conversion failed.
 */
wxChar HexToDec(const wxString &hex);

/**
 * Converts all valid HTML escape-codes to their corresponding chars.
 */
wxString UnescapeHTML(const wxString &str);

/**
 * Restores the '|' delimiters of an eD2k link that were percent-encoded.
 *
 * Chromium refuses to hand over an ed2k:// URL containing a literal '|' at all,
 * so sites publish the encoded spelling and that is what reaches the clipboard.
 * Only the delimiters are touched: a filename keeps whatever escapes it arrived
 * with, and CED2KFileLink unescapes it once the link tokenizes.
 */
wxString RestoreEncodedPipes(const wxString &link);

/**
 * Escapes the chars needed to make the url valid.
 */
wxString validateURI(const wxString &url);

/**
 * Compares two strings, taking numerals into consideration.
 *
 * Splits both into fields on whitespace and non-alphanumerics, converts
 * numerals to integers, then compares field by field, so "a (2)" sorts before
 * "a (10)". Floats become two fields; negative numbers are not handled.
 *
 * @return -1 if a < b, 1 if a > b, 0 if equal.
 */
int FuzzyStrCmp(const wxString &a, const wxString &b);

/**
 * As FuzzyStrCmp, but case insensitive.
 */
int FuzzyStrCaseCmp(const wxString &a, const wxString &b);

class CSimpleTokenizer
{
public:
	CSimpleTokenizer(const wxString &str, wxChar delim);

	/**
	 * The next part of the string, empty once fully tokenized. Empty tokens
	 * are returned too.
	 */
	wxString next();

	/**
	 * The part after the last token, or empty once fully tokenized. Before the
	 * first next() call, the whole string.
	 */
	wxString remaining() const;

	size_t tokenCount() const;

private:
	//! The string being tokenized.
	wxString m_string;

	//! The delimiter used to split the string.
	wxChar m_delim;

	//! A pointer to the current position in the string.
	const wxChar *m_ptr;

	//! The number of tokens encountered.
	size_t m_count;
};

#endif // STRING_FUNCTIONS_H
// File_checked_for_headers
