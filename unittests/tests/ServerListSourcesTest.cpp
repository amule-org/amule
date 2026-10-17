// This file is part of the aMule Project.
// Copyright (c) 2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#include <muleunit/test.h>
#include <ServerListSources.h>
#include <wx/filename.h>
#include <wx/filefn.h>

using namespace muleunit;
DECLARE_SIMPLE(ServerListSources)

namespace
{
struct SourceFile
{
	wxString path = wxFileName::CreateTempFileName("amule-sources-");
	~SourceFile() { wxRemoveFile(path); }
	void Write(const wxString &text) const
	{
		wxFFile file(path, "wb");
		ASSERT_TRUE(file.Write(text));
	}
};
} // namespace

TEST(ServerListSources, FiltersCommentsInvalidSchemesAndDuplicatesInOrder)
{
	SourceFile file;
	file.Write(" # preserved comment\r\n  https://one.example/server.met \r\n\r\n"
		   "ftp://unsupported.example/server.met\nnot a URL\nhttp:///missing-host\n"
		   "http://two.example/list\nhttps://one.example/server.met\n");
	const auto sources = ServerListSources::Read(file.path);
	ASSERT_EQUALS(size_t(2), sources.size());
	ASSERT_EQUALS(wxString("https://one.example/server.met"), sources[0]);
	ASSERT_EQUALS(wxString("http://two.example/list"), sources[1]);
	CTextFile original;
	ASSERT_TRUE(original.Open(file.path, CTextFile::read));
	ASSERT_EQUALS(wxString(" # preserved comment"), original.GetNextLine());
}

TEST(ServerListSources, ReloadsEditsAndDoesNotLimitSourcesTo255)
{
	SourceFile file;
	wxString text;
	for (unsigned i = 0; i < 300; ++i) {
		text += wxString::Format("https://source.example/%u\n", i);
	}
	file.Write(text);
	ASSERT_EQUALS(size_t(300), ServerListSources::Read(file.path).size());
	file.Write("https://changed.example/list\n");
	const auto changed = ServerListSources::Read(file.path);
	ASSERT_EQUALS(size_t(1), changed.size());
	ASSERT_EQUALS(wxString("https://changed.example/list"), changed[0]);
	file.Write("");
	ASSERT_TRUE(ServerListSources::Read(file.path).empty());
}

TEST(ServerListSources, MissingFileDoesNotCreateDefaults)
{
	SourceFile file;
	ASSERT_TRUE(wxRemoveFile(file.path));
	ASSERT_TRUE(ServerListSources::Read(file.path).empty());
	ASSERT_FALSE(wxFileExists(file.path));
	file.Write(""); // Restore for RAII cleanup.
}
