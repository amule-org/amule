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

#ifndef JSONDEPTHSCAN_H
#define JSONDEPTHSCAN_H

#include <cstddef>
#include <string>

namespace webapi
{

//! Deepest nesting a request body may carry before it is refused unparsed.
//!
//! The deepest legitimate body on this surface is 3
//! (`{"remote_controls":{"webserver":{...}}}`), so 32 leaves ample headroom.
constexpr std::size_t kMaxJsonDepth = 32;

/**
 * Whether @a body nests no deeper than @a maxDepth.
 *
 * A pre-parse guard, not a validator. picojson's `_parse_array` /
 * `_parse_object` recurse without a depth limit of their own, so a
 * `{"a":{"a":...}}` body nested deep enough exhausts the stack of the
 * handler-pool thread the parse runs on. Whatever gets past here still has to
 * satisfy picojson, which has the final say on syntax.
 *
 * Nesting depth, not a count of openers: a flat body is legal at any length.
 * An earlier version incremented on every `{` and `[` and never decremented,
 * which turned the cap into a budget of 32 containers for the whole request --
 * a PATCH /preferences whose `shared` array held directory names like
 * "Some.Release-BRD [2023]" was refused on the 33rd bracket while only three
 * levels deep (issue #1083, fixed in #1084).
 *
 * String literals are skipped for that same reason, with backslash escapes
 * honoured so a `\"` inside one does not end it early. Unbalanced closers are
 * ignored rather than rejected.
 *
 * Skipping strings cannot hide real nesting. To undercount, this would have to
 * believe it is inside a string where picojson does not, which needs an
 * unmatched quote -- and picojson then fails on the unterminated string rather
 * than recursing. Every way the two can disagree is either conservative here
 * or a parse error there.
 */
inline bool JsonNestingWithinLimit(const std::string &body, std::size_t maxDepth = kMaxJsonDepth)
{
	std::size_t depth = 0;
	bool in_string = false;
	for (std::size_t i = 0; i < body.size(); ++i) {
		const char c = body[i];
		if (in_string) {
			if (c == '\\') {
				// Escaped char, never closes the string. Safe at the
				// end of the buffer: this can only push i to size(),
				// which the loop condition then stops on.
				++i;
			} else if (c == '"') {
				in_string = false;
			}
		} else if (c == '"') {
			in_string = true;
		} else if (c == '{' || c == '[') {
			if (++depth > maxDepth) {
				return false;
			}
		} else if ((c == '}' || c == ']') && depth > 0) {
			--depth;
		}
	}
	return true;
}

} // namespace webapi

#endif // JSONDEPTHSCAN_H
// File_checked_for_headers
