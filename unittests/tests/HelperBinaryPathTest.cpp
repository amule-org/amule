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

#include <initializer_list>
#include <set>
#include <utility>

#include "HelperBinaryPath.h"

using namespace muleunit;

DECLARE_SIMPLE(HelperBinaryPath)

namespace
{

const wxString kSelfDir = wxFileName::DirName("/opt/amule").GetPath();
const wxString kSelf = wxFileName(kSelfDir, "amuled").GetFullPath();

#ifdef __WINDOWS__
const wxString kRoot = "C:\\";
#else
const wxString kRoot = "/";
#endif

// `name` as CreateProcess would complete it.
wxString Exe(const wxString &name)
{
#ifdef __WINDOWS__
	return name + ".exe";
#else
	return name;
#endif
}

wxString SiblingOf(const wxString &name)
{
	return wxFileName(kSelfDir, Exe(name)).GetFullPath();
}

wxString InDir(const wxString &dir, const wxString &name)
{
	return wxFileName(dir, Exe(name)).GetFullPath();
}

const wxString kBin = kRoot + "bin";
const wxString kTools = kRoot + "tools";

wxString SearchPathOf(std::initializer_list<wxString> dirs)
{
	wxString path;
	for (const wxString &dir : dirs) {
		if (!path.IsEmpty()) {
			path += wxPATH_SEP;
		}
		path += dir;
	}
	return path;
}

// A file system holding exactly `present`.
std::function<bool(const wxString &)> Holding(std::set<wxString> present)
{
	return [present = std::move(present)](const wxString &path) { return present.count(path) > 0; };
}

} // namespace

TEST(HelperBinaryPath, BareNameFoundNextToSelf)
{
	ASSERT_EQUALS(SiblingOf("amuleapi"),
		ResolveHelperBinary("amuleapi",
			kSelf,
			kBin,
			Holding({ SiblingOf("amuleapi"), InDir(kBin, "amuleapi") })));
}

TEST(HelperBinaryPath, BareNameMissingNextToSelf)
{
#ifdef __WINDOWS__
	// Never the bare name: CreateProcess would try the current directory before PATH.
	ASSERT_EQUALS(InDir(kBin, "amuleapi"),
		ResolveHelperBinary("amuleapi", kSelf, kBin, Holding({ InDir(kBin, "amuleapi") })));
	ASSERT_EQUALS(wxString(""), ResolveHelperBinary("amuleapi", kSelf, kBin, Holding({})));
#else
	// execvp searches PATH, and only PATH.
	ASSERT_EQUALS(wxString("amuleapi"),
		ResolveHelperBinary("amuleapi", kSelf, kBin, Holding({ InDir(kBin, "amuleapi") })));
	ASSERT_EQUALS(wxString("amuleapi"), ResolveHelperBinary("amuleapi", kSelf, kBin, Holding({})));
#endif
}

TEST(HelperBinaryPath, ConfiguredDirectoryIsKept)
{
	// A path the user set wins, even with a copy next to our own binary.
	const wxString configured = wxFileName("/usr/local/bin", "amuleapi").GetFullPath();
	ASSERT_EQUALS(
		configured, ResolveHelperBinary(configured, kSelf, kBin, Holding({ SiblingOf("amuleapi") })));
	const wxString relative = wxFileName(".", "amuleapi").GetFullPath();
	ASSERT_EQUALS(
		relative, ResolveHelperBinary(relative, kSelf, kBin, Holding({ SiblingOf("amuleapi") })));
}

TEST(HelperBinaryPath, UnknownSelfOrEmptyNameChangesNothing)
{
#ifdef __WINDOWS__
	ASSERT_EQUALS(
		wxString(""), ResolveHelperBinary("amuleweb", "", "", Holding({ SiblingOf("amuleweb") })));
#else
	ASSERT_EQUALS(wxString("amuleweb"),
		ResolveHelperBinary("amuleweb", "", "", Holding({ SiblingOf("amuleweb") })));
#endif
	ASSERT_EQUALS(wxString(""), ResolveHelperBinary("", kSelf, kBin, Holding({ SiblingOf("") })));
}

TEST(HelperBinaryPath, FindProgramSearchesOwnFolderThenPathInOrder)
{
	const wxString path = SearchPathOf({ kBin, kTools });
	ASSERT_EQUALS(SiblingOf("player"),
		FindProgram("player",
			kSelf,
			path,
			Holding({ SiblingOf("player"), InDir(kBin, "player"), InDir(kTools, "player") })));
	ASSERT_EQUALS(InDir(kBin, "player"),
		FindProgram(
			"player", kSelf, path, Holding({ InDir(kBin, "player"), InDir(kTools, "player") })));
	ASSERT_EQUALS(InDir(kTools, "player"),
		FindProgram("player", kSelf, path, Holding({ InDir(kTools, "player") })));
	ASSERT_EQUALS(InDir(kTools, "player"),
		FindProgram("player", "", path, Holding({ InDir(kTools, "player") })));
	ASSERT_EQUALS(wxString(""), FindProgram("player", kSelf, path, Holding({})));
}

TEST(HelperBinaryPath, FindProgramSkipsRelativePathEntries)
{
	// Each would resolve against the current directory.
	const wxString path = SearchPathOf({ ".", "bin", kBin });
	ASSERT_EQUALS(InDir(kBin, "player"),
		FindProgram("player",
			kSelf,
			path,
			Holding({ InDir(".", "player"), InDir("bin", "player"), InDir(kBin, "player") })));
	ASSERT_EQUALS(wxString(""),
		FindProgram("player",
			kSelf,
			SearchPathOf({ ".", "bin" }),
			Holding({ InDir(".", "player"), InDir("bin", "player") })));
}

TEST(HelperBinaryPath, FindProgramKeepsADirectory)
{
	const wxString absolute = InDir(kTools, "player");
	ASSERT_EQUALS(absolute, FindProgram(absolute, kSelf, kBin, Holding({ InDir(kBin, "player") })));
	const wxString relative = InDir("bin", "player");
	ASSERT_EQUALS(relative, FindProgram(relative, kSelf, kBin, Holding({ InDir(kBin, "player") })));
	ASSERT_EQUALS(wxString(""), FindProgram("", kSelf, kBin, Holding({ kBin })));
}

#ifdef __WINDOWS__
TEST(HelperBinaryPath, FindProgramCompletesAndUnquotesLikeCreateProcess)
{
	// An extension is kept; ".exe" is added only without one.
	const wxString script = wxFileName(kBin, "notify.cmd").GetFullPath();
	ASSERT_EQUALS(script, FindProgram("notify.cmd", kSelf, kBin, Holding({ script })));
	ASSERT_EQUALS(wxString(""), FindProgram("notify", kSelf, kBin, Holding({ script })));

	const wxString spaced = "C:\\Program Files\\Player";
	ASSERT_EQUALS(InDir(spaced, "player"),
		FindProgram("player",
			kSelf,
			"\" " + spaced + " \";" + kBin,
			Holding({ InDir(spaced, "player") })));
}
#endif

namespace
{
#ifdef __WINDOWS__
const wxString q = "\"";
#else
const wxString q = "'";
#endif
const wxString kBase = q + "amuleapi" + q + " " + q + "--config-dir=/c/" + q + " " + q + "--bind=0.0.0.0" +
		       q + " --http-port=4711";
} // namespace

TEST(HelperBinaryPath, AmuleApiCommandWithoutEcEndpointKeepsItsConfig)
{
	ASSERT_EQUALS(kBase, AmuleApiCommand("amuleapi", "/c/", "0.0.0.0", 4711, "", 0));
	// A port without an address is no endpoint either.
	ASSERT_EQUALS(kBase, AmuleApiCommand("amuleapi", "/c/", "0.0.0.0", 4711, "", 4952));
	ASSERT_EQUALS(kBase, AmuleApiCommand("amuleapi", "/c/", "0.0.0.0", 4711, "10.0.0.2", 0));
}

TEST(HelperBinaryPath, AmuleApiCommandPassesTheBoundEcEndpoint)
{
	ASSERT_EQUALS(kBase + " " + q + "--host=10.0.0.2" + q + " --port=4952",
		AmuleApiCommand("amuleapi", "/c/", "0.0.0.0", 4711, "10.0.0.2", 4952));
}

TEST(HelperBinaryPath, AmuleApiCommandReachesAWildcardBindOverLoopback)
{
	ASSERT_EQUALS(kBase + " " + q + "--host=127.0.0.1" + q + " --port=4712",
		AmuleApiCommand("amuleapi", "/c/", "0.0.0.0", 4711, "0.0.0.0", 4712));
}
