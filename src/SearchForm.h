//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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

#ifndef SEARCHFORM_H
#define SEARCHFORM_H

#include "SearchList.h"
#include "SearchRequest.h"
#include <ec/cpp/ECSpecialTags.h>
#include <tags/FileTags.h>
#include <array>
#include <limits>
#include <optional>
#include <map>
#include <algorithm>

// Recorded form data is independent of a tab's running/reusable request.
struct CSearchForm
{
	CSearchList::CSearchParams params;
	SearchType type = LocalSearch;
};

template <typename Choices, typename Value>
inline int FindSearchFormChoice(const Choices &choices, const Value &value)
{
	const auto found = std::find(choices.begin(), choices.end(), value);
	return found == choices.end() ? wxNOT_FOUND : static_cast<int>(found - choices.begin());
}

inline const std::array<wxString, 8> &SearchFormFileTypes()
{
	static const std::array<wxString, 8> types = { wxString(),
		ED2KFTSTR_ARCHIVE,
		ED2KFTSTR_AUDIO,
		ED2KFTSTR_CDIMAGE,
		ED2KFTSTR_IMAGE,
		ED2KFTSTR_PROGRAM,
		ED2KFTSTR_DOCUMENT,
		ED2KFTSTR_VIDEO };
	return types;
}

// Empty is the form's zero/no-limit value. Reject signs, overflow and trailing junk.
inline bool ParseSearchFormNumber(const wxString &text, uint64_t limit, uint64_t &value)
{
	value = 0;
	for (const wxUniChar digit : text) {
		if (digit < '0' || digit > '9')
			return false;
		const auto n = static_cast<uint64_t>(digit.GetValue() - '0');
		if (n > limit || value > (limit - n) / 10)
			return false;
		value = value * 10 + n;
	}
	return true;
}

inline bool ParseSearchFormSize(const wxString &text, int unit, uint64_t &bytes)
{
	if (unit < 0 || unit > 3)
		return false;
	const uint64_t factor = uint64_t(1) << (10 * unit);
	uint64_t number = 0;
	if (!ParseSearchFormNumber(text, std::numeric_limits<uint64_t>::max() / factor, number))
		return false;
	bytes = number * factor;
	return true;
}

struct CSearchFormSize
{
	wxString number;
	int unit = 2;
};

inline CSearchFormSize FormatSearchFormSize(uint64_t bytes)
{
	if (bytes == 0)
		return { "0", 2 };
	for (int unit = 3; unit >= 0; --unit) {
		const uint64_t factor = uint64_t(1) << (10 * unit);
		if (bytes % factor == 0)
			return { wxString::Format("%llu", static_cast<unsigned long long>(bytes / factor)),
				unit };
	}
	return {};
}

inline bool SearchFormHasMediaFilters(const CSearchList::CSearchParams &params)
{
	return params.minBitrateKbps != 0 || params.minLength != 0 || !params.codec.empty() ||
	       !params.title.empty() || !params.album.empty() || !params.artist.empty();
}

inline bool SearchFormHasFilters(const CSearchList::CSearchParams &params)
{
	return !params.typeText.empty() || !params.extension.empty() || params.minSize != 0 ||
	       params.maxSize != 0 || params.availability != 0 || SearchFormHasMediaFilters(params);
}

// Numeric tags are always sent for a recorded form, even with all-default filters.
// Their absence is different from zero: old daemons and legacy saved searches have
// no recorded form. Decode optional strings without synthesizing missing filters.
inline std::optional<CSearchForm> DecodeSearchForm(const CECTag &entry)
{
	CSearchForm form;
	const CECTag *name = entry.GetTagByName(EC_TAG_SEARCH_NAME);
	const CECTag *kind = entry.GetTagByName(EC_TAG_SEARCH_LIFECYCLE_KIND);
	if (!name || !name->IsString() || !kind || !kind->IsInt())
		return std::nullopt;
	const uint64_t type = kind->GetInt();
	if (type != LocalSearch && type != GlobalSearch && type != KadSearch && type != AllSearch)
		return std::nullopt;
	form.type = static_cast<SearchType>(type);
	form.params.searchString = name->GetStringData();
	if (IsRelatedSearchQuery(form.params.searchString))
		return std::nullopt;
	const auto readNumber =
		[&entry](ec_tagname_t tagName, uint64_t limit, uint64_t &value, bool required) {
			const CECTag *tag = entry.GetTagByName(tagName);
			if (!tag)
				return !required;
			if (!tag->IsInt() || tag->GetInt() > limit)
				return false;
			value = tag->GetInt();
			return true;
		};
	const auto readString = [&entry](ec_tagname_t tagName, wxString &value) {
		const CECTag *tag = entry.GetTagByName(tagName);
		if (!tag)
			return true;
		if (!tag->IsString())
			return false;
		value = tag->GetStringData();
		return true;
	};
	uint64_t availability = 0, bitrate = 0, length = 0;
	if (!readNumber(EC_TAG_SEARCH_MIN_SIZE,
		    std::numeric_limits<uint64_t>::max(),
		    form.params.minSize,
		    true) ||
		!readNumber(EC_TAG_SEARCH_MAX_SIZE,
			std::numeric_limits<uint64_t>::max(),
			form.params.maxSize,
			true) ||
		!readNumber(EC_TAG_SEARCH_AVAILABILITY,
			std::numeric_limits<uint32_t>::max(),
			availability,
			true) ||
		!readNumber(EC_TAG_SEARCH_MIN_BITRATE, SEARCH_MAX_MIN_BITRATE_KBPS, bitrate, false) ||
		!readNumber(EC_TAG_SEARCH_MIN_LENGTH, SEARCH_MAX_MIN_DURATION_SECONDS, length, false) ||
		!readString(EC_TAG_SEARCH_FILE_TYPE, form.params.typeText) ||
		!readString(EC_TAG_SEARCH_EXTENSION, form.params.extension) ||
		!readString(EC_TAG_SEARCH_CODEC, form.params.codec) ||
		!readString(EC_TAG_SEARCH_TITLE, form.params.title) ||
		!readString(EC_TAG_SEARCH_ALBUM, form.params.album) ||
		!readString(EC_TAG_SEARCH_ARTIST, form.params.artist))
		return std::nullopt;
	form.params.availability = static_cast<uint32_t>(availability);
	form.params.minBitrateKbps = static_cast<uint32_t>(bitrate);
	form.params.minLength = static_cast<uint32_t>(length);
	return form;
}

// A listing replaces every recorded form, including forms for already-open tabs.
// Session changes also invalidate actions captured by an open context menu.
class CSearchFormCache
{
public:
	const CSearchForm *Get(uint32_t id) const
	{
		const auto it = m_forms.find(id);
		return it == m_forms.end() ? nullptr : &it->second;
	}
	uint64_t GetSession() const { return m_session; }
	void Clear()
	{
		m_forms.clear();
		++m_session;
	}
	void Erase(uint32_t id) { m_forms.erase(id); }
	void Replace(const CECTag &listing)
	{
		std::map<uint32_t, CSearchForm> forms;
		for (const auto &entry : listing) {
			if (entry.GetTagName() != EC_TAG_SEARCH_ID || !entry.IsInt() || entry.GetInt() == 0 ||
				entry.GetInt() > std::numeric_limits<uint32_t>::max())
				continue;
			if (auto form = DecodeSearchForm(entry))
				forms.emplace(static_cast<uint32_t>(entry.GetInt()), std::move(*form));
		}
		m_forms.swap(forms);
	}

private:
	std::map<uint32_t, CSearchForm> m_forms;
	uint64_t m_session = 0;
};

#endif // SEARCHFORM_H
