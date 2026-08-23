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

// The pre-parse guard that stops a deeply nested request body exhausting the
// handler thread's stack inside picojson's recursive descent.
//
// It has already been wrong once in the direction that rejects valid input:
// counting every opener in the buffer rather than tracking nesting turned the
// cap into a budget of 32 containers for the whole request, so a flat array of
// directory names containing brackets was refused three levels deep (#1084).
// These pin both directions -- that legal bodies get through whatever their
// length, and that the depth limit itself still bites.

#include <muleunit/test.h>

#include <JsonDepthScan.h>

using namespace muleunit;
using webapi::JsonNestingWithinLimit;

DECLARE_SIMPLE(JsonDepthScan)

namespace
{
//! `open` repeated n times, then its closer n times: n levels of real nesting.
std::string Nested(std::size_t n, char open, char close)
{
	return std::string(n, open) + std::string(n, close);
}
} // namespace

// The regression #1084 fixed: brackets inside string values are not nesting.
// Forty of them in a flat array is three levels deep, not forty-three.
TEST(JsonDepthScan, BracketsInsideStringsAreNotNesting)
{
	std::string body = "{\"shared\":[";
	for (int i = 0; i < 40; ++i) {
		if (i) {
			body += ",";
		}
		body += "\"Some.Release-BRD [2023]\"";
	}
	body += "]}";
	ASSERT_TRUE(JsonNestingWithinLimit(body));
}

// A flat body is legal at any length; only nesting counts.
TEST(JsonDepthScan, ManySiblingContainersAreNotNesting)
{
	std::string body = "[";
	for (int i = 0; i < 5000; ++i) {
		if (i) {
			body += ",";
		}
		body += "[1]";
	}
	body += "]";
	ASSERT_TRUE(JsonNestingWithinLimit(body));
}

// The limit still bites, which is the whole point of the guard.
TEST(JsonDepthScan, RealNestingIsCappedAtTheLimit)
{
	ASSERT_TRUE(JsonNestingWithinLimit(Nested(webapi::kMaxJsonDepth, '[', ']')));
	ASSERT_FALSE(JsonNestingWithinLimit(Nested(webapi::kMaxJsonDepth + 1, '[', ']')));
	ASSERT_TRUE(JsonNestingWithinLimit(Nested(webapi::kMaxJsonDepth, '{', '}')));
	ASSERT_FALSE(JsonNestingWithinLimit(Nested(webapi::kMaxJsonDepth + 1, '{', '}')));
}

// The cap is a parameter so the boundary can be pinned without building a
// 32-deep body for every case.
TEST(JsonDepthScan, BoundaryIsInclusive)
{
	ASSERT_TRUE(JsonNestingWithinLimit("[[[]]]", 3));
	ASSERT_FALSE(JsonNestingWithinLimit("[[[[]]]]", 3));
}

// A quote escaped inside a string must not end it early, or the openers that
// follow would be counted as nesting.
TEST(JsonDepthScan, EscapedQuoteDoesNotEndTheString)
{
	ASSERT_TRUE(JsonNestingWithinLimit("{\"a\":\"he said \\\" then [[[[[[[[[[\"}", 3));
}

// A backslash escaping a backslash leaves the next quote free to close.
TEST(JsonDepthScan, EscapedBackslashLetsTheStringClose)
{
	// {"a":"c:\\"} -- the string ends at the quote, so the closing brace
	// is real and the body is one level deep.
	ASSERT_TRUE(JsonNestingWithinLimit("{\"a\":\"c:\\\\\"}", 1));
}

// A backslash as the final byte must not read past the buffer.
TEST(JsonDepthScan, TrailingBackslashDoesNotOverrun)
{
	ASSERT_TRUE(JsonNestingWithinLimit("{\"a\":\"x\\", 2));
}

// Unbalanced closers are ignored rather than rejected: this is a stack guard,
// and picojson below still has the final say on syntax.
TEST(JsonDepthScan, UnbalancedClosersAreIgnored)
{
	ASSERT_TRUE(JsonNestingWithinLimit("}}}]]]{\"a\":1}", 1));
}

// Openers hidden in a string are ignored here, and are equally not nesting to
// picojson -- the two agree, which is what keeps the guard sound.
TEST(JsonDepthScan, OpenersInsideAStringDoNotCount)
{
	ASSERT_TRUE(JsonNestingWithinLimit("{\"a\":\"[[[[[[[[[[[[[[[[[[[[\"}", 1));
}

// The deepest body this surface legitimately sends, per the curl suite.
TEST(JsonDepthScan, DeepestLegitimateBodyIsWellWithinTheCap)
{
	const std::string body =
		"{\"remote_controls\":{\"webserver\":{\"port\":4711},\"amuleapi\":{\"bind\":\"x\"}}}";
	ASSERT_TRUE(JsonNestingWithinLimit(body, 3));
	ASSERT_FALSE(JsonNestingWithinLimit(body, 2));
}
