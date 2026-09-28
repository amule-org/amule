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

#ifndef HELPERBINARYPATH_H
#define HELPERBINARYPATH_H

#include <functional>

#include <wx/filename.h>
#include <wx/string.h>

// The command that starts a helper binary (amuleweb, amuleapi) configured as `configured`. A value
// with a directory is used as is. A bare name, the default, is looked for next to `selfExePath`
// first, so an unpacked static build finds its helpers without PATH; failing that, PATH decides.
inline wxString ResolveHelperBinary(const wxString &configured,
	const wxString &selfExePath,
	const std::function<bool(const wxString &)> &isExecutable)
{
	if (configured.IsEmpty() || selfExePath.IsEmpty() || !wxFileName(configured).GetPath().IsEmpty()) {
		return configured;
	}
	wxFileName sibling(wxFileName(selfExePath).GetPath(), configured);
#ifdef __WINDOWS__
	if (!sibling.HasExt()) {
		sibling.SetExt("exe");
	}
#endif
	const wxString candidate = sibling.GetFullPath();
	return isExecutable(candidate) ? candidate : configured;
}

inline wxString ResolveHelperBinary(const wxString &configured, const wxString &selfExePath)
{
	return ResolveHelperBinary(configured, selfExePath, [](const wxString &path) {
		return wxFileName::IsFileExecutable(path);
	});
}

// The command that starts amuleapi for this core. `ecIp` and `ecPort`, the EC listener's bound
// endpoint, tell it where to connect: amuleapi.conf's defaults need not match the core's EC settings.
// A wildcard bind is reached over loopback. Without them, amuleapi.conf decides.
inline wxString AmuleApiCommand(const wxString &path,
	const wxString &configDir,
	const wxString &bind,
	unsigned httpPort,
	const wxString &ecIp,
	unsigned ecPort)
{
#ifdef __WINDOWS__
	const wxString q = "\"";
#else
	const wxString q = "'";
#endif
	wxString cmd = q + path + q + " " + q + "--config-dir=" + configDir + q + " " + q + "--bind=" + bind +
		       q + wxString::Format(" --http-port=%u", httpPort);
	if (!ecIp.IsEmpty() && ecPort != 0) {
		const wxString host = (ecIp == "0.0.0.0") ? wxString("127.0.0.1") : ecIp;
		cmd += " " + q + "--host=" + host + q + wxString::Format(" --port=%u", ecPort);
	}
	return cmd;
}

#endif // HELPERBINARYPATH_H
