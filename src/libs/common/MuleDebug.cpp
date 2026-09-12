//
// This file is part of the aMule Project.
//
// Copyright (c) 2005-2011 Mikkel Schubert ( xaignar@users.sourceforge.net )
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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
//

#include <cstdlib> // Needed for std::abort()
#include <cstdio>  // Needed for popen/pclose/fgets in the addr2line fallback
#include <cstring> // Needed for strlen in the addr2line fallback
#include <cerrno>  // Needed for EINTR in the SIGABRT handler's write loop
#include <csignal> // Needed for sigaction/raise in the SIGABRT handler
#include <cstdint> // uintptr_t for pointer subtraction in the bfd
		   // section-bounds check -- `unsigned long` is
		   // 4 bytes on LLP64 and would silently truncate.

#include "config.h" // Needed for HAVE_CXXABI and HAVE_EXECINFO

#include "MuleDebug.h" // Interface declaration
#include <string>      // Needed for the terminate report buffer

#include "StringFunctions.h" // Needed for unicode2char
#include "Format.h"          // Needed for CFormat

#ifndef __WINDOWS__
#include <fcntl.h>    // Needed for FD_CLOEXEC on the reserved descriptor
#include <poll.h>     // Needed for the writability waits in WriteAll and the handler
#include <sys/stat.h> // Needed for fstat in SameFile
#include <unistd.h>   // Needed for dup/dup2 in ReserveCrashFd and write in WriteAll
#endif

#ifdef HAVE_EXECINFO
#include <execinfo.h>
#include <poll.h>     // Needed for the writability probe in the SIGABRT handler
#include <sys/stat.h> // Needed for fstat in the SIGABRT handler
#include <unistd.h>   // Needed for write()/STDERR_FILENO in the SIGABRT handler
#include <wx/utils.h> // Needed for wxArrayString
#ifndef HAVE_BFD
#include <wx/thread.h> // Needed for wxThread
#endif
#endif

#ifdef HAVE_CXXABI
#ifdef HAVE_TYPEINFO
#include <typeinfo> // Needed for some MacOSX versions with broken system headers
#endif
#include <cxxabi.h>
#endif

#if wxUSE_STACKWALKER && defined(__WINDOWS__)
#include <wx/stackwalk.h> // Do_not_auto_remove
#elif defined(HAVE_BFD)
#include <ansidecl.h> // Do_not_auto_remove
#include <bfd.h>      // Do_not_auto_remove
#endif

#include <vector>
#include <exception>

// Defined below with the rest of the abort-redirect state.
static int AbortConsoleFd(int fallback);
#ifndef __WINDOWS__
static int AbortRedirectFd();
#endif
#ifndef __WINDOWS__
static void WriteAll(int fd, const char *data, size_t len);
static bool SameFile(int a, int b);
static bool FdCanTakeAWrite(int fd);
#endif

// One chunk of the terminate report, to every sink that wants it.
static void EmitTerminateChunk(const std::string &chunk, FILE *output)
{
#ifdef __WINDOWS__
	fputs(chunk.c_str(), output);
	fflush(output);
#else
	const int redirect = AbortRedirectFd();
	const int console = AbortConsoleFd(fileno(output));
	// Compared against the descriptor this actually writes to, not fd 2: a registered console
	// descriptor naming the same file as the log is a different number, and comparing the wrong
	// pair would put the report in there twice. The signal handler compares the same way.
	const bool haveDurable = redirect >= 0 && !SameFile(redirect, console);

	// The probe picks the order. A console that polls ready will not block on a chunk this size,
	// so taking it first costs nothing and guarantees something is emitted even if the durable
	// sink then hangs -- a logfile on a hard-mounted NFS share blocks in write(2), and being a
	// regular file it never returns EAGAIN, so neither the stall loop nor poll() can see it
	// coming. poll() reports every regular file ready, so the durable sink cannot be probed at
	// all; only the alarm bounds it.
	const bool consoleFirst = FdCanTakeAWrite(console);
	if (consoleFirst) {
		WriteAll(console, chunk.data(), chunk.size());
	}
	if (haveDurable) {
		WriteAll(redirect, chunk.data(), chunk.size());
	}
	// A console that failed the probe is skipped once a copy is out elsewhere. With no durable
	// sink it is all there is, so it goes unprobed and the alarm bounds it.
	if (!consoleFirst && !haveDurable) {
		WriteAll(console, chunk.data(), chunk.size());
	}
#endif
}

// Prints a verbose description of any unhandled exception, then raises
// SIGABRT.
void OnUnhandledException()
{
	// Revert to the original handler so a fault in here cannot recurse.
	std::set_terminate(std::abort);

#ifndef __WINDOWS__
	// Bounded like the SIGABRT handler. get_backtrace() popen()s addr2line on the non-BFD path,
	// and the only sink may be a descriptor that never drains; a process that reports nothing
	// and never exits is worse than one that dies.
	signal(SIGALRM, SIG_DFL);
	alarm(10);
#endif

#ifdef HAVE_CXXABI
	std::type_info *t = __cxxabiv1::__cxa_current_exception_type();
	FILE *output = stderr;
#else
	FILE *output = stdout;
	bool t = true;
#endif
	if (t) {
		int status = -1;
		char *dem = 0;
#ifdef HAVE_CXXABI
		// Note that "name" is the mangled name.
		char const *name = t->name();

		dem = __cxxabiv1::__cxa_demangle(name, 0, 0, &status);
#else
		const char *name = "Unknown";
#endif
		// The type and what() go out before the backtrace is built, not with it.
		// get_backtrace() symbolicates: it allocates throughout and on the non-BFD path
		// popen()s addr2line. If that subprocess hangs or the step faults, this much is
		// already reported, which is what master did by writing straight to unbuffered
		// stderr. Each chunk goes to both sinks; writing only to `output` while the
		// reserved descriptor was dup2()ed over fd 2 took the report off the console
		// entirely for a terminal-launched amule.
		std::string head("\nTerminated after throwing an instance of '");
		head += (status ? name : dem);
		head += "'\n";
		free(dem);

		try {
			throw;
		} catch (const std::exception &e) {
			head += "\twhat(): ";
			head += e.what();
			head += "\n";
		} catch (const CMuleException &e) {
			head += "\twhat(): ";
			head += (const char *)unicode2char(e.what());
			head += "\n";
		} catch (const wxString &e) {
			head += "\twhat(): ";
			head += (const char *)unicode2char(e);
			head += "\n";
		} catch (...) {
			// Unable to retrieve cause of exception
		}
		EmitTerminateChunk(head, output);

		std::string trace("\tbacktrace:\n");
		trace += (const char *)unicode2char(get_backtrace(1));
		trace += "\n";
		EmitTerminateChunk(trace, output);

		// Inside the branch on purpose. std::terminate() with no active exception prints
		// nothing at all, and suppressing there would trade the handler's raw backtrace for
		// silence. A joinable std::thread destroyed without join() reaches exactly that.
		SuppressNextAbortBacktrace();
	}
	std::abort();
}

void InstallMuleExceptionHandler()
{
	std::set_terminate(OnUnhandledException);
}

// Make it 1 for getting the file path also
#define TOO_VERBOSE_BACKTRACE 0

#if wxUSE_STACKWALKER && defined(__WINDOWS__)

// Derived class to define the actions to be done on frame print.
// I was tempted to name it MuleSkyWalker
class MuleStackWalker : public wxStackWalker
{
public:
	MuleStackWalker() {};
	~MuleStackWalker() {};

	void OnStackFrame(const wxStackFrame &frame)
	{
		wxString btLine = CFormat("[%u] ") % frame.GetLevel();
		wxString filename = frame.GetName();

		if (!filename.IsEmpty()) {
			btLine += filename + " (" +
#if TOO_VERBOSE_BACKTRACE
				  frame.GetModule()
#else
				  frame.GetModule().AfterLast('/')
#endif
				  + ")";
		} else {
			btLine += CFormat("%p") % frame.GetAddress();
		}

		if (frame.HasSourceLocation()) {
			btLine += " at " +
#if TOO_VERBOSE_BACKTRACE
				  frame.GetFileName()
#else
				  frame.GetFileName().AfterLast('/')
#endif
				  + CFormat(":%u") % frame.GetLine();
		} else {
			btLine += " (Unknown file/line)";
		}

		//! Contains the entire backtrace
		m_trace += btLine + "\n";
	}

	wxString m_trace;
};

wxString get_backtrace(unsigned n)
{
	MuleStackWalker walker; // Texas ranger?
	walker.Walk(n);         // Skip this one and Walk() also!

	return walker.m_trace;
}

#elif defined(__LINUX__)

// Do_not_auto_remove -- needed for dl_iterate_phdr on PIE-base lookup
#include <link.h> // IWYU pragma: keep

// PIE relocation offset for the main executable, subtracted from runtime
// backtrace addresses before they reach bfd or addr2line, which both expect
// link-time addresses. 0 for non-PIE. Populated lazily by init_pie_base().
static intptr_t s_pie_base = 0;
static bool s_pie_base_init = false;

static int find_pie_base_cb(struct dl_phdr_info *info, size_t /*size*/, void *data)
{
	// The main executable is dl_iterate_phdr's first callback and has an empty
	// dlpi_name. dlpi_addr is the load-time relocation: 0 for ET_EXEC.
	if (info->dlpi_name == NULL || info->dlpi_name[0] == '\0') {
		*reinterpret_cast<intptr_t *>(data) = static_cast<intptr_t>(info->dlpi_addr);
		return 1; // stop iteration
	}
	return 0;
}

static void init_pie_base()
{
	if (!s_pie_base_init) {
		dl_iterate_phdr(find_pie_base_cb, &s_pie_base);
		s_pie_base_init = true;
	}
}

#ifdef HAVE_BFD

static bfd *s_a_bfd;
static asymbol **s_symbol_list;
static bool s_have_backtrace_symbols = false;
static const char *s_file_name;
static const char *s_function_name;
static unsigned int s_line_number;
static int s_found;

/*
 * Read all symbols in the executable into an array, returning the count, or
 * -1 on error.
 */
static int get_backtrace_symbols(bfd *a_bfd, asymbol ***symbol_list_ptr)
{
	int vectorsize = bfd_get_symtab_upper_bound(a_bfd);

	if (vectorsize < 0) {
		fprintf(stderr,
			"Error while getting vector size for backtrace symbols : %s",
			bfd_errmsg(bfd_get_error()));
		return -1;
	}

	if (vectorsize == 0) {
		fprintf(stderr,
			"Error while getting backtrace symbols : No symbols (%s)",
			bfd_errmsg(bfd_get_error()));
		return -1;
	}

	*symbol_list_ptr = (asymbol **)malloc(vectorsize);

	if (*symbol_list_ptr == NULL) {
		fprintf(stderr, "Error while getting backtrace symbols : Cannot allocate memory");
		return -1;
	}

	vectorsize = bfd_canonicalize_symtab(a_bfd, *symbol_list_ptr);

	if (vectorsize < 0) {
		fprintf(stderr, "Error while getting symbol table : %s", bfd_errmsg(bfd_get_error()));
		return -1;
	}

	return vectorsize;
}

/*
 * Set file, line and function information for an address into globals.
 * Called from the bfd_map_over_sections iterator.
 */
void init_backtrace_info()
{
	bfd_init();
	s_a_bfd = bfd_openr("/proc/self/exe", NULL);

	if (s_a_bfd == NULL) {
		fprintf(stderr,
			"Error while opening file for backtrace symbols : %s",
			bfd_errmsg(bfd_get_error()));
		return;
	}

	if (!(bfd_check_format_matches(s_a_bfd, bfd_object, NULL))) {
		fprintf(stderr, "Error while init. backtrace symbols : %s", bfd_errmsg(bfd_get_error()));
		bfd_close(s_a_bfd);
		return;
	}

	s_have_backtrace_symbols = (get_backtrace_symbols(s_a_bfd, &s_symbol_list) > 0);

	// Same PIE-offset lookup the addr2line fallback uses: untranslated PCs fail
	// bfd's section-bounds check and every amule frame symbolicates to "??" on
	// PIE-by-default distros. init_pie_base() is idempotent.
	init_pie_base();
}

void get_file_line_info(bfd *a_bfd, asection *section, void *_address)
{
	wxASSERT(s_symbol_list);

	if (s_found) {
		return;
	}

	if ((section->flags & SEC_ALLOC) == 0) {
		return;
	}

	bfd_vma vma = section->vma;

	// Translate the runtime PC back to a link-time address so it lines up with
	// bfd's section vmas. 0 on non-PIE, so a no-op there. Library frames land
	// far outside amule's range either way, and the bounds check below still
	// skips them.
	uintptr_t address = (uintptr_t)_address - s_pie_base;
	if (address < vma) {
		return;
	}

	bfd_size_type size = section->size;
	if (address > (vma + size)) {
		return;
	}

	s_found = bfd_find_nearest_line(
		a_bfd, section, s_symbol_list, address - vma, &s_file_name, &s_function_name, &s_line_number);
}

#endif // HAVE_BFD

static wxString demangle(const wxString &function)
{
#ifdef HAVE_CXXABI
	wxString result;

	if (function.Mid(0, 2) == "_Z") {
		int status;
		char *demangled = abi::__cxa_demangle(function.mb_str(), NULL, NULL, &status);

		if (!status) {
			result = wxConvLibc.cMB2WX(demangled);
		}

		if (demangled) {
			free(demangled);
		}
	}

	return result;
#else
	return "";
#endif
}

// Print a stack backtrace if available
wxString get_backtrace(unsigned n)
{
#ifdef HAVE_EXECINFO
	// (stkn) create backtrace
	void *bt_array[100]; // 100 should be enough ?!?
	char **bt_strings;
	int num_entries;

	if ((num_entries = backtrace(bt_array, 100)) < 0) {
		fprintf(stderr, "* Could not generate backtrace\n");
		return "";
	}

	if ((bt_strings = backtrace_symbols(bt_array, num_entries)) == NULL) {
		fprintf(stderr, "* Could not get symbol names for backtrace\n");
		return "";
	}

	std::vector<wxString> libname(num_entries);
	std::vector<wxString> funcname(num_entries);
	std::vector<wxString> address(num_entries);
	wxString AllAddresses;

	for (int i = 0; i < num_entries; ++i) {
		wxString wxBtString = wxConvLibc.cMB2WX(bt_strings[i]);
		int posLPar = wxBtString.Find('(');
		int posRPar = wxBtString.Find(')');
		int posLBra = wxBtString.Find('[');
		int posRBra = wxBtString.Find(']');
		bool hasFunction = true;
		if (posLPar == -1 || posRPar == -1) {
			if (posLBra == -1 || posRBra == -1) {
				/* It is important to have exactly num_entries
				 * addresses in AllAddresses */
				AllAddresses += "0x0000000 ";
				continue;
			}
			posLPar = posLBra;
			hasFunction = false;
		}
		/* Library name */
		int len = posLPar;
		libname[i] = wxBtString.Mid(0, len);
		/* Function name */
		if (hasFunction) {
			int posPlus = wxBtString.Find('+', true);
			if (posPlus == -1)
				posPlus = posRPar;
			len = posPlus - posLPar - 1;
			funcname[i] = wxBtString.Mid(posLPar + 1, len);
			wxString demangled = demangle(funcname[i]);
			if (!demangled.IsEmpty()) {
				funcname[i] = demangled;
			}
		}
		/* Address */
		if (posLBra == -1 || posRBra == -1) {
			address[i] = "0x0000000";
		} else {
			len = posRBra - posLBra - 1;
			address[i] = wxBtString.Mid(posLBra + 1, len);
			AllAddresses += address[i] + " ";
		}
	}
	// bt_strings is backtrace_symbols()'s char** block; passing it through
	// free()'s void* is the documented idiom.
	// NOLINTNEXTLINE(bugprone-multi-level-implicit-pointer-conversion)
	free(bt_strings);

	/* Get line numbers from addresses */
	wxArrayString out;
	bool hasLineNumberInfo = false;

#ifdef HAVE_BFD
	if (!s_have_backtrace_symbols) {
		init_backtrace_info();
		wxASSERT(s_have_backtrace_symbols);
	}

	for (int i = 0; i < num_entries; ++i) {
		s_file_name = NULL;
		s_function_name = NULL;
		s_line_number = 0;
		s_found = false;

		unsigned long addr;
		address[i].ToULong(
			&addr, 0); // As it's "0x" prepended, wx will read it as base 16. Hopefully.

		bfd_map_over_sections(s_a_bfd, get_file_line_info, (void *)addr);

		if (s_found) {
			wxString function = wxConvLibc.cMB2WX(s_function_name);
			wxString demangled = demangle(function);
			if (!demangled.IsEmpty()) {
				function = demangled;
				funcname[i] = demangled;
			}
			out.Insert(wxConvLibc.cMB2WX(s_function_name), i * 2);
			out.Insert(
				CFormat("%s:%u") % wxString(wxConvLibc.cMB2WX(s_file_name)) % s_line_number,
				i * 2 + 1);
		} else {
			out.Insert("??", i * 2);
			out.Insert("??", i * 2 + 1);
		}
	}

	hasLineNumberInfo = true;

#else /* !HAVE_BFD */
	if (wxThread::IsMain()) {
		// Translate runtime PCs to link-time addresses first, or PIE binaries
		// return "??" for every amule frame -- the bug #677 fixed on the bfd
		// path, exposed again here. A no-op for non-PIE.
		init_pie_base();
		wxString translatedAddresses;
		for (int i = 0; i < num_entries; ++i) {
			unsigned long addr = 0;
			address[i].ToULong(&addr, 0);
			translatedAddresses +=
				CFormat("0x%lx ") % static_cast<unsigned long>(addr - s_pie_base);
		}

		wxString command;
		command << "addr2line -C -f -s -e /proc/" << getpid() << "/exe " << translatedAddresses;
		// Even elements of the output are function names, odd ones line numbers.

		// popen() rather than wxExecute(): on Linux the GUI wxExecute waits via
		// wxGUIAppTraits::WaitForChild, which runs a nested wx event loop. If the
		// assert that brought us here came from a periodic handler, that loop
		// fires the same handler again and the second-level assert handler falls
		// through to wxTrap()'s int3. popen() is fork+waitpid with no wx in it.
		FILE *pipe = popen((const char *)command.mb_str(), "r");
		if (pipe) {
			char line[1024];
			while (fgets(line, sizeof(line), pipe)) {
				size_t len = std::strlen(line);
				while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
					line[--len] = '\0';
				}
				out.Add(wxConvLibc.cMB2WX(line));
			}
			hasLineNumberInfo = (pclose(pipe) != -1);
		} else {
			hasLineNumberInfo = false;
		}
	}

#endif /* HAVE_BFD / !HAVE_BFD */

	wxString trace;
	// Remove 'n+1' first entries (+1 because of this function)
	for (int i = n + 1; i < num_entries; ++i) {
		/* If we have no function name, use the result from addr2line */
		if (funcname[i].IsEmpty()) {
			if (hasLineNumberInfo) {
				funcname[i] = out[2 * i];
			} else {
				funcname[i] = "??";
			}
		}
		wxString btLine;
		btLine << "[" << i << "] " << funcname[i] << " in ";
		/* If addr2line did not find a line number, use bt_string */
		if (!hasLineNumberInfo || out[2 * i + 1].Mid(0, 2) == "??") {
			btLine += libname[i] + "[" + address[i] + "]";
		} else if (hasLineNumberInfo) {
#if TOO_VERBOSE_BACKTRACE
			btLine += out[2 * i + 1];
#else
			btLine += out[2 * i + 1].AfterLast('/');
#endif
		} else {
			btLine += libname[i];
		}

		trace += btLine + "\n";
	}

	return trace;
#else  /* !HAVE_EXECINFO */
	fprintf(stderr, "--== cannot generate backtrace ==--\n\n");
	return "";
#endif /* HAVE_EXECINFO */
}

#elif defined(__APPLE__)

// According to sources, parts of this code originate at
// http://www.tlug.org.za/wiki/index.php/Obtaining_a_stack_trace_in_C_upon_SIGSEGV which doesn't exist
// anymore.

// Other code (stack frame list and demangle related) has been modified from code with license:

// Copyright 2007 Edd Dawson.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#include <string>
#include <dlfcn.h>
#include <cxxabi.h>
#include <sstream>

class stack_frame
{
public:
	stack_frame(const void *f_instruction, const std::string &f_function)
	: frame_instruction(f_instruction)
	, frame_function(f_function) {};

	const void *instruction() const { return frame_instruction; }
	const std::string &function() const { return frame_function; }

private:
	const void *frame_instruction;
	const std::string frame_function;
};

std::string demangle(const char *name)
{
	int status = 0;
	char *d = 0;
	std::string ret = name;
	try {
		if ((d = abi::__cxa_demangle(name, 0, 0, &status))) {
			ret = d;
		}
	} catch (...) {
	}

	std::free(d);
	return ret;
}

void fill_frames(std::list<stack_frame> &frames)
{
	try {
		void **fp = (void **)__builtin_frame_address(1);
		void *saved_pc = NULL;

		// First frame is skipped
		while (fp != NULL) {
			fp = (void **)(*fp);
			if (*fp == NULL) {
				break;
			}

#if defined(__i386__)
			saved_pc = fp[1];
#elif defined(__ppc__)
			saved_pc = *(fp + 2);
#else
			// ?
			saved_pc = *(fp + 2);
#endif
			if (saved_pc) {
				Dl_info info;

				if (dladdr(saved_pc, &info)) {
					frames.push_back(stack_frame(saved_pc,
						demangle(info.dli_sname) + " in " + info.dli_fname));
				}
			}
		}
	} catch (...) {
		// Nothing to be done here, just leave.
	}
}

wxString get_backtrace(unsigned n)
{
	std::list<stack_frame> frames;
	fill_frames(frames);
	std::ostringstream backtrace;
	std::list<stack_frame>::iterator it = frames.begin();

	int count = 0;
	while (it != frames.end()) {
		if (count >= n) {
			backtrace << (*it).instruction() << " : " << (*it).function() << std::endl;
			++it;
		}

		++count;
	}

	return wxString(backtrace.str().c_str(), wxConvUTF8);
}

#else /* ! __APPLE__ */

wxString get_backtrace(unsigned WXUNUSED(n))
{
	return "--== no BACKTRACE for your platform ==--\n\n";
}

#endif /* !__LINUX__ */

void print_backtrace(unsigned n)
{
	wxString trace = get_backtrace(n);

	// This is because the string is ansi anyway, and the conv classes are very slow
	fprintf(stderr, "%s\n", (const char *)unicode2char(trace));
}

// Set by SuppressNextAbortBacktrace(). sig_atomic_t because the handler reads it; volatile because
// the write and the read are in different control flows and nothing else orders them.
static volatile sig_atomic_t s_suppressAbortBacktrace = 0;

void SuppressNextAbortBacktrace()
{
	s_suppressAbortBacktrace = 1;
}

// -1 until a caller redirects; read by the handler, so sig_atomic_t rather than int.
static volatile sig_atomic_t s_abortRedirectFd = -1;

// Where the console copy goes when fd 2 is not it; see the header.
static volatile sig_atomic_t s_abortConsoleFd = -1;

// Written once before any crash, read from a handler, so a fixed buffer rather than a wxString.
// The length is kept with it: strlen() is not on the async-signal-safe list.
static char s_versionLine[256] = { 0 };
static volatile sig_atomic_t s_versionLineLen = 0;

void SetFatalAbortRedirectFd(int fd)
{
	s_abortRedirectFd = fd;
}

void SetFatalAbortConsoleFd(int fd)
{
	s_abortConsoleFd = fd;
}

void SetFatalAbortVersionLine(const char *text)
{
	if (text == nullptr) {
		s_versionLine[0] = '\0';
		s_versionLineLen = 0;
		return;
	}
	const int written = std::snprintf(s_versionLine, sizeof(s_versionLine), "%s\n", text);
	if (written < 0) {
		// Encoding failure: nothing was written, so the buffer still holds whatever it held.
		// Reporting a length here would paste uninitialised bytes into a public issue.
		s_versionLine[0] = '\0';
		s_versionLineLen = 0;
		return;
	}
	if (static_cast<size_t>(written) < sizeof(s_versionLine)) {
		s_versionLineLen = written;
		return;
	}
	// Truncated. The character lost is the trailing newline, which would glue the first
	// backtrace frame onto the version line, so put it back over the last byte.
	s_versionLine[sizeof(s_versionLine) - 2] = '\n';
	s_versionLineLen = static_cast<int>(sizeof(s_versionLine) - 1);
}

#ifndef __WINDOWS__
static int AbortRedirectFd()
{
	return s_abortRedirectFd;
}
#endif

// Whether the console is worth a copy. With no redirect it is the only sink there is. With one, the
// caller decides: amuleapi passes stderrUsable = false because its fd 2 is the tee pipe, and that
// pipe's pump writes into the very log the reserved descriptor names, so a console copy would land
// there twice. SameFile() cannot catch that, since it compares a pipe against a file.
// The console copy's destination: the descriptor a caller handed over, else fd 2 itself.
static int AbortConsoleFd(int fallback)
{
	return s_abortConsoleFd >= 0 ? s_abortConsoleFd : fallback;
}

#ifndef __WINDOWS__
// write(2) can return short, and returns EINTR if a signal lands mid-call. Looping is what makes
// the banner arrive whole; a partial one splices into whatever glibc printed and reads as garbage.
static void WriteAll(int fd, const char *data, size_t len)
{
	// Waiting out a sink that is merely busy, bounded so a handler cannot sit here forever. A
	// second is far longer than a report needs and far shorter than the alarm.
	const int kStallMs = 50;
	const int kMaxStalls = 20;

	int stalls = 0;
	while (len > 0) {
		const ssize_t written = write(fd, data, len);
		if (written > 0) {
			data += written;
			len -= static_cast<size_t>(written);
			continue;
		}
		if (written < 0 && errno == EINTR) {
			continue;
		}
		if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			// Not gone, just not ready. This is reachable: poll(POLLOUT) on a pipe
			// promises only PIPE_BUF, and a 64-frame backtrace is bigger than that, so
			// a non-blocking sink goes short partway through a report that had already
			// passed the probe. Returning here would truncate it mid-line.
			if (++stalls > kMaxStalls) {
				return;
			}
			struct pollfd wait;
			wait.fd = fd;
			wait.events = POLLOUT;
			wait.revents = 0;
			(void)poll(&wait, 1, kStallMs);
			continue;
		}
		return; // EBADF, EPIPE and friends: there really is nowhere left to report this
	}
}

// Whether a descriptor can take a write right now. A wedged journald socket or a pipe whose reader
// has stopped answers no, and a reporting path with a copy already out elsewhere skips it rather
// than blocking. poll() is on the async-signal-safe list.
static bool FdCanTakeAWrite(int fd)
{
	// A budget rather than a poll of zero. In a container without a tty fd 2 is a pipe, and a
	// reader that is merely behind would otherwise cost the `docker logs` copy on the instant
	// the buffer happens to be full. Long enough to ride out a busy reader, short enough that a
	// genuinely wedged sink is still skipped well inside the alarm.
	const int kProbeMs = 100;

	struct pollfd probe;
	probe.fd = fd;
	probe.events = POLLOUT;
	probe.revents = 0;
	return poll(&probe, 1, kProbeMs) == 1 && (probe.revents & POLLOUT) != 0;
}

// Two descriptors naming one file, so a report written to both would arrive twice. fstat() is on
// the async-signal-safe list, which matters for the caller that runs from a signal handler.
static bool SameFile(int a, int b)
{
	struct stat sa;
	struct stat sb;
	return fstat(a, &sa) == 0 && fstat(b, &sb) == 0 && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}
#endif

int ReserveCrashFd(int reserved, std::FILE *fp)
{
#ifdef __WINDOWS__
	// An open handle blocks rename() on Windows, and the SIGABRT handler this feeds never runs
	// there: wx goes through SEH and ExitProcess() instead. Reserve nothing.
	(void)fp;
	return reserved;
#else
	if (fp == nullptr) {
		return reserved;
	}
	const int fd = fileno(fp);
	if (fd < 0) {
		return reserved;
	}
	// FD_CLOEXEC on every path: this descriptor is deliberately never closed, so without it the
	// logfile would be held open by every child aMule spawns (wxExecute in FileLaunch and
	// friends). dup() and dup2() both clear the flag, so it is set again after each.
	if (reserved < 0) {
		const int dupped = dup(fd);
		if (dupped >= 0) {
			(void)fcntl(dupped, F_SETFD, FD_CLOEXEC);
		}
		return dupped;
	}
	// dup2() onto the number already handed out, so the handler's copy follows the reopen. A
	// failure leaves it on the previous file, which is still better than a closed descriptor.
	(void)dup2(fd, reserved);
	(void)fcntl(reserved, F_SETFD, FD_CLOEXEC);
	return reserved;
#endif
}

#ifdef HAVE_EXECINFO

// sizeof - 1 rather than strlen(): strlen is not on the async-signal-safe list, and for a literal
// the length is known at compile time anyway.
#define WRITE_LITERAL(fd, lit) WriteAll((fd), (lit), sizeof(lit) - 1)

static void WriteAbortReport(int fd, void *const *frames, int count)
{
	WRITE_LITERAL(fd,
		"\n-------------------------=| ABORT BACKTRACE FOLLOWS |=-------------------------\n"
		"aMule was aborted (SIGABRT). If this followed a glibc allocator message such as\n"
		"'double free or corruption', the frames below are where the damage was NOTICED,\n"
		"not where it was caused. Please report them at\n"
		"    https://github.com/amule-org/amule/issues\n\n");
	// Which build produced this. Pasted reports are useless without it, and a handler cannot
	// format one, so it was written into a fixed buffer before the crash.
	if (s_versionLineLen > 0) {
		WriteAll(fd, s_versionLine, static_cast<size_t>(s_versionLineLen));
	}
	if (count > 0) {
		backtrace_symbols_fd(frames, count, fd);
	} else {
		WRITE_LITERAL(fd, "* Could not generate backtrace\n");
	}
	WRITE_LITERAL(
		fd, "-------------------------------------------------------------------------------\n");
}

extern "C" void MuleFatalAbortHandler(int /*sig*/)
{
	// Bound the handler before doing anything that can block. backtrace() unwinds through
	// dl_iterate_phdr(), which takes the loader's load lock, so an abort that interrupted
	// another thread inside dlopen()/dlclose() would wait here forever. A process that hangs is
	// worse than one that dies without a trace: a supervisor watching for a non-zero exit never
	// restarts it. The install-time warm-up removes the dlopen this handler would do itself, not
	// a lock some other thread already holds. SIG_DFL first, in case something installed a
	// SIGALRM handler; both calls are async-signal-safe.
	signal(SIGALRM, SIG_DFL);
	alarm(10);

	// Clear it here so the name stays true: the suppression covers this abort only.
	const bool suppressed = s_suppressAbortBacktrace != 0;
	s_suppressAbortBacktrace = 0;

	if (!suppressed) {
		// 64 frames: the deepest real trace on amule-org/amule#1338 was 31, and the array is
		// on the handler's stack, which is the process stack here rather than a sigaltstack.
		void *frames[64];
		const int count = backtrace(frames, 64);

		// Both sinks, the way EmergencyLog() already reports the SIGSEGV path: the file is
		// the copy that survives a daemon, the console is the one an operator reads out of
		// `docker logs`. Written to the descriptors directly rather than dup2()ed over fd 2,
		// so neither sink costs the other. Ordering as in EmitTerminateChunk: whichever sink
		// can be probed goes first, because the other one cannot be.
		const int redirect = s_abortRedirectFd;
		const int console = AbortConsoleFd(STDERR_FILENO);
		const bool haveDurable = redirect >= 0 && !SameFile(redirect, console);
		const bool consoleFirst = FdCanTakeAWrite(console);

		if (consoleFirst) {
			WriteAbortReport(console, frames, count);
		}
		if (haveDurable) {
			WriteAbortReport(redirect, frames, count);
		}
		if (!consoleFirst && !haveDurable) {
			WriteAbortReport(console, frames, count);
		}
	}

	// SA_RESETHAND has already put SIG_DFL back, so this terminates. Raising rather than
	// returning covers the delivery paths that are not abort(): a plain `kill -ABRT` would
	// otherwise be swallowed and the process would carry on.
	raise(SIGABRT);
}

void InstallFatalAbortHandler()
{
	// glibc's backtrace() dlopen()s libgcc's unwinder on first use, which allocates. Doing that
	// for the first time inside the handler is exactly what this design exists to avoid, so
	// force the lazy init now, while the heap is still sound.
	//
	// Load-bearing, not belt and braces: measured on glibc 2.43 against a smashed top-chunk
	// size, a handler without this warm-up printed nothing at all -- backtrace() reached the
	// corruption through its own first-use allocation and the process died mid-report. The same
	// handler with the warm-up printed the full trace.
	void *warmup[1];
	(void)backtrace(warmup, 1);

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = MuleFatalAbortHandler;
	sigemptyset(&sa.sa_mask);
	// SA_RESETHAND so a fault inside the handler cannot loop back into it.
	sa.sa_flags = SA_RESETHAND;
	sigaction(SIGABRT, &sa, nullptr);
}

#else /* !HAVE_EXECINFO */

void InstallFatalAbortHandler()
{
	// Nothing to print without backtrace(); leave the default disposition alone.
}

#endif /* HAVE_EXECINFO */

// File_checked_for_headers
