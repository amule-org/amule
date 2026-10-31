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

#include "SearchQueryTranslator.h"

#include <wx/intl.h>
#include <wx/wxcrt.h>
#include <set>

namespace
{
bool ParseDuration(const wxString &text, std::uint64_t &seconds)
{
	std::uint64_t parts[3] = { 0, 0, 0 };
	size_t component = 0;
	size_t digits = 0;
	for (wxUniChar c : text) {
		if (c == ':') {
			if (digits == 0 || component == 2 || (component > 0 && digits != 2))
				return false;
			++component;
			digits = 0;
		} else {
			if (c < '0' || c > '9' ||
				parts[component] > (SEARCH_MAX_MIN_DURATION_SECONDS - (c - '0')) / 10)
				return false;
			parts[component] = parts[component] * 10 + (c - '0');
			++digits;
		}
	}
	if (digits == 0)
		return false;
	std::uint64_t result = parts[0];
	if (component != 0) {
		if (component != 2 || digits != 2 || parts[1] > 59 || parts[2] > 59)
			return false;
		result = parts[0] * 3600 + parts[1] * 60 + parts[2];
	}
	if (result > SEARCH_MAX_MIN_DURATION_SECONDS)
		return false;
	seconds = result;
	return true;
}

bool ParseMinimum(wxString text, bool duration, uint64_t &value)
{
	bool exclusive = false;
	if (text.StartsWith(">=")) {
		text = text.Mid(2);
	} else if (text.StartsWith(">")) {
		exclusive = true;
		text = text.Mid(1);
	}
	uint64_t multiplier = 1;
	if (duration && !text.empty()) {
		const wxUniChar unit = text.Last();
		if (unit == 's' || unit == 'm' || unit == 'h') {
			multiplier = unit == 'h' ? 3600 : unit == 'm' ? 60 : 1;
			text.RemoveLast();
			if (text.Contains(':'))
				return false;
		}
	}
	const uint64_t limit = duration ? SEARCH_MAX_MIN_DURATION_SECONDS : SEARCH_MAX_MIN_BITRATE_KBPS;
	uint64_t number = 0;
	if (duration) {
		if (!ParseDuration(text, number))
			return false;
	} else {
		if (text.empty())
			return false;
		for (wxUniChar c : text) {
			if (c < '0' || c > '9' || number > (limit - (c - '0')) / 10)
				return false;
			number = number * 10 + (c - '0');
		}
	}
	if (number > limit / multiplier)
		return false;
	number *= multiplier;
	if (exclusive) {
		if (number == limit)
			return false;
		++number;
	}
	value = number;
	return true;
}

bool DecodeValue(const wxString &raw, wxString &value)
{
	if (raw.empty())
		return false;
	if (raw[0] != '"') {
		if (raw.Contains('"'))
			return false;
		value = raw;
		return true;
	}
	value.clear();
	for (size_t i = 1; i < raw.size(); ++i) {
		if (raw[i] == '"')
			return i == raw.size() - 1 && !value.empty();
		if (raw[i] == '\\') {
			if (++i == raw.size() || (raw[i] != '\\' && raw[i] != '"'))
				return false;
		}
		value += raw[i];
	}
	return false;
}
} // namespace

bool TranslateSearchQuery(const wxString &input, CSearchQueryPreview &preview, wxString &error)
{
	CSearchQueryPreview result;
	std::set<wxString> seen;
	bool lastWasFilter = false;
	bool lastWasOperator = false;
	int keywordDepth = 0;
	for (size_t start = 0; start < input.size();) {
		if (wxIsspace(input[start])) {
			++start;
			continue;
		}
		size_t end = start;
		bool quoted = false;
		for (; end < input.size(); ++end) {
			const wxUniChar c = input[end];
			if (quoted && c == '\\' && end + 1 < input.size()) {
				++end;
			} else if (c == '"') {
				quoted = !quoted;
			} else if (!quoted && wxIsspace(c)) {
				break;
			}
		}
		if (quoted) {
			error = _("Close the double quote before previewing the query.");
			return false;
		}
		const wxString token = input.Mid(start, end - start);
		start = end;
		// A colon in a quoted keyword is literal, even inside parentheses.
		int colon = wxNOT_FOUND;
		bool inQuote = false;
		for (size_t i = 0; i < token.size(); ++i) {
			if (inQuote && token[i] == '\\' && i + 1 < token.size())
				++i;
			else if (token[i] == '"')
				inQuote = !inQuote;
			else if (!inQuote && token[i] == ':') {
				colon = static_cast<int>(i);
				break;
			}
		}
		const wxString name = colon == wxNOT_FOUND ? wxString() : token.Left(colon).Lower();
		const bool known = name == "artist" || name == "album" || name == "title" ||
				   name == "codec" || name == "duration" || name == "bitrate";
		if (colon == wxNOT_FOUND || token[0] == '"' ||
			(!known && token.Mid(colon + 1).StartsWith("//"))) {
			// Operators may touch a closing parenthesis: `(jazz)OR artist:A`
			// still uses the filter as an operand and cannot become a global filter.
			const wxString last = token.AfterLast(')').AfterLast('(');
			const bool operation = last == "AND" || last == "OR" || last == "NOT";
			const wxString first = token.BeforeFirst('(').BeforeFirst(')').BeforeFirst('"');
			const bool beginsWithOperator = first == "AND" || first == "OR" || first == "NOT";
			if (lastWasFilter && beginsWithOperator) {
				error = _("Media filters apply to the whole query. Put them outside "
					  "parentheses and do not join them with AND, OR or NOT.");
				return false;
			}
			lastWasOperator = operation;
			lastWasFilter = false;
			inQuote = false;
			for (size_t i = 0; i < token.size(); ++i) {
				if (inQuote && token[i] == '\\' && i + 1 < token.size())
					++i;
				else if (token[i] == '"')
					inQuote = !inQuote;
				else if (!inQuote && token[i] == '(')
					++keywordDepth;
				else if (!inQuote && token[i] == ')') {
					if (--keywordDepth < 0) {
						error = _("Balance the parentheses before previewing the "
							  "query.");
						return false;
					}
				}
			}
			if (!result.keywords.empty())
				result.keywords += ' ';
			result.keywords += token;
			continue;
		}
		if (lastWasOperator || keywordDepth > 0) {
			error = _("Media filters apply to the whole query. Put them outside parentheses and "
				  "do not join them with AND, OR or NOT.");
			return false;
		}
		lastWasFilter = true;
		wxString *field = nullptr;
		if (name == "artist")
			field = &result.metadata.artist;
		else if (name == "album")
			field = &result.metadata.album;
		else if (name == "title")
			field = &result.metadata.title;
		else if (name == "codec")
			field = &result.metadata.codec;
		else if (name != "duration" && name != "bitrate") {
			error = wxString::Format(
				_("Unknown filter '%s'. Quote the whole token to keep it as keywords."),
				name);
			return false;
		}
		if (!seen.insert(name).second) {
			error = wxString::Format(
				_("Filter '%s' appears twice. Use one value per filter."), name);
			return false;
		}
		wxString value;
		if (!DecodeValue(token.Mid(colon + 1), value)) {
			error = wxString::Format(_("Filter '%s' needs a value. Use double quotes around "
						   "values containing spaces."),
				name);
			return false;
		}
		if (field) {
			*field = value;
		} else {
			const bool duration = name == "duration";
			uint64_t number = 0;
			if (!ParseMinimum(value, duration, number)) {
				error = duration ? _("Duration must be seconds, h:mm:ss, or a whole number "
						     "followed by s, m or h (up to 24 hours). Use >= or > "
						     "for a minimum.")
						 : _("Bitrate must be a whole number in kbps (up to "
						     "1000000). Use >= or > for a minimum.");
				return false;
			}
			if (duration)
				result.metadata.minLength = number;
			else
				result.metadata.minBitrateKbps = number;
		}
	}
	if (input.find(wxChar(0)) != wxString::npos) {
		error = _("Search input must not contain NUL characters.");
		return false;
	}
	if (keywordDepth != 0) {
		error = _("Balance the parentheses before previewing the query.");
		return false;
	}
	if (result.keywords.empty()) {
		error = _("Enter keywords as well as filters; filters alone cannot start a search.");
		return false;
	}
	preview = result;
	error.clear();
	return true;
}
