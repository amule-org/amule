// SPDX-License-Identifier: GPL-2.0-or-later
// Native process fixture: exercise the runner's actual exit-status handling.
#include <cstdio>
#include <chrono>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <csignal>
#include <sys/resource.h>
#endif

int main()
{
#ifdef _WIN32
	_setmode(_fileno(stdout), _O_BINARY);
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#else
	rlimit noCore{ 0, 0 };
	setrlimit(RLIMIT_CORE, &noCore);
#endif
	const int mode = std::getchar();
	if (mode == 'f') {
		std::fputs("media-probe sandbox: setup failed (fixture)\n", stderr);
		return 12;
	}
	if (mode == 't') {
		std::this_thread::sleep_for(std::chrono::seconds(30));
		return 12;
	}
	if (mode == 'd') {
		const char oversized[10000] = {};
		std::fwrite(oversized, 1, sizeof(oversized), stderr);
		return 11;
	}
	if (mode != 's') {
		std::fputs("AMMP 1\n", stdout);
		if (mode == 'o') {
			const char oversized[70000] = {};
			std::fwrite(oversized, 1, sizeof(oversized), stdout);
		}
		std::fflush(stdout);
	}
	if (mode == 'p') {
		std::fputs("format.duration=123\nformat.bit_rate=1000\n", stdout);
		return 12;
	}
	if (mode == 'e')
		return 1;
	if (mode == 'o')
		return 11;
#ifdef _WIN32
	RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
#else
	std::raise(SIGSEGV);
#endif
	return 12;
}
