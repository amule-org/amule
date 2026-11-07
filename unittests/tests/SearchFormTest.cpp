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

#include <muleunit/test.h>
#include "SearchForm.h"

using namespace muleunit;
DECLARE_SIMPLE(SearchForm)

namespace
{
CECTag RecordedForm()
{
	CECTag entry(EC_TAG_SEARCH_ID, uint32_t(42));
	entry.AddTag(EC_TAG_SEARCH_NAME, wxString::FromUTF8("曲 AND OR NOT (12)"));
	entry.AddTag(EC_TAG_SEARCH_LIFECYCLE_KIND, uint64_t(AllSearch));
	entry.AddTag(EC_TAG_SEARCH_MIN_SIZE, uint64_t(0));
	entry.AddTag(EC_TAG_SEARCH_MAX_SIZE, uint64_t(0));
	entry.AddTag(EC_TAG_SEARCH_AVAILABILITY, uint32_t(0));
	return entry;
}
} // namespace

TEST(SearchForm, ExactSizeDisplayAndRoundTrip)
{
	struct Case
	{
		uint64_t bytes;
		const char *number;
		int unit;
	};
	const Case cases[] = { { 0, "0", 2 },
		{ 1, "1", 0 },
		{ 1024, "1", 1 },
		{ 1048576, "1", 2 },
		{ 1073741824, "1", 3 },
		{ 1048576000, "1000", 2 },
		{ 5368709121ULL, "5368709121", 0 },
		{ 18446744073709551615ULL, "18446744073709551615", 0 } };
	for (const auto &test : cases) {
		const auto display = FormatSearchFormSize(test.bytes);
		ASSERT_EQUALS(wxString(test.number), display.number);
		ASSERT_EQUALS(test.unit, display.unit);
		uint64_t actual = 0;
		ASSERT_TRUE(ParseSearchFormSize(display.number, display.unit, actual));
		ASSERT_EQUALS(test.bytes, actual);
	}
}

TEST(SearchForm, RejectOverflowAndInvalidNumbers)
{
	uint64_t value = 0;
	ASSERT_TRUE(ParseSearchFormSize("17179869183", 3, value));
	ASSERT_EQUALS(uint64_t(18446744072635809792ULL), value);
	ASSERT_TRUE(!ParseSearchFormSize("17179869184", 3, value));
	ASSERT_TRUE(!ParseSearchFormSize("18446744073709551616", 0, value));
	ASSERT_TRUE(!ParseSearchFormSize("1", -1, value));
	ASSERT_TRUE(!ParseSearchFormSize("1", 4, value));
	for (const wxString &invalid : { wxString("-1"),
		     wxString("+1"),
		     wxString("1.5"),
		     wxString("1x"),
		     wxString(" 1"),
		     wxString("1 ") }) {
		ASSERT_TRUE(!ParseSearchFormSize(invalid, 0, value));
	}
	ASSERT_TRUE(ParseSearchFormNumber("4294967295", std::numeric_limits<uint32_t>::max(), value));
	ASSERT_EQUALS(uint64_t(4294967295ULL), value);
	ASSERT_TRUE(!ParseSearchFormNumber("4294967296", std::numeric_limits<uint32_t>::max(), value));
	ASSERT_TRUE(ParseSearchFormSize("", 2, value));
	ASSERT_EQUALS(uint64_t(0), value);
}

TEST(SearchForm, DefaultsAreRecordedButLegacySearchesAreNot)
{
	CECTag legacy(EC_TAG_SEARCH_ID, uint32_t(42));
	legacy.AddTag(EC_TAG_SEARCH_NAME, wxString("query"));
	legacy.AddTag(EC_TAG_SEARCH_LIFECYCLE_KIND, uint64_t(GlobalSearch));
	ASSERT_TRUE(!DecodeSearchForm(legacy).has_value());
	const auto form = DecodeSearchForm(RecordedForm());
	ASSERT_TRUE(form.has_value());
	ASSERT_TRUE(!SearchFormHasFilters(form->params));
	ASSERT_EQUALS(static_cast<int>(AllSearch), static_cast<int>(form->type));
	ASSERT_EQUALS(wxString::FromUTF8("曲 AND OR NOT (12)"), form->params.searchString);
}

TEST(SearchForm, AllFiltersDecodeWithoutLosingBytesOrSeconds)
{
	CECTag entry(EC_TAG_SEARCH_ID, uint32_t(42));
	entry.AddTag(EC_TAG_SEARCH_NAME, wxString("music"));
	entry.AddTag(EC_TAG_SEARCH_LIFECYCLE_KIND, uint64_t(KadSearch));
	entry.AddTag(EC_TAG_SEARCH_FILE_TYPE, wxString("Audio"));
	entry.AddTag(EC_TAG_SEARCH_EXTENSION, wxString(".flac"));
	entry.AddTag(EC_TAG_SEARCH_MIN_SIZE, uint64_t(5368709121ULL));
	entry.AddTag(EC_TAG_SEARCH_MAX_SIZE, std::numeric_limits<uint64_t>::max());
	entry.AddTag(EC_TAG_SEARCH_AVAILABILITY, uint32_t(1001));
	entry.AddTag(EC_TAG_SEARCH_MIN_BITRATE, uint64_t(320));
	entry.AddTag(EC_TAG_SEARCH_MIN_LENGTH, uint64_t(61));
	entry.AddTag(EC_TAG_SEARCH_CODEC, wxString("FLAC"));
	entry.AddTag(EC_TAG_SEARCH_TITLE, wxString("title"));
	entry.AddTag(EC_TAG_SEARCH_ALBUM, wxString("album"));
	entry.AddTag(EC_TAG_SEARCH_ARTIST, wxString::FromUTF8("演奏者"));
	const auto form = DecodeSearchForm(entry);
	ASSERT_TRUE(form.has_value());
	const auto &params = form->params;
	ASSERT_TRUE(SearchFormHasFilters(params));
	ASSERT_EQUALS(uint64_t(5368709121ULL), params.minSize);
	ASSERT_EQUALS(std::numeric_limits<uint64_t>::max(), params.maxSize);
	ASSERT_EQUALS(uint32_t(1001), params.availability);
	ASSERT_EQUALS(wxString("Audio"), params.typeText);
	ASSERT_EQUALS(wxString(".flac"), params.extension);
	ASSERT_EQUALS(uint32_t(320), params.minBitrateKbps);
	ASSERT_EQUALS(uint32_t(61), params.minLength);
	ASSERT_EQUALS(wxString("FLAC"), params.codec);
	ASSERT_EQUALS(wxString("title"), params.title);
	ASSERT_EQUALS(wxString("album"), params.album);
	ASSERT_EQUALS(wxString::FromUTF8("演奏者"), params.artist);
}

TEST(SearchForm, RejectWrongTypesAndOutOfRangeMedia)
{
	auto entry = RecordedForm();
	entry.AddTag(EC_TAG_SEARCH_EXTENSION, uint64_t(123));
	ASSERT_TRUE(!DecodeSearchForm(entry).has_value());
	entry = RecordedForm();
	entry.AddTag(EC_TAG_SEARCH_MIN_LENGTH, uint64_t(86401));
	ASSERT_TRUE(!DecodeSearchForm(entry).has_value());
	entry = RecordedForm();
	entry.AddTag(EC_TAG_SEARCH_MIN_BITRATE, wxString("320"));
	ASSERT_TRUE(!DecodeSearchForm(entry).has_value());
	CECTag malformed(EC_TAG_SEARCH_ID, uint32_t(42));
	malformed.AddTag(EC_TAG_SEARCH_NAME, wxString("query"));
	malformed.AddTag(EC_TAG_SEARCH_LIFECYCLE_KIND, uint64_t(GlobalSearch));
	malformed.AddTag(EC_TAG_SEARCH_MIN_SIZE, wxString("0"));
	malformed.AddTag(EC_TAG_SEARCH_MAX_SIZE, uint64_t(0));
	malformed.AddTag(EC_TAG_SEARCH_AVAILABILITY, uint32_t(0));
	ASSERT_TRUE(!DecodeSearchForm(malformed).has_value());
}

TEST(SearchForm, BrowseAndRelatedAreNotSearchForms)
{
	CECTag entry(EC_TAG_SEARCH_ID, uint32_t(42));
	entry.AddTag(EC_TAG_SEARCH_NAME, wxString("related::0123456789ABCDEF"));
	entry.AddTag(EC_TAG_SEARCH_LIFECYCLE_KIND, uint64_t(KadSearch));
	entry.AddTag(EC_TAG_SEARCH_MIN_SIZE, uint64_t(0));
	entry.AddTag(EC_TAG_SEARCH_MAX_SIZE, uint64_t(0));
	entry.AddTag(EC_TAG_SEARCH_AVAILABILITY, uint32_t(0));
	ASSERT_TRUE(!DecodeSearchForm(entry).has_value());
	for (const auto type : { BrowseSearch, static_cast<SearchType>(3) }) {
		CECTag browse(EC_TAG_SEARCH_ID, uint32_t(42));
		browse.AddTag(EC_TAG_SEARCH_NAME, wxString("peer"));
		browse.AddTag(EC_TAG_SEARCH_LIFECYCLE_KIND, uint64_t(type));
		browse.AddTag(EC_TAG_SEARCH_MIN_SIZE, uint64_t(0));
		browse.AddTag(EC_TAG_SEARCH_MAX_SIZE, uint64_t(0));
		browse.AddTag(EC_TAG_SEARCH_AVAILABILITY, uint32_t(0));
		ASSERT_TRUE(!DecodeSearchForm(browse).has_value());
	}
}

TEST(SearchForm, FullListingRefreshesExistingAndDropsMissingForms)
{
	CSearchFormCache cache;
	CECEmptyTag listing(0);
	listing.AddTag(RecordedForm());
	cache.Replace(listing);
	ASSERT_TRUE(cache.Get(42) != nullptr);
	const CSearchForm before = *cache.Get(42);
	const uint64_t session = cache.GetSession();
	auto updated = RecordedForm();
	updated.AddTag(EC_TAG_SEARCH_EXTENSION, wxString("flac"));
	CECEmptyTag refreshed(0);
	refreshed.AddTag(updated);
	cache.Replace(refreshed);
	ASSERT_TRUE(cache.Get(42) != nullptr);
	ASSERT_EQUALS(wxString("flac"), cache.Get(42)->params.extension);
	ASSERT_TRUE(!SameSearchForm(before, *cache.Get(42)));
	ASSERT_EQUALS(session, cache.GetSession());
	// A legacy form for the same ID must remove, rather than retain, old filters.
	CECTag legacy(EC_TAG_SEARCH_ID, uint32_t(42));
	legacy.AddTag(EC_TAG_SEARCH_NAME, wxString("legacy"));
	legacy.AddTag(EC_TAG_SEARCH_LIFECYCLE_KIND, uint64_t(GlobalSearch));
	CECEmptyTag oldDaemon(0);
	oldDaemon.AddTag(legacy);
	cache.Replace(oldDaemon);
	ASSERT_TRUE(cache.Get(42) == nullptr);
	cache.Replace(listing);
	cache.Replace(CECEmptyTag(0));
	ASSERT_TRUE(cache.Get(42) == nullptr);
}

TEST(SearchForm, ClosingAndReconnectInvalidateRecordedForms)
{
	CSearchFormCache cache;
	CECEmptyTag listing(0);
	listing.AddTag(RecordedForm());
	cache.Replace(listing);
	const CSearchForm captured = *cache.Get(42);
	cache.Erase(42);
	ASSERT_TRUE(cache.Get(42) == nullptr);
	cache.Replace(listing);
	const uint64_t oldSession = cache.GetSession();
	cache.Clear();
	ASSERT_TRUE(cache.Get(42) == nullptr);
	ASSERT_TRUE(cache.GetSession() != oldSession);
	cache.Replace(listing);
	// Identical IDs and fields in a new daemon session still invalidate an open menu.
	ASSERT_TRUE(SameSearchForm(captured, *cache.Get(42)));
	ASSERT_TRUE(cache.GetSession() != oldSession);
}

TEST(SearchForm, FormIdentityIncludesEveryEditableFilter)
{
	CSearchForm expected;
	expected.params.searchString = "query";
	ASSERT_TRUE(SameSearchForm(expected, expected));
	const auto different = [&expected](auto change) {
		CSearchForm altered = expected;
		change(altered.params);
		return !SameSearchForm(expected, altered);
	};
	ASSERT_TRUE(different([](auto &p) { p.searchString = "other"; }));
	ASSERT_TRUE(different([](auto &p) { p.typeText = "Audio"; }));
	ASSERT_TRUE(different([](auto &p) { p.extension = "flac"; }));
	ASSERT_TRUE(different([](auto &p) { p.minSize = 1; }));
	ASSERT_TRUE(different([](auto &p) { p.maxSize = 1; }));
	ASSERT_TRUE(different([](auto &p) { p.availability = 1; }));
	ASSERT_TRUE(different([](auto &p) { p.minLength = 1; }));
	ASSERT_TRUE(different([](auto &p) { p.minBitrateKbps = 1; }));
	ASSERT_TRUE(different([](auto &p) { p.artist = "artist"; }));
	ASSERT_TRUE(different([](auto &p) { p.album = "album"; }));
	ASSERT_TRUE(different([](auto &p) { p.title = "title"; }));
	ASSERT_TRUE(different([](auto &p) { p.codec = "codec"; }));
	auto anotherKind = expected;
	anotherKind.type = AllSearch;
	ASSERT_TRUE(!SameSearchForm(expected, anotherKind));
}
