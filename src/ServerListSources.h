// This file is part of the aMule Project.
// Copyright (c) 2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#ifndef SERVERLISTSOURCES_H
#define SERVERLISTSOURCES_H

#include <wx/arrstr.h>
#include <wx/uri.h>
#include <common/TextFile.h>

namespace ServerListSources
{
inline bool IsValid(const wxString &url)
{
	const wxURI uri(url);
	const wxString scheme = uri.GetScheme().Lower();
	return uri.HasServer() && !uri.GetServer().empty() && (scheme == "http" || scheme == "https");
}

// Read on each new update, so changes made by the editor or a daemon administrator
// take effect without restarting. Never create or rewrite the user's file.
inline wxArrayString Read(const wxString &path)
{
	CTextFile file;
	wxArrayString sources;
	if (file.Open(path, CTextFile::read)) {
		for (const auto &url : file.ReadLines()) {
			if (IsValid(url) && sources.Index(url) == wxNOT_FOUND) {
				sources.Add(url);
			}
		}
	}
	return sources;
}
} // namespace ServerListSources

#endif // SERVERLISTSOURCES_H
