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

CECTag WithoutField(ec_tagname_t omitted)
{
	CECTag entry(EC_TAG_SEARCH_ID, uint32_t(42));
	for (const auto &field : RecordedForm()) {
		if (field.GetTagName() != omitted)
			entry.AddTag(field);
	}
	return entry;
}

CECTag WithField(const CECTag &field)
{
	auto entry = WithoutField(field.GetTagName());
	entry.AddTag(field);
	return entry;
}

CECTag WithId(ec_tagname_t name, uint64_t id)
{
	CECTag entry(name, id);
	for (const auto &field : RecordedForm())
		entry.AddTag(field);
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
	for (const uint64_t type : { uint64_t(BrowseSearch), uint64_t(3) }) {
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
	ASSERT_TRUE(!CSearchRequest(before.type, before.params)
			     .SameRequest(CSearchRequest(cache.Get(42)->type, cache.Get(42)->params)));
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
	ASSERT_TRUE(CSearchRequest(captured.type, captured.params)
			    .SameRequest(CSearchRequest(cache.Get(42)->type, cache.Get(42)->params)));
	ASSERT_TRUE(cache.GetSession() != oldSession);
}

TEST(SearchForm, SizeUnitBoundariesRemainExact)
{
	struct Case
	{
		const char *maximum;
		const char *overflow;
		int unit;
		uint64_t bytes;
	};
	const Case cases[] = { { "18446744073709551615", "18446744073709551616", 0, 18446744073709551615ULL },
		{ "18014398509481983", "18014398509481984", 1, 18446744073709550592ULL },
		{ "17592186044415", "17592186044416", 2, 18446744073708503040ULL },
		{ "17179869183", "17179869184", 3, 18446744072635809792ULL } };
	for (const auto &test : cases) {
		uint64_t value = 0;
		ASSERT_TRUE(ParseSearchFormSize(test.maximum, test.unit, value));
		ASSERT_EQUALS(test.bytes, value);
		ASSERT_TRUE(!ParseSearchFormSize(test.overflow, test.unit, value));
	}
	for (uint64_t bytes : { 1023ULL, 1025ULL, 1048575ULL, 1048577ULL, 1073741823ULL, 1073741825ULL }) {
		const auto display = FormatSearchFormSize(bytes);
		ASSERT_EQUALS(0, display.unit);
		uint64_t value = 0;
		ASSERT_TRUE(ParseSearchFormSize(display.number, display.unit, value));
		ASSERT_EQUALS(bytes, value);
	}
}

TEST(SearchForm, NumberParserAcceptsZeroAndRejectsNonAsciiDigits)
{
	uint64_t value = 99;
	ASSERT_TRUE(ParseSearchFormNumber("", 0, value));
	ASSERT_EQUALS(uint64_t(0), value);
	ASSERT_TRUE(ParseSearchFormNumber("0000", 0, value));
	ASSERT_EQUALS(uint64_t(0), value);
	ASSERT_TRUE(!ParseSearchFormNumber("1", 0, value));
	ASSERT_TRUE(ParseSearchFormNumber("00042", 42, value));
	ASSERT_EQUALS(uint64_t(42), value);
	ASSERT_TRUE(!ParseSearchFormNumber("43", 42, value));
	for (const auto *invalid : { "１２", "١٢", "1\n", "1\t", "1e3", "0x10" })
		ASSERT_TRUE(!ParseSearchFormNumber(wxString::FromUTF8(invalid), UINT64_MAX, value));
}

TEST(SearchForm, EveryRequiredFieldMustBePresentAndCorrectlyTyped)
{
	const ec_tagname_t fields[] = { EC_TAG_SEARCH_NAME,
		EC_TAG_SEARCH_LIFECYCLE_KIND,
		EC_TAG_SEARCH_MIN_SIZE,
		EC_TAG_SEARCH_MAX_SIZE,
		EC_TAG_SEARCH_AVAILABILITY };
	for (const auto field : fields) {
		ASSERT_TRUE(!DecodeSearchForm(WithoutField(field)).has_value());
		const CECTag wrongType = field == EC_TAG_SEARCH_NAME ? CECTag(field, uint64_t(0))
								     : CECTag(field, wxString("0"));
		ASSERT_TRUE(!DecodeSearchForm(WithField(wrongType)).has_value());
	}
	ASSERT_TRUE(DecodeSearchForm(RecordedForm()).has_value());
}

TEST(SearchForm, AllSearchNetworksDecodeWithoutChangingTheirIdentity)
{
	for (const auto kind : { LocalSearch, GlobalSearch, KadSearch, AllSearch }) {
		const auto form =
			DecodeSearchForm(WithField(CECTag(EC_TAG_SEARCH_LIFECYCLE_KIND, uint64_t(kind))));
		ASSERT_TRUE(form.has_value());
		ASSERT_EQUALS(static_cast<int>(kind), static_cast<int>(form->type));
	}
	ASSERT_TRUE(
		!DecodeSearchForm(WithField(CECTag(EC_TAG_SEARCH_LIFECYCLE_KIND, UINT64_MAX))).has_value());
}

TEST(SearchForm, OptionalStringsRejectWrongTypesAndPreserveText)
{
	const ec_tagname_t fields[] = { EC_TAG_SEARCH_FILE_TYPE,
		EC_TAG_SEARCH_EXTENSION,
		EC_TAG_SEARCH_CODEC,
		EC_TAG_SEARCH_TITLE,
		EC_TAG_SEARCH_ALBUM,
		EC_TAG_SEARCH_ARTIST };
	for (const auto field : fields)
		ASSERT_TRUE(!DecodeSearchForm(WithField(CECTag(field, uint64_t(1)))).has_value());
	auto entry = RecordedForm();
	const auto artist = wxString::FromUTF8("演奏者 AND OR NOT");
	entry.AddTag(EC_TAG_SEARCH_ARTIST, artist);
	entry.AddTag(EC_TAG_SEARCH_EXTENSION, wxString(".FLAC"));
	const auto form = DecodeSearchForm(entry);
	ASSERT_TRUE(form.has_value());
	ASSERT_EQUALS(artist, form->params.artist);
	ASSERT_EQUALS(wxString(".FLAC"), form->params.extension);
}

TEST(SearchForm, NumericFilterLimitsAcceptTheirMaximumAndRejectTheNextValue)
{
	struct Case
	{
		ec_tagname_t field;
		uint64_t maximum;
	};
	const Case cases[] = { { EC_TAG_SEARCH_AVAILABILITY, 4294967295ULL },
		{ EC_TAG_SEARCH_MIN_LENGTH, 86400 },
		{ EC_TAG_SEARCH_MIN_BITRATE, 1000000 } };
	for (const auto &test : cases) {
		const auto form = DecodeSearchForm(WithField(CECTag(test.field, test.maximum)));
		ASSERT_TRUE(form.has_value());
		const uint64_t actual = test.field == EC_TAG_SEARCH_AVAILABILITY ? form->params.availability
					: test.field == EC_TAG_SEARCH_MIN_LENGTH
						? form->params.minLength
						: form->params.minBitrateKbps;
		ASSERT_EQUALS(test.maximum, actual);
		ASSERT_TRUE(!DecodeSearchForm(WithField(CECTag(test.field, test.maximum + 1))).has_value());
		ASSERT_TRUE(!DecodeSearchForm(WithField(CECTag(test.field, wxString("1")))).has_value());
	}
}

TEST(SearchForm, EachFilterOpensExtendedParametersButQueryAndKeywordDoNot)
{
	CSearchList::CSearchParams params;
	params.searchString = "query";
	params.strKeyword = "derived keyword";
	ASSERT_TRUE(!SearchFormHasFilters(params));
	const auto hasFilter = [&params](auto change) {
		auto edited = params;
		change(edited);
		return SearchFormHasFilters(edited);
	};
	ASSERT_TRUE(hasFilter([](auto &p) { p.typeText = "Audio"; }));
	ASSERT_TRUE(hasFilter([](auto &p) { p.extension = "flac"; }));
	ASSERT_TRUE(hasFilter([](auto &p) { p.minSize = 1; }));
	ASSERT_TRUE(hasFilter([](auto &p) { p.maxSize = 1; }));
	ASSERT_TRUE(hasFilter([](auto &p) { p.availability = 1; }));
	ASSERT_TRUE(hasFilter([](auto &p) { p.minLength = 1; }));
	ASSERT_TRUE(hasFilter([](auto &p) { p.minBitrateKbps = 1; }));
	ASSERT_TRUE(hasFilter([](auto &p) { p.artist = "artist"; }));
	ASSERT_TRUE(hasFilter([](auto &p) { p.album = "album"; }));
	ASSERT_TRUE(hasFilter([](auto &p) { p.title = "title"; }));
	ASSERT_TRUE(hasFilter([](auto &p) { p.codec = "codec"; }));
}

TEST(SearchForm, FileTypesUseProtocolValuesInTheDesktopChoiceOrder)
{
	const std::array<wxString, 8> expected = {
		"", "Arc", "Audio", "Iso", "Image", "Pro", "Doc", "Video"
	};
	const auto &actual = SearchFormFileTypes();
	for (size_t i = 0; i < expected.size(); ++i)
		ASSERT_EQUALS(expected[i], actual[i]);
}

TEST(SearchForm, CacheIgnoresMalformedRowsWithoutAliasingValidSearchIds)
{
	CECEmptyTag listing(0);
	listing.AddTag(RecordedForm());
	listing.AddTag(WithId(EC_TAG_SEARCH_ID, 0));
	listing.AddTag(WithId(EC_TAG_SEARCH_ID, 4294967338ULL)); // Would truncate to 42.
	listing.AddTag(WithId(EC_TAG_SEARCH_NAME, 43));
	CECTag stringId(EC_TAG_SEARCH_ID, wxString("44"));
	for (const auto &field : RecordedForm())
		stringId.AddTag(field);
	listing.AddTag(stringId);
	listing.AddTag(WithId(EC_TAG_SEARCH_ID, 4294967295ULL));
	CSearchFormCache cache;
	CECEmptyTag overflowingId(0);
	overflowingId.AddTag(WithId(EC_TAG_SEARCH_ID, 4294967338ULL));
	cache.Replace(overflowingId);
	ASSERT_TRUE(cache.Get(42) == nullptr);
	cache.Replace(listing);
	ASSERT_TRUE(cache.Get(0) == nullptr);
	ASSERT_TRUE(cache.Get(42) != nullptr);
	ASSERT_TRUE(cache.Get(43) == nullptr);
	ASSERT_TRUE(cache.Get(44) == nullptr);
	ASSERT_TRUE(cache.Get(UINT32_MAX) != nullptr);
	ASSERT_EQUALS(wxString::FromUTF8("曲 AND OR NOT (12)"), cache.Get(42)->params.searchString);
}

TEST(SearchForm, RefreshAndCloseOnlyInvalidateTheirOwnSearch)
{
	CECEmptyTag listing(0);
	listing.AddTag(RecordedForm());
	listing.AddTag(WithId(EC_TAG_SEARCH_ID, 43));
	CSearchFormCache cache;
	cache.Replace(listing);
	const CSearchForm captured = *cache.Get(42);
	const auto session = cache.GetSession();
	CECEmptyTag refresh(0);
	refresh.AddTag(WithField(CECTag(EC_TAG_SEARCH_MAX_SIZE, wxString("broken"))));
	refresh.AddTag(WithId(EC_TAG_SEARCH_ID, 43));
	cache.Replace(refresh);
	ASSERT_TRUE(cache.Get(42) == nullptr);
	ASSERT_TRUE(cache.Get(43) != nullptr);
	ASSERT_EQUALS(session, cache.GetSession());
	ASSERT_EQUALS(wxString::FromUTF8("曲 AND OR NOT (12)"), captured.params.searchString);
	cache.Replace(listing);
	cache.Erase(42);
	cache.Erase(999); // An already-closed tab must not affect another form.
	ASSERT_TRUE(cache.Get(42) == nullptr);
	ASSERT_TRUE(cache.Get(43) != nullptr);
	ASSERT_EQUALS(session, cache.GetSession());
}

TEST(SearchForm, MediaCapabilityChecksIgnoreOrdinaryFilters)
{
	CSearchList::CSearchParams params;
	params.searchString = "query";
	params.typeText = "Audio";
	params.extension = "flac";
	params.minSize = 1;
	params.maxSize = 2;
	params.availability = 3;
	ASSERT_TRUE(SearchFormHasFilters(params));
	ASSERT_TRUE(!SearchFormHasMediaFilters(params));
	const auto hasMedia = [&params](auto change) {
		auto altered = params;
		change(altered);
		return SearchFormHasMediaFilters(altered) && SearchFormHasFilters(altered);
	};
	ASSERT_TRUE(hasMedia([](auto &p) { p.minLength = 1; }));
	ASSERT_TRUE(hasMedia([](auto &p) { p.minBitrateKbps = 1; }));
	ASSERT_TRUE(hasMedia([](auto &p) { p.artist = "artist"; }));
	ASSERT_TRUE(hasMedia([](auto &p) { p.album = "album"; }));
	ASSERT_TRUE(hasMedia([](auto &p) { p.title = "title"; }));
	ASSERT_TRUE(hasMedia([](auto &p) { p.codec = "codec"; }));
}

TEST(SearchForm, ChoiceIndicesFollowAvailableValuesRatherThanProtocolNumbers)
{
	const std::vector<SearchType> types = { KadSearch, AllSearch, LocalSearch };
	ASSERT_EQUALS(0, FindSearchFormChoice(types, KadSearch));
	ASSERT_EQUALS(1, FindSearchFormChoice(types, AllSearch));
	ASSERT_EQUALS(2, FindSearchFormChoice(types, LocalSearch));
	ASSERT_EQUALS(wxNOT_FOUND, FindSearchFormChoice(types, GlobalSearch));
	ASSERT_EQUALS(wxNOT_FOUND, FindSearchFormChoice(std::vector<SearchType>(), KadSearch));
	ASSERT_EQUALS(0, FindSearchFormChoice(SearchFormFileTypes(), wxString()));
	ASSERT_EQUALS(2, FindSearchFormChoice(SearchFormFileTypes(), wxString("Audio")));
	ASSERT_EQUALS(7, FindSearchFormChoice(SearchFormFileTypes(), wxString("Video")));
	ASSERT_EQUALS(wxNOT_FOUND, FindSearchFormChoice(SearchFormFileTypes(), wxString("unknown")));
}
