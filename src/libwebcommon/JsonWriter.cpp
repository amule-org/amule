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
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
//

#include "JsonWriter.h"

#include <clocale>
#include <cmath>
#include <cstdio>

CJsonWriter::CJsonWriter()
: m_buf(&m_internal)
, m_needs_comma(false)
{
}

CJsonWriter::CJsonWriter(wxString *external_buf)
: m_buf(external_buf)
, m_needs_comma(false)
{
}

void CJsonWriter::MaybeComma()
{
	if (m_needs_comma) {
		*m_buf += wxT(",");
	}
}

void CJsonWriter::BeginObject()
{
	MaybeComma();
	*m_buf += wxT("{");
	m_needs_comma = false;
}

void CJsonWriter::EndObject()
{
	*m_buf += wxT("}");
	m_needs_comma = true;
}

void CJsonWriter::BeginArray()
{
	MaybeComma();
	*m_buf += wxT("[");
	m_needs_comma = false;
}

void CJsonWriter::EndArray()
{
	*m_buf += wxT("]");
	m_needs_comma = true;
}

void CJsonWriter::Key(const char *name)
{
	Key(wxString::FromUTF8(name));
}

void CJsonWriter::Key(const wxString &name)
{
	MaybeComma();
	WriteEscapedString(name);
	*m_buf += wxT(":");
	m_needs_comma = false;
}

void CJsonWriter::ValueNull()
{
	MaybeComma();
	*m_buf += wxT("null");
	m_needs_comma = true;
}

void CJsonWriter::ValueBool(bool v)
{
	MaybeComma();
	*m_buf += v ? wxT("true") : wxT("false");
	m_needs_comma = true;
}

void CJsonWriter::ValueInt(int64_t v)
{
	MaybeComma();
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
	*m_buf += wxString::FromAscii(buf);
	m_needs_comma = true;
}

void CJsonWriter::ValueUInt(uint64_t v)
{
	MaybeComma();
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
	*m_buf += wxString::FromAscii(buf);
	m_needs_comma = true;
}

void CJsonWriter::ValueDouble(double v)
{
	MaybeComma();
	if (std::isnan(v) || std::isinf(v)) {
		*m_buf += wxT("null");
	} else {
		// %.17g is the shortest round-trippable form for IEEE 754
		// doubles. JSON doesn't allow `+Inf`, `-Inf` or `NaN` so we
		// already handled those above.
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%.17g", v);
		// JSON numbers are always C-locale (a '.' decimal point), but snprintf
		// honours LC_NUMERIC, which amuleapi/amuleweb inherit from --locale.
		// %g never emits digit grouping, so the only possible locale artifact is
		// the decimal separator; normalise it to '.' so the output is valid JSON
		// regardless of the process locale.
		const char decimal_point = *std::localeconv()->decimal_point;
		if (decimal_point != '.') {
			for (char *p = buf; *p; ++p) {
				if (*p == decimal_point) {
					*p = '.';
				}
			}
		}
		*m_buf += wxString::FromAscii(buf);
	}
	m_needs_comma = true;
}

void CJsonWriter::ValueString(const wxString &s)
{
	MaybeComma();
	WriteEscapedString(s);
	m_needs_comma = true;
}

void CJsonWriter::ValueString(const char *s)
{
	ValueString(s ? wxString::FromUTF8(s) : wxString());
}

void CJsonWriter::ValueRaw(const wxString &json_fragment)
{
	MaybeComma();
	*m_buf += json_fragment;
	m_needs_comma = true;
}

void CJsonWriter::WriteEscapedString(const wxString &s)
{
	*m_buf += wxT("\"");
	for (wxString::const_iterator i = s.begin(); i != s.end(); ++i) {
		wxUniChar uc = *i;
		uint32_t cp = uc.GetValue();
		// wxString on Windows uses UTF-16 internally so supplementary-
		// plane code points (U+10000+) come through as two surrogate
		// halves; Linux + macOS use UTF-32 and yield the combined
		// code point in one step. Combine the halves here so both
		// backends emit identical `\uXXXX\uXXXX` escapes.
		if (cp >= 0xD800 && cp <= 0xDBFF) {
			wxString::const_iterator j = i;
			++j;
			bool paired = false;
			if (j != s.end()) {
				const uint32_t lo = wxUniChar(*j).GetValue();
				if (lo >= 0xDC00 && lo <= 0xDFFF) {
					cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
					i = j;
					paired = true;
				}
			}
			if (!paired) {
				// Unpaired high surrogate. Falling through would
				// emit invalid UTF-8 (CESU-8) once the buffer is
				// utf8_str()-flushed. Replace with U+FFFD so the
				// JSON output stays valid Unicode. Same treatment
				// for an unpaired low surrogate below.
				cp = 0xFFFD;
			}
		} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
			cp = 0xFFFD;
		}
		switch (cp) {
		case '"':
			*m_buf += wxT("\\\"");
			continue;
		case '\\':
			*m_buf += wxT("\\\\");
			continue;
		case '\b':
			*m_buf += wxT("\\b");
			continue;
		case '\f':
			*m_buf += wxT("\\f");
			continue;
		case '\n':
			*m_buf += wxT("\\n");
			continue;
		case '\r':
			*m_buf += wxT("\\r");
			continue;
		case '\t':
			*m_buf += wxT("\\t");
			continue;
		default:
			break;
		}
		if (cp < 0x20 || cp == 0x7F) {
			// Control characters: \uXXXX form.
			char buf[8];
			std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(cp));
			*m_buf += wxString::FromAscii(buf);
		} else if (cp <= 0xFFFF) {
			// BMP non-control: emit verbatim. Non-ASCII bytes ride
			// through as the wxString native encoding and are
			// converted to UTF-8 by the response serializer.
			*m_buf += uc;
		} else {
			// Supplementary plane: emit as UTF-16 surrogate pair.
			// This is the only escape form JSON allows above U+FFFF.
			uint32_t v = cp - 0x10000;
			uint32_t hi = 0xD800 | ((v >> 10) & 0x3FF);
			uint32_t lo = 0xDC00 | (v & 0x3FF);
			char buf[16];
			std::snprintf(buf,
				sizeof(buf),
				"\\u%04x\\u%04x",
				static_cast<unsigned>(hi),
				static_cast<unsigned>(lo));
			*m_buf += wxString::FromAscii(buf);
		}
	}
	*m_buf += wxT("\"");
}
