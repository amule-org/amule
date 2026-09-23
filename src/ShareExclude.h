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

#ifndef SHAREEXCLUDE_H
#define SHAREEXCLUDE_H

#include <functional>
#include <vector>

#include <wx/arrstr.h>

#include <common/Path.h>

class wxRegEx;

// Compiled shared-file-name exclusion filter. Holds either a set of wildcard globs (matched
// case-insensitively with wxMatchWild) or a single regex, chosen by the useRegex flag passed
// to Compile(). An empty pattern -- or a regex that fails to compile -- yields an inactive
// filter (Matches() always false): a bad pattern fails open, it never excludes everything.
// Owns a heap wxRegEx, so non-copyable.
class CShareExcludeFilter
{
public:
	CShareExcludeFilter();
	~CShareExcludeFilter();

	// In wildcard mode the string is split on '|' into globs. In regex mode the whole string is
	// one regex, so '|' is native alternation and is NOT split.
	void Compile(const wxString &patterns, bool useRegex);

	bool Matches(const wxString &fileName) const;

	bool IsActive() const { return m_active; }
	bool IsValid() const { return m_valid; }

private:
	CShareExcludeFilter(const CShareExcludeFilter &);
	CShareExcludeFilter &operator=(const CShareExcludeFilter &);

	wxArrayString m_globs; // lowercased globs (wildcard mode)
	wxRegEx *m_regex;      // compiled regex (regex mode), owned
	bool m_useRegex;
	bool m_active;
	bool m_valid;
};

namespace ShareExclude
{

typedef std::vector<CPath> PathList;

// True if a folder below `root`, up to and including `path`, matches `filter`. `root` itself is
// never tested: the user picked it.
bool HasExcludedFolderBelow(const CShareExcludeFilter &filter, const CPath &root, const CPath &path);

enum class RecursiveCoverage
{
	None,
	Covered,
	Excluded // under a recursive root, but only through an excluded folder
};

// How `path` relates to the recursive share `roots`.
RecursiveCoverage GetRecursiveCoverage(
	const CShareExcludeFilter &filter, const PathList &roots, const CPath &path);

// Appends `root` and every descendant directory to `out`, hidden ones included, matching the
// runtime watcher's AddTree(). A subfolder
// `filter` matches is skipped with its subtree and reported to `onExcluded`, if set. `seen`
// collects every subfolder name met, excluded or not, up to `seenCap`.
void ExpandRecursiveRoot(const CShareExcludeFilter &filter,
	const CPath &root,
	PathList &out,
	wxArrayString &seen,
	size_t seenCap,
	bool &seenTruncated,
	const std::function<void(const CPath &)> &onExcluded = nullptr);

} // namespace ShareExclude

#endif // SHAREEXCLUDE_H
