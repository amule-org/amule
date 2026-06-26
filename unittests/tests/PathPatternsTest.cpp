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
#include "PathPatterns.h"

using namespace muleunit;
using namespace web_api_path;

DECLARE_SIMPLE(PathPatterns)

// ----------------------------------------------------------------------
// SplitPath
// ----------------------------------------------------------------------

TEST(PathPatterns, SplitPath_Empty)
{
	auto s = SplitPath("");
	ASSERT_EQUALS(static_cast<size_t>(0), s.size());
}

TEST(PathPatterns, SplitPath_Root)
{
	// "/" parses to a single empty segment — distinguishable from "".
	auto s = SplitPath("/");
	ASSERT_EQUALS(static_cast<size_t>(1), s.size());
	ASSERT_EQUALS(std::string(""), s[0]);
}

TEST(PathPatterns, SplitPath_Single)
{
	auto s = SplitPath("/version");
	ASSERT_EQUALS(static_cast<size_t>(1), s.size());
	ASSERT_EQUALS(std::string("version"), s[0]);
}

TEST(PathPatterns, SplitPath_Multiple)
{
	auto s = SplitPath("/downloads/abc/pause");
	ASSERT_EQUALS(static_cast<size_t>(3), s.size());
	ASSERT_EQUALS(std::string("downloads"), s[0]);
	ASSERT_EQUALS(std::string("abc"), s[1]);
	ASSERT_EQUALS(std::string("pause"), s[2]);
}

TEST(PathPatterns, SplitPath_TrailingSlash)
{
	// A trailing slash emits an empty trailing segment so the matcher
	// can distinguish "/a/" from "/a".
	auto s = SplitPath("/a/");
	ASSERT_EQUALS(static_cast<size_t>(2), s.size());
	ASSERT_EQUALS(std::string("a"), s[0]);
	ASSERT_EQUALS(std::string(""), s[1]);
}

TEST(PathPatterns, SplitPath_NoLeadingSlash)
{
	// Absolute path is conventional, but the splitter doesn't require it.
	auto s = SplitPath("a/b");
	ASSERT_EQUALS(static_cast<size_t>(2), s.size());
	ASSERT_EQUALS(std::string("a"), s[0]);
	ASSERT_EQUALS(std::string("b"), s[1]);
}

// ----------------------------------------------------------------------
// ParseQuery
// ----------------------------------------------------------------------

TEST(PathPatterns, ParseQuery_Empty)
{
	auto m = ParseQuery("");
	ASSERT_EQUALS(static_cast<size_t>(0), m.size());
}

TEST(PathPatterns, ParseQuery_Single)
{
	auto m = ParseQuery("k=v");
	ASSERT_EQUALS(static_cast<size_t>(1), m.size());
	ASSERT_EQUALS(std::string("v"), m["k"]);
}

TEST(PathPatterns, ParseQuery_Multiple)
{
	auto m = ParseQuery("a=1&b=2&c=3");
	ASSERT_EQUALS(static_cast<size_t>(3), m.size());
	ASSERT_EQUALS(std::string("1"), m["a"]);
	ASSERT_EQUALS(std::string("2"), m["b"]);
	ASSERT_EQUALS(std::string("3"), m["c"]);
}

TEST(PathPatterns, ParseQuery_KeyWithoutValue)
{
	// "k" with no "=v" is parsed as key "k" with empty value (HTTP /
	// HTML form convention).
	auto m = ParseQuery("a&b=2");
	ASSERT_EQUALS(static_cast<size_t>(2), m.size());
	ASSERT_EQUALS(std::string(""), m["a"]);
	ASSERT_EQUALS(std::string("2"), m["b"]);
}

TEST(PathPatterns, ParseQuery_EqualsInValue)
{
	// A `=` after the first one is part of the value, not a new
	// key/value separator.
	auto m = ParseQuery("expr=a=b");
	ASSERT_EQUALS(std::string("a=b"), m["expr"]);
}

TEST(PathPatterns, ParseQuery_PercentDecode)
{
	// %20 in both keys and values; values are application/x-www-form-
	// urlencoded so `+` decodes to space too.
	auto m = ParseQuery("a%20b=c%3Dd&e=f+g");
	ASSERT_EQUALS(std::string("c=d"), m["a b"]);
	ASSERT_EQUALS(std::string("f g"), m["e"]);
}

TEST(PathPatterns, ParseQuery_MalformedPercentPassThrough)
{
	// A stray `%` with no two hex digits behind it passes through
	// verbatim — we don't drop the character (would silently shift
	// downstream parsing).
	auto m = ParseQuery("k=ab%cz");
	ASSERT_EQUALS(std::string("ab%cz"), m["k"]);
	auto m2 = ParseQuery("k=trailing%");
	ASSERT_EQUALS(std::string("trailing%"), m2["k"]);
}

TEST(PathPatterns, ParseQuery_PercentCaseInsensitive)
{
	// Both `%2F` and `%2f` decode to `/` per RFC 3986.
	auto m = ParseQuery("a=foo%2Fbar&b=foo%2fbar");
	ASSERT_EQUALS(std::string("foo/bar"), m["a"]);
	ASSERT_EQUALS(std::string("foo/bar"), m["b"]);
}

// ----------------------------------------------------------------------
// ParsePattern
// ----------------------------------------------------------------------

TEST(PathPatterns, ParsePattern_LiteralOnly)
{
	RoutePattern p = ParsePattern("/version");
	ASSERT_EQUALS(static_cast<size_t>(1), p.segments.size());
	ASSERT_EQUALS(std::string("version"), p.segments[0]);
	ASSERT_EQUALS(std::string(""), p.capture_names[0]);
}

TEST(PathPatterns, ParsePattern_SingleCapture)
{
	RoutePattern p = ParsePattern("/downloads/{hash}");
	ASSERT_EQUALS(static_cast<size_t>(2), p.segments.size());
	ASSERT_EQUALS(std::string("downloads"), p.segments[0]);
	ASSERT_EQUALS(std::string("{hash}"), p.segments[1]);
	ASSERT_EQUALS(std::string(""), p.capture_names[0]);
	ASSERT_EQUALS(std::string("hash"), p.capture_names[1]);
}

TEST(PathPatterns, ParsePattern_CaptureMidPath)
{
	RoutePattern p = ParsePattern("/downloads/{hash}/pause");
	ASSERT_EQUALS(static_cast<size_t>(3), p.segments.size());
	ASSERT_EQUALS(std::string("hash"), p.capture_names[1]);
	ASSERT_EQUALS(std::string(""), p.capture_names[2]);
}

// ----------------------------------------------------------------------
// Match
// ----------------------------------------------------------------------

TEST(PathPatterns, Match_Literal_OK)
{
	RoutePattern p = ParsePattern("/version");
	std::map<std::string, std::string> caps;
	ASSERT_TRUE(Match(p, SplitPath("/version"), caps));
	ASSERT_EQUALS(static_cast<size_t>(0), caps.size());
}

TEST(PathPatterns, Match_Literal_Mismatch)
{
	RoutePattern p = ParsePattern("/version");
	std::map<std::string, std::string> caps;
	ASSERT_FALSE(Match(p, SplitPath("/status"), caps));
}

TEST(PathPatterns, Match_Literal_DifferentLength)
{
	RoutePattern p = ParsePattern("/a/b");
	std::map<std::string, std::string> caps;
	ASSERT_FALSE(Match(p, SplitPath("/a/b/c"), caps));
	ASSERT_FALSE(Match(p, SplitPath("/a"), caps));
}

TEST(PathPatterns, Match_Capture_Single)
{
	RoutePattern p = ParsePattern("/downloads/{hash}");
	std::map<std::string, std::string> caps;
	ASSERT_TRUE(Match(p, SplitPath("/downloads/31d6cfe0"), caps));
	ASSERT_EQUALS(std::string("31d6cfe0"), caps["hash"]);
}

TEST(PathPatterns, Match_Capture_Mid)
{
	RoutePattern p = ParsePattern("/downloads/{hash}/pause");
	std::map<std::string, std::string> caps;
	ASSERT_TRUE(Match(p, SplitPath("/downloads/abc/pause"), caps));
	ASSERT_EQUALS(std::string("abc"), caps["hash"]);
}

TEST(PathPatterns, Match_Capture_LengthMismatch)
{
	RoutePattern p = ParsePattern("/downloads/{hash}/pause");
	std::map<std::string, std::string> caps;
	ASSERT_FALSE(Match(p, SplitPath("/downloads/abc"), caps));
	ASSERT_FALSE(Match(p, SplitPath("/downloads/abc/pause/extra"), caps));
}

// ----------------------------------------------------------------------
// ShapeEqual
// ----------------------------------------------------------------------

TEST(PathPatterns, ShapeEqual_SamePattern)
{
	RoutePattern a = ParsePattern("/downloads/{hash}/pause");
	RoutePattern b = ParsePattern("/downloads/{hash}/pause");
	ASSERT_TRUE(ShapeEqual(a, b));
}

TEST(PathPatterns, ShapeEqual_DifferentCaptureName)
{
	// Two patterns differing only in capture name shape-collide.
	RoutePattern a = ParsePattern("/downloads/{hash}/pause");
	RoutePattern b = ParsePattern("/downloads/{id}/pause");
	ASSERT_TRUE(ShapeEqual(a, b));
}

TEST(PathPatterns, ShapeEqual_DifferentLiteral)
{
	RoutePattern a = ParsePattern("/downloads/{hash}/pause");
	RoutePattern b = ParsePattern("/downloads/{hash}/resume");
	ASSERT_FALSE(ShapeEqual(a, b));
}

TEST(PathPatterns, ShapeEqual_CaptureVsLiteral)
{
	RoutePattern a = ParsePattern("/downloads/{x}");
	RoutePattern b = ParsePattern("/downloads/all");
	ASSERT_FALSE(ShapeEqual(a, b));
}

TEST(PathPatterns, ShapeEqual_DifferentLengths)
{
	RoutePattern a = ParsePattern("/a/b");
	RoutePattern b = ParsePattern("/a/b/c");
	ASSERT_FALSE(ShapeEqual(a, b));
}
