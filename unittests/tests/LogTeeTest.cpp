//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// Any parts of this program derived from the xMule, lMule or eMule project,
// or contributed by third-party developers are copyrighted by their
// respective authors.
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
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
//

#include <muleunit/test.h>

#include "LogTee.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace muleunit;
using namespace webapi;

DECLARE_SIMPLE(LogTee)

namespace
{

std::string ReadFile(const std::string &path)
{
	std::ifstream f(path, std::ios::binary);
	std::ostringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

bool Exists(const std::string &path)
{
	return std::ifstream(path).good();
}

// Relative to the test's working directory (the build tree, always writable)
// so the path is valid on every platform -- a hardcoded /tmp does not resolve
// for a native Windows binary. Each case uses a distinct suffix, so the files
// never collide within a run.
std::string TmpPath(const char *suffix)
{
	return std::string("amule_logtee_test") + suffix;
}

void Cleanup(const std::string &path)
{
	std::remove(path.c_str());
	std::remove((path + ".1").c_str());
}

} // namespace

// A plain append with no cap keeps every byte in order.
TEST(LogTee, AppendsAndPersists)
{
	const std::string path = TmpPath("_a.log");
	Cleanup(path);
	{
		CRotatingLog log;
		ASSERT_TRUE(log.Open(path, 0));
		log.Write("hello ", 6);
		log.Write("world", 5);
	}
	ASSERT_EQUALS(std::string("hello world"), ReadFile(path));
	Cleanup(path);
}

// Crossing the cap rotates: the prior content moves to "<path>.1" and the
// triggering write starts a fresh main file.
TEST(LogTee, RotatesAtCap)
{
	const std::string path = TmpPath("_b.log");
	const std::string rot = path + ".1";
	Cleanup(path);
	{
		CRotatingLog log;
		ASSERT_TRUE(log.Open(path, 10));
		log.Write("AAAAA", 5); // size 5
		log.Write("BBBBB", 5); // size 10 -- not over the cap, no rotation
		log.Write("CCCCC", 5); // 10 + 5 > 10 -> rotate first, then write
	}
	ASSERT_TRUE(Exists(rot));
	ASSERT_EQUALS(std::string("AAAAABBBBB"), ReadFile(rot));
	ASSERT_EQUALS(std::string("CCCCC"), ReadFile(path));
	Cleanup(path);
}

// Open() seeds the running size from an existing file so the cap accounts for
// content written in earlier runs (append mode).
TEST(LogTee, SeedsSizeFromExistingFile)
{
	const std::string path = TmpPath("_c.log");
	const std::string rot = path + ".1";
	Cleanup(path);
	{
		std::ofstream f(path, std::ios::binary);
		f << "01234567"; // 8 pre-existing bytes
	}
	{
		CRotatingLog log;
		ASSERT_TRUE(log.Open(path, 10)); // seeds curSize = 8
		log.Write("XYZ", 3);             // 8 + 3 > 10 -> rotate
	}
	ASSERT_TRUE(Exists(rot));
	ASSERT_EQUALS(std::string("01234567"), ReadFile(rot));
	ASSERT_EQUALS(std::string("XYZ"), ReadFile(path));
	Cleanup(path);
}

// maxBytes == 0 disables rotation entirely.
TEST(LogTee, NoRotationWhenCapZero)
{
	const std::string path = TmpPath("_d.log");
	const std::string rot = path + ".1";
	Cleanup(path);
	{
		CRotatingLog log;
		ASSERT_TRUE(log.Open(path, 0));
		for (int i = 0; i < 100; ++i) {
			log.Write("0123456789", 10);
		}
	}
	ASSERT_FALSE(Exists(rot));
	ASSERT_EQUALS(static_cast<size_t>(1000), ReadFile(path).size());
	Cleanup(path);
}

// A single chunk larger than the cap written to an empty file is not rotated --
// it has to land somewhere, and rotating an empty file would lose it.
TEST(LogTee, OversizedChunkOnEmptyFileIsNotRotated)
{
	const std::string path = TmpPath("_e.log");
	const std::string rot = path + ".1";
	Cleanup(path);
	{
		CRotatingLog log;
		ASSERT_TRUE(log.Open(path, 4));
		log.Write("abcdefgh", 8); // curSize == 0 -> no rotation despite 8 > 4
	}
	ASSERT_FALSE(Exists(rot));
	ASSERT_EQUALS(std::string("abcdefgh"), ReadFile(path));
	Cleanup(path);
}
