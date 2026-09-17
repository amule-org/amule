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

#include <algorithm>
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
#include <vector>

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
// what CLogTee::RedirectStderrForCrash() depends on for amuleapi's wx fatal handler. The
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

namespace
{

// A fixed stamp, so outputs compare byte for byte, and a count of lines stamped.
struct FakeClock
{
	int calls = 0;
	CLineStamper::Clock Get()
	{
		return [this] {
			++calls;
			return std::string("[T]");
		};
	}
};

std::string Feed(CLineStamper &stamper, const std::string &in)
{
	return stamper.Feed(in.data(), in.size());
}

std::string Take(CLineAssembler &lines, const std::string &in)
{
	return lines.Take(in.data(), in.size());
}

std::string FeedInChunks(CLineStamper &stamper, const std::string &in, std::size_t chunk)
{
	std::string out;
	for (std::size_t i = 0; i < in.size(); i += chunk) {
		out += stamper.Feed(in.data() + i, std::min(chunk, in.size() - i));
	}
	return out;
}

// "YYYY-MM-DD HH:MM:SS: "
bool HasStampShape(const std::string &line)
{
	static const char kShape[] = "dddd-dd-dd dd:dd:dd: ";
	if (line.size() < sizeof(kShape) - 1) {
		return false;
	}
	for (std::size_t i = 0; i < sizeof(kShape) - 1; ++i) {
		const bool ok = kShape[i] == 'd' ? (line[i] >= '0' && line[i] <= '9') : line[i] == kShape[i];
		if (!ok) {
			return false;
		}
	}
	return true;
}

// Lines without their terminator; a trailing newline adds no empty last line.
std::vector<std::string> SplitLines(const std::string &text)
{
	std::vector<std::string> lines;
	std::size_t start = 0;
	while (start < text.size()) {
		std::size_t nl = text.find('\n', start);
		if (nl == std::string::npos) {
			nl = text.size();
		}
		lines.push_back(text.substr(start, nl - start));
		start = nl + 1;
	}
	return lines;
}

int TestDup(int fd)
{
#ifdef _WIN32
	return _dup(fd);
#else
	return ::dup(fd);
#endif
}

int TestDup2(int from, int to)
{
#ifdef _WIN32
	return _dup2(from, to);
#else
	return ::dup2(from, to);
#endif
}

void TestClose(int fd)
{
#ifdef _WIN32
	_close(fd);
#else
	::close(fd);
#endif
}

int TestFileno(std::FILE *fp)
{
#ifdef _WIN32
	return _fileno(fp);
#else
	return fileno(fp);
#endif
}

// Points fd 1 and 2 at files for the life of the object, so a tee installed inside it takes
// those files as its console and a test can read back what the console got.
class CConsoleCapture
{
public:
	CConsoleCapture(const std::string &outPath, const std::string &errPath)
	{
		std::fflush(stdout);
		std::fflush(stderr);
		m_savedOut = TestDup(1);
		m_savedErr = TestDup(2);
		m_out = std::fopen(outPath.c_str(), "wb");
		m_err = std::fopen(errPath.c_str(), "wb");
		TestDup2(TestFileno(m_out), 1);
		TestDup2(TestFileno(m_err), 2);
	}
	~CConsoleCapture()
	{
		std::fflush(stdout);
		std::fflush(stderr);
		TestDup2(m_savedOut, 1);
		TestDup2(m_savedErr, 2);
		TestClose(m_savedOut);
		TestClose(m_savedErr);
		std::fclose(m_out);
		std::fclose(m_err);
	}

private:
	int m_savedOut = -1;
	int m_savedErr = -1;
	std::FILE *m_out = nullptr;
	std::FILE *m_err = nullptr;
};

} // namespace

TEST(LogTee, StamperStampsEachLine)
{
	FakeClock clock;
	CLineStamper stamper(clock.Get());
	ASSERT_EQUALS(std::string("[T]one\n[T]two\n"), Feed(stamper, "one\ntwo\n"));
	ASSERT_EQUALS(2, clock.calls);
}

// amuled writes blank lines without a stamp. "\r\n" counts as blank, for Windows.
TEST(LogTee, StamperLeavesBlankLinesUnstamped)
{
	FakeClock clock;
	CLineStamper stamper(clock.Get());
	ASSERT_EQUALS(std::string("[T]a\n\n[T]b\n"), Feed(stamper, "a\n\nb\n"));
	ASSERT_EQUALS(std::string("[T]c\r\n\r\n[T]d\r\n"), Feed(stamper, "c\r\n\r\nd\r\n"));
	ASSERT_EQUALS(4, clock.calls);
}

// A line split across reads gets one stamp, taken when its first byte arrives.
TEST(LogTee, StamperStampsASplitLineOnce)
{
	FakeClock clock;
	CLineStamper stamper(clock.Get());
	ASSERT_EQUALS(std::string("[T]par"), Feed(stamper, "par"));
	ASSERT_EQUALS(1, clock.calls);
	ASSERT_EQUALS(std::string("tial\n"), Feed(stamper, "tial\n"));
	ASSERT_EQUALS(1, clock.calls);
	ASSERT_EQUALS(std::string(""), Feed(stamper, ""));
	ASSERT_EQUALS(1, clock.calls);
	ASSERT_EQUALS(std::string("[T]next"), Feed(stamper, "next"));
	ASSERT_EQUALS(2, clock.calls);
}

// A newline as the last byte of a read: the next read starts a line.
TEST(LogTee, StamperNewlineEndingARead)
{
	FakeClock clock;
	CLineStamper stamper(clock.Get());
	ASSERT_EQUALS(std::string("[T]abc\n"), Feed(stamper, "abc\n"));
	ASSERT_EQUALS(std::string("[T]def\n"), Feed(stamper, "def\n"));
}

// The pump reads CLogTee::kReadChunk bytes at a time, but a pipe read can return fewer, so a
// read boundary can fall anywhere. Splitting at every offset must give the same stamped text.
TEST(LogTee, StamperOutputIndependentOfReadBoundaries)
{
	const std::size_t chunk = CLogTee::kReadChunk;
	const std::string longX(chunk + 100, 'x');
	const std::string longY(chunk - 1, 'y');
	const std::string input = longX + "\n" + "short\n\n" + longY + "\r\n" + "tail";
	const std::string expected =
		"[T]" + longX + "\n" + "[T]short\n\n" + "[T]" + longY + "\r\n" + "[T]tail";

	for (std::size_t split = 0; split <= input.size(); ++split) {
		FakeClock clock;
		CLineStamper stamper(clock.Get());
		std::string out = stamper.Feed(input.data(), split);
		out += stamper.Feed(input.data() + split, input.size() - split);
		ASSERT_EQUALS(expected, out);
		ASSERT_EQUALS(4, clock.calls);
	}

	for (const std::size_t size : { chunk, chunk - 1, chunk + 1, static_cast<std::size_t>(1) }) {
		FakeClock clock;
		CLineStamper stamper(clock.Get());
		ASSERT_EQUALS(expected, FeedInChunks(stamper, input, size));
	}
}

TEST(LogTee, AssemblerHoldsAPartialLine)
{
	CLineAssembler lines;
	ASSERT_EQUALS(std::string(""), Take(lines, "par"));
	ASSERT_EQUALS(std::string("partial\none\n"), Take(lines, "tial\none\ntwo"));
	ASSERT_EQUALS(std::string("two\n"), lines.Flush());
	ASSERT_EQUALS(std::string(""), lines.Flush());
}

// One line that never ends must not grow without bound.
TEST(LogTee, AssemblerReleasesAnOverlongLine)
{
	CLineAssembler lines;
	const std::string chunk(CLineAssembler::kMaxPending - 1, 'q');
	ASSERT_EQUALS(std::string(""), Take(lines, chunk));
	ASSERT_EQUALS(chunk + "qq", Take(lines, "qq"));
	ASSERT_EQUALS(std::string(""), lines.Flush());
}

TEST(LogTee, LocalLogStampShape)
{
	const std::string stamp = LocalLogStamp();
	ASSERT_EQUALS(static_cast<size_t>(21), stamp.size());
	ASSERT_TRUE(HasStampShape(stamp));
}

// Through the real pipes, both streams, a line longer than one read: the console and the file
// get the same stamped lines.
TEST(LogTee, TeeStampsConsoleAndFile)
{
	const std::string logPath = TmpPath("_k.log");
	const std::string outPath = TmpPath("_k.out");
	const std::string errPath = TmpPath("_k.err");
	Cleanup(logPath);
	const std::string longLine(CLogTee::kReadChunk + 10, 'z');
	bool installed = false;
	{
		CConsoleCapture capture(outPath, errPath);
		CLogTee tee;
		installed = tee.Install(logPath, 0);
		if (installed) {
			// Raw write, not fwrite(stdout): muleunit prints with wxPuts, which leaves glibc's
			// stdout wide-oriented, and glibc then drops byte writes to it.
			const std::string out = longLine + "\nout-two\n";
			OsWriteFd(1, out.data(), static_cast<unsigned>(out.size()));
			OsWriteFd(2, "err-one\n\nerr-two\n", 17);
			tee.Uninstall();
		}
	}
	ASSERT_TRUE(installed);

	const std::vector<std::string> outLines = SplitLines(ReadFile(outPath));
	ASSERT_EQUALS(static_cast<size_t>(2), outLines.size());
	ASSERT_TRUE(HasStampShape(outLines[0]));
	ASSERT_EQUALS(longLine, outLines[0].substr(21));
	ASSERT_EQUALS(std::string("out-two"), outLines[1].substr(21));

	const std::vector<std::string> errLines = SplitLines(ReadFile(errPath));
	ASSERT_EQUALS(static_cast<size_t>(3), errLines.size());
	ASSERT_TRUE(HasStampShape(errLines[0]));
	ASSERT_EQUALS(std::string("err-one"), errLines[0].substr(21));
	ASSERT_EQUALS(std::string(""), errLines[1]);
	ASSERT_EQUALS(std::string("err-two"), errLines[2].substr(21));

	// The two streams interleave in the file, so compare contents rather than order.
	std::vector<std::string> fileLines = SplitLines(ReadFile(logPath));
	ASSERT_EQUALS(static_cast<size_t>(5), fileLines.size());
	int blank = 0;
	std::vector<std::string> payloads;
	for (const std::string &line : fileLines) {
		if (line.empty()) {
			++blank;
			continue;
		}
		ASSERT_TRUE(HasStampShape(line));
		payloads.push_back(line.substr(21));
	}
	ASSERT_EQUALS(1, blank);
	std::sort(payloads.begin(), payloads.end());
	const std::vector<std::string> expected = { "err-one", "err-two", "out-two", longLine };
	ASSERT_TRUE(payloads == expected);

	Cleanup(logPath);
	std::remove(outPath.c_str());
	std::remove(errPath.c_str());
}

// A line one stream never finishes (getpass() leaves its prompt that way) must still end its own
// line in the file, so the other stream's next line is not appended to it.
TEST(LogTee, TeeEndsAnUnfinishedLineInTheFile)
{
	const std::string logPath = TmpPath("_n.log");
	const std::string outPath = TmpPath("_n.out");
	const std::string errPath = TmpPath("_n.err");
	Cleanup(logPath);
	const std::string prompt = "prompt: ";
	const std::string after = "after\n";
	bool installed = false;
	{
		CConsoleCapture capture(outPath, errPath);
		CLogTee tee;
		installed = tee.Install(logPath, 0);
		if (installed) {
			OsWriteFd(2, prompt.data(), static_cast<unsigned>(prompt.size()));
			OsWriteFd(1, after.data(), static_cast<unsigned>(after.size()));
			tee.Uninstall();
		}
	}
	ASSERT_TRUE(installed);

	std::vector<std::string> payloads;
	for (const std::string &line : SplitLines(ReadFile(logPath))) {
		ASSERT_TRUE(HasStampShape(line));
		payloads.push_back(line.substr(21));
	}
	std::sort(payloads.begin(), payloads.end());
	const std::vector<std::string> expected = { "after", "prompt: " };
	ASSERT_TRUE(payloads == expected);

	Cleanup(logPath);
	std::remove(outPath.c_str());
	std::remove(errPath.c_str());
}

// An empty path tees to the console only, still stamped, with no crash descriptor.
TEST(LogTee, ConsoleOnlyInstallStamps)
{
	const std::string outPath = TmpPath("_l.out");
	const std::string errPath = TmpPath("_l.err");
	bool installed = false;
	int crashFd = 0;
	int consoleFd = -1;
	{
		CConsoleCapture capture(outPath, errPath);
		CLogTee tee;
		installed = tee.Install(std::string(), 0);
		if (installed) {
			crashFd = tee.CrashFd();
			consoleFd = tee.ConsoleFd();
			OsWriteFd(2, "hello\n", 6);
			tee.Uninstall();
		}
	}
	ASSERT_TRUE(installed);
	ASSERT_EQUALS(-1, crashFd);
	ASSERT_TRUE(consoleFd >= 0);

	const std::vector<std::string> errLines = SplitLines(ReadFile(errPath));
	ASSERT_EQUALS(static_cast<size_t>(1), errLines.size());
	ASSERT_TRUE(HasStampShape(errLines[0]));
	ASSERT_EQUALS(std::string("hello"), errLines[0].substr(21));

	std::remove(outPath.c_str());
	std::remove(errPath.c_str());
}

// Without a file the crash redirect falls back to the console. Otherwise wx's backtrace goes into
// the tee pipe, whose pump thread dies with the process.
TEST(LogTee, CrashRedirectWithoutFileReachesConsole)
{
	const std::string outPath = TmpPath("_m.out");
	const std::string errPath = TmpPath("_m.err");
	bool installed = false;
	{
		CConsoleCapture capture(outPath, errPath);
		CLogTee tee;
		installed = tee.Install(std::string(), 0);
		if (installed) {
			tee.RedirectStderrForCrash();
			OsWriteFd(2, "backtrace\n", 10);
			tee.Uninstall();
		}
	}
	ASSERT_TRUE(installed);
	// Written straight to the console, not through the pump, so unstamped.
	ASSERT_EQUALS(std::string("backtrace\n"), ReadFile(errPath));

	std::remove(outPath.c_str());
	std::remove(errPath.c_str());
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
