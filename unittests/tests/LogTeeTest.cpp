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
#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
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

// Raw write to a descriptor, the way the crash handler reaches the log.
long OsWriteFd(int fd, const void *buf, unsigned n)
{
#ifdef _WIN32
	return _write(fd, buf, n);
#else
	return ::write(fd, buf, n);
#endif
}

bool Exists(const std::string &path)
{
	return std::ifstream(path).good();
}

// Relative to the test's working directory (the build tree, always writable) so the path is valid
// on every platform -- a hardcoded /tmp does not resolve for a native Windows binary. Each case
// uses a distinct suffix, so the files never collide within a run.
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

// Every platform has to hand the crash path something to write to while the log is open. The rest
// of the contract differs -- POSIX reserves a number, Windows resolves the current file, since an
// open handle there blocks the rename() that rotation needs -- but this much is common, and it is
// what CLogTee::RedirectStderrToFileForCrash() depends on for amuleapi's wx fatal handler. The
// other cases below are POSIX-only, which is how a Windows regression here went unnoticed.
TEST(LogTee, CrashFdIsAvailableWhileOpen)
{
	const std::string path = TmpPath("_j.log");
	Cleanup(path);
	{
		CRotatingLog log;
		ASSERT_TRUE(log.Open(path, 0));
		ASSERT_TRUE(log.CrashFd() >= 0);
	}
	Cleanup(path);
}

#ifndef _WIN32

// The crash descriptor is a reserved number that the log's own open/close cycle cannot free. A
// fatal handler cannot re-resolve it (every Fd lookup locks), so it holds this number from install
// to exit. If it were the FILE*'s own descriptor, Rotate()'s fclose() would free it and whatever
// the process opened next would inherit it -- in amuleapi, an accepted client socket.
//
// Rotation must not move it, and it must address the file that is current afterwards.
TEST(LogTee, CrashFdSurvivesRotation)
{
	const std::string path = TmpPath("_g.log");
	const std::string rot = path + ".1";
	Cleanup(path);
	{
		CRotatingLog log;
		ASSERT_TRUE(log.Open(path, 10));
		const int crashFd = log.CrashFd();

		log.Write("AAAAAAAAAA", 10); // fills the cap
		log.Write("BBBBB", 5);       // crosses it -> rotate, reopen

		ASSERT_EQUALS(crashFd, log.CrashFd());
		ASSERT_EQUALS(2L, static_cast<long>(OsWriteFd(crashFd, "CC", 2)));
	}
	ASSERT_EQUALS(std::string("AAAAAAAAAA"), ReadFile(rot));
	ASSERT_EQUALS(std::string("BBBBBCC"), ReadFile(path));
	Cleanup(path);
}

// Close() leaves it open on the last file on purpose, so a holder of the number never has a closed
// descriptor. Writing to it after Close() must reach the file rather than fail. This is also the
// assertion that tells the reserved descriptor apart from the FILE*'s own: a rotation hands the
// same number back in a single-threaded test, so only closing the file separates the two.
TEST(LogTee, CrashFdStaysUsableAfterClose)
{
	const std::string path = TmpPath("_h.log");
	Cleanup(path);
	{
		CRotatingLog log;
		ASSERT_TRUE(log.Open(path, 0));
		log.Write("live", 4);
		const int crashFd = log.CrashFd();

		log.Close();

		ASSERT_EQUALS(4L, static_cast<long>(OsWriteFd(crashFd, "dead", 4)));
	}
	ASSERT_EQUALS(std::string("livedead"), ReadFile(path));
	Cleanup(path);
}

// The reserved descriptor is never closed, so it must not follow an exec'd child and hold the log
// file open there. aMule spawns children via wxExecute.
TEST(LogTee, CrashFdIsCloseOnExec)
{
	const std::string path = TmpPath("_i.log");
	Cleanup(path);
	{
		CRotatingLog log;
		ASSERT_TRUE(log.Open(path, 0));
		const int flags = fcntl(log.CrashFd(), F_GETFD);
		ASSERT_TRUE(flags >= 0);
		ASSERT_TRUE((flags & FD_CLOEXEC) != 0);
	}
	Cleanup(path);
}

#endif /* !_WIN32 */
