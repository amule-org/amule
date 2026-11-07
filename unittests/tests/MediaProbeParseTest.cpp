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

#include <vector>

#include "MediaProbe.h"

using namespace muleunit;

DECLARE_SIMPLE(MediaProbeParse)

namespace
{

const wxChar *const kValid[] = { wxT("AMMP 1"),
	wxT("format.duration=3"),
	wxT("format.bit_rate=128000"),
	wxT("streams.stream.0.codec_type=\"audio\""),
	wxT("streams.stream.0.codec_name=\"mp3\""),
	wxT("format.tags.artist=\"Artist\""),
	wxT("format.tags.album=\"Album\""),
	wxT("format.tags.title=\"Song\""),
	wxT("END") };

using Changes = std::vector<std::pair<size_t, wxString>>;

// The valid reply with some lines replaced.
bool Parse(MediaMetadata &out, const Changes &changes = {})
{
	wxArrayString lines;
	for (const wxChar *line : kValid)
		lines.Add(line);
	for (const auto &change : changes)
		lines[change.first] = change.second;
	return MediaProbe::ParseHelperOutput(lines, out);
}

wxString Title(const wxString &quoted)
{
	MediaMetadata info;
	return Parse(info, { { 7, wxT("format.tags.title=") + quoted } }) ? info.title
									  : wxString(wxT("<rejected>"));
}

} // namespace

TEST(MediaProbeParse, ValidReplyIsParsed)
{
	MediaMetadata info;
	ASSERT_TRUE(Parse(info));
	ASSERT_EQUALS(uint32(3), info.length_seconds);
	ASSERT_EQUALS(uint32(128), info.bitrate_kbps);
	ASSERT_EQUALS(wxString(wxT("mp3")), info.codec);
	ASSERT_EQUALS(wxString(wxT("Artist")), info.artist);
	ASSERT_EQUALS(wxString(wxT("Album")), info.album);
	ASSERT_EQUALS(wxString(wxT("Song")), info.title);
}

TEST(MediaProbeParse, DurationOrCodecIsEnough)
{
	const std::pair<size_t, wxString> noDuration(1, wxT("format.duration=0"));
	const std::pair<size_t, wxString> noCodec(4, wxT("streams.stream.0.codec_name=\"\""));
	MediaMetadata info;
	ASSERT_TRUE(Parse(info, { noDuration }));
	ASSERT_TRUE(Parse(info, { noCodec }));
	ASSERT_FALSE(Parse(info, { noDuration, noCodec }));
}

TEST(MediaProbeParse, MalformedFieldsAreRejected)
{
	const Changes cases = {
		{ 0, wxT("AMMP 2") },
		{ 1, wxT("format.duration=NaN") },
		{ 1, wxT("format.duration=-1") },
		{ 1, wxT("format.duration=4294967296") },
		{ 2, wxT("format.bit_rate=1001") },
		{ 2, wxT("format.bit_rate=4294967296000") },
		{ 3, wxT("streams.stream.0.codec_type=\"video\"") },
		{ 4, wxT("streams.stream.0.codec_name=\"H264\"") },
		{ 4, wxT("streams.stream.0.codec_name=\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"") },
		{ 6, wxT("format.tags.title=\"\"") },
		{ 7, wxT("format.tags.title=Song") },
		{ 7, wxT("format.tags.title=\"a\"b\"") },
		{ 7, wxT("format.tags.title=\"a\\x\"") },
		{ 8, wxT("END ") },
	};
	for (const auto &c : cases) {
		MediaMetadata info;
		ASSERT_FALSE(Parse(info, { c }));
	}
}

TEST(MediaProbeParse, BoundaryValuesAreAccepted)
{
	MediaMetadata info;
	ASSERT_TRUE(Parse(info,
		{ { 1, wxT("format.duration=4294967295") }, { 2, wxT("format.bit_rate=4294967295000") } }));
	ASSERT_EQUALS(uint32(0xffffffffu), info.length_seconds);
	ASSERT_EQUALS(uint32(0xffffffffu), info.bitrate_kbps);
}

TEST(MediaProbeParse, TagsAreUnescapedAndSanitised)
{
	// Unescaped newlines and carriage returns are then dropped as control characters.
	ASSERT_EQUALS(wxString(wxT("a\"b\\cd")), Title(wxT("\"a\\\"b\\\\c\\nd\"")));
	ASSERT_EQUALS(wxString(wxT("ab\tc")), Title(wxT("\"a\\rb\tc\"")));
	ASSERT_EQUALS(wxString(wxT("a=b=c")), Title(wxT("\"a=b=c\"")));
	ASSERT_EQUALS(
		wxString::FromUTF8("Sigur R\xc3\xb3s & Friends"), Title(wxT("\" Sigur Rós & Friends \"")));
	ASSERT_EQUALS(size_t(256), Title(wxT("\"") + wxString(wxT('A'), 5000) + wxT("\"")).length());
}
