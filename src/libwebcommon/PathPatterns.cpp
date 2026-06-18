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

#include "PathPatterns.h"


namespace web_api_path {


std::vector<std::string> SplitPath(const std::string &path)
{
	std::vector<std::string> out;
	if (path.empty()) {
		return out;
	}
	// A leading '/' is the conventional "absolute" form. We skip the
	// empty segment it would otherwise produce; a trailing '/' still
	// emits its empty segment so it's distinguishable from "no
	// trailing slash".
	size_t i = (path[0] == '/') ? 1 : 0;
	std::string cur;
	for (; i < path.size(); ++i) {
		if (path[i] == '/') {
			out.push_back(cur);
			cur.clear();
		} else {
			cur += path[i];
		}
	}
	out.push_back(cur);
	return out;
}


namespace {
int HexNibble(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

// Percent-decode an application/x-www-form-urlencoded fragment.
// `+` → space (form convention); `%hh` → byte; malformed `%hh`
// passes through verbatim so a stray `%` doesn't drop characters.
std::string PercentDecode(const std::string &in)
{
	std::string out;
	out.reserve(in.size());
	for (size_t i = 0; i < in.size(); ++i) {
		if (in[i] == '+') { out += ' '; }
		else if (in[i] == '%' && i + 2 < in.size()) {
			const int hi = HexNibble(in[i + 1]);
			const int lo = HexNibble(in[i + 2]);
			if (hi >= 0 && lo >= 0) {
				out += static_cast<char>((hi << 4) | lo);
				i += 2;
			} else {
				out += in[i];
			}
		} else {
			out += in[i];
		}
	}
	return out;
}
}  // namespace

std::map<std::string, std::string> ParseQuery(const std::string &q)
{
	std::map<std::string, std::string> out;
	std::string key, val;
	bool in_val = false;
	for (size_t i = 0; i < q.size(); ++i) {
		const char c = q[i];
		if (c == '=' && !in_val) {
			in_val = true;
		} else if (c == '&') {
			if (!key.empty()) {
				out[PercentDecode(key)] = PercentDecode(val);
			}
			key.clear();
			val.clear();
			in_val = false;
		} else {
			(in_val ? val : key) += c;
		}
	}
	if (!key.empty()) {
		out[PercentDecode(key)] = PercentDecode(val);
	}
	return out;
}


RoutePattern ParsePattern(const std::string &pattern)
{
	RoutePattern out;
	out.segments = SplitPath(pattern);
	out.capture_names.reserve(out.segments.size());
	for (size_t i = 0; i < out.segments.size(); ++i) {
		const std::string &seg = out.segments[i];
		if (seg.size() >= 2 && seg.front() == '{' && seg.back() == '}') {
			out.capture_names.push_back(seg.substr(1, seg.size() - 2));
		} else {
			out.capture_names.push_back(std::string());
		}
	}
	return out;
}


bool Match(const RoutePattern &pattern,
           const std::vector<std::string> &path_segments,
           std::map<std::string, std::string> &out_captures)
{
	if (pattern.segments.size() != path_segments.size()) {
		return false;
	}
	std::map<std::string, std::string> caps;
	for (size_t i = 0; i < pattern.segments.size(); ++i) {
		if (!pattern.capture_names[i].empty()) {
			caps[pattern.capture_names[i]] = path_segments[i];
		} else if (pattern.segments[i] != path_segments[i]) {
			return false;
		}
	}
	out_captures.swap(caps);
	return true;
}


bool ShapeEqual(const RoutePattern &a, const RoutePattern &b)
{
	if (a.segments.size() != b.segments.size()) return false;
	for (size_t i = 0; i < a.segments.size(); ++i) {
		const bool a_cap = !a.capture_names[i].empty();
		const bool b_cap = !b.capture_names[i].empty();
		if (a_cap != b_cap) return false;
		if (!a_cap && a.segments[i] != b.segments[i]) return false;
	}
	return true;
}


} // namespace web_api_path
