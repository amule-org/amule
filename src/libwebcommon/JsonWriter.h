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

// Streaming JSON output. Appends to an internal or caller-owned wxString
// buffer; the buffer holds JSON text suitable for UTF-8 emission via
// wxString::utf8_str() at flush time.
//
// Usage:
//  CJsonWriter w;
//  w.BeginObject();
//    w.Key("name"); w.ValueString("aMule");
//    w.Key("version"); w.ValueString("2.3.3");
//  w.EndObject();
//  const wxString &out = w.GetBuffer();
//
// Commas between siblings are inserted automatically. Calling Key()
// outside an object, or omitting it inside one, is a programmer error
// (no runtime check; tests cover the legal patterns).
class CJsonWriter
{
public:
	CJsonWriter();
	explicit CJsonWriter(wxString *external_buf);

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
	// for valid syntax. Useful when the writer is composing a response
	// from a sub-component that already produced JSON text.
	void ValueRaw(const wxString &json_fragment);

	const wxString &GetBuffer() const { return *m_buf; }

private:
	wxString m_internal;
	wxString *m_buf;
	// True when the next value/key/closer must be preceded by a comma.
	// Reset by BeginObject/BeginArray/Key.
	bool m_needs_comma;

	void MaybeComma();
	void WriteEscapedString(const wxString &s);
};

#endif // LIBWEBCOMMON_JSONWRITER_H
