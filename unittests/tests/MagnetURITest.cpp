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

#include <MagnetURI.h>

using namespace muleunit;

DECLARE_SIMPLE(MagnetURI)

namespace
{
// A syntactically plausible (32-char) AICH-shaped base32 string -- the
// converter treats it as an opaque token, so it doesn't need to be a real
// hash for these tests.
const char *const AICH = "QVVWMK4S7ZJUV3ASZLPFYU4A5WOAAAAA";
const char *const ED2K_HASH = "d41d8cd98f00b204e9800998ecf8427e";
} // namespace

TEST(MagnetURI, GetLinkIncludesAichField)
{
	CMagnetURI uri;
	uri.AddField("dn", "example.iso");
	uri.AddField("xt", wxString("urn:ed2k:") + ED2K_HASH);
	uri.AddField("xt", wxString("urn:aich:") + AICH);

	wxString link = uri.GetLink();

	ASSERT_TRUE(link.Contains(wxString("xt=urn:aich:") + AICH));
}

TEST(MagnetURI, ConvertsAichUrnIntoEd2kHField)
{
	wxString magnet =
		wxString("magnet:?xt=urn:ed2k:") + ED2K_HASH + "&dn=example.iso&xl=42&xt=urn:aich:" + AICH;

	CMagnetED2KConverter conv(magnet);

	ASSERT_TRUE(conv.CanConvertToED2K());
	wxString ed2k = conv.GetED2KLink();
	ASSERT_TRUE(ed2k.Contains(wxString("|h=") + AICH + "|"));
	// The AICH segment must land before the closing "/", not appended after it.
	ASSERT_TRUE(ed2k.EndsWith("/"));
	ASSERT_TRUE(ed2k.Contains(wxString("h=") + AICH + "|/"));
}

TEST(MagnetURI, NoAichUrnMeansNoHField)
{
	wxString magnet = wxString("magnet:?xt=urn:ed2k:") + ED2K_HASH + "&dn=example.iso&xl=42";

	CMagnetED2KConverter conv(magnet);

	ASSERT_TRUE(conv.CanConvertToED2K());
	wxString ed2k = conv.GetED2KLink();
	ASSERT_TRUE(!ed2k.Contains("h="));
	ASSERT_TRUE(ed2k.EndsWith("|/"));
}

TEST(MagnetURI, MalformedAichUrnIsDropped)
{
	// ED2KLink.cpp's parser throws on a bad master-hash, so a magnet with a
	// junk/truncated urn:aich: must convert as if the AICH urn were absent
	// -- not embed the garbage into "h=" and make the whole link unusable,
	// even though its ed2k hash is perfectly valid.
	wxString tooShort =
		wxString("magnet:?xt=urn:ed2k:") + ED2K_HASH + "&dn=example.iso&xl=42&xt=urn:aich:TOOSHORT";
	CMagnetED2KConverter convShort(tooShort);
	ASSERT_TRUE(convShort.CanConvertToED2K());
	wxString ed2kShort = convShort.GetED2KLink();
	ASSERT_TRUE(!ed2kShort.Contains("h="));
	ASSERT_TRUE(ed2kShort.EndsWith("|/"));

	// Right length (32), but not base32 (lowercase letters plus a '!').
	wxString notBase32 = wxString("magnet:?xt=urn:ed2k:") + ED2K_HASH +
			     "&dn=example.iso&xl=42&xt=urn:aich:qvvwmk4s7zju!3aszlpfyu4a5woaaaaa";
	CMagnetED2KConverter convBad(notBase32);
	ASSERT_TRUE(convBad.CanConvertToED2K());
	wxString ed2kBad = convBad.GetED2KLink();
	ASSERT_TRUE(!ed2kBad.Contains("h="));
	ASSERT_TRUE(ed2kBad.EndsWith("|/"));
}

TEST(MagnetURI, AichUrnOrderBeforeEd2kUrnStillWorks)
{
	// Field order in the magnet isn't guaranteed -- the converter shouldn't
	// care whether the AICH urn or the ed2k urn comes first.
	wxString magnet = wxString("magnet:?xt=urn:aich:") + AICH + "&xt=urn:ed2k:" + ED2K_HASH +
			  "&dn=example.iso&xl=42";

	CMagnetED2KConverter conv(magnet);

	ASSERT_TRUE(conv.CanConvertToED2K());
	wxString ed2k = conv.GetED2KLink();
	ASSERT_TRUE(ed2k.Contains(wxString("h=") + AICH + "|/"));
	ASSERT_TRUE(ed2k.Contains(wxString("|") + ED2K_HASH + "|h="));
}
