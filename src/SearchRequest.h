//                                                       -*- C++ -*-
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

#ifndef SEARCHREQUEST_H
#define SEARCHREQUEST_H

#include <wx/string.h>
#include <cstdint>
#include <optional>
#include <vector>

inline constexpr char RELATED_SEARCH_PREFIX[] = "related::";

inline bool IsRelatedSearchQuery(const wxString &query)
{
	return query.StartsWith(RELATED_SEARCH_PREFIX);
}

inline bool IsRunningSearchStatus(uint32_t status)
{
	return status != 0xffff && status != 0xfffe;
}

// The submitted request, independent of the result label and the search ID.
// Keep exact text: merging requests with different spelling or Boolean syntax
// is less safe than occasionally leaving two equivalent searches open.
class CSearchRequest
{
public:
	template <typename Params>
	CSearchRequest(int type, const Params &params)
	: m_type(type)
	, m_query(params.searchString)
	, m_fileType(params.typeText)
	, m_extension(params.extension)
	, m_minSize(params.minSize)
	, m_maxSize(params.maxSize)
	, m_availability(params.availability)
	, m_minBitrateKbps(params.minBitrateKbps)
	, m_minLength(params.minLength)
	, m_codec(params.codec)
	, m_title(params.title)
	, m_album(params.album)
	, m_artist(params.artist)
	{
	}

	int GetType() const { return m_type; }

	// Compare editable fields only; the Kad keyword is derived from the query.
	bool SameRequest(const CSearchRequest &other) const
	{
		return m_type == other.m_type && m_query == other.m_query && m_fileType == other.m_fileType &&
		       m_extension == other.m_extension && m_minSize == other.m_minSize &&
		       m_maxSize == other.m_maxSize && m_availability == other.m_availability &&
		       m_minBitrateKbps == other.m_minBitrateKbps && m_minLength == other.m_minLength &&
		       m_codec == other.m_codec && m_title == other.m_title && m_album == other.m_album &&
		       m_artist == other.m_artist;
	}

	bool CanReuse(const CSearchRequest &other, uint32_t progress) const
	{
		// The eD2k/Kad terminal sentinels must allow a fresh request.
		return IsRunningSearchStatus(progress) && SameRequest(other);
	}

private:
	int m_type;
	wxString m_query;
	wxString m_fileType;
	wxString m_extension;
	uint64_t m_minSize;
	uint64_t m_maxSize;
	uint32_t m_availability;
	uint32_t m_minBitrateKbps;
	uint32_t m_minLength;
	wxString m_codec, m_title, m_album, m_artist;
};

// A null request denotes a restored/discovered/browse tab or a locally stopped search.
// Missing progress is pending only when this session submitted the request.
struct CSearchReuseCandidate
{
	const CSearchRequest *request = nullptr;
	std::optional<uint32_t> progress;
};

inline size_t FindReusableSearch(
	const std::vector<CSearchReuseCandidate> &pages, const CSearchRequest &request)
{
	for (size_t i = 0; i < pages.size(); ++i) {
		if (pages[i].request && pages[i].request->CanReuse(request, pages[i].progress.value_or(0))) {
			return i;
		}
	}
	return pages.size();
}

#endif
