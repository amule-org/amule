/*
 *  Name:         Shared functions
 *
 *  Purpose:      Functions that are used various times in cas
 *
 *  Author:       Pedro de Oliveira <falso@rdk.homeip.net>
 *
 *  Copyright (c) 2004-2011 Pedro de Oliveira ( falso@rdk.homeip-net )
 *
 *  This file is part of aMule.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the
 *  Free Software Foundation, Inc.,
 *  51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h> /* PATH_MAX */

/* XXX This needs to be replaced so that we use
 * autoconf to detect the target OS -- As of now
 * it should compile fine in other non UNIX-like
 * platforms, at the expense of only being sure
 * that UNIX-specific code will be compiled if
 * using GCC (which defines those __unix__ macros)
 * autoconf defines should be in place to test for
 * functions like getpwuid() in UNIX-like systems
 * instead of doing this...
 */

#ifdef __APPLE__
#include <mach-o/dyld.h> /* _NSGetExecutablePath */
#define CAS_DIR_SEPARATOR "/"
#elif defined(__WIN32__)
#define COBJMACROS
#include <winerror.h>
#include <shlobj.h>
#include <objidl.h>
#define CAS_DIR_SEPARATOR "\\"
#else
#define CAS_DIR_SEPARATOR "/"
#if defined(unix) || defined(__unix__) || defined(__unix)
#include <unistd.h>
#include <pwd.h>
#include <fcntl.h>
#define CAS_UNIX
#endif
#endif

/* dir and file joined by one separator.
 * !! it's caller's responsibility to free return value
 */
static char *join_path(const char *dir, const char *file)
{
	size_t len = strlen(dir);
	const char *sep = (len > 0 && dir[len - 1] != CAS_DIR_SEPARATOR[0]) ? CAS_DIR_SEPARATOR : "";
	char *ret = malloc(len + strlen(sep) + strlen(file) + 1);

	if (ret != NULL)
		sprintf(ret, "%s%s%s", dir, sep, file);
	return ret;
}

/* try (hard) to get correct path for aMule signature
 * !! it's caller's responsibility to free return value
 */

char *get_path(const char *file)
{
	static char *saved_home = NULL;

	if (saved_home == NULL) {
#ifdef __APPLE__

		/* ~/Library/Application Support always exists on macOS >= 10.5
		 * and matches what FSFindFolder(kUserDomain,
		 * kApplicationSupportFolderType, ...) used to return. Carbon's
		 * FSRef API is gone in 64-bit macOS, so just derive the path
		 * from $HOME. */
		char home[PATH_MAX];
		home[0] = '\0';

		const char *home_env = getenv("HOME");
		if (home_env) {
			snprintf(home,
				sizeof(home),
				"%s/Library/Application Support" CAS_DIR_SEPARATOR "aMule",
				home_env);
		}

#elif defined(__WIN32__)

		LPITEMIDLIST pidl;
		char home[MAX_PATH];
		home[0] = '\0';

		HRESULT hr = SHGetSpecialFolderLocation(NULL, CSIDL_APPDATA, &pidl);

		if (SUCCEEDED(hr)) {
			if (SHGetPathFromIDList(pidl, home)) {
				strcat(home, CAS_DIR_SEPARATOR "aMule");
			}
		}

		if (pidl) {
			LPMALLOC pMalloc;
			SHGetMalloc(&pMalloc);
			if (pMalloc) {
				IMalloc_Free(pMalloc, pidl);
				IMalloc_Release(pMalloc);
			}
		}

#else
		char *home;

		/* get home directory */
		if ((home = getenv("HOME")) == NULL) {
#ifndef CAS_UNIX
			return NULL;
#else
			/* if $HOME is not available try user database */
			uid_t uid;
			struct passwd *pwd;

			uid = getuid();
			pwd = getpwuid(uid);
			endpwent();

			/* XXX
			 * Section 6.5.14 of ANSI C specs (grab C99 at http://www.nirvani.net/docs/ansi_c.pdf)
			 * states this:
			 * "Unlike the bitwise | operator, the || operator _guarantees_ left-to-right
			 * evaluation; there is a sequence point after the evaluation of the first
			 * operand. If the first operand compares unequal to 0, the second operand
			 * _is not evaluated_."
			 *
			 * I'm going to revert it and wait until Jacobo or whoever changed it
			 * screams loudly, tries to kill me, and explains why this has to be
			 * this way. Maybe pre-ansi compilers? Doubtful.
			 *
			 * - Unleashed
			 */
			if (pwd == NULL || pwd->pw_dir == NULL)
				return NULL;

			home = pwd->pw_dir;
#endif /* CAS_UNIX */
		}
		/* Not strcat: home is the environment's own buffer. */
		saved_home = join_path(home, ".aMule");

#endif /* !__APPLE__ && !__WIN32__ */

#if defined(__APPLE__) || defined(__WIN32__)
		saved_home = strdup(home);
#endif
		/* kept for future calls */
		if (saved_home == NULL)
			return NULL;
	}

	return join_path(saved_home, file);
}

char *get_amule_path(const char *file, int force_directory, const char *cmdline_path)
{
	size_t len;

	if (!cmdline_path) {
		return get_path(file);
	}

	/* A directory gets the default name appended; a file path is used as is. */
	len = strlen(cmdline_path);
	if (force_directory || (len > 0 && cmdline_path[len - 1] == CAS_DIR_SEPARATOR[0])) {
		return join_path(cmdline_path, file);
	}
	return strdup(cmdline_path);
}

/*
 * this function is used to convert bytes to any other unit
 * nicer to the eye.
 *
 * return "Bad format" if could not convert string
 */
char *convbytes(char *input)
{
	char *units[] = { "Bytes", "KB", "MB", "GB", "TB", "PB" };
	char *endptr;
	static char output[50];
	float bytes;
	unsigned int i = 0;

	/* do proper conversion and check for errors */
	errno = 0;
	bytes = (float)strtod(input, &endptr);

	/* check bad string conversion or value out of range */
	if (*endptr != '\0' || errno == ERANGE)
		return "Bad format";

	/* this loop converts bytes and sets i to the appropriate
	 * index in 'units' array
	 * note: sizeof(units) / sizeof(char*) is the number of
	 * elements in 'units' array
	 */
	for (; i < (sizeof(units) / sizeof(char *)) - 1; i++) {
		if (bytes < 1024)
			break;
		bytes /= 1024;
	}

	snprintf(output, 50, "%.2f %s", bytes, units[i]);

	return output;
}

void replace(char *tmpl, const char *search, const char *to_replace)
{
	char *dest = NULL;
	char *retStr = NULL;

	/* returning the 'tmpl' if 'search' is NULL */
	if (NULL == tmpl || NULL == search) /* || NULL == to_replace) */
	{
		return;
	}

	while (1) {
		int befLen, srchLen, repLen, totLen;
		/* if 'search' is found in 'tmpl' */
		retStr = strstr(tmpl, search);
		if (NULL == retStr) {
			return;
		}

		totLen = strlen(tmpl);
		befLen = (int)(retStr - tmpl);
		srchLen = strlen(search);
		repLen = strlen(to_replace);

		/* dynamic buffer creation... */
		dest = (char *)malloc(totLen + 1 + repLen - srchLen);
		if (NULL == dest)
			return;

		/* copy the before buffer */
		strncpy(dest, tmpl, befLen);
		/* copy the replace string */
		memcpy((dest + befLen), to_replace, repLen); /* strcat(dest, to_replace); */
		/* copy the after buffer */
		memcpy((dest + befLen + repLen),
			&tmpl[befLen + srchLen],
			strlen(&tmpl[befLen + srchLen])); /*strcat(dest, &tmpl[befLen + repLen]); */

		/* now replace the template string with the resulting and search it again */
		strcpy(tmpl, dest);
		/* we need this, because we're modifying the 'tmpl' instead of creating a new one (so we need
		 * to update the position of the null char) */
		tmpl[totLen - srchLen + repLen] = '\0';
		/* clean up... */
		free(dest);
	}
}

char *timeconv(char *input)
{
	int count = atoi(input);
	static char ret[50];

	if (count < 0)
		snprintf(ret, 50, "?");
	else if (count < 60)
		snprintf(ret, 50, "%02i %s", count, "secs");
	else if (count < 3600)
		snprintf(ret, 50, "%i:%02i %s", count / 60, (count % 60), "mins");
	else if (count < 86400)
		snprintf(ret, 50, "%i:%02i %s", count / 3600, (count % 3600) / 60, "h");
	else
		snprintf(ret, 50, "%i %s %02i %s", count / 86400, "D", (count % 86400) / 3600, "h");

	return (ret);
}

/* Path of a file installed with cas (stat.png, tmp.html). Found from the
 * executable rather than a build-time prefix, so relocated installs work:
 * ../Resources/cas in the macOS bundle, ../share/cas elsewhere.
 * !! it's caller's responsibility to free return value
 */
char *get_data_path(const char *file)
{
	char exe[PATH_MAX];
	char *sep, *dir, *ret;
#if defined(__APPLE__)
	const char *rel = "../Resources/cas";
	uint32_t size = sizeof(exe);

	if (_NSGetExecutablePath(exe, &size) != 0)
		return NULL;
#elif defined(__WIN32__)
	const char *rel = "..\\share\\cas";
	DWORD len = GetModuleFileNameA(NULL, exe, sizeof(exe));

	if (len == 0 || len == sizeof(exe))
		return NULL;
#else
	const char *rel = "../share/cas";
	ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);

	if (len <= 0) {
		/* No /proc: fall back to the usual prefix. */
		strcpy(exe, "/usr/bin/cas");
	} else {
		exe[len] = '\0';
	}
#endif
	if ((sep = strrchr(exe, CAS_DIR_SEPARATOR[0])) == NULL)
		return NULL;
	*sep = '\0';

	if ((dir = join_path(exe, rel)) == NULL)
		return NULL;
	ret = join_path(dir, file);
	free(dir);
	return ret;
}

/* A TrueType font that exists on this system, for the default casrc.
 * !! it's caller's responsibility to free return value
 */
char *get_default_font(void)
{
#if defined(__APPLE__)
	return strdup("/System/Library/Fonts/Supplemental/Arial.ttf");
#elif defined(__WIN32__)
	const char *windir = getenv("WINDIR");
	char *ret;

	if (windir == NULL)
		windir = "C:\\Windows";
	ret = malloc(strlen(windir) + sizeof("\\Fonts\\arial.ttf"));
	if (ret != NULL)
		sprintf(ret, "%s\\Fonts\\arial.ttf", windir);
	return ret;
#else
	static const char *const fonts[] = {
		"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",   /* Debian, Ubuntu */
		"/usr/share/fonts/TTF/DejaVuSans.ttf",               /* Arch */
		"/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", /* Fedora */
		"/usr/share/fonts/dejavu/DejaVuSans.ttf",
		"/usr/share/fonts/corefonts/times.ttf",
	};
	size_t i;

	for (i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) {
		if (access(fonts[i], R_OK) == 0)
			return strdup(fonts[i]);
	}
	return strdup(fonts[0]);
#endif
}
