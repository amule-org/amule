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

#include <wx/file.h>
#include <wx/filename.h>
#include <wx/log.h>

#include <muleunit/test.h>

#include <MuleCollection.h>

#include <string>

using namespace muleunit;

DECLARE_SIMPLE(MuleCollection)

// Collection files arrive from a file manager, so every test here feeds
// CMuleCollection something it did not write. Most cases are malformed and
// awkward to keep on disk, which is why they go through OpenBuffer.

namespace
{
void PutU8(std::string &s, unsigned v)
{
	s += static_cast<char>(v & 0xFF);
}

void PutU16(std::string &s, unsigned v)
{
	PutU8(s, v);
	PutU8(s, v >> 8);
}

void PutU32(std::string &s, uint32_t v)
{
	PutU16(s, static_cast<unsigned>(v));
	PutU16(s, static_cast<unsigned>(v >> 16));
}

/**
 * Builds a binary collection holding exactly one entry, in the tag order
 * eMule writes: FT_FILEHASH, FT_FILESIZE, FT_FILENAME.
 */
std::string MakeBinaryCollection(const std::string &fileName, uint32_t fileSize, bool withHash = true)
{
	std::string s;
	PutU32(s, 0x02); // collection version
	PutU32(s, 0);    // header tag count
	PutU32(s, 1);    // file count
	PutU32(s, withHash ? 3 : 2);

	if (withHash) {
		PutU8(s, 0x81); // tag type, unused for FT_FILEHASH
		PutU8(s, 0x28); // FT_FILEHASH
		for (int i = 0; i < 16; ++i) {
			PutU8(s, 0xAB);
		}
	}

	PutU8(s, 0x83); // TAGTYPE_UINT32
	PutU8(s, 0x02); // FT_FILESIZE
	PutU32(s, fileSize);

	PutU8(s, 0x82); // 0x82 ^ 0x80 == 0x02, a uint16-length-prefixed string
	PutU8(s, 0x01); // FT_FILENAME
	PutU16(s, static_cast<unsigned>(fileName.size()));
	s += fileName;

	return s;
}

bool OpenString(CMuleCollection &collection, const std::string &data)
{
	return collection.OpenBuffer(data.data(), data.size());
}

const char *const VALID_LINK = "ed2k://|file|some.file.name.iso|1234567|0123456789abcdef0123456789abcdef|/";
} // namespace

TEST(MuleCollection, TextCollection)
{
	CMuleCollection collection;
	std::string data = std::string(VALID_LINK) + "\n" + VALID_LINK + "\n";

	ASSERT_TRUE(OpenString(collection, data));
	ASSERT_EQUALS((size_t)2, collection.size());
	ASSERT_EQUALS(std::string(VALID_LINK), collection[0]);
}

TEST(MuleCollection, TextCollectionStripsCarriageReturns)
{
	CMuleCollection collection;
	std::string data = std::string(VALID_LINK) + "\r\n";

	ASSERT_TRUE(OpenString(collection, data));
	ASSERT_EQUALS((size_t)1, collection.size());
	ASSERT_EQUALS(std::string(VALID_LINK), collection[0]);
}

// Regression: an empty line used to underflow the length arithmetic and index
// far past the end of the string, throwing std::out_of_range out of Open().
TEST(MuleCollection, TextCollectionSurvivesBlankLines)
{
	CMuleCollection collection;
	std::string data = std::string("\n\n") + VALID_LINK + "\n\n\r\n";

	ASSERT_TRUE(OpenString(collection, data));
	ASSERT_EQUALS((size_t)1, collection.size());
	ASSERT_EQUALS(std::string(VALID_LINK), collection[0]);
}

// Text collections are hand-made link lists, and a Windows editor writes a
// UTF-8 BOM by default; it used to leave the first link failing its prefix
// check, so a single-entry collection looked empty.
TEST(MuleCollection, TextCollectionSkipsUtf8Bom)
{
	CMuleCollection collection;
	std::string data = "\xEF\xBB\xBF" + std::string(VALID_LINK) + "\n";

	ASSERT_TRUE(OpenString(collection, data));
	ASSERT_EQUALS((size_t)1, collection.size());
	ASSERT_EQUALS(std::string(VALID_LINK), collection[0]);
}

TEST(MuleCollection, BomOnlyInput)
{
	CMuleCollection collection;

	ASSERT_FALSE(OpenString(collection, "\xEF\xBB\xBF"));
	ASSERT_EQUALS((size_t)0, collection.size());
}

TEST(MuleCollection, TextCollectionIgnoresNonLinks)
{
	CMuleCollection collection;

	ASSERT_FALSE(OpenString(collection, "just some text\nand more text\n"));
	ASSERT_EQUALS((size_t)0, collection.size());
}

TEST(MuleCollection, BinaryCollection)
{
	CMuleCollection collection;

	ASSERT_TRUE(OpenString(collection, MakeBinaryCollection("movie.avi", 700000)));
	ASSERT_EQUALS((size_t)1, collection.size());
	ASSERT_EQUALS(std::string("ed2k://|file|movie.avi|700000|abababababababababababababababab|/"),
		collection[0]);
}

// An entry with no FT_FILEHASH used to yield a link naming the all-zero file
// id: syntactically valid, but a download that can never find a source.
TEST(MuleCollection, BinaryCollectionSkipsEntriesWithoutHash)
{
	CMuleCollection collection;

	ASSERT_FALSE(OpenString(collection, MakeBinaryCollection("movie.avi", 700000, false)));
	ASSERT_EQUALS((size_t)0, collection.size());
}

// A newline in a filename would forge extra lines once the link is written to
// the ED2KLinks file - including the RAISE_DIALOG token and extra downloads.
TEST(MuleCollection, BinaryCollectionRejectsNewlineInFilename)
{
	CMuleCollection collection;

	ASSERT_FALSE(OpenString(collection,
		MakeBinaryCollection(
			"evil.avi\ned2k://|file|injected|1|0123456789abcdef0123456789abcdef|/", 700000)));
	ASSERT_EQUALS((size_t)0, collection.size());
}

// A pipe would forge extra fields inside the eD2k URI itself.
TEST(MuleCollection, BinaryCollectionRejectsPipeInFilename)
{
	CMuleCollection collection;

	ASSERT_FALSE(OpenString(collection, MakeBinaryCollection("evil|name.avi", 700000)));
	ASSERT_EQUALS((size_t)0, collection.size());
}

TEST(MuleCollection, BinaryCollectionKeepsNonAsciiFilename)
{
	CMuleCollection collection;
	// UTF-8 for "filename" in Japanese - every byte is >= 0x80, so a signed
	// char comparison in the safety check would throw the entry away.
	const std::string name = "\xE3\x83\x95\xE3\x82\xA1\xE3\x82\xA4\xE3\x83\xAB.avi";

	ASSERT_TRUE(OpenString(collection, MakeBinaryCollection(name, 42)));
	ASSERT_EQUALS((size_t)1, collection.size());
	ASSERT_EQUALS(std::string("ed2k://|file|") + name + "|42|abababababababababababababababab|/",
		collection[0]);
}

// A twelve-byte crafted file used to ask for a 4 GiB allocation, and the
// resulting bad_alloc escaped Open() uncaught.
TEST(MuleCollection, BinaryCollectionRejectsOversizedBlob)
{
	CMuleCollection collection;
	std::string s;
	PutU32(s, 0x02); // version
	PutU32(s, 1);    // one header tag
	PutU8(s, 0x02);  // tag type
	PutU16(s, 0x0001);
	PutU8(s, 0x32); // FT_COLLECTIONAUTHORKEY
	PutU32(s, 0xFFFFFFFFu);

	ASSERT_FALSE(OpenString(collection, s));
	ASSERT_EQUALS((size_t)0, collection.size());
}

// A binary file that goes bad partway used to leave its already-parsed entries
// behind, which the text parser then reported as a successful open.
TEST(MuleCollection, TruncatedBinaryCollectionYieldsNothing)
{
	CMuleCollection collection;
	std::string data = MakeBinaryCollection("movie.avi", 700000);
	// Claim two files but supply only one.
	data[8] = 0x02;

	ASSERT_FALSE(OpenString(collection, data));
	ASSERT_EQUALS((size_t)0, collection.size());
}

TEST(MuleCollection, EmptyInput)
{
	CMuleCollection collection;

	ASSERT_FALSE(collection.OpenBuffer(nullptr, 0));
	ASSERT_FALSE(OpenString(collection, ""));
	ASSERT_EQUALS((size_t)0, collection.size());
}

TEST(MuleCollection, MissingFile)
{
	CMuleCollection collection;
	wxLogNull noLog; // wxFile logs the expected open failure

	ASSERT_FALSE(collection.Open(wxString("/no/such/directory/no-such-file.emulecollection")));
	ASSERT_EQUALS((size_t)0, collection.size());
}

// The wxString overload exists so that paths std::ifstream cannot represent -
// non-ASCII ones on Windows, or under a non-UTF-8 locale on POSIX - still open.
TEST(MuleCollection, NonAsciiPath)
{
	const wxString path = wxFileName::GetTempDir() + wxFileName::GetPathSeparator() +
			      wxString::FromUTF8("aMule-\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88") +
			      ".emulecollection";

	{
		wxFile file;
		ASSERT_TRUE(file.Create(path, true));
		const std::string data = std::string(VALID_LINK) + "\n";
		ASSERT_TRUE(file.Write(data.data(), data.size()) == data.size());
	}

	CMuleCollection collection;
	const bool opened = collection.Open(path);
	wxRemoveFile(path);

	ASSERT_TRUE(opened);
	ASSERT_EQUALS((size_t)1, collection.size());
	ASSERT_EQUALS(std::string(VALID_LINK), collection[0]);
}

TEST(MuleCollection, OversizedFileRejected)
{
	const wxString path =
		wxFileName::GetTempDir() + wxFileName::GetPathSeparator() + "aMule-oversized.emulecollection";

	{
		wxFile file;
		ASSERT_TRUE(file.Create(path, true));
		// One byte past the 4 MiB ceiling.
		const std::string chunk(64 * 1024, 'x');
		for (int i = 0; i < 64; ++i) {
			file.Write(chunk.data(), chunk.size());
		}
		file.Write("x", 1);
	}

	CMuleCollection collection;
	const bool opened = collection.Open(path);
	wxRemoveFile(path);

	ASSERT_FALSE(opened);
	ASSERT_EQUALS((size_t)0, collection.size());
}
