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

#ifndef LIBWEBCOMMON_JSONWRITER_H
#define LIBWEBCOMMON_JSONWRITER_H

#include <wx/string.h>
#include <cstdint>
#include <string>

// Streaming JSON output. Appends UTF-8 bytes to an internal or caller-owned
// std::string, ready to be a response body with no conversion. Inputs stay
// wxString -- that is what callers hold -- and are encoded on append. Do not
// put the buffer back to wxString: it is UTF-32 here (wxUSE_UNICODE_WCHAR),
// which held an ASCII body at four bytes a character.
//
// Usage:
//  CJsonWriter w;
//  w.BeginObject();
//    w.Key("name"); w.ValueString("aMule");
//    w.Key("version"); w.ValueString("2.3.3");
//  w.EndObject();
//  const std::string &out = w.GetBuffer();
//
// Commas between siblings are inserted automatically. Calling Key()
// outside an object, or omitting it inside one, is a programmer error
// (no runtime check; tests cover the legal patterns).
// One JSON number formatting for doubles, shared by CJsonWriter::ValueDouble
// and by the hand-rolled ostringstream payload builders in the SSE layer.
//
// Two things it gets right that `ostream << double` does not: %.17g, the
// shortest round-trippable form for an IEEE 754 double (the stream default is
// 6 significant digits, so the same value reads differently on SSE and REST),
// and a C-locale decimal point. JSON numbers are always '.'-separated, but
// ostream and snprintf both honour LC_NUMERIC -- which amuleapi inherits from
// --locale -- so on an it/de/fr locale an unnormalised double emits "33,3333"
// and the whole frame stops being valid JSON.
//
// NaN and the infinities become `null`; JSON has no spelling for them.
std::string JsonDoubleToString(double v);

class CJsonWriter
{
public:
	CJsonWriter();
	explicit CJsonWriter(std::string *external_buf);

	void BeginObject();
	void EndObject();
	void BeginArray();
	void EndArray();

	void Key(const char *name);
	void Key(const wxString &name);

	void ValueNull();
	void ValueBool(bool v);
	void ValueInt(int64_t v);
	void ValueUInt(uint64_t v);
	// NaN / +Inf / -Inf are emitted as `null` per JSON.
	void ValueDouble(double v);
	void ValueString(const wxString &s);
	void ValueString(const char *s);
	// Pre-formatted JSON fragment, written verbatim. Caller responsible
	// for valid syntax and for it being UTF-8. Useful when the writer is
	// composing a response from a sub-component that already produced JSON
	// text.
	void ValueRaw(const std::string &json_fragment);

	const std::string &GetBuffer() const { return *m_buf; }

	// Move the accumulated text out, leaving this writer empty and ready to
	// build another document. Saves copying a multi-megabyte body at the end of
	// a response.
	//
	// A caller-owned buffer is copied instead, and left exactly as it was:
	// emptying someone else's is not ours to do, and clearing the comma state
	// while its text is still there would drop the separator before whatever is
	// written next.
	std::string TakeBuffer()
	{
		if (m_buf != &m_internal) {
			return *m_buf;
		}
		std::string taken = std::move(m_internal);
		// A moved-from string is valid but unspecified; make it definitely empty.
		m_internal.clear();
		m_needs_comma = false;
		return taken;
	}

private:
	std::string m_internal;
	std::string *m_buf;
	// True when the next value/key/closer must be preceded by a comma.
	// Reset by BeginObject/BeginArray/Key.
	bool m_needs_comma;

	void MaybeComma();
	void WriteEscapedString(const wxString &s);
	// Append one code point as UTF-8. BMP scalars only -- control characters
	// and supplementary planes take escape forms, surrogates become U+FFFD.
	void AppendUtf8(std::uint32_t cp);
};

#endif // LIBWEBCOMMON_JSONWRITER_H
