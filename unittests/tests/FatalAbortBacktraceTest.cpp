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

// get_backtrace() resolves frames only on Linux, through bfd or else addr2line. Elsewhere --
// FreeBSD has execinfo.h in base -- it returns a placeholder, with no frames to check.
#if defined(MULE_HAVE_ABORT_BACKTRACE) && defined(__linux__)
#define MULE_HAVE_LINUX_BACKTRACE 1
#endif

#ifdef MULE_HAVE_ABORT_BACKTRACE
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <thread>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <climits>
#include <fstream>
#include <spawn.h>
#include <ostream>
#include <streambuf>
#include <utility>

extern char **environ; // for posix_spawnp()
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
	int exit_code = -1; // only meaningful when the child was not signalled
	bool timed_out = false;
};

// What a child exits with when its own double free went through unnoticed, so the parent can tell
// an allocator that absorbed the corruption from any other quiet exit.
const int kAllocatorAbsorbedIt = 70;

// Runs `body` in a forked child with its stderr on a pipe, and returns what it wrote plus how it
// died. The alarm is the deadlock detector: a handler that allocates can block forever inside
// malloc, and without a bound that would hang the whole suite instead of failing one case.
//
// The parent has a backstop of its own. On Debian's hppa builders a child outlived its alarm and
// the build sat in waitpid() for five hours, and an addr2line the report spawned can hold the
// pipe open after the child is gone. The handlers re-arm 10 s alarms up to three times, so the
// backstop only fires once every alarm has had its chance, and it kills the whole process group.
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
		// Child. Its own process group, so the backstop also reaches whatever it spawns. Redirect
		// stderr to the pipe, then never return: body() is expected to abort.
		setpgid(0, 0);
		close(fds[0]);
		dup2(fds[1], STDERR_FILENO);
		close(fds[1]);
		alarm(timeout_seconds); // SIGALRM kills us if the handler wedges
		body();
		_exit(0); // body() did not abort, which the assertions below will catch
	}

	// Both sides set the group, so the backstop cannot race the child's own setpgid().
	setpgid(pid, pid);
	close(fds[1]);

	const auto backstop =
		std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds * 4 + 30);
	bool pipeOpen = true;
	bool reaped = false;
	bool killed = false;
	int status = 0;
	while (pipeOpen || !reaped) {
		if (!reaped && waitpid(pid, &status, WNOHANG) == pid) {
			reaped = true;
		}
		if (!killed && std::chrono::steady_clock::now() >= backstop) {
			kill(-pid, SIGKILL);
			killed = true;
		}
		if (!pipeOpen) {
			(void)poll(nullptr, 0, 50);
			continue;
		}
		struct pollfd ready;
		ready.fd = fds[0];
		ready.events = POLLIN;
		ready.revents = 0;
		if (poll(&ready, 1, 50) <= 0) {
			continue;
		}
		char buffer[4096];
		const ssize_t got = read(fds[0], buffer, sizeof(buffer));
		if (got > 0) {
			result.stderr_text.append(buffer, static_cast<size_t>(got));
		} else if (got == 0 || errno != EINTR) {
			pipeOpen = false;
		}
	}
	close(fds[0]);

	if (WIFSIGNALED(status)) {
		result.exited_on_signal = true;
		result.signal_number = WTERMSIG(status);
		result.timed_out = killed || result.signal_number == SIGALRM;
	} else if (WIFEXITED(status)) {
		result.exit_code = WEXITSTATUS(status);
		result.timed_out = killed;
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
	// glibc's tcache absorbs frees up to 1032 bytes and catches a double free there before
	// taking the arena lock; above that ceiling the check runs inside the locked region, which
	// is the state the handler has to survive. macOS picks between abort() and a trap on its
	// own, and not purely by size -- this case ran for weeks on 64 bytes and then trapped
	// anyway. Both signals are handled, so it no longer matters which it chooses.
#if defined(__GLIBC__)
	void *p = Launder(malloc(2048));
#else
	void *p = Launder(malloc(64));
#endif
	free(p);
	free(Launder(p));
	_exit(kAllocatorAbsorbedIt); // reached only where the allocator lets a double free through
}

// The trap path on its own, without depending on what an allocator decides to do. __builtin_trap()
// is what hardened libc++ and a violated std::unreachable() reach for, so this is not a synthetic
// case. Which signal it raises is the target's business: brk on arm64 gives SIGTRAP, ud2 on x86_64
// gives SIGILL.
void ChildTrap()
{
	InstallFatalAbortHandler();
	__builtin_trap();
	_exit(42); // not reached: a trap instruction cannot be resumed
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

// Stands in for wx's SIGILL handler. wx's ends in abort(), so this one does too: that is the path
// that re-enters our handler through the still-armed SIGABRT disposition.
extern "C" void PriorIllHandler(int)
{
	static const char marker[] = "PRIOR SIGILL HANDLER RAN\n";
	const ssize_t ignored = write(STDERR_FILENO, marker, sizeof(marker) - 1);
	(void)ignored;
	abort();
}

void ChildIllWithPriorHandler()
{
	struct sigaction prior;
	memset(&prior, 0, sizeof(prior));
	prior.sa_handler = PriorIllHandler;
	sigemptyset(&prior.sa_mask);
	sigaction(SIGILL, &prior, nullptr);

	InstallFatalAbortHandler();
	raise(SIGILL);
	_exit(42); // not reached: the chained handler aborts
}

void ChildTrapSuppressed()
{
	InstallFatalAbortHandler();
	SuppressNextTrapBacktrace();
	raise(SIGTRAP);
	_exit(42); // not reached
}

// The suppression belongs to the thread about to trap at an assert site. A trap on any other
// thread is a real one and must still be reported.
void ChildTrapOnAnotherThread()
{
	InstallFatalAbortHandler();
	SuppressNextTrapBacktrace();
	std::thread([] { raise(SIGTRAP); }).join();
	_exit(42); // not reached
}

#if wxDEBUG_LEVEL
// Choosing Stop is wx setting wxTrapInAssert inside its handler, and the trap follows once the
// handler returns. So the suppression has to outlive the call, which a scoped guard would not.
void ChildWxWillTrap()
{
	InstallFatalAbortHandler();
	RunWxAssertHandler([] { wxTrapInAssert = true; });
	wxTrapInAssert = false; // the assert macro clears it before trapping
	raise(SIGTRAP);
	_exit(42); // not reached
}

// An assert the user let continue arms nothing, so a later trap still reports.
void ChildWxWillNotTrap()
{
	InstallFatalAbortHandler();
	RunWxAssertHandler([] { wxTrapInAssert = false; });
	raise(SIGTRAP);
	_exit(42); // not reached
}
#endif

// It mutes a trap, not the next death of any kind.
void ChildTrapSuppressionThenAbort()
{
	InstallFatalAbortHandler();
	SuppressNextTrapBacktrace();
	abort();
}

int CountOccurrences(const std::string &haystack, const char *needle)
{
	int found = 0;
	const std::string what(needle);
	for (std::string::size_type at = haystack.find(what); at != std::string::npos;
		at = haystack.find(what, at + what.size())) {
		++found;
	}
	return found;
}

bool Contains(const std::string &haystack, const char *needle)
{
	return haystack.find(needle) != std::string::npos;
}

#ifdef MULE_HAVE_LINUX_BACKTRACE
// A binary bfd or addr2line gets no symbols from: one the running user may execute but not read
// (an execute-only mode, or a container user the file's owner does not map to), so
// /proc/self/exe cannot be opened, or a stripped one, which opens but has no symbol table. Or,
// without bfd, no addr2line to ask at all. get_backtrace() is
// called from the fatal-signal and assert handlers, so it has to degrade to raw frames rather than
// assert -- the assert re-entered get_backtrace() and turned the crash being reported into a
// SIGTRAP.
//
// The child is such a copy of this binary -- or, for the frame format, an intact one -- exec'd
// with this variable set. It runs the scenario from static initialisation and exits before the
// suite starts. So get_backtrace() runs before main(), with neither wx's app nor muleunit set up,
// and has to keep needing nothing from them.
const char kBacktraceChildEnv[] = "MULE_TEST_BACKTRACE_CHILD";

std::string s_childCopy;
// The PATH the child runs with, when not this process's own.
std::string s_childPath;

void OnAssertInBacktraceChild(const wxString &, int, const wxString &, const wxString &cond, const wxString &)
{
	fprintf(stderr, "ASSERT FIRED: %s\n", static_cast<const char *>(cond.utf8_str()));
	_exit(99);
}

// The canonical path this process runs from; readlink() needs no read access to the binary.
std::string ThisBinaryPath()
{
	char self[PATH_MAX];
	const ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
	if (len <= 0) {
		return std::string();
	}
	return std::string(self, static_cast<size_t>(len));
}

// The frame the backtraces are taken in. Its name runs to over 1100 characters, mangled or not,
// longer than a line fgets() reads in one go: Boost.Asio has hundreds of names like it, and
// addr2line prints them whole.
template <int... I> __attribute__((noinline)) void ReportFromALongName(std::integer_sequence<int, I...>)
{
	// Twice: the second call must neither retry the open nor use the handle the first one
	// failed to set up.
	fprintf(stderr, "%s", static_cast<const char *>(get_backtrace(0).utf8_str()));
	fprintf(stderr, "%s", static_cast<const char *>(get_backtrace(0).utf8_str()));
	_exit(0);
}

// Reports from the virtual call std::ostream::write() makes into it, so the stack holds a
// library frame with a C++ name, which has to come out demangled whether or not symbols loaded.
struct ReportingStreamBuf : public std::streambuf
{
	std::streamsize xsputn(const char *, std::streamsize) override
	{
		ReportFromALongName(std::make_integer_sequence<int, 200>());
		return 0; // not reached
	}
};

struct BacktraceChild
{
	BacktraceChild()
	{
		// The variable holds the copy's path, and only that copy acts on it. Set to anything
		// for a whole run, it would otherwise end every run of this binary here, with exit 0
		// and no test run.
		const char *copy = getenv(kBacktraceChildEnv);
		if (!copy || ThisBinaryPath() != copy) {
			return;
		}
		wxSetAssertHandler(OnAssertInBacktraceChild);
		ReportingStreamBuf buf;
		std::ostream(&buf).write("x", 1);
		_exit(97); // the stream buffer was never called
	}
} s_backtraceChild;

void ChildExecCopy()
{
	setenv(kBacktraceChildEnv, s_childCopy.c_str(), 1);
	if (!s_childPath.empty()) {
		setenv("PATH", s_childPath.c_str(), 1);
	}
	execl(s_childCopy.c_str(), s_childCopy.c_str(), static_cast<char *>(nullptr));
	_exit(98); // exec failed
}

// Copies this binary next to itself, rather than to /tmp, which may be mounted noexec. The tag
// ends up in the copy's name, so the raw frames of a backtrace taken in it can be recognised.
bool CopyThisBinary(const char *tag)
{
	const std::string self = ThisBinaryPath();
	if (self.empty()) {
		return false;
	}
	s_childCopy = self + tag + std::to_string(getpid());
	std::ifstream in(self, std::ios::binary);
	std::ofstream out(s_childCopy, std::ios::binary);
	out << in.rdbuf();
	return out.good();
}

// Removes the copy when the test leaves, also when an assertion throws on the way out.
struct RemoveTheCopyOnExit
{
	~RemoveTheCopyOnExit() { unlink(s_childCopy.c_str()); }
};

// Strips the copy in place with binutils' strip, run directly rather than through a shell. False
// when strip is missing or fails.
bool StripTheCopy()
{
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
	std::string path = s_childCopy;
	char strip[] = "strip";
	char stripAll[] = "--strip-all";
	char *const argv[] = { strip, stripAll, &path[0], nullptr };
	pid_t pid = 0;
	const int rc = posix_spawnp(&pid, "strip", &actions, nullptr, argv, environ);
	posix_spawn_file_actions_destroy(&actions);
	if (rc != 0) {
		return false;
	}
	int status = 0;
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR) {
			return false;
		}
	}
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Whether this binary is position-independent: ELF e_type ET_DYN (3), read in the byte order
// e_ident[EI_DATA] gives (1 little-endian, 2 big-endian).
bool ThisBinaryIsPie()
{
	std::ifstream self("/proc/self/exe", std::ios::binary);
	unsigned char header[18] = {};
	if (!self.read(reinterpret_cast<char *>(header), sizeof(header)) ||
		memcmp(header, "\177ELF", 4) != 0) {
		return false;
	}
	const unsigned type =
		header[5] == 2 ? (header[16] << 8 | header[17]) : (header[17] << 8 | header[16]);
	return type == 3;
}

// Both calls printed the copy's frames unresolved, as "?? in path(+0x...)[0x...]". For a PIE
// binary the "+0x" offset into it must be there: ASLR makes the runtime address worthless, and the
// offset is what addr2line resolves later. A non-PIE binary loads where it was linked and has none.
bool HasRawFramesOfTheCopy(const std::string &stderrText)
{
	const std::string frame = "?? in " + s_childCopy;
	if (CountOccurrences(stderrText, frame.c_str()) < 2) {
		return false;
	}
	return !ThisBinaryIsPie() || CountOccurrences(stderrText, (frame + "(+0x").c_str()) >= 2;
}

// Both calls named a shared library's C++ frame, "ns::function(args)+0x... in lib.so...", and
// no line holds a mangled name. Which function it is depends on the C++ library's version.
bool NamesLibraryFramesDemangled(const std::string &stderrText)
{
	int named = 0;
	std::string::size_type start = 0;
	while (start < stderrText.size()) {
		std::string::size_type end = stderrText.find('\n', start);
		if (end == std::string::npos) {
			end = stderrText.size();
		}
		const std::string line = stderrText.substr(start, end - start);
		const std::string::size_type in = line.find("+0x");
		if (line.find("::") < in && in != std::string::npos &&
			line.find(" in ", in) != std::string::npos &&
			line.find(".so", in) != std::string::npos) {
			++named;
		}
		start = end + 1;
	}
	return named >= 2 && !Contains(stderrText, "(_Z") && !Contains(stderrText, "] _Z");
}

// How many lines name `what` and run longer than `length` characters.
int CountLinesLongerThan(const std::string &stderrText, const char *what, size_t length)
{
	int found = 0;
	std::string::size_type start = 0;
	while (start < stderrText.size()) {
		std::string::size_type end = stderrText.find('\n', start);
		if (end == std::string::npos) {
			end = stderrText.size();
		}
		if (end - start > length &&
			stderrText.substr(start, end - start).find(what) != std::string::npos) {
			++found;
		}
		start = end + 1;
	}
	return found;
}

// Every line that names the path names it as the module, after " in ", and never where the
// function goes -- which is where a parser that takes the path's '(' for glibc's put it. bfd may
// resolve the frame to file:line instead, so the path may not appear at all.
bool ThePathOnlyFollowsIn(const std::string &stderrText, const char *pathPart)
{
	std::string::size_type start = 0;
	while (start < stderrText.size()) {
		std::string::size_type end = stderrText.find('\n', start);
		if (end == std::string::npos) {
			end = stderrText.size();
		}
		const std::string line = stderrText.substr(start, end - start);
		const std::string::size_type at = line.find(pathPart);
		if (at != std::string::npos && line.rfind(" in ", at) == std::string::npos) {
			return false;
		}
		start = end + 1;
	}
	return true;
}
#endif // MULE_HAVE_LINUX_BACKTRACE

} // namespace

DECLARE_SIMPLE(FatalAbortBacktrace)

// The case the feature exists for. Everything else here is a control for this one.
TEST(FatalAbortBacktrace, RealHeapCorruptionStillProducesABacktrace)
{
	const ChildResult r = RunInChild(ChildDoubleFree);

	ASSERT_FALSE(r.timed_out); // a handler that allocates deadlocks here

	// Whether a double free is noticed at all is the allocator's choice, not ours. glibc and
	// macOS libmalloc both kill the child. FreeBSD's jemalloc takes the second free into its
	// thread cache and says nothing unless libc was built --enable-debug, and the test cannot
	// change that from inside the process: MALLOC_CONF is read at allocator init, long before
	// the fork. Where nothing died there is no report to assert on, and the trap and abort
	// cases below still cover the handler. amule-org/amule#1524.
	if (!r.exited_on_signal) {
		ASSERT_EQUALS(kAllocatorAbsorbedIt, r.exit_code);
		return;
	}

	// Which signal carries the corruption is the allocator's business: glibc abort()s, macOS
	// libmalloc may trap for the very same double free. Asserting one of them is what made this
	// case fail intermittently on macOS. The report is the behaviour under test.
	ASSERT_TRUE(r.signal_number == SIGABRT || r.signal_number == SIGTRAP || r.signal_number == SIGILL);
	ASSERT_TRUE(Contains(r.stderr_text, "FATAL BACKTRACE FOLLOWS"));
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
	ASSERT_TRUE(Contains(r.stderr_text, "FATAL BACKTRACE FOLLOWS"));
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
	ASSERT_TRUE(Contains(r.stderr_text, "FATAL BACKTRACE FOLLOWS"));
	ASSERT_TRUE(Contains(r.stderr_text, "FatalAbortBacktraceTest"));
}

// A trap must report and then still kill the process. Returning from the handler would re-execute
// the trap instruction and spin, so the timeout here is load-bearing rather than belt and braces.
TEST(FatalAbortBacktrace, ATrapReportsAndStillDies)
{
	const ChildResult r = RunInChild(ChildTrap);

	ASSERT_FALSE(r.timed_out);
	ASSERT_TRUE(r.exited_on_signal);
#ifdef __hppa__
	// hppa's trap is a conditional trap, which raises SIGFPE. wx reports that one, not this handler.
	ASSERT_EQUALS(SIGFPE, r.signal_number);
#else
	const bool trapSignal = r.signal_number == SIGTRAP || r.signal_number == SIGILL;
	ASSERT_TRUE(trapSignal);
	ASSERT_TRUE(Contains(r.stderr_text, "FATAL BACKTRACE FOLLOWS"));
	// The report has to name the one that arrived, not a guess.
	ASSERT_TRUE(Contains(r.stderr_text, r.signal_number == SIGILL ? "SIGILL" : "SIGTRAP"));
#endif
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
	ASSERT_FALSE(Contains(r.stderr_text, "FATAL BACKTRACE FOLLOWS"));
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
	ASSERT_FALSE(Contains(r.stderr_text, "FATAL BACKTRACE FOLLOWS"));
}

// wx claims SIGILL too, so installing over it has to hand control back: whoever held the
// disposition must still run, and the banner must appear once and not once per handler entry. The
// chained handler aborts, exactly as wx's does, which re-enters through the SIGABRT disposition.
TEST(FatalAbortBacktrace, ASigillChainsToThePreviousHandler)
{
	const ChildResult r = RunInChild(ChildIllWithPriorHandler);

	ASSERT_FALSE(r.timed_out);
	ASSERT_TRUE(r.exited_on_signal);
	ASSERT_EQUALS(SIGABRT, r.signal_number);
	ASSERT_TRUE(Contains(r.stderr_text, "PRIOR SIGILL HANDLER RAN"));
	ASSERT_EQUALS(1, CountOccurrences(r.stderr_text, "FATAL BACKTRACE FOLLOWS"));
}

// The assert path reaches wx's dialog through wxTrap(), and the symbolicated backtrace is already
// printed by then, so a suppressed trap must stay quiet.
TEST(FatalAbortBacktrace, SuppressedTrapPrintsNoBacktrace)
{
	const ChildResult r = RunInChild(ChildTrapSuppressed);

	ASSERT_FALSE(r.timed_out);
	ASSERT_TRUE(r.exited_on_signal);
	ASSERT_EQUALS(SIGTRAP, r.signal_number);
	ASSERT_FALSE(Contains(r.stderr_text, "FATAL BACKTRACE FOLLOWS"));
}

TEST(FatalAbortBacktrace, ATrapOnAnotherThreadIsStillReported)
{
	const ChildResult r = RunInChild(ChildTrapOnAnotherThread);

	ASSERT_FALSE(r.timed_out);
	ASSERT_TRUE(r.exited_on_signal);
	ASSERT_EQUALS(SIGTRAP, r.signal_number);
	ASSERT_TRUE(Contains(r.stderr_text, "FATAL BACKTRACE FOLLOWS"));
}

#if wxDEBUG_LEVEL
TEST(FatalAbortBacktrace, AnAssertTheUserStoppedMutesItsTrap)
{
	const ChildResult r = RunInChild(ChildWxWillTrap);

	ASSERT_FALSE(r.timed_out);
	ASSERT_TRUE(r.exited_on_signal);
	ASSERT_EQUALS(SIGTRAP, r.signal_number);
	ASSERT_FALSE(Contains(r.stderr_text, "FATAL BACKTRACE FOLLOWS"));
}

TEST(FatalAbortBacktrace, AnAssertTheUserContinuedArmsNothing)
{
	const ChildResult r = RunInChild(ChildWxWillNotTrap);

	ASSERT_FALSE(r.timed_out);
	ASSERT_TRUE(r.exited_on_signal);
	ASSERT_EQUALS(SIGTRAP, r.signal_number);
	ASSERT_TRUE(Contains(r.stderr_text, "FATAL BACKTRACE FOLLOWS"));
}
#endif

TEST(FatalAbortBacktrace, TheTrapSuppressionDoesNotMuteAnAbort)
{
	const ChildResult r = RunInChild(ChildTrapSuppressionThenAbort);

	ASSERT_FALSE(r.timed_out);
	ASSERT_TRUE(r.exited_on_signal);
	ASSERT_EQUALS(SIGABRT, r.signal_number);
	ASSERT_TRUE(Contains(r.stderr_text, "FATAL BACKTRACE FOLLOWS"));
}

#ifdef MULE_HAVE_LINUX_BACKTRACE
TEST(FatalAbortBacktrace, AnUnreadableBinaryStillGivesRawFrames)
{
	// Root reads an execute-only file regardless, so the case cannot be set up there.
	if (geteuid() == 0) {
		return;
	}

	const bool copied = CopyThisBinary(".unreadable.");
	RemoveTheCopyOnExit removeTheCopy;
	ASSERT_TRUE(copied);
	ASSERT_EQUALS(0, chmod(s_childCopy.c_str(), 0111));

	const ChildResult r = RunInChild(ChildExecCopy);

	ASSERT_FALSE(r.timed_out);
	ASSERT_FALSE(r.exited_on_signal);
	ASSERT_EQUALS(0, r.exit_code);
	ASSERT_FALSE(Contains(r.stderr_text, "ASSERT FIRED"));
#ifdef HAVE_BFD
	ASSERT_EQUALS(1, CountOccurrences(r.stderr_text, "Error while opening file for backtrace symbols"));
#endif
	ASSERT_TRUE(HasRawFramesOfTheCopy(r.stderr_text));
	ASSERT_TRUE(NamesLibraryFramesDemangled(r.stderr_text));
}

// Stripped opens fine and reaches the symbol table, which is empty -- a different way to the
// same missing symbols, and one that prints no error on its way there.
TEST(FatalAbortBacktrace, AStrippedBinaryStillGivesRawFrames)
{
	const bool copied = CopyThisBinary(".stripped.");
	RemoveTheCopyOnExit removeTheCopy;
	ASSERT_TRUE(copied);
	// strip comes with binutils, like the bfd headers, but a build host can have one without
	// the other. Without it the case cannot be set up.
	if (!StripTheCopy()) {
		return;
	}
	ASSERT_EQUALS(0, chmod(s_childCopy.c_str(), 0755));

	const ChildResult r = RunInChild(ChildExecCopy);

	ASSERT_FALSE(r.timed_out);
	ASSERT_FALSE(r.exited_on_signal);
	ASSERT_EQUALS(0, r.exit_code);
	ASSERT_FALSE(Contains(r.stderr_text, "ASSERT FIRED"));
#ifdef HAVE_BFD
	ASSERT_EQUALS(1, CountOccurrences(r.stderr_text, "No symbols in the executable"));
#endif
	ASSERT_TRUE(HasRawFramesOfTheCopy(r.stderr_text));
	ASSERT_TRUE(NamesLibraryFramesDemangled(r.stderr_text));
}

// With symbols loaded, bfd resolves this binary's own frames, never a shared library's. Those keep
// the offset backtrace_symbols() gave, which is what looks them up later. The name the copy runs
// under holds '+' and parentheses, which the line parser must not take for its own.
TEST(FatalAbortBacktrace, AnUnresolvedFrameKeepsItsOffset)
{
	const bool copied = CopyThisBinary(".odd+(name).");
	RemoveTheCopyOnExit removeTheCopy;
	ASSERT_TRUE(copied);
	ASSERT_EQUALS(0, chmod(s_childCopy.c_str(), 0755));

	const ChildResult r = RunInChild(ChildExecCopy);

	ASSERT_FALSE(r.timed_out);
	ASSERT_FALSE(r.exited_on_signal);
	ASSERT_EQUALS(0, r.exit_code);
	ASSERT_FALSE(Contains(r.stderr_text, "ASSERT FIRED"));
	// A piece rather than the whole name: a parser that splits the path at its '(' leaves the
	// whole nowhere, and this piece in the function's place.
	ASSERT_TRUE(ThePathOnlyFollowsIn(r.stderr_text, "name)."));
	// glibc's exported entry point, on every startup stack, with its offset into the function.
	ASSERT_TRUE(CountOccurrences(r.stderr_text, "] __libc_start_main+0x") >= 2);
}

#ifndef HAVE_BFD
// addr2line prints the long name of the frame the backtraces are taken in on one line, longer than
// a single fgets() reads. Read in pieces, the extra lines would pair every later frame with the
// wrong name, or fail the count and leave every frame unresolved.
TEST(FatalAbortBacktrace, ALongFunctionNameKeepsTheFramesResolved)
{
	const bool copied = CopyThisBinary(".longname.");
	RemoveTheCopyOnExit removeTheCopy;
	ASSERT_TRUE(copied);
	ASSERT_EQUALS(0, chmod(s_childCopy.c_str(), 0755));

	const ChildResult r = RunInChild(ChildExecCopy);

	ASSERT_FALSE(r.timed_out);
	ASSERT_FALSE(r.exited_on_signal);
	ASSERT_EQUALS(0, r.exit_code);
	ASSERT_FALSE(Contains(r.stderr_text, "ASSERT FIRED"));
	// Named by addr2line on one line longer than fgets() reads at once -- its demangler may leave
	// the name mangled, which is beside the point here -- and the frame after it named right,
	// which a pairing shifted by extra lines would not give.
	ASSERT_TRUE(CountLinesLongerThan(r.stderr_text, "ReportFromALongName", 1023) >= 2);
	ASSERT_TRUE(
		CountOccurrences(r.stderr_text, "] (anonymous namespace)::ReportingStreamBuf::xsputn(") >= 2);
}

// Without bfd, frames are resolved by running addr2line, which may not be installed. A failed run
// still prints nothing, and its frames must not be read as though it had answered.
TEST(FatalAbortBacktrace, AMissingAddr2lineStillGivesRawFrames)
{
	const bool copied = CopyThisBinary(".noaddr2line.");
	RemoveTheCopyOnExit removeTheCopy;
	ASSERT_TRUE(copied);
	ASSERT_EQUALS(0, chmod(s_childCopy.c_str(), 0755));

	s_childPath = "/nonexistent";
	const ChildResult r = RunInChild(ChildExecCopy);
	s_childPath.clear();

	ASSERT_FALSE(r.timed_out);
	ASSERT_FALSE(r.exited_on_signal);
	ASSERT_EQUALS(0, r.exit_code);
	ASSERT_FALSE(Contains(r.stderr_text, "ASSERT FIRED"));
	ASSERT_TRUE(HasRawFramesOfTheCopy(r.stderr_text));
	ASSERT_TRUE(NamesLibraryFramesDemangled(r.stderr_text));
}
#endif
#endif // MULE_HAVE_LINUX_BACKTRACE

#else /* !MULE_HAVE_ABORT_BACKTRACE */

// fork() has no Windows equivalent, and the handler is POSIX-only anyway.
DECLARE_SIMPLE(FatalAbortBacktrace)

TEST(FatalAbortBacktrace, NotApplicableOnWindows)
{
	ASSERT_TRUE(true);
}

#endif /* MULE_HAVE_ABORT_BACKTRACE */
