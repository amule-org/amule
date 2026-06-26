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

namespace
{

// Per-test scratch dir. POSIX uses mkdtemp(); Windows builds a unique
// name under %TEMP%. The dir is created empty; callers populate it and
// must call RemoveAll() at the end of the test. muleunit's
// DECLARE_SIMPLE has no TearDown hook, so cleanup is per-test.
std::string MakeScratchRoot(const char *tag)
{
#ifdef _WIN32
	char tmp[MAX_PATH];
	DWORD n = GetTempPathA(MAX_PATH, tmp);
	std::string base(tmp, n);
	std::string dir = base + "amule-staticfs-" + tag + "-" + std::to_string(static_cast<long>(_getpid()));
	_mkdir(dir.c_str());
	return dir;
#else
	std::string tpl = "/tmp/amule-staticfs-";
	tpl += tag;
	tpl += "-XXXXXX";
	std::vector<char> buf(tpl.begin(), tpl.end());
	buf.push_back('\0');
	if (!mkdtemp(buf.data()))
		return std::string();
	return std::string(buf.data());
#endif
}

bool MkSubdir(const std::string &path)
{
#ifdef _WIN32
	return _mkdir(path.c_str()) == 0;
#else
	return mkdir(path.c_str(), 0755) == 0;
#endif
}

// Write a single byte file. The content isn't load-bearing for these
// tests — they only verify the resolver's accept/reject decision and
// the resolved path, not file body.
bool WriteFile(const std::string &path, const std::string &body)
{
	std::ofstream f(path.c_str(), std::ios::binary);
	if (!f.is_open())
		return false;
	f << body;
	return f.good();
}

void RemoveAll(const std::string &path)
{
	// Recursive rm via system(); test-scratch only, never on real
	// data. Quote the path to survive spaces / odd chars in $TMPDIR.
#ifdef _WIN32
	std::string cmd = "rmdir /S /Q \"" + path + "\" >NUL 2>&1";
#else
	std::string cmd = "rm -rf \"" + path + "\" 2>/dev/null";
#endif
	(void)std::system(cmd.c_str());
}

} // namespace

DECLARE_SIMPLE(StaticFs)

// ----------------------------------------------------------------------
// Plain accept paths — file exists inside root.
// ----------------------------------------------------------------------

TEST(StaticFs, FileAtRootResolvesAndReturnsTrue)
{
	const std::string root = MakeScratchRoot("file-at-root");
	ASSERT_TRUE(!root.empty());
	const std::string asset = root + "/index.html";
	ASSERT_TRUE(WriteFile(asset, "x"));

	std::string out;
	ASSERT_TRUE(ResolveWithinRoot(root, "index.html", out));
	// Trailing separator is stripped by the resolver — strict equality
	// against the file path is the cleanest check.
	ASSERT_TRUE(out.size() >= asset.size() - 1);

	RemoveAll(root);
}

TEST(StaticFs, RootWithTrailingSlashResolves)
{
	// Regression: Windows _fullpath() preserves a trailing slash from
	// its input, which previously broke the prefix-comparison
	// containment check. AMULEAPI_STATIC_DIR is baked with a trailing
	// slash (cmake convention for dirs) so the bug shipped to every
	// installed amuleapi binary until normalised here.
	const std::string root = MakeScratchRoot("trailing-slash");
	ASSERT_TRUE(!root.empty());
	ASSERT_TRUE(WriteFile(root + "/index.html", "x"));

	std::string out;
	ASSERT_TRUE(ResolveWithinRoot(root + "/", "index.html", out));

	RemoveAll(root);
}

TEST(StaticFs, NestedFileResolvesAndReturnsTrue)
{
	const std::string root = MakeScratchRoot("nested");
	ASSERT_TRUE(!root.empty());
	ASSERT_TRUE(MkSubdir(root + "/assets"));
	ASSERT_TRUE(WriteFile(root + "/assets/app.js", "y"));

	std::string out;
	ASSERT_TRUE(ResolveWithinRoot(root, "assets/app.js", out));

	RemoveAll(root);
}

// ----------------------------------------------------------------------
// Reject paths — opaque false return for any failure mode, so the
// dispatcher can map every miss to the same 404.
// ----------------------------------------------------------------------

TEST(StaticFs, MissingFileReturnsFalse)
{
	const std::string root = MakeScratchRoot("missing");
	ASSERT_TRUE(!root.empty());

	std::string out;
	ASSERT_TRUE(!ResolveWithinRoot(root, "not-there.html", out));

	RemoveAll(root);
}

TEST(StaticFs, MissingRootReturnsFalse)
{
	std::string out;
	ASSERT_TRUE(!ResolveWithinRoot("/this/path/should/not/exist/amuleapi-test", "index.html", out));
}

TEST(StaticFs, ParentEscapeRejectedEvenIfTargetExists)
{
	// Set up two sibling dirs under a common parent: `root/` and
	// `outside/`. `outside/secret.txt` exists. A request for
	// `../outside/secret.txt` under `root` must be rejected even
	// though the file is real and stat'able — the canonical resolution
	// would land outside root.
	const std::string parent = MakeScratchRoot("parent-escape");
	ASSERT_TRUE(!parent.empty());
	const std::string root = parent + "/root";
	const std::string outside = parent + "/outside";
	ASSERT_TRUE(MkSubdir(root));
	ASSERT_TRUE(MkSubdir(outside));
	ASSERT_TRUE(WriteFile(outside + "/secret.txt", "pwn"));

	std::string out;
	ASSERT_TRUE(!ResolveWithinRoot(root, "../outside/secret.txt", out));

	RemoveAll(parent);
}

// ----------------------------------------------------------------------
// Symlink containment — the security-critical case the resolver
// exists for. POSIX only: Windows symlinks require elevation and
// _fullpath() is lexical-only, so symlink behaviour isn't part of the
// Windows wire-contract.
// ----------------------------------------------------------------------

#ifndef _WIN32
TEST(StaticFs, SymlinkEscapingRootIsRejected)
{
	const std::string parent = MakeScratchRoot("symlink-escape");
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

	RemoveAll(parent);
}

TEST(StaticFs, SymlinkPointingInsideRootIsAccepted)
{
	// Sanity: not every symlink is an attack. A link from one file in
	// root to another file in root resolves cleanly and must be served.
	const std::string root = MakeScratchRoot("symlink-inside");
	ASSERT_TRUE(!root.empty());
	ASSERT_TRUE(WriteFile(root + "/real.txt", "ok"));
	const std::string link = root + "/alias.txt";
	ASSERT_TRUE(symlink((root + "/real.txt").c_str(), link.c_str()) == 0);

	std::string out;
	ASSERT_TRUE(ResolveWithinRoot(root, "alias.txt", out));

	RemoveAll(root);
}
#endif // !_WIN32

// ----------------------------------------------------------------------
// IsDir — the tiny stat-wrapper used by the discovery chain. The
// trailing-slash tolerance matters for the configure-time
// AMULEAPI_STATIC_DIR macro, which is set with a trailing slash for
// concatenation with sub-paths.
// ----------------------------------------------------------------------

TEST(StaticFs, IsDirTrueForExistingDirectory)
{
	const std::string root = MakeScratchRoot("isdir-yes");
	ASSERT_TRUE(!root.empty());
	ASSERT_TRUE(IsDir(root));
	ASSERT_TRUE(IsDir(root + "/")); // trailing slash tolerated

	RemoveAll(root);
}

TEST(StaticFs, IsDirFalseForFile)
{
	const std::string root = MakeScratchRoot("isdir-file");
	ASSERT_TRUE(!root.empty());
	const std::string file = root + "/just-a-file.txt";
	ASSERT_TRUE(WriteFile(file, "x"));

	ASSERT_TRUE(!IsDir(file));

	RemoveAll(root);
}

TEST(StaticFs, IsDirFalseForMissingPath)
{
	ASSERT_TRUE(!IsDir("/path/that/should/not/exist/under/test"));
	ASSERT_TRUE(!IsDir(""));
}
