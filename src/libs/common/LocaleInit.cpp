//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// Any parts of this program contributed by third-party developers are
// copyrighted by their respective authors.
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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
//

#include "LocaleInit.h"

#include <clocale>

#ifndef __WXMSW__
#include <cstdlib>
#include <cstring>
#include <langinfo.h>

namespace
{
// True when `s` names the C / POSIX locale (or is empty / unset), i.e. a
// locale with no useful character encoding for filesystem paths.
bool IsCLocaleName(const char *s)
{
	return s == nullptr || s[0] == '\0' || std::strcmp(s, "C") == 0 || std::strcmp(s, "POSIX") == 0;
}

// True when the current LC_CTYPE codeset is a bare ASCII one, meaning the
// process cannot represent accented / UTF-8 filesystem paths.
bool CtypeCodesetIsAscii()
{
	const char *codeset = nl_langinfo(CODESET);
	return codeset != nullptr && (std::strcmp(codeset, "ANSI_X3.4-1968") == 0 ||  // glibc C/POSIX
					     std::strcmp(codeset, "US-ASCII") == 0 || // BSD/macOS C
					     std::strcmp(codeset, "ASCII") == 0);
}
} // namespace
#endif

void aMuleInitLocale()
{
	std::setlocale(LC_ALL, "");

#ifndef __WXMSW__
	// A headless daemon in a bare container (systemd / Docker) frequently has
	// no LANG / LC_* exported, so the call above leaves LC_CTYPE at POSIX/C.
	// Its ASCII codeset cannot represent accented / UTF-8 filesystem paths, so
	// wxConvFileName fails to open them — an accented shared folder silently
	// becomes invisible to the file scan (verified on glibc: `LC_ALL=C` finds
	// 0 files in an accented dir where UTF-8 finds them).
	//
	// Promote just the character type to UTF-8. The guard is the codeset
	// itself, so a real locale is never overridden (a deliberate UTF-8 or even
	// latin1 locale never reports ASCII here) and musl — whose C locale is
	// already UTF-8 — never triggers it. Windows is exempt: it uses wide-char
	// filesystem APIs, so CPath never routes a path through a narrow-locale
	// conversion.
	if (CtypeCodesetIsAscii()) {
		// Pick a UTF-8 ctype the C library actually has: C.UTF-8 is portable
		// (glibc >= 2.35, musl, most runtimes), en_US.UTF-8 the common
		// fallback. If neither exists we stay on C — no worse than before.
		const char *utf8 = nullptr;
		if (std::setlocale(LC_CTYPE, "C.UTF-8") != nullptr) {
			utf8 = "C.UTF-8";
		} else if (std::setlocale(LC_CTYPE, "en_US.UTF-8") != nullptr) {
			utf8 = "en_US.UTF-8";
		}
		if (utf8 != nullptr) {
			// Export it, not just setlocale() it: later re-resolution of the
			// locale (wx sets up wxConvFileName from the environment during
			// app init) would otherwise fall back to C and undo the promotion.
			// LC_ALL outranks LC_CTYPE, so drop a C-valued LC_ALL that would
			// keep forcing the process back to ASCII.
			if (IsCLocaleName(std::getenv("LC_ALL"))) {
				unsetenv("LC_ALL");
			}
			setenv("LC_CTYPE", utf8, 1);
			// Dropping LC_ALL above also stops forcing LC_NUMERIC to C for any
			// child process (media probe, spawned amuleweb/amuleapi): its
			// numeric category would then resolve from LANG and could inherit a
			// comma decimal separator. Pin LC_NUMERIC=C in the environment too
			// so children keep parsing "1.5", matching the in-process setting
			// below.
			setenv("LC_NUMERIC", "C", 1);
		}
	}
#endif

	std::setlocale(LC_NUMERIC, "C");
}
