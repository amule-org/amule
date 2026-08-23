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

#include <clocale>
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
	ASSERT_EQUALS(std::string("{}"), w.GetBuffer());
}

TEST(JsonWriter, EmptyArray)
{
	CJsonWriter w;
	w.BeginArray();
	w.EndArray();
	ASSERT_EQUALS(std::string("[]"), w.GetBuffer());
}

TEST(JsonWriter, ObjectWithStringValue)
{
	CJsonWriter w;
	w.BeginObject();
	w.Key("app");
	w.ValueString("aMule");
	w.EndObject();
	ASSERT_EQUALS(std::string("{\"app\":\"aMule\"}"), w.GetBuffer());
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
	ASSERT_EQUALS(
		std::string("{\"app\":\"aMule\",\"version\":\"2.3.3\",\"api\":\"v0.1\"}"), w.GetBuffer());
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
	ASSERT_EQUALS(std::string("{\"n\":null,\"t\":true,\"f\":false,\"i\":-42,\"u\":42}"), w.GetBuffer());
}

TEST(JsonWriter, IntegerBoundaries)
{
	CJsonWriter w;
	w.BeginArray();
	w.ValueInt(std::numeric_limits<int64_t>::min());
	w.ValueInt(std::numeric_limits<int64_t>::max());
	w.ValueUInt(std::numeric_limits<uint64_t>::max());
	w.EndArray();
	ASSERT_EQUALS(std::string("[-9223372036854775808,9223372036854775807,18446744073709551615]"),
		w.GetBuffer());
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
	ASSERT_EQUALS(std::string("[null,null,null]"), w.GetBuffer());
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
	ASSERT_EQUALS(std::string("{\"outer\":{\"inner\":\"v\"}}"), w.GetBuffer());
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
	ASSERT_EQUALS(std::string("{\"items\":[{\"k\":1},{\"k\":2}]}"), w.GetBuffer());
}

TEST(JsonWriter, EscapesQuoteAndBackslash)
{
	CJsonWriter w;
	w.ValueString(wxString::FromUTF8("a\"b\\c"));
	ASSERT_EQUALS(std::string("\"a\\\"b\\\\c\""), w.GetBuffer());
}

TEST(JsonWriter, EscapesShortControlChars)
{
	CJsonWriter w;
	w.ValueString(wxString::FromUTF8("\b\f\n\r\t"));
	ASSERT_EQUALS(std::string("\"\\b\\f\\n\\r\\t\""), w.GetBuffer());
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
	ASSERT_EQUALS(std::string("[\"\\u0000\",\"\\u0001\",\"\\u001f\",\"\\u007f\"]"), w.GetBuffer());
}

TEST(JsonWriter, SupplementaryPlaneAsSurrogatePair)
{
	// U+1F600 (GRINNING FACE) is in the supplementary plane; it must be
	// emitted as the UTF-16 surrogate pair 😀.
	CJsonWriter w;
	w.ValueString(wxString(wxUniChar(uint32_t(0x1F600))));
	ASSERT_EQUALS(std::string("\"\\ud83d\\ude00\""), w.GetBuffer());
}

TEST(JsonWriter, BmpNonAsciiEmittedAsUtf8)
{
	// Non-control BMP codepoints need no JSON escape and are emitted as the
	// UTF-8 the buffer holds -- byte for byte what went in, since the input
	// was built from the same UTF-8.
	const char cyrillic[] = "\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"; // "привет"
	CJsonWriter w;
	w.ValueString(wxString::FromUTF8(cyrillic));
	ASSERT_EQUALS(std::string("\"") + cyrillic + "\"", w.GetBuffer());
	// Two bytes per character here, not one wide unit: the buffer is UTF-8.
	ASSERT_EQUALS(static_cast<size_t>(14), w.GetBuffer().size());
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
	ASSERT_EQUALS(std::string("{\"a\\\"b\":1}"), w.GetBuffer());
}

TEST(JsonWriter, ValueRawFragment)
{
	// A pre-formatted JSON fragment is appended verbatim. Caller is
	// responsible for ensuring it's valid JSON; the writer only tracks
	// whether a comma is needed before/after.
	CJsonWriter w;
	w.BeginObject();
	w.Key("pre");
	w.ValueRaw("[1,2,3]");
	w.Key("post");
	w.ValueInt(4);
	w.EndObject();
	ASSERT_EQUALS(std::string("{\"pre\":[1,2,3],\"post\":4}"), w.GetBuffer());
}

TEST(JsonWriter, ExternalBuffer)
{
	// The writer can append into a caller-owned buffer instead of its
	// own. Used when composing a response from multiple writers.
	std::string shared;
	shared += "prefix:";
	{
		CJsonWriter w(&shared);
		w.BeginObject();
		w.Key("x");
		w.ValueInt(1);
		w.EndObject();
	}
	ASSERT_EQUALS(std::string("prefix:{\"x\":1}"), shared);
}

TEST(JsonWriter, LargeString)
{
	// 50 KB string of printable ASCII should encode in linear time with
	// the only overhead being the surrounding quotes.
	const wxString big(wxT('x'), 50000);
	CJsonWriter w;
	w.ValueString(big);
	// Compared as bytes. Building the expectation as a wxString would convert
	// the buffer back with the locale codec on the way into ASSERT_EQUALS,
	// which is the conversion the writer exists to avoid -- and it would pass
	// here regardless, the payload being pure ASCII.
	ASSERT_EQUALS(std::string("\"") + std::string(50000, 'x') + "\"", w.GetBuffer());
}

TEST(JsonWriter, TakeBufferLeavesTheWriterReusable)
{
	// EndArray() leaves a comma pending for the next sibling, so a writer that
	// kept that state across a take would open its next document with a stray
	// separator.
	CJsonWriter w;
	w.BeginArray();
	w.ValueInt(1);
	w.EndArray();
	ASSERT_EQUALS(std::string("[1]"), w.TakeBuffer());
	ASSERT_TRUE(w.GetBuffer().empty());

	w.BeginArray();
	w.ValueInt(2);
	w.EndArray();
	ASSERT_EQUALS(std::string("[2]"), w.TakeBuffer());
}

TEST(JsonWriter, TakeBufferLeavesACallerOwnedBufferAlone)
{
	// The external buffer is the caller's; taking copies out of it rather than
	// emptying it.
	std::string shared;
	CJsonWriter w(&shared);
	w.BeginObject();
	w.Key("x");
	w.ValueInt(1);
	w.EndObject();
	ASSERT_EQUALS(std::string("{\"x\":1}"), w.TakeBuffer());
	ASSERT_EQUALS(std::string("{\"x\":1}"), shared);
}

// JsonDoubleToString is the one formatting both the REST writer and the SSE
// payload builders go through. It exists because `ostream << double` differs
// from it in two ways that matter on the wire.
TEST(JsonWriter, DoubleToStringIsRoundTrippableNotSixDigits)
{
	// The stream default is 6 significant digits, which turns 1-of-3 parts
	// into "33.3333" -- a different number from the one REST reports for the
	// same value.
	const double third = 100.0 / 3.0;
	ASSERT_EQUALS(std::string("33.333333333333336"), JsonDoubleToString(third));
	// Whole values stay short; %.17g does not pad.
	ASSERT_EQUALS(std::string("87.5"), JsonDoubleToString(87.5));
	ASSERT_EQUALS(std::string("100"), JsonDoubleToString(100.0));
	ASSERT_EQUALS(std::string("0"), JsonDoubleToString(0.0));
}

TEST(JsonWriter, DoubleToStringUsesACLocaleDecimalPoint)
{
	// snprintf and ostream both honour LC_NUMERIC, which amuleapi inherits
	// from --locale. A comma separator would make the frame invalid JSON, so
	// the formatter normalises it. Skipped where the locale is unavailable,
	// which is normal in a minimal container.
	const char *prev = std::setlocale(LC_NUMERIC, nullptr);
	const std::string saved = prev ? prev : "C";
	if (std::setlocale(LC_NUMERIC, "de_DE.UTF-8") == nullptr &&
		std::setlocale(LC_NUMERIC, "it_IT.UTF-8") == nullptr) {
		return;
	}
	const std::string got = JsonDoubleToString(87.5);
	std::setlocale(LC_NUMERIC, saved.c_str());
	ASSERT_EQUALS(std::string("87.5"), got);
}

TEST(JsonWriter, DoubleToStringSpellsNonFiniteAsNull)
{
	ASSERT_EQUALS(std::string("null"), JsonDoubleToString(std::nan("")));
	ASSERT_EQUALS(std::string("null"), JsonDoubleToString(std::numeric_limits<double>::infinity()));
}
