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

#include <map>
#include <string>
#include <vector>

// URL-path primitives used by the REST router. Kept dependency-free
// (no wx, no amule-internal headers) so the unit tests can link this
// translation unit on its own.

namespace web_api_path
{

// Splits `path` on '/'. A leading '/' produces no leading empty
// segment; a trailing '/' produces a trailing empty segment (so
// "/a/" → ["a", ""] is distinguishable from "/a" → ["a"]).
std::vector<std::string> SplitPath(const std::string &path);

// Returns true if the raw path looks like a traversal/injection
// attempt: contains a NUL byte, encoded NUL (%00), a literal ".."
// segment, or percent-encoded ".." (`%2e%2e` in any case).
// Defence-in-depth — call before routing, reject with 400. Any
// future endpoint that admits path captures inherits the
// protection.
bool LooksMalicious(const std::string &path);

// Parses ?k=v&k2=v2 into a map. Percent-decodes `%hh` pairs and
// converts `+` to space per application/x-www-form-urlencoded.
// Malformed `%hh` triplets pass through verbatim so a stray `%` in
// a path query doesn't silently drop characters.
std::map<std::string, std::string> ParseQuery(const std::string &q);

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

// Matches `path_segments` against `pattern`. On match, fills
// `out_captures` with the captured segment values and returns true.
bool Match(const RoutePattern &pattern,
	const std::vector<std::string> &path_segments,
	std::map<std::string, std::string> &out_captures);

// Two patterns are "shape-equivalent" if they would match the same
// set of paths regardless of capture names. Used at route-registration
// time to flag duplicate routes.
bool ShapeEqual(const RoutePattern &a, const RoutePattern &b);

} // namespace web_api_path

#endif // LIBWEBCOMMON_PATHPATTERNS_H
