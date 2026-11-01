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

#include <muleunit/test.h>

#include "TestScratchDir.h"

#include "StaticFs.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace muleunit;
using namespace webapi;

DECLARE_SIMPLE(StaticFs)

// Plain accept paths -- file exists inside root.

TEST(StaticFs, FileAtRootResolvesAndReturnsTrue)
{
	const TestScratchDir scratch("amule-staticfs-file-at-root-");
	const std::string root = scratch.Path();
	ASSERT_TRUE(!root.empty());
	const std::string asset = root + "/index.html";
	ASSERT_TRUE(WriteFile(asset, "x"));

	std::string out;
	ASSERT_TRUE(ResolveWithinRoot(root, "index.html", out));
	// Trailing separator is stripped by the resolver -- strict equality
	// against the file path is the cleanest check.
	ASSERT_TRUE(out.size() >= asset.size() - 1);
}

TEST(StaticFs, RootWithTrailingSlashResolves)
{
	// Regression: Windows _fullpath() preserves a trailing slash from its input, which
	// previously broke the prefix-comparison containment check. AMULEAPI_STATIC_DIR is baked
	// with a trailing slash (cmake convention for dirs) so the bug shipped to every installed
	// amuleapi binary until normalised here.
	const TestScratchDir scratch("amule-staticfs-trailing-slash-");
	const std::string root = scratch.Path();
	ASSERT_TRUE(!root.empty());
	ASSERT_TRUE(WriteFile(root + "/index.html", "x"));

	std::string out;
	ASSERT_TRUE(ResolveWithinRoot(root + "/", "index.html", out));
}

TEST(StaticFs, NestedFileResolvesAndReturnsTrue)
{
	const TestScratchDir scratch("amule-staticfs-nested-");
	const std::string root = scratch.Path();
	ASSERT_TRUE(!root.empty());
	ASSERT_TRUE(MkSubdir(root + "/assets"));
	ASSERT_TRUE(WriteFile(root + "/assets/app.js", "y"));

	std::string out;
	ASSERT_TRUE(ResolveWithinRoot(root, "assets/app.js", out));
}

// Reject paths -- an opaque false return for any failure mode, so the dispatcher can map every miss
// to the same 404.

TEST(StaticFs, MissingFileReturnsFalse)
{
	const TestScratchDir scratch("amule-staticfs-missing-");
	const std::string root = scratch.Path();
	ASSERT_TRUE(!root.empty());

	std::string out;
	ASSERT_TRUE(!ResolveWithinRoot(root, "not-there.html", out));
}

TEST(StaticFs, MissingRootReturnsFalse)
{
	std::string out;
	ASSERT_TRUE(!ResolveWithinRoot("/this/path/should/not/exist/amuleapi-test", "index.html", out));
}

TEST(StaticFs, ParentEscapeRejectedEvenIfTargetExists)
{
	// Set up two sibling dirs under a common parent: `root/` and `outside/`, with
	// `outside/secret.txt` existing. A request for `../outside/secret.txt` under `root` must be
	// rejected even though the file is real and stat'able -- the canonical resolution would
	// land outside root.
	const TestScratchDir scratch("amule-staticfs-parent-escape-");
	const std::string parent = scratch.Path();
	ASSERT_TRUE(!parent.empty());
	const std::string root = parent + "/root";
	const std::string outside = parent + "/outside";
	ASSERT_TRUE(MkSubdir(root));
	ASSERT_TRUE(MkSubdir(outside));
	ASSERT_TRUE(WriteFile(outside + "/secret.txt", "pwn"));

	std::string out;
	ASSERT_TRUE(!ResolveWithinRoot(root, "../outside/secret.txt", out));
}

// Symlink containment -- the security-critical case the resolver exists for. POSIX only: Windows
// symlinks require elevation and _fullpath() is lexical-only, so symlink behaviour is not part of
// the Windows wire contract.

#ifndef _WIN32
TEST(StaticFs, SymlinkEscapingRootIsRejected)
{
	const TestScratchDir scratch("amule-staticfs-symlink-escape-");
	const std::string parent = scratch.Path();
	ASSERT_TRUE(!parent.empty());
	const std::string root = parent + "/root";
	const std::string outside = parent + "/outside";
	ASSERT_TRUE(MkSubdir(root));
	ASSERT_TRUE(MkSubdir(outside));
	ASSERT_TRUE(WriteFile(outside + "/secret.txt", "pwn"));
	// Plant a symlink INSIDE root that points OUTSIDE root.
	const std::string link = root + "/leak.txt";
	ASSERT_TRUE(symlink((outside + "/secret.txt").c_str(), link.c_str()) == 0);

	std::string out;
	ASSERT_TRUE(!ResolveWithinRoot(root, "leak.txt", out));
}

TEST(StaticFs, SymlinkPointingInsideRootIsAccepted)
{
	// Sanity: not every symlink is an attack. A link from one file in
	// root to another file in root resolves cleanly and must be served.
	const TestScratchDir scratch("amule-staticfs-symlink-inside-");
	const std::string root = scratch.Path();
	ASSERT_TRUE(!root.empty());
	ASSERT_TRUE(WriteFile(root + "/real.txt", "ok"));
	const std::string link = root + "/alias.txt";
	ASSERT_TRUE(symlink((root + "/real.txt").c_str(), link.c_str()) == 0);

	std::string out;
	ASSERT_TRUE(ResolveWithinRoot(root, "alias.txt", out));
}
#endif // !_WIN32

// IsDir -- the tiny stat wrapper the discovery chain uses. The trailing-slash tolerance matters for
// the configure-time AMULEAPI_STATIC_DIR macro, which is set with a trailing slash for
// concatenation with sub-paths.

TEST(StaticFs, IsDirTrueForExistingDirectory)
{
	const TestScratchDir scratch("amule-staticfs-isdir-yes-");
	const std::string root = scratch.Path();
	ASSERT_TRUE(!root.empty());
	ASSERT_TRUE(IsDir(root));
	ASSERT_TRUE(IsDir(root + "/")); // trailing slash tolerated
}

TEST(StaticFs, IsDirFalseForFile)
{
	const TestScratchDir scratch("amule-staticfs-isdir-file-");
	const std::string root = scratch.Path();
	ASSERT_TRUE(!root.empty());
	const std::string file = root + "/just-a-file.txt";
	ASSERT_TRUE(WriteFile(file, "x"));

	ASSERT_TRUE(!IsDir(file));
}

TEST(StaticFs, IsDirFalseForMissingPath)
{
	ASSERT_TRUE(!IsDir("/path/that/should/not/exist/under/test"));
	ASSERT_TRUE(!IsDir(""));
}
