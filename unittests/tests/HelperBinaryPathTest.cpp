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

#include <set>
#include <utility>

#include "HelperBinaryPath.h"

using namespace muleunit;

DECLARE_SIMPLE(HelperBinaryPath)

namespace
{

const wxString kSelfDir = wxFileName::DirName("/opt/amule").GetPath();
const wxString kSelf = wxFileName(kSelfDir, "amuled").GetFullPath();

wxString SiblingOf(const wxString &name)
{
	wxFileName sibling(kSelfDir, name);
#ifdef __WINDOWS__
	sibling.SetExt("exe");
#endif
	return sibling.GetFullPath();
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
		ResolveHelperBinary("amuleapi", kSelf, Holding({ SiblingOf("amuleapi") })));
}

TEST(HelperBinaryPath, BareNameMissingNextToSelfIsLeftToPath)
{
	ASSERT_EQUALS(wxString("amuleapi"), ResolveHelperBinary("amuleapi", kSelf, Holding({})));
}

TEST(HelperBinaryPath, ConfiguredDirectoryIsKept)
{
	// A path the user set wins, even with a copy next to our own binary.
	const wxString configured = wxFileName("/usr/local/bin", "amuleapi").GetFullPath();
	ASSERT_EQUALS(configured, ResolveHelperBinary(configured, kSelf, Holding({ SiblingOf("amuleapi") })));
	const wxString relative = wxFileName(".", "amuleapi").GetFullPath();
	ASSERT_EQUALS(relative, ResolveHelperBinary(relative, kSelf, Holding({ SiblingOf("amuleapi") })));
}

TEST(HelperBinaryPath, UnknownSelfOrEmptyNameChangesNothing)
{
	ASSERT_EQUALS(wxString("amuleweb"),
		ResolveHelperBinary("amuleweb", "", Holding({ SiblingOf("amuleweb") })));
	ASSERT_EQUALS(wxString(""), ResolveHelperBinary("", kSelf, Holding({ SiblingOf("") })));
}

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
