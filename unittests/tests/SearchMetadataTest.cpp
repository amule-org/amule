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

#include <muleunit/test.h>
#include <ec/cpp/ECSpecialTags.h>

using namespace muleunit;
DECLARE_SIMPLE(SearchMetadata)

TEST(SearchMetadata, LegacyRequestsHaveNoNewTags)
{
	CEC_Search_Tag tag("linux", EC_SEARCH_KAD, "", "", 0, 0, 0);
	ASSERT_TRUE(tag.GetTagByName(EC_TAG_SEARCH_MIN_BITRATE) == nullptr);
	ASSERT_TRUE(tag.GetTagByName(EC_TAG_SEARCH_MIN_LENGTH) == nullptr);
	ASSERT_EQUALS(size_t(2), tag.GetTagCount());
	ASSERT_EQUALS(uint64(0), tag.MinBitrate());
	ASSERT_EQUALS(uint64(0), tag.MinLength());
	ASSERT_TRUE(tag.Codec().empty());
}

TEST(SearchMetadata, AllFieldsRoundTrip)
{
	CSearchMetadata sent;
	sent.minBitrateKbps = 1000000;
	sent.minLength = 86400;
	sent.codec = "FLAC";
	sent.title = wxString::FromUTF8("曲 AND OR NOT");
	sent.album = "Test Album";
	sent.artist = "Test Artist";
	CEC_Search_Tag tag("music", EC_SEARCH_KAD, "Audio", "flac", 1, 0, 0, sent);
	ASSERT_EQUALS(sent.minBitrateKbps, tag.MinBitrate());
	ASSERT_EQUALS(sent.minLength, tag.MinLength());
	ASSERT_EQUALS(sent.codec, tag.Codec());
	ASSERT_EQUALS(sent.title, tag.Title());
	ASSERT_EQUALS(sent.album, tag.Album());
	ASSERT_EQUALS(sent.artist, tag.Artist());
}
