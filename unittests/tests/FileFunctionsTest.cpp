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

#include <set>

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
