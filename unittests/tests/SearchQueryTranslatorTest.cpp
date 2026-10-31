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

#include <wx/init.h>
#include <iostream>
#include <stdexcept>

namespace
{
void Check(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}
void TestTranslation()
{
	CSearchQueryPreview preview;
	wxString error;
	Check(TranslateSearchQuery(
		      "Miles Davis album:\"Kind of Blue\" bitrate:>=192 duration:>5m", preview, error),
		"Example rejected");
	Check(preview.keywords == "Miles Davis" && preview.metadata.album == "Kind of Blue" &&
			preview.metadata.minBitrateKbps == 192 && preview.metadata.minLength == 301,
		"Example mapped incorrectly");
	Check(TranslateSearchQuery(wxString::FromUTF8("爵士 ARTIST:\"演奏者\" title:\"OR AND NOT\" "
						      "codec:FLAC duration:1:02:03 bitrate:>319"),
		      preview,
		      error),
		"Unicode query rejected");
	Check(preview.keywords == wxString::FromUTF8("爵士") &&
			preview.metadata.artist == wxString::FromUTF8("演奏者") &&
			preview.metadata.title == "OR AND NOT" && preview.metadata.codec == "FLAC" &&
			preview.metadata.minLength == 3723 && preview.metadata.minBitrateKbps == 320,
		"Fields/units mapped incorrectly");
	Check(TranslateSearchQuery("jazz artist:\"https://example.test/artist\"", preview, error) &&
			preview.metadata.artist == "https://example.test/artist",
		"URL inside metadata lost");
	Check(TranslateSearchQuery(R"(live artist:"A \"quoted\" \\ name")", preview, error),
		"Escaped metadata rejected");
	Check(preview.metadata.artist == "A \"quoted\" \\ name", "Escapes decoded incorrectly");
	Check(TranslateSearchQuery(
		      R"("genre:jazz" OR live NOT remix https://example.test/a)", preview, error),
		"Ordinary expression rejected");
	Check(preview.keywords == R"("genre:jazz" OR live NOT remix https://example.test/a)" &&
			(preview.metadata.minBitrateKbps == 0 && preview.metadata.minLength == 0 &&
				preview.metadata.artist.empty() && preview.metadata.album.empty() &&
				preview.metadata.title.empty() && preview.metadata.codec.empty()),
		"Keyword expression changed");
	for (const auto &duration : { "0", "0s", "5m", "24h", "24:00:00", "86400", ">86399", ">=86400" }) {
		Check(TranslateSearchQuery(wxString("jazz duration:") + duration, preview, error),
			"Valid duration rejected");
	}
	for (const auto &bitrate : { "0", "1000000", ">999999", ">=1000000" }) {
		Check(TranslateSearchQuery(wxString("jazz bitrate:") + bitrate, preview, error),
			"Valid bitrate rejected");
	}
	Check(TranslateSearchQuery("   Queen  live   ", preview, error) && preview.keywords == "Queen live" &&
			(preview.metadata.minBitrateKbps == 0 && preview.metadata.minLength == 0 &&
				preview.metadata.artist.empty() && preview.metadata.album.empty() &&
				preview.metadata.title.empty() && preview.metadata.codec.empty()),
		"Ordinary words inferred as filters");
	Check(TranslateSearchQuery("(jazz OR blues) artist:Queen", preview, error) &&
			preview.keywords == "(jazz OR blues)" && preview.metadata.artist == "Queen",
		"Global filter after grouped keywords rejected");
	Check(TranslateSearchQuery("jazz artist://Queen", preview, error) &&
			preview.metadata.artist == "//Queen",
		"Known label treated as URL");
	Check(TranslateSearchQuery(R"(("genre:jazz" OR blues) artist:Queen)", preview, error) &&
			preview.keywords == R"(("genre:jazz" OR blues))" &&
			preview.metadata.artist == "Queen",
		"Quoted keyword label inside parentheses changed");
	Check(TranslateSearchQuery(R"(jazz OR("genre:blues") artist:Queen)", preview, error) &&
			preview.keywords == R"(jazz OR("genre:blues"))" && preview.metadata.artist == "Queen",
		"Operator-adjacent quoted keyword label changed");
	// Every failure must leave an earlier editable preview intact.
	preview.keywords = "keep keywords";
	preview.metadata.artist = "keep artist";
	for (const auto &input : { "",
		     "  ",
		     "artist:Queen",
		     "jazz genre:fusion",
		     "jazz artist:A ARTIST:B",
		     "jazz OR artist:Queen",
		     "(jazz)OR artist:Queen",
		     "(jazz)AND artist:Queen",
		     "(jazz)NOT artist:Queen",
		     "(jazz artist:Queen",
		     "jazz) artist:Queen",
		     "jazz NOT artist:Queen live",
		     "artist:Queen OR jazz",
		     "jazz artist:Queen OR(blues)",
		     "jazz ( artist:Queen )",
		     "jazz duration://5m",
		     "jazz album:",
		     "jazz album:\"\"",
		     "jazz album:\"unclosed",
		     "jazz \"unclosed",
		     "jazz artist:\"A\"trailing",
		     "jazz artist:a\"b\"",
		     R"(jazz artist:"bad\q")",
		     "jazz bitrate:>=",
		     "jazz bitrate:-1",
		     "jazz bitrate:1.5",
		     "jazz bitrate:320kbps",
		     "jazz bitrate:<192",
		     "jazz bitrate:=192",
		     "jazz bitrate:1000001",
		     "jazz bitrate:>1000000",
		     "jazz bitrate:99999999999999999999999",
		     "jazz duration:-1",
		     "jazz duration:1.5m",
		     "jazz duration:1:02",
		     "jazz duration:1:60:00",
		     "jazz duration:24:00:01",
		     "jazz duration:86401",
		     "jazz duration:>24h",
		     "jazz duration:25h",
		     "jazz duration:1:00:00m",
		     "jazz duration:<=5m",
		     "jazz duration:9999999999999999999999h" }) {
		Check(!TranslateSearchQuery(input, preview, error) && !error.empty(),
			"Invalid syntax accepted or error missing");
		Check(preview.keywords == "keep keywords" && preview.metadata.artist == "keep artist",
			"Failure overwrote preview");
	}
	wxString nul = "jazz";
	nul += wxChar(0);
	nul += "artist:A";
	Check(!TranslateSearchQuery(nul, preview, error), "NUL accepted");
}
} // namespace

int main()
{
	wxInitializer initializer;
	if (!initializer.IsOk())
		return 1;
	try {
		TestTranslation();
		std::cout << "PASS: explicit fields, keywords, Unicode, escapes, units, bounds and rejected "
			     "input\n";
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
	return 0;
}
