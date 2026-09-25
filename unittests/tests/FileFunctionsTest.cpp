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

#include <common/FileFunctions.h>
#include <common/Path.h>

#include <wx/file.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/tarstrm.h>
#include <wx/wfstream.h>
#include <wx/zstream.h>

#include <set>
#include <string>

#ifndef __WINDOWS__
#include <unistd.h> // Needed for symlink
#endif

using namespace muleunit;

namespace
{

// Sorted and comma-joined, so a mismatch prints as a readable diff.
wxString Joined(const std::set<wxString> &names)
{
	wxString joined;
	for (const wxString &name : names) {
		joined += (joined.empty() ? "" : ",") + name;
	}
	return joined;
}

wxString Listed(const CPath &dir)
{
	std::set<wxString> names;
	for (const CPath &sub : ListSubdirectories(dir)) {
		names.insert(sub.GetRaw());
	}
	return Joined(names);
}

wxString IteratedAsDirs(const CPath &dir)
{
	std::set<wxString> names;
	CDirIterator it(dir);
	for (CPath sub = it.GetFirstFile(CDirIterator::Dir); sub.IsOk(); sub = it.GetNextFile()) {
		names.insert(sub.GetRaw());
	}
	return Joined(names);
}

struct CTempTree
{
	wxString root;
	~CTempTree() { wxFileName::Rmdir(root, wxPATH_RMDIR_RECURSIVE); }
};

struct CTempFile
{
	wxString path = wxFileName::CreateTempFileName("amule-unpack");
	~CTempFile() { wxRemoveFile(path); }
};

// Binary, like an .mmdb: GuessFiletype() sees it as EFT_Unknown once unpacked.
std::string Payload()
{
	std::string data(3000, '\0');
	for (size_t i = 0; i < data.size(); ++i) {
		data[i] = static_cast<char>(i * 7);
	}
	return data;
}

void AddMember(wxTarOutputStream &tar, const wxString &name, const std::string &data)
{
	tar.PutNextEntry(name, wxDateTime::Now(), static_cast<wxFileOffset>(data.size()));
	tar.Write(data.data(), data.size());
}

// The layout MaxMind ships.
void WriteMaxMindTar(wxOutputStream &out, bool withMmdb = true)
{
	wxTarOutputStream tar(out);
	tar.PutNextDirEntry("GeoLite2-Country_20260925");
	if (withMmdb) {
		AddMember(tar, "GeoLite2-Country_20260925/GeoLite2-Country.mmdb", Payload());
	}
	AddMember(tar, "GeoLite2-Country_20260925/COPYRIGHT.txt", "Copyright text\n");
	AddMember(tar, "GeoLite2-Country_20260925/LICENSE.txt", "License text\n");
	tar.Close();
}

std::string ReadAll(const wxString &path)
{
	wxFile in(path);
	std::string data(in.Length(), '\0');
	in.Read(&data[0], data.size());
	return data;
}

const char *mmdbFiles[] = { "*.mmdb", nullptr };

} // namespace

DECLARE_SIMPLE(FileFunctions)

TEST(FileFunctions, ListSubdirectoriesMatchesDirIterator)
{
	CTempTree tree;
	tree.root = wxFileName::CreateTempFileName("amule-subdirs");
	wxRemoveFile(tree.root);
	ASSERT_TRUE(wxMkdir(tree.root));
	const wxString in = tree.root + wxFileName::GetPathSeparator();

	ASSERT_TRUE(wxMkdir(in + "a"));
	ASSERT_TRUE(wxMkdir(in + "b"));
	ASSERT_TRUE(wxMkdir(in + ".hidden"));
	ASSERT_TRUE(wxFile().Create(in + "file"));
	ASSERT_TRUE(wxFile().Create(in + ".hiddenfile"));
	std::set<wxString> expected = { "a", "b", ".hidden" };
#ifndef __WINDOWS__
	// A symlink to a folder counts as one, as it does for wxDir; one to a file or to nothing
	// does not.
	ASSERT_EQUALS(0, symlink("a", (in + "link-to-dir").fn_str()));
	ASSERT_EQUALS(0, symlink("file", (in + "link-to-file").fn_str()));
	ASSERT_EQUALS(0, symlink("missing", (in + "broken-link").fn_str()));
	expected.insert("link-to-dir");
#endif

	const CPath root(tree.root);
	ASSERT_EQUALS(Joined(expected), Listed(root));
	ASSERT_EQUALS(IteratedAsDirs(root), Listed(root));
}

TEST(FileFunctions, ListSubdirectoriesOfMissingDirIsEmpty)
{
	ASSERT_EQUALS(wxString(), Listed(CPath("/nonexistent/amule-subdirs-test")));
}

TEST(FileFunctions, UnpackArchiveExtractsMmdbFromMaxMindTarGz)
{
	CTempFile file;
	{
		wxFileOutputStream out(file.path);
		wxZlibOutputStream gzip(out, -1, wxZLIB_GZIP);
		WriteMaxMindTar(gzip);
		gzip.Close();
	}

	const UnpackResult result = UnpackArchive(CPath(file.path), mmdbFiles);
	ASSERT_TRUE(result.first);
	ASSERT_EQUALS(static_cast<int>(EFT_Unknown), static_cast<int>(result.second));
	ASSERT_TRUE(ReadAll(file.path) == Payload());
}

TEST(FileFunctions, UnpackArchiveFailsOnTarWithoutMatchingMember)
{
	CTempFile file;
	{
		wxFileOutputStream out(file.path);
		WriteMaxMindTar(out, false);
	}

	const UnpackResult result = UnpackArchive(CPath(file.path), mmdbFiles);
	ASSERT_EQUALS(static_cast<int>(EFT_Error), static_cast<int>(result.second));
}

TEST(FileFunctions, UnpackArchiveFailsOnTruncatedTar)
{
	CTempFile file;
	{
		wxFileOutputStream out(file.path);
		WriteMaxMindTar(out);
	}
	// Cut into the .mmdb member: two 512-byte headers (folder, file) plus part of its data.
	const std::string whole = ReadAll(file.path);
	ASSERT_TRUE(whole.size() > 2048);
	{
		wxFile out(file.path, wxFile::write);
		ASSERT_TRUE(out.Write(whole.data(), 2048) == 2048);
	}

	const UnpackResult result = UnpackArchive(CPath(file.path), mmdbFiles);
	ASSERT_EQUALS(static_cast<int>(EFT_Error), static_cast<int>(result.second));
}
