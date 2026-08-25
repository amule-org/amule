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

// ----------------------------------------------------------------------
// StripTrailingSlash
// ----------------------------------------------------------------------

// `/x/` and `/x` name the same resource. Without this the two spellings
// disagree by route kind: a literal route misses outright, a capture route
// matches with an empty capture.
TEST(PathPatterns, StripTrailingSlash_RemovesOne)
{
	ASSERT_EQUALS(std::string("/api/v0/status"), StripTrailingSlash("/api/v0/status/"));
	ASSERT_EQUALS(std::string("/api/v0/clients"), StripTrailingSlash("/api/v0/clients/"));
}

// Already-bare paths are returned unchanged, and the root is not a spelling
// of the empty string.
TEST(PathPatterns, StripTrailingSlash_LeavesBareAndRootAlone)
{
	ASSERT_EQUALS(std::string("/api/v0/status"), StripTrailingSlash("/api/v0/status"));
	ASSERT_EQUALS(std::string("/"), StripTrailingSlash("/"));
	ASSERT_EQUALS(std::string(""), StripTrailingSlash(""));
}

// Only one. `//` is a malformed path rather than a synonym -- collapsing it
// would let `/a//b` reach the route for `/a/b`.
TEST(PathPatterns, StripTrailingSlash_StripsOnlyOne)
{
	ASSERT_EQUALS(std::string("/a/"), StripTrailingSlash("/a//"));
}

// ----------------------------------------------------------------------
// Match: empty captures
// ----------------------------------------------------------------------

// Every capture on the surface names a resource -- a hash, an ecid, an
// index, an address -- and none of them can be the empty string. Binding
// one used to hand the handler a URL that names nothing and leave it to
// pick a status code, which is why an empty {ecid} was a 400 while an empty
// {hash} was a 404.
TEST(PathPatterns, Match_RejectsAnEmptyCapture)
{
	const auto pat = ParsePattern("/api/v0/clients/{ecid}");
	std::map<std::string, std::string> caps;
	ASSERT_TRUE(!Match(pat, SplitPath("/api/v0/clients/"), caps));
	// The non-empty case still matches, so the guard is not just refusing
	// everything.
	ASSERT_TRUE(Match(pat, SplitPath("/api/v0/clients/42"), caps));
	ASSERT_EQUALS(std::string("42"), caps["ecid"]);
}

// A literal segment that happens to be empty is a different question and was
// already handled: the comparison simply fails.
TEST(PathPatterns, Match_StillRejectsAMissegmentedLiteral)
{
	// Same segment count, so the rejection has to come from the literal
	// comparison rather than from the length check.
	const auto pat = ParsePattern("/api/v0/version/check");
	std::map<std::string, std::string> caps;
	ASSERT_TRUE(!Match(pat, SplitPath("/api/v0/version/"), caps));
}

// ----------------------------------------------------------------------
// ParseBoundedUint / ParseBoolValue
// ----------------------------------------------------------------------

// Seven hand-written count parsers disagreed about the two questions that
// decide what a client sees: what an unparseable value does, and what an
// out-of-range one does. A typo was a hard error on `interval` and a silent
// behaviour change on `width` -- on the same endpoint.
TEST(PathPatterns, ParseBoundedUint_AcceptsInRange)
{
	std::uint64_t v = 999;
	ASSERT_TRUE(ParseBoundedUint("0", 0, 500, v));
	ASSERT_EQUALS(static_cast<std::uint64_t>(0), v);
	ASSERT_TRUE(ParseBoundedUint("500", 0, 500, v));
	ASSERT_EQUALS(static_cast<std::uint64_t>(500), v);
	ASSERT_TRUE(ParseBoundedUint("1", 1, 3600, v));
	ASSERT_EQUALS(static_cast<std::uint64_t>(1), v);
}

// Unparseable and out-of-range are the same answer, which is the whole point:
// neither silently becomes a default.
TEST(PathPatterns, ParseBoundedUint_RejectsGarbageAndOutOfRange)
{
	std::uint64_t v = 42;
	ASSERT_TRUE(!ParseBoundedUint("", 0, 500, v));
	ASSERT_TRUE(!ParseBoundedUint("abc", 0, 500, v));
	ASSERT_TRUE(!ParseBoundedUint("12x", 0, 500, v));
	ASSERT_TRUE(!ParseBoundedUint("-1", 0, 500, v));
	ASSERT_TRUE(!ParseBoundedUint("501", 0, 500, v));
	ASSERT_TRUE(!ParseBoundedUint("0", 1, 3600, v));
	// A rejected parse leaves the caller's default alone.
	ASSERT_EQUALS(static_cast<std::uint64_t>(42), v);
}

// The bound is checked while accumulating, so a digit string long enough to
// wrap a uint64 is rejected rather than wrapping into range.
TEST(PathPatterns, ParseBoundedUint_DoesNotWrapOnLongInput)
{
	std::uint64_t v = 7;
	ASSERT_TRUE(!ParseBoundedUint("99999999999999999999999999", 0, 500, v));
	ASSERT_TRUE(!ParseBoundedUint("18446744073709551617", 0, 500, v));
	ASSERT_EQUALS(static_cast<std::uint64_t>(7), v);
}

// `include_completed` read every unrecognised value as false while its
// neighbour `include_parts` answered 400, so one typo was silent and the next
// was fatal. One vocabulary, and anything outside it is answerable.
TEST(PathPatterns, ParseBoolValue_AcceptsTheThreeSpellings)
{
	bool b = false;
	ASSERT_TRUE(ParseBoolValue("1", b) && b);
	ASSERT_TRUE(ParseBoolValue("true", b) && b);
	ASSERT_TRUE(ParseBoolValue("yes", b) && b);
	ASSERT_TRUE(ParseBoolValue("0", b) && !b);
	ASSERT_TRUE(ParseBoolValue("false", b) && !b);
	ASSERT_TRUE(ParseBoolValue("no", b) && !b);
}

TEST(PathPatterns, ParseBoolValue_RejectsAnythingElse)
{
	bool b = true;
	ASSERT_TRUE(!ParseBoolValue("maybe", b));
	ASSERT_TRUE(!ParseBoolValue("", b));
	ASSERT_TRUE(!ParseBoolValue("TRUE", b));
	// Left alone on rejection.
	ASSERT_TRUE(b);
}
