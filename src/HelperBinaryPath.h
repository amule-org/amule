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

#include <wx/arrstr.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/string.h>
#include <wx/tokenzr.h>
#include <wx/utils.h>

inline bool IsExecutableFile(const wxString &path)
{
	return wxFileName::IsFileExecutable(path);
}

inline wxString CurrentSearchPath()
{
	wxString path;
	wxGetEnv("PATH", &path);
	return path;
}

// The absolute path of the first `name` in `searchPath`, a PATH value, that `isExecutable` accepts,
// or empty. Relative entries are skipped: they resolve against the current directory.
inline wxString FindOnSearchPath(const wxString &name,
	const wxString &searchPath,
	const std::function<bool(const wxString &)> &isExecutable)
{
	for (const wxString &entry : wxStringTokenize(searchPath, wxPATH_SEP, wxTOKEN_STRTOK)) {
#ifdef __WINDOWS__
		wxString dir = entry;
		dir.Replace("\"", wxEmptyString); // an entry may be quoted
		dir.Trim(false).Trim(true);
#else
		const wxString &dir = entry;
#endif
		const wxFileName candidate(dir, name);
		if (candidate.IsAbsolute() && isExecutable(candidate.GetFullPath())) {
			return candidate.GetFullPath();
		}
	}
	return wxEmptyString;
}

// Where a bare `program` lives: next to `selfExePath`, else the first match in `searchPath`. The
// absolute path, or empty if there is none; a value with a directory is returned as is. Windows adds
// ".exe" to a name without an extension, as CreateProcess does. Give CreateProcess this, not the bare
// name: it tries the current directory before PATH, and anyone who can write there can plant a copy.
inline wxString FindProgram(const wxString &program,
	const wxString &selfExePath,
	const wxString &searchPath,
	const std::function<bool(const wxString &)> &isExecutable)
{
	if (program.IsEmpty() || !wxFileName(program).GetPath().IsEmpty()) {
		return program;
	}
	wxFileName name(program);
#ifdef __WINDOWS__
	if (!name.HasExt()) {
		name.SetExt("exe");
	}
#endif
	if (!selfExePath.IsEmpty()) {
		const wxString sibling =
			wxFileName(wxFileName(selfExePath).GetPath(), name.GetFullName()).GetFullPath();
		if (isExecutable(sibling)) {
			return sibling;
		}
	}
	return FindOnSearchPath(name.GetFullName(), searchPath, isExecutable);
}

// FindProgram for this process: our own folder, then PATH.
inline wxString FindProgram(const wxString &program)
{
	return FindProgram(
		program, wxStandardPaths::Get().GetExecutablePath(), CurrentSearchPath(), IsExecutableFile);
}

// The command that starts a helper binary (amuleweb, amuleapi) configured as `configured`. A value
// with a directory is used as is. A bare name, the default, is looked for next to `selfExePath`
// first, so an unpacked static build finds its helpers without PATH. Failing that, POSIX leaves the
// name to execvp's PATH search, and Windows looks in `searchPath` itself (see FindProgram). Empty
// means Windows found nothing.
inline wxString ResolveHelperBinary(const wxString &configured,
	const wxString &selfExePath,
	const wxString &searchPath,
	const std::function<bool(const wxString &)> &isExecutable)
{
#ifdef __WINDOWS__
	return FindProgram(configured, selfExePath, searchPath, isExecutable);
#else
	wxUnusedVar(searchPath);
	const wxString sibling = FindProgram(configured, selfExePath, wxEmptyString, isExecutable);
	return sibling.IsEmpty() ? configured : sibling;
#endif
}

inline wxString ResolveHelperBinary(const wxString &configured, const wxString &selfExePath)
{
	return ResolveHelperBinary(configured, selfExePath, CurrentSearchPath(), IsExecutableFile);
}

// Keep executable and argument boundaries intact, including profile paths with quotes.
inline wxArrayString AmuleApiArguments(const wxString &path,
	const wxString &configDir,
	const wxString &bind,
	unsigned httpPort,
	const wxString &ecIp,
	unsigned ecPort)
{
	// No brace initialisation: wxArrayString has no initializer_list constructor before wx 3.2.4.
	wxArrayString args;
	args.Add(path);
	args.Add("--config-dir=" + configDir);
	args.Add("--bind=" + bind);
	args.Add(wxString::Format("--http-port=%u", httpPort));
	if (!ecIp.IsEmpty() && ecPort != 0) {
		const wxString host = ecIp == "0.0.0.0" ? wxString("127.0.0.1") : ecIp;
		args.Add("--host=" + host);
		args.Add(wxString::Format("--port=%u", ecPort));
	}
	return args;
}

// The display command for amuleapi (execution uses AmuleApiArguments). `ecIp` and `ecPort`, the EC listener's
// bound endpoint, tell it where to connect: amuleapi.conf's defaults need not match the core's EC settings.
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
