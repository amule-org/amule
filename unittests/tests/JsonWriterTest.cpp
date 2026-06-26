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

#include <muleunit/test.h>
#include "JsonWriter.h"

#include <cmath>
#include <limits>

using namespace muleunit;

DECLARE_SIMPLE(JsonWriter)

TEST(JsonWriter, EmptyObject)
{
	CJsonWriter w;
	w.BeginObject();
	w.EndObject();
	ASSERT_EQUALS(wxString("{}"), w.GetBuffer());
}

TEST(JsonWriter, EmptyArray)
{
	CJsonWriter w;
	w.BeginArray();
	w.EndArray();
	ASSERT_EQUALS(wxString("[]"), w.GetBuffer());
}

TEST(JsonWriter, ObjectWithStringValue)
{
	CJsonWriter w;
	w.BeginObject();
	w.Key("app");
	w.ValueString("aMule");
	w.EndObject();
	ASSERT_EQUALS(wxString("{\"app\":\"aMule\"}"), w.GetBuffer());
}

TEST(JsonWriter, ObjectWithMultipleKeys)
{
	CJsonWriter w;
	w.BeginObject();
	w.Key("app");
	w.ValueString("aMule");
	w.Key("version");
	w.ValueString("2.3.3");
	w.Key("api");
	w.ValueString("v0.1");
	w.EndObject();
	ASSERT_EQUALS(wxString("{\"app\":\"aMule\",\"version\":\"2.3.3\",\"api\":\"v0.1\"}"), w.GetBuffer());
}

TEST(JsonWriter, Primitives)
{
	CJsonWriter w;
	w.BeginObject();
	w.Key("n");
	w.ValueNull();
	w.Key("t");
	w.ValueBool(true);
	w.Key("f");
	w.ValueBool(false);
	w.Key("i");
	w.ValueInt(-42);
	w.Key("u");
	w.ValueUInt(uint64_t(42));
	w.EndObject();
	ASSERT_EQUALS(wxString("{\"n\":null,\"t\":true,\"f\":false,\"i\":-42,\"u\":42}"), w.GetBuffer());
}

TEST(JsonWriter, IntegerBoundaries)
{
	CJsonWriter w;
	w.BeginArray();
	w.ValueInt(std::numeric_limits<int64_t>::min());
	w.ValueInt(std::numeric_limits<int64_t>::max());
	w.ValueUInt(std::numeric_limits<uint64_t>::max());
	w.EndArray();
	ASSERT_EQUALS(
		wxString("[-9223372036854775808,9223372036854775807,18446744073709551615]"), w.GetBuffer());
}

TEST(JsonWriter, DoubleSpecials)
{
	// NaN, +Inf, -Inf are not representable in JSON; the writer emits null.
	CJsonWriter w;
	w.BeginArray();
	w.ValueDouble(std::nan(""));
	w.ValueDouble(std::numeric_limits<double>::infinity());
	w.ValueDouble(-std::numeric_limits<double>::infinity());
	w.EndArray();
	ASSERT_EQUALS(wxString("[null,null,null]"), w.GetBuffer());
}

TEST(JsonWriter, NestedObject)
{
	CJsonWriter w;
	w.BeginObject();
	w.Key("outer");
	w.BeginObject();
	w.Key("inner");
	w.ValueString("v");
	w.EndObject();
	w.EndObject();
	ASSERT_EQUALS(wxString("{\"outer\":{\"inner\":\"v\"}}"), w.GetBuffer());
}

TEST(JsonWriter, ArrayOfObjects)
{
	CJsonWriter w;
	w.BeginObject();
	w.Key("items");
	w.BeginArray();
	w.BeginObject();
	w.Key("k");
	w.ValueInt(1);
	w.EndObject();
	w.BeginObject();
	w.Key("k");
	w.ValueInt(2);
	w.EndObject();
	w.EndArray();
	w.EndObject();
	ASSERT_EQUALS(wxString("{\"items\":[{\"k\":1},{\"k\":2}]}"), w.GetBuffer());
}

TEST(JsonWriter, EscapesQuoteAndBackslash)
{
	CJsonWriter w;
	w.ValueString(wxString::FromUTF8("a\"b\\c"));
	ASSERT_EQUALS(wxString("\"a\\\"b\\\\c\""), w.GetBuffer());
}

TEST(JsonWriter, EscapesShortControlChars)
{
	CJsonWriter w;
	w.ValueString(wxString::FromUTF8("\b\f\n\r\t"));
	ASSERT_EQUALS(wxString("\"\\b\\f\\n\\r\\t\""), w.GetBuffer());
}

TEST(JsonWriter, EscapesGenericControlChars)
{
	// Control chars without short forms get \uXXXX. DEL (0x7F) is treated
	// the same so it never lands in the output verbatim.
	CJsonWriter w;
	w.BeginArray();
	w.ValueString(wxString(wxUniChar(uint32_t(0x00))));
	w.ValueString(wxString(wxUniChar(uint32_t(0x01))));
	w.ValueString(wxString(wxUniChar(uint32_t(0x1F))));
	w.ValueString(wxString(wxUniChar(uint32_t(0x7F))));
	w.EndArray();
	ASSERT_EQUALS(wxString("[\"\\u0000\",\"\\u0001\",\"\\u001f\",\"\\u007f\"]"), w.GetBuffer());
}

TEST(JsonWriter, SupplementaryPlaneAsSurrogatePair)
{
	// U+1F600 (GRINNING FACE) is in the supplementary plane; it must be
	// emitted as the UTF-16 surrogate pair 😀.
	CJsonWriter w;
	w.ValueString(wxString(wxUniChar(uint32_t(0x1F600))));
	ASSERT_EQUALS(wxString("\"\\ud83d\\ude00\""), w.GetBuffer());
}

TEST(JsonWriter, BmpNonAsciiEmittedVerbatim)
{
	// Non-control codepoints in the BMP are passed through as wxString
	// content. The serializer encodes the whole buffer as UTF-8 at
	// flush time; here we just verify the round trip is invariant.
	const wxString input =
		wxString::FromUTF8("\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"); // "привет"
	CJsonWriter w;
	w.ValueString(input);
	// The wxString contains exactly: " <6 cyrillic chars> "
	wxString expected;
	expected += wxT("\"");
	expected += input;
	expected += wxT("\"");
	ASSERT_EQUALS(expected, w.GetBuffer());
}

TEST(JsonWriter, KeyEscaping)
{
	// Keys go through the same escaping path as values. A key containing
	// a quote must be escaped or the output is invalid JSON.
	CJsonWriter w;
	w.BeginObject();
	w.Key(wxString::FromUTF8("a\"b"));
	w.ValueInt(1);
	w.EndObject();
	ASSERT_EQUALS(wxString("{\"a\\\"b\":1}"), w.GetBuffer());
}

TEST(JsonWriter, ValueRawFragment)
{
	// A pre-formatted JSON fragment is appended verbatim. Caller is
	// responsible for ensuring it's valid JSON; the writer only tracks
	// whether a comma is needed before/after.
	CJsonWriter w;
	w.BeginObject();
	w.Key("pre");
	w.ValueRaw(wxT("[1,2,3]"));
	w.Key("post");
	w.ValueInt(4);
	w.EndObject();
	ASSERT_EQUALS(wxString("{\"pre\":[1,2,3],\"post\":4}"), w.GetBuffer());
}

TEST(JsonWriter, ExternalBuffer)
{
	// The writer can append into a caller-owned buffer instead of its
	// own. Used when composing a response from multiple writers.
	wxString shared;
	shared += wxT("prefix:");
	{
		CJsonWriter w(&shared);
		w.BeginObject();
		w.Key("x");
		w.ValueInt(1);
		w.EndObject();
	}
	ASSERT_EQUALS(wxString("prefix:{\"x\":1}"), shared);
}

TEST(JsonWriter, LargeString)
{
	// 50 KB string of printable ASCII should encode in linear time with
	// the only overhead being the surrounding quotes.
	wxString big(wxT('x'), 50000);
	CJsonWriter w;
	w.ValueString(big);
	wxString expected;
	expected += wxT("\"");
	expected += big;
	expected += wxT("\"");
	ASSERT_EQUALS(expected, w.GetBuffer());
}
