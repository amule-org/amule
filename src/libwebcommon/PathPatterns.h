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

#ifndef LIBWEBCOMMON_PATHPATTERNS_H
#define LIBWEBCOMMON_PATHPATTERNS_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// URL-path primitives used by the REST router. Dependency-free (no wx, no amule-internal headers)
// so the unit tests can link this translation unit on its own.

namespace web_api_path
{

// Splits `path` on '/'. A leading '/' produces no leading empty segment; a trailing one produces a
// trailing empty segment, so "/a/" -> ["a", ""] differs from "/a" -> ["a"].
std::vector<std::string> SplitPath(const std::string &path);

// True if the raw path looks like a traversal/injection attempt: a NUL byte, an encoded NUL (%00),
// a literal ".." segment, or percent-encoded ".." (`%2e%2e`, any case). Defence in depth -- call
// before routing, reject with 400. Any future endpoint admitting path captures inherits it.
bool LooksMalicious(const std::string &path);

// Strips one trailing '/' so `/x/` routes exactly as `/x` does.
//
// Without it the two spellings disagree depending on which kind of route they land on: a literal
// route is compared with `==` and simply misses, while a capture route matches with the capture
// bound to the empty string, leaving the handler to reject a URL that names no resource -- picking
// its own status code as it does so.
//
// Never strips the root, and never strips more than one: `//` is malformed rather than a synonym,
// and collapsing it would let `/a//b` reach the route for `/a/b`.
std::string StripTrailingSlash(const std::string &path);

// Parses ?k=v&k2=v2 into a map. Percent-decodes `%hh` pairs and converts `+` to space per
// application/x-www-form-urlencoded. Malformed `%hh` triplets pass through verbatim, so a stray `%`
// does not silently drop characters.
std::map<std::string, std::string> ParseQuery(const std::string &q);

// Parses a decimal query-parameter value into `out`, requiring [min, max] inclusive. False on an
// empty value, any non-digit, or a value out of range -- the caller turns that into its own
// rejection.
//
// The running value is bounded inside the loop, so a long digit string cannot wrap before the range
// check sees it.
bool ParseBoundedUint(const std::string &s, std::uint64_t min, std::uint64_t max, std::uint64_t &out);

// Parses a boolean query-parameter value: 1/0, true/false, yes/no. Returns
// false on anything else rather than defaulting, so a typo is answerable.
bool ParseBoolValue(const std::string &s, bool &out);

// A pattern is a path string with optional `{name}` capture segments.
// Example: "/downloads/{hash}/pause" parses to
//  segments      = ["downloads", "{hash}", "pause"]
//  capture_names = ["", "hash", ""]
struct RoutePattern
{
	std::vector<std::string> segments;
	// Per-segment capture name. Empty when the segment is a literal.
	std::vector<std::string> capture_names;
};

RoutePattern ParsePattern(const std::string &pattern);

// Matches `path_segments` against `pattern`. On match, fills `out_captures` with the captured
// values and returns true.
//
// An empty capture is not a match. Every capture on the surface names a resource -- a hash, an
// ecid, an index, an address -- and none can be the empty string, so a path binding one is
// malformed rather than merely absent. Rejecting here keeps that judgement in one place instead of
// leaving each handler to invent a status code.
bool Match(const RoutePattern &pattern,
	const std::vector<std::string> &path_segments,
	std::map<std::string, std::string> &out_captures);

// Two patterns are "shape-equivalent" if they match the same set of paths regardless of capture
// names. Used at route-registration time to flag duplicates.
bool ShapeEqual(const RoutePattern &a, const RoutePattern &b);

} // namespace web_api_path

#endif // LIBWEBCOMMON_PATHPATTERNS_H
