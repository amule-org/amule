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

#ifndef HTTPURL_H
#define HTTPURL_H

#include <wx/string.h>

// Whether `url` names an http or https resource. aMule only ever downloads over HTTP, and libcurl
// would otherwise also open other schemes (file, ftp, gopher, ...) handed to it in a URL.
inline bool IsHttpUrl(const wxString &url)
{
	const wxString lower = url.Strip(wxString::both).Lower();
	return lower.StartsWith(wxT("http://")) || lower.StartsWith(wxT("https://"));
}

#endif // HTTPURL_H
