// SPDX-License-Identifier: GPL-2.0-or-later
// Native process fixture: exercise the runner's actual exit-status handling.
#include <cstdio>
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
	if (mode != 's') {
		std::fputs("AMMP 1\n", stdout);
		if (mode == 'o') {
			const char oversized[70000] = {};
			std::fwrite(oversized, 1, sizeof(oversized), stdout);
		}
		std::fflush(stdout);
	}
	if (mode == 'i')
		return 12; // I/O/configuration failures must not poison known.met.
	if (mode == 't')
		return 13; // The helper's internal deadline is also retryable.
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
