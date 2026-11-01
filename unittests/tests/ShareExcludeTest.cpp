//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
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

#include <muleunit/test.h>

#include <wx/filename.h>

#include <algorithm>

#include "ShareExclude.h"
#include "TestScratchDir.h"

using namespace muleunit;
using ShareExclude::PathList;
using ShareExclude::RecursiveCoverage;

DECLARE_SIMPLE(ShareExclude)

namespace
{

CPath MakeDir(const CPath &parent, const char *name)
{
	const CPath dir = parent.JoinPaths(CPath(name));
	wxFileName::Mkdir(dir.GetRaw(), 0700, wxPATH_MKDIR_FULL);
	return dir;
}

bool Contains(const PathList &list, const CPath &path)
{
	return std::any_of(
		list.begin(), list.end(), [&](const CPath &p) { return p.GetRaw() == path.GetRaw(); });
}

} // namespace

TEST(ShareExclude, FilterMatchesGlobsCaseInsensitively)
{
	CShareExcludeFilter filter;
	filter.Compile(".unwanted|*Incomplete*", false);
	ASSERT_TRUE(filter.Matches(".unwanted"));
	ASSERT_TRUE(filter.Matches("My INCOMPLETE files"));
	ASSERT_TRUE(!filter.Matches("Music"));
}

TEST(ShareExclude, EmptyFilterMatchesNothing)
{
	CShareExcludeFilter filter;
	filter.Compile("", false);
	ASSERT_TRUE(!filter.IsActive());
	ASSERT_TRUE(!filter.Matches(".unwanted"));
}

TEST(ShareExclude, ExcludedFolderAnywhereBelowTheRoot)
{
	CShareExcludeFilter filter;
	filter.Compile(".unwanted", false);
	const CPath root("/share");
	const CPath deep = root.JoinPaths(CPath("a")).JoinPaths(CPath(".unwanted")).JoinPaths(CPath("b"));
	const CPath clean = root.JoinPaths(CPath("a")).JoinPaths(CPath("b"));
	ASSERT_TRUE(ShareExclude::HasExcludedFolderBelow(filter, root, deep));
	ASSERT_TRUE(!ShareExclude::HasExcludedFolderBelow(filter, root, clean));
}

TEST(ShareExclude, RootItselfIsNeverTested)
{
	// The user picked the root; a pattern must not undo that.
	CShareExcludeFilter filter;
	filter.Compile(".unwanted", false);
	const CPath root = CPath("/share").JoinPaths(CPath(".unwanted"));
	ASSERT_TRUE(!ShareExclude::HasExcludedFolderBelow(filter, root, root.JoinPaths(CPath("a"))));
	ASSERT_TRUE(!ShareExclude::HasExcludedFolderBelow(filter, root, root));
}

TEST(ShareExclude, CoverageClassifiesPaths)
{
	CShareExcludeFilter filter;
	filter.Compile(".unwanted", false);
	const CPath root("/share");
	PathList roots;
	roots.push_back(root);

	ASSERT_TRUE(ShareExclude::GetRecursiveCoverage(filter, roots, root) == RecursiveCoverage::Covered);
	ASSERT_TRUE(ShareExclude::GetRecursiveCoverage(filter, roots, root.JoinPaths(CPath("a"))) ==
		    RecursiveCoverage::Covered);
	ASSERT_TRUE(ShareExclude::GetRecursiveCoverage(
			    filter, roots, root.JoinPaths(CPath(".unwanted")).JoinPaths(CPath("a"))) ==
		    RecursiveCoverage::Excluded);
	// Separator boundary: /share2 is not under /share.
	ASSERT_TRUE(ShareExclude::GetRecursiveCoverage(filter, roots, CPath("/share2")) ==
		    RecursiveCoverage::None);
}

TEST(ShareExclude, NestedRootCoversWhatItsParentExcludes)
{
	CShareExcludeFilter filter;
	filter.Compile(".unwanted", false);
	const CPath outer("/share");
	const CPath inner = outer.JoinPaths(CPath(".unwanted")).JoinPaths(CPath("keep"));
	PathList roots;
	roots.push_back(outer);
	roots.push_back(inner);
	ASSERT_TRUE(ShareExclude::GetRecursiveCoverage(filter, roots, inner.JoinPaths(CPath("a"))) ==
		    RecursiveCoverage::Covered);
}

TEST(ShareExclude, ExpansionSkipsExcludedSubtree)
{
	const TestScratchDir scratch("amule-share-exclude-expand-");
	const CPath root(scratch.WxPath());
	const CPath music = MakeDir(root, "Music");
	const CPath unwanted = MakeDir(root, ".unwanted");
	const CPath below = MakeDir(unwanted, "inner");
	const CPath deepUnwanted = MakeDir(music, "Incomplete");

	CShareExcludeFilter filter;
	filter.Compile(".unwanted|Incomplete", false);

	PathList out;
	wxArrayString seen;
	bool truncated = false;
	PathList excluded;
	ShareExclude::ExpandRecursiveRoot(
		filter, root, out, seen, 100, truncated, [&](const CPath &dir) { excluded.push_back(dir); });

	ASSERT_TRUE(Contains(out, root));
	ASSERT_TRUE(Contains(out, music));
	ASSERT_TRUE(!Contains(out, unwanted));
	ASSERT_TRUE(!Contains(out, below));
	ASSERT_TRUE(!Contains(out, deepUnwanted));
	ASSERT_EQUALS(2u, static_cast<unsigned>(excluded.size()));

	// The preview counts excluded folders too, but nothing inside one is ever visited.
	ASSERT_TRUE(seen.Index(".unwanted") != wxNOT_FOUND);
	ASSERT_TRUE(seen.Index("Incomplete") != wxNOT_FOUND);
	ASSERT_TRUE(seen.Index("inner") == wxNOT_FOUND);
	ASSERT_TRUE(!truncated);
}

TEST(ShareExclude, ExpansionCapsSeenNames)
{
	const TestScratchDir scratch("amule-share-exclude-cap-");
	const CPath root(scratch.WxPath());
	MakeDir(root, "a");
	MakeDir(root, "b");

	CShareExcludeFilter filter;
	filter.Compile("", false);

	PathList out;
	wxArrayString seen;
	bool truncated = false;
	ShareExclude::ExpandRecursiveRoot(filter, root, out, seen, 1, truncated);

	ASSERT_EQUALS(3u, static_cast<unsigned>(out.size()));
	ASSERT_EQUALS(1u, static_cast<unsigned>(seen.GetCount()));
	ASSERT_TRUE(truncated);
}

// Office documents are excluded as plain globs inside the default ExcludeSharePatterns, so the
// user sees and edits them in the same field as every other pattern. The globs are pinned
// against the shipped default itself, so the two cannot drift apart.
TEST(ShareExclude, OfficeDocumentGlobsMatchExactExtensions)
{
	CShareExcludeFilter filter;
	filter.Compile(ShareExclude::kDefaultPatterns, false);
	ASSERT_TRUE(filter.IsValid());

	const char *blocked[] = { "Letter.DOC",
		"letter.docx",
		"macro.DoCm",
		"template.dot",
		"template.dotx",
		"template.dotm",
		"budget.xls",
		"budget.xlsx",
		"macro.xlsm",
		"book.xlsb",
		"template.xlt",
		"template.xltx",
		"template.xltm",
		"addin.xlam",
		"slides.ppt",
		"slides.PPTX",
		"macro.pptm",
		"template.pot",
		"template.potx",
		"template.potm",
		"show.pps",
		"show.ppsx",
		"show.ppsm",
		"report.odt",
		"template.ott",
		"sheet.ods",
		"template.ots",
		"slides.odp",
		"template.otp",
		"drawing.odg",
		"template.otg",
		"formula.odf",
		"database.odb",
		"notes.rtf",
		"contacts.csv",
		"contacts.TSV" };
	for (const char *name : blocked) {
		ASSERT_TRUE_M(filter.Matches(name), name);
	}
	const char *allowed[] = { "music.mp3",
		"movie.mkv",
		"document.docx.zip",
		"docx",
		"report.docx.bak",
		"photo.jpeg",
		"README",
		"report.docx.exe" };
	for (const char *name : allowed) {
		ASSERT_TRUE_M(!filter.Matches(name), name);
	}
}

TEST(ShareExclude, DefaultMetadataPatternsRemainExcluded)
{
	CShareExcludeFilter filter;
	filter.Compile(ShareExclude::kDefaultPatterns, false);
	const char *blocked[] = { ".DS_Store",
		"._photo.jpg",
		".Spotlight-V100",
		".Trashes",
		".fseventsd",
		".DocumentRevisions-V100",
		".TemporaryItems",
		".apdisk",
		"Thumbs.db",
		"ehthumbs.db",
		"desktop.ini",
		".directory" };
	for (const char *name : blocked) {
		ASSERT_TRUE_M(filter.Matches(name), name);
	}
	const char *allowed[] = { "download.part",
		"download.tmp",
		"download.!ut",
		"INCOMPLETE.bin",
		"report.docx.bak",
		"Thumbs.db.backup" };
	for (const char *name : allowed) {
		ASSERT_TRUE_M(!filter.Matches(name), name);
	}
}
