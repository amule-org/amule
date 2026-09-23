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

#include "ShareExclude.h"

#include <wx/filename.h>
#include <wx/regex.h>
#include <wx/tokenzr.h>

#include <common/FileFunctions.h> // ListSubdirectories

CShareExcludeFilter::CShareExcludeFilter()
: m_regex(nullptr)
, m_useRegex(false)
, m_active(false)
, m_valid(true)
{
}

CShareExcludeFilter::~CShareExcludeFilter()
{
	delete m_regex;
}

void CShareExcludeFilter::Compile(const wxString &patterns, bool useRegex)
{
	m_globs.Clear();
	delete m_regex;
	m_regex = nullptr;
	m_useRegex = useRegex;
	m_active = false;
	m_valid = true;

	wxString trimmed = patterns;
	trimmed.Trim(true).Trim(false);
	if (trimmed.IsEmpty()) {
		return;
	}

	if (useRegex) {
		// The whole string is one regex: '|' is native alternation, so
		// it is NOT split. Case-insensitive to match the wildcard mode.
		m_regex = new wxRegEx(trimmed, wxRE_ADVANCED | wxRE_ICASE | wxRE_NOSUB);
		if (m_regex->IsValid()) {
			m_active = true;
		} else {
			// Fail open: a bad regex disables the filter rather than
			// excluding everything.
			delete m_regex;
			m_regex = nullptr;
			m_valid = false;
		}
	} else {
		// Wildcard mode: '|' separates globs, matched case-insensitively (globs are
		// lowercased here, the filename is lowercased in Matches()).
		wxStringTokenizer tokenizer(trimmed, wxT("|"));
		while (tokenizer.HasMoreTokens()) {
			wxString glob = tokenizer.GetNextToken();
			glob.Trim(true).Trim(false);
			if (!glob.IsEmpty()) {
				m_globs.Add(glob.Lower());
			}
		}
		m_active = !m_globs.IsEmpty();
	}
}

bool CShareExcludeFilter::Matches(const wxString &fileName) const
{
	if (!m_active) {
		return false;
	}
	if (m_useRegex) {
		return m_regex && m_regex->Matches(fileName);
	}
	const wxString lower = fileName.Lower();
	for (size_t i = 0; i < m_globs.GetCount(); ++i) {
		if (wxMatchWild(m_globs[i], lower, false)) {
			return true;
		}
	}
	return false;
}

namespace ShareExclude
{

bool HasExcludedFolderBelow(const CShareExcludeFilter &filter, const CPath &root, const CPath &path)
{
	const size_t rootLength = root.GetRaw().length();
	for (CPath dir = path; dir.IsOk() && dir.GetRaw().length() > rootLength;) {
		const wxString name = dir.GetFullName().GetPrintable();
		if (!name.IsEmpty() && filter.Matches(name)) {
			return true;
		}
		const CPath parent = dir.GetPath();
		// Guards against a parent that does not shorten the path.
		if (parent.GetRaw().length() >= dir.GetRaw().length()) {
			break;
		}
		dir = parent;
	}
	return false;
}

RecursiveCoverage GetRecursiveCoverage(
	const CShareExcludeFilter &filter, const PathList &roots, const CPath &path)
{
	if (!path.IsOk()) {
		return RecursiveCoverage::None;
	}
	RecursiveCoverage result = RecursiveCoverage::None;
	const wxString target = path.GetRaw();
	for (const CPath &rootPath : roots) {
		const wxString root = rootPath.GetRaw();
		if (root.IsEmpty()) {
			continue;
		}
		// Exact match: the path *is* a recursive root.
		if (target == root) {
			return RecursiveCoverage::Covered;
		}
		// Prefix match with a separator boundary, so /home is not reported as an ancestor of
		// /home2. A trailing separator on root is tolerated.
		const wxChar sep = wxFileName::GetPathSeparator();
		const wxChar lastChar = root.Last();
		if (target.length() > root.length() && target.StartsWith(root) &&
			(lastChar == sep || target[root.length()] == sep)) {
			// Nested roots: another root may cover what this one excludes.
			if (!HasExcludedFolderBelow(filter, rootPath, path)) {
				return RecursiveCoverage::Covered;
			}
			result = RecursiveCoverage::Excluded;
		}
	}
	return result;
}

void ExpandRecursiveRoot(const CShareExcludeFilter &filter,
	const CPath &root,
	PathList &out,
	wxArrayString &seen,
	size_t seenCap,
	bool &seenTruncated,
	const std::function<void(const CPath &)> &onExcluded)
{
	if (!root.IsOk() || !root.DirExists()) {
		return;
	}
	out.push_back(root);
	for (const CPath &sub : ListSubdirectories(root)) {
		const wxString name = sub.GetPrintable();
		if (seen.GetCount() < seenCap) {
			seen.Add(name);
		} else {
			seenTruncated = true;
		}
		if (filter.Matches(name)) {
			if (onExcluded) {
				onExcluded(root.JoinPaths(sub));
			}
			continue;
		}
		ExpandRecursiveRoot(
			filter, root.JoinPaths(sub), out, seen, seenCap, seenTruncated, onExcluded);
	}
}

} // namespace ShareExclude
