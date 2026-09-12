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

// The SIGABRT reporting path, exercised against a real glibc heap abort.
//
// amule-org/amule#1338 died on `double free or corruption (out)` and produced no backtrace at all,
// because wx's fatal-exception support covers SIGSEGV/SIGBUS/SIGILL/SIGFPE and a heap abort is
// SIGABRT. InstallFatalAbortHandler() closes that, and the interesting question is not whether it
// prints -- it is whether it prints when the allocator is already broken.
//
// So the cases below fork, corrupt the heap in the child for real, and read what reaches the
// child's stderr through a pipe. A handler that allocates would deadlock against the arena lock
// malloc_printerr can hold, and that shows up here as the child never exiting, which the alarm
// turns into a failure rather than a hung suite. That is the property worth the machinery: the
// naive version of this handler passes every test that does not actually corrupt the heap.
//
// What these cases deliberately do NOT cover is the difference between this handler and an
// allocating one, because that difference is not reachable from here. Measured directly on glibc
// 2.43 with standalone probes: under corruption detected inside malloc() ("corrupted top size"),
// an allocating handler re-enters malloc_printerr and dies having printed nothing, and a handler
// that skips the install-time backtrace() warm-up fails the same way. Both are real, and both need
// the fault to land on the top chunk, which needs a known heap layout. This binary links wx, so by
// the time a child forks the layout is nothing like a minimal program's and the corruption simply
// does not reproduce -- an attempt at it here aborted nothing and passed silently, for the safe and
// unsafe builds alike. A test that always skips is worse than no test, so it is not kept.

#include <muleunit/test.h>

#include <common/MuleDebug.h>

#include "config.h" // Needed for HAVE_EXECINFO

// Without execinfo.h InstallFatalAbortHandler() is a no-op by design, so there is no backtrace to
// assert on. Our own static musl build is exactly that case: Alpine has not packaged libexecinfo
// since 3.17, so these cases would fail there for a reason that is not a regression.
#if !defined(_WIN32) && defined(HAVE_EXECINFO)
#define MULE_HAVE_ABORT_BACKTRACE 1
#endif

#ifdef MULE_HAVE_ABORT_BACKTRACE
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace muleunit;

#ifdef MULE_HAVE_ABORT_BACKTRACE

namespace
{

//! What a forked child did: everything it wrote to stderr, and how it died.
struct ChildResult
{
	std::string stderr_text;
	bool exited_on_signal = false;
	int signal_number = 0;
	bool timed_out = false;
};

// Runs `body` in a forked child with its stderr on a pipe, and returns what it wrote plus how it
// died. The alarm is the deadlock detector: a handler that allocates can block forever inside
// malloc, and without a bound that would hang the whole suite instead of failing one case.
ChildResult RunInChild(void (*body)(), unsigned timeout_seconds = 10)
{
	ChildResult result;

	int fds[2];
	if (pipe(fds) != 0) {
		return result;
	}

	// fork() duplicates whatever muleunit has buffered on stdout, and the child dies by signal
	// without flushing -- except where it does, which replays the suite's own output once per
	// case. Drain it here so the child inherits nothing to replay.
	fflush(nullptr);

	const pid_t pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return result;
	}

	if (pid == 0) {
		// Child. Redirect stderr to the pipe, then never return: body() is expected to abort.
		close(fds[0]);
		dup2(fds[1], STDERR_FILENO);
		close(fds[1]);
		alarm(timeout_seconds); // SIGALRM kills us if the handler wedges
		body();
		_exit(0); // body() did not abort, which the assertions below will catch
	}

	close(fds[1]);
	char buffer[4096];
	ssize_t got;
	while ((got = read(fds[0], buffer, sizeof(buffer))) != 0) {
		if (got > 0) {
			result.stderr_text.append(buffer, static_cast<size_t>(got));
		} else if (errno != EINTR) {
			break;
		}
	}
	close(fds[0]);

	int status = 0;
	waitpid(pid, &status, 0);
	if (WIFSIGNALED(status)) {
		result.exited_on_signal = true;
		result.signal_number = WTERMSIG(status);
		result.timed_out = (result.signal_number == SIGALRM);
	}
	return result;
}

// Hands a pointer back through an empty asm barrier. Without it an optimising build deletes the
// malloc/free pair outright -- a double free is undefined, so the compiler is entitled to assume it
// never happens. The Release CI job caught exactly that: the child ran to _exit(0) and the test
// asserted on a process that had never corrupted anything.
void *Launder(void *p)
{
	__asm__ __volatile__("" : "+r"(p) : : "memory");
	return p;
}

void ChildDoubleFree()
{
	InstallFatalAbortHandler();
	// A real double free, so the allocator's own detection runs and calls abort() from its
	// error path. Deliberately not raise(SIGABRT): that would test the handler with a healthy
	// heap and prove nothing about the hard case.
	//
	// The size is per-allocator, and both ends of it matter. glibc's tcache absorbs frees up to
	// 1032 bytes and catches a double free there before taking the arena lock; above that
	// ceiling the check runs inside the locked region, which is the state the handler has to
	// survive. macOS goes the other way: a large block traps (SIGTRAP) rather than aborting, so
	// the handler would never be entered and the case would test nothing.
#if defined(__GLIBC__)
	void *p = Launder(malloc(2048));
#else
	void *p = Launder(malloc(64));
#endif
	free(p);
	free(Launder(p));
	_exit(0); // not reached while the allocator detects the double free
}

void ChildPlainAbort()
{
	InstallFatalAbortHandler();
	abort();
}

void ChildRaiseAbrt()
{
	InstallFatalAbortHandler();
	raise(SIGABRT);
	// Reached only if the handler swallowed the signal, which is the regression this catches.
	_exit(42);
}

// std::terminate() with no active exception: OnUnhandledException() prints nothing at all, so the
// raw backtrace is the only report there is. Reachable in production by destroying a joinable
// std::thread without join(), which webapi has several of.
void ChildTerminateWithoutException()
{
	InstallFatalAbortHandler();
	InstallMuleExceptionHandler();
	std::terminate();
	_exit(43); // not reached
}

// Relative to the test's working directory, the build tree, rather than a fixed name in /tmp: a
// leftover there owned by another uid makes the child's open() fail, and the failure then reports
// as a missing signal, which says nothing about what broke. LogTeeTest picks paths the same way.
// The pid keeps two binaries in one build tree apart; it is resolved at static-init time, so the
// forked child and its parent name the same file.
std::string ReadWholeFile(const std::string &path)
{
	std::string out;
	char buf[4096];
	const int fd = open(path.c_str(), O_RDONLY);
	if (fd < 0) {
		return out;
	}
	ssize_t n;
	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		out.append(buf, static_cast<size_t>(n));
	}
	close(fd);
	return out;
}

std::string TmpPath(const char *suffix)
{
	return std::string("amule_fatalabort_") + std::to_string(getpid()) + "_" + suffix;
}

const std::string kTeePath = TmpPath("tee");
const std::string kDurablePath = TmpPath("durable");
const std::string kConsolePath = TmpPath("console");
const std::string kTeeWedgedPath = TmpPath("tee_wedged");

// An unhandled exception with a crash descriptor armed. The report has to reach both sinks: the
// descriptor, which is what survives a daemon, and stderr, which is the console an operator reads.
void ChildTerminateTee()
{
	InstallFatalAbortHandler();
	InstallMuleExceptionHandler();
	const int fd = open(kTeePath.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
	if (fd < 0) {
		_exit(44);
	}
	SetFatalAbortRedirectFd(fd);
	throw std::runtime_error("tee probe");
}

// amuleapi's shape: fd 2 is the tee pipe, the durable sink is the log, and the console copy has to
// go to the saved console descriptor instead of fd 2. Writing it to fd 2 would reach the console
// only while the pump thread still runs, which is true at std::terminate and false in a signal
// handler, and in amuleapi it would also land a second copy in the log the pump feeds.
void ChildTerminateConsoleFd()
{
	InstallFatalAbortHandler();
	InstallMuleExceptionHandler();
	const int durable = open(kDurablePath.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
	const int console = open(kConsolePath.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
	if (durable < 0 || console < 0) {
		_exit(44);
	}
	SetFatalAbortRedirectFd(durable);
	SetFatalAbortConsoleFd(console);
	throw std::runtime_error("console fd probe");
}

// A console that will never accept a write: a pipe filled to capacity with nobody draining it,
// left blocking. Taking the console copy first would park in WriteAll() forever and the durable
// copy would never happen, so the process would neither report nor exit.
void ChildTerminateWedgedConsole()
{
	InstallFatalAbortHandler();
	InstallMuleExceptionHandler();
	const int fd = open(kTeeWedgedPath.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
	if (fd < 0) {
		_exit(44);
	}
	SetFatalAbortRedirectFd(fd);

	int pipefd[2];
	if (pipe(pipefd) != 0) {
		_exit(45);
	}
	// Fill it without blocking, then hand back a blocking descriptor with no room left.
	const int flags = fcntl(pipefd[1], F_GETFL);
	fcntl(pipefd[1], F_SETFL, flags | O_NONBLOCK);
	char filler[4096];
	memset(filler, 'x', sizeof(filler));
	while (write(pipefd[1], filler, sizeof(filler)) > 0) {
		// keep going until the pipe refuses
	}
	fcntl(pipefd[1], F_SETFL, flags);
	dup2(pipefd[1], STDERR_FILENO);

	throw std::runtime_error("wedged console probe");
}

// A version line longer than the buffer. The character truncation drops is the trailing newline,
// which would glue the first backtrace frame onto the end of the version line.
void ChildLongVersionLine()
{
	InstallFatalAbortHandler();
	const std::string huge(400, 'V');
	SetFatalAbortVersionLine(huge.c_str());
	abort();
}

void ChildSuppressed()
{
	InstallFatalAbortHandler();
	SuppressNextAbortBacktrace();
	abort();
}

void ChildNoHandler()
{
	abort();
}

bool Contains(const std::string &haystack, const char *needle)
{
	return haystack.find(needle) != std::string::npos;
}

} // namespace

DECLARE_SIMPLE(FatalAbortBacktrace)

// The case the feature exists for. Everything else here is a control for this one.
TEST(FatalAbortBacktrace, RealHeapCorruptionStillProducesABacktrace)
{
	const ChildResult r = RunInChild(ChildDoubleFree);

	ASSERT_FALSE(r.timed_out); // a handler that allocates deadlocks here
	ASSERT_TRUE(r.exited_on_signal);
	ASSERT_EQUALS(SIGABRT, r.signal_number);
	ASSERT_TRUE(Contains(r.stderr_text, "ABORT BACKTRACE FOLLOWS"));
	// Frames, not just the banner. Deliberately matched on the module name rather than on the
	// allocator's own wording: glibc says "free(): double free detected in tcache 2" and macOS
	// reports through libmalloc, so any assertion on that text is a platform check. Matching a
	// substring of our own banner would be worse still -- it says "double free or corruption"
	// in its explanatory line, so asserting that passes whenever the banner prints at all.
	ASSERT_TRUE(Contains(r.stderr_text, "FatalAbortBacktraceTest"));
}

// A terminate with nothing to describe must not end up silent. The suppression exists to stop a
// second backtrace when a symbolicated one was already printed; when none was, it must not fire.
TEST(FatalAbortBacktrace, TerminateWithoutExceptionStillReports)
{
	const ChildResult r = RunInChild(ChildTerminateWithoutException);

	ASSERT_FALSE(r.timed_out);
	ASSERT_TRUE(r.exited_on_signal);
	ASSERT_EQUALS(SIGABRT, r.signal_number);
	ASSERT_TRUE(Contains(r.stderr_text, "ABORT BACKTRACE FOLLOWS"));
}

// The terminate report goes to the console and to the durable descriptor both. Redirecting instead
// of tee-ing made it durable by taking it off the console, and the SIGABRT handler cannot make up
// for that: this path suppresses it.
TEST(FatalAbortBacktrace, TerminateReportReachesBothSinks)
{
	unlink(kTeePath.c_str());
	const ChildResult r = RunInChild(ChildTerminateTee);

	ASSERT_FALSE(r.timed_out);
	ASSERT_TRUE(r.exited_on_signal);

	// The console copy.
	ASSERT_TRUE(Contains(r.stderr_text, "Terminated after throwing an instance of"));
	ASSERT_TRUE(Contains(r.stderr_text, "tee probe"));

	// The durable copy.
	std::string durable;
	{
		char buf[4096];
		const int fd = open(kTeePath.c_str(), O_RDONLY);
		ASSERT_TRUE(fd >= 0);
		ssize_t n;
		while ((n = read(fd, buf, sizeof(buf))) > 0) {
			durable.append(buf, static_cast<size_t>(n));
		}
		close(fd);
	}
	ASSERT_TRUE(Contains(durable, "Terminated after throwing an instance of"));
	ASSERT_TRUE(Contains(durable, "tee probe"));
	unlink(kTeePath.c_str());
}

// Both files get the report and the inherited fd 2 gets none: the console copy followed the
// descriptor it was given rather than fd 2.
TEST(FatalAbortBacktrace, TerminateReportUsesTheConsoleDescriptor)
{
	unlink(kDurablePath.c_str());
	unlink(kConsolePath.c_str());
	const ChildResult r = RunInChild(ChildTerminateConsoleFd);

	ASSERT_FALSE(r.timed_out);
	ASSERT_TRUE(r.exited_on_signal);
	ASSERT_FALSE(Contains(r.stderr_text, "console fd probe")); // fd 2 stays out of it

	ASSERT_TRUE(Contains(ReadWholeFile(kDurablePath), "console fd probe"));
	ASSERT_TRUE(Contains(ReadWholeFile(kConsolePath), "console fd probe"));
	unlink(kDurablePath.c_str());
	unlink(kConsolePath.c_str());
}

// A console that cannot take the write must not cost the durable copy, nor hang the process.
TEST(FatalAbortBacktrace, TerminateSurvivesAWedgedConsole)
{
	unlink(kTeeWedgedPath.c_str());
	const ChildResult r = RunInChild(ChildTerminateWedgedConsole);

	ASSERT_FALSE(r.timed_out); // parked in fputs() on the console copy
	ASSERT_TRUE(r.exited_on_signal);
	ASSERT_EQUALS(SIGABRT, r.signal_number);

	std::string durable;
	{
		char buf[4096];
		const int fd = open(kTeeWedgedPath.c_str(), O_RDONLY);
		ASSERT_TRUE(fd >= 0);
		ssize_t n;
		while ((n = read(fd, buf, sizeof(buf))) > 0) {
			durable.append(buf, static_cast<size_t>(n));
		}
		close(fd);
	}
	ASSERT_TRUE(Contains(durable, "wedged console probe"));
	unlink(kTeeWedgedPath.c_str());
}

// An over-long build string must still end the line it is on. Truncation that ate the newline would
// run the version straight into the first frame.
TEST(FatalAbortBacktrace, OverlongVersionLineStillEndsItsLine)
{
	const ChildResult r = RunInChild(ChildLongVersionLine);

	ASSERT_FALSE(r.timed_out);
	ASSERT_TRUE(r.exited_on_signal);

	const std::string::size_type at = r.stderr_text.find("VVVV");
	ASSERT_TRUE(at != std::string::npos);

	// The run of V's has to end at a newline, not at a backtrace frame.
	const std::string::size_type endOfRun = r.stderr_text.find_first_not_of('V', at);
	ASSERT_TRUE(endOfRun != std::string::npos);
	ASSERT_EQUALS('\n', r.stderr_text[endOfRun]);
}

// Frames, not just the banner. backtrace_symbols_fd() writes one line per frame naming the module,
// so the test binary's own name has to appear -- that is the difference between a working unwind
// and an empty one.
TEST(FatalAbortBacktrace, TheBacktraceCarriesFrames)
{
	const ChildResult r = RunInChild(ChildPlainAbort);

	ASSERT_FALSE(r.timed_out);
	ASSERT_TRUE(Contains(r.stderr_text, "ABORT BACKTRACE FOLLOWS"));
	ASSERT_TRUE(Contains(r.stderr_text, "FatalAbortBacktraceTest"));
}

// The process must still die, and die of SIGABRT. A handler that reports and returns would swallow
// the signal, and a supervisor watching for a non-zero exit would never restart. This raises the
// signal in-process rather than from outside; delivery to a single-threaded child is the same path.
TEST(FatalAbortBacktrace, TheProcessStillDiesOfSigabrt)
{
	const ChildResult r = RunInChild(ChildRaiseAbrt);

	ASSERT_FALSE(r.timed_out);
	ASSERT_TRUE(r.exited_on_signal);
	ASSERT_EQUALS(SIGABRT, r.signal_number);
}

// ReportAssertFailure() prints a full symbolicated backtrace and then raises SIGABRT, so the raw
// dump has to stay quiet or every assertion failure reports itself twice.
TEST(FatalAbortBacktrace, SuppressedAbortPrintsNoBacktrace)
{
	const ChildResult r = RunInChild(ChildSuppressed);

	ASSERT_FALSE(r.timed_out);
	ASSERT_TRUE(r.exited_on_signal);
	ASSERT_EQUALS(SIGABRT, r.signal_number);
	ASSERT_FALSE(Contains(r.stderr_text, "ABORT BACKTRACE FOLLOWS"));
}

// The control that stops the assertions above passing for the wrong reason: without the handler
// the banner must be absent, so its presence elsewhere is attributable to InstallFatalAbortHandler
// and not to something else on the process's stderr.
TEST(FatalAbortBacktrace, WithoutTheHandlerThereIsNoBacktrace)
{
	const ChildResult r = RunInChild(ChildNoHandler);

	ASSERT_FALSE(r.timed_out);
	ASSERT_TRUE(r.exited_on_signal);
	ASSERT_EQUALS(SIGABRT, r.signal_number);
	ASSERT_FALSE(Contains(r.stderr_text, "ABORT BACKTRACE FOLLOWS"));
}

#else /* !MULE_HAVE_ABORT_BACKTRACE */

// fork() has no Windows equivalent, and the handler is POSIX-only anyway.
DECLARE_SIMPLE(FatalAbortBacktrace)

TEST(FatalAbortBacktrace, NotApplicableOnWindows)
{
	ASSERT_TRUE(true);
}

#endif /* MULE_HAVE_ABORT_BACKTRACE */
