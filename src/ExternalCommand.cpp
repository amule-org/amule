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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
//

#include "ExternalCommand.h"
#include "AppImageEnv.h"
#include "HelperBinaryPath.h"
#include "TerminationProcess.h"

#include <algorithm>
#include <limits>

#include <wx/cmdline.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/intl.h>
#include <wx/utils.h>

namespace ExternalCommand
{

namespace
{
wxString NormalizedProgramPath(const wxString &path, Platform platform)
{
	wxString normalized = path;
	if (platform == Platform::Windows) {
		// Win32 treats trailing dots/spaces as aliases of the same file.
		while (normalized.EndsWith(".") || normalized.EndsWith(" ")) {
			normalized.RemoveLast();
		}
#ifdef __WINDOWS__
		// Expand existing 8.3 aliases before recognizing interpreters. Otherwise
		// a standard powers~1.exe path could bypass the PowerShell restriction.
		wxString candidate = normalized;
		if (!wxFileExists(candidate) && wxFileName(candidate).GetExt().empty()) {
			candidate += ".exe"; // CreateProcess appends this extension too
		}
		wxString resolved = candidate;
		if (!wxFileExists(resolved) && normalized.Find('\\') == wxNOT_FOUND &&
			normalized.Find('/') == wxNOT_FOUND) {
			wxString searchPath;
			if (wxGetEnv("PATH", &searchPath)) {
				wxFindFileInPath(&resolved, searchPath, candidate);
			}
		}
		if (wxFileExists(resolved)) {
			normalized = wxFileName(resolved).GetLongPath();
		}
#endif
	}
	return normalized;
}

wxString ProgramName(const wxString &path, Platform platform)
{
	const wxString normalized = NormalizedProgramPath(path, platform);
	// Recognize both separators, including Windows paths tested on POSIX.
	const int separator = std::max(normalized.Find('/', true), normalized.Find('\\', true));
	wxString name = normalized.Mid(separator + 1).Lower();
	if (name.EndsWith(".exe") || name.EndsWith(".com")) {
		name = name.Left(name.length() - 4);
	}
	return name;
}

bool IsPosixShell(const wxString &name)
{
	return name == "sh" || name == "bash" || name == "dash" || name == "ash" || name == "ksh" ||
	       name == "mksh" || name == "zsh";
}

bool IsInterpreter(const wxString &name)
{
	return IsPosixShell(name) || name == "fish" || name == "csh" || name == "tcsh" ||
	       name.StartsWith("python") || name == "py" || name == "perl" || name == "ruby" ||
	       name == "node" || name == "nodejs" || name == "php" || name == "lua";
}

bool IsWindowsCommandShell(const wxString &program, Platform platform)
{
	const wxString name = ProgramName(program, platform);
	const wxString lower = NormalizedProgramPath(program, platform).Lower();
	return platform == Platform::Windows &&
	       (name == "cmd" || lower.EndsWith(".bat") || lower.EndsWith(".cmd"));
}

bool IsWindowsBuiltin(const wxString &program)
{
	const wxString builtins =
		"|echo|echo.|break|cd|chdir|cls|color|copy|date|del|dir|erase|exit|assoc|ftype|md|mkdir|"
		"mklink|move|path|pause|popd|prompt|pushd|rd|ren|rename|rmdir|set|setlocal|endlocal|"
		"shift|time|title|type|ver|verify|vol|";
	return builtins.Contains("|" + program.Lower() + "|");
}

bool HasExplicitWindowsProgramExtension(const wxString &program)
{
	// Inspect the written token, not a PATH-resolved .exe. cmd also searches
	// PATHEXT and could select a batch file requiring different argument quoting.
	wxString name = program.Lower();
	while (name.EndsWith(".") || name.EndsWith(" ")) {
		name.RemoveLast();
	}
	return name.EndsWith(".exe") || name.EndsWith(".com") || name.EndsWith(".bat") ||
	       name.EndsWith(".cmd");
}

// Keep cmd's switches and first command token fixed. Support /c or /k, with
// common switches preceding it; the data arguments come after the fixed token.
size_t CmdCommandIndex(const wxArrayString &args)
{
	for (size_t i = 1; i < args.size(); ++i) {
		const wxString option = args[i].Lower();
		if (option == "/c" || option == "/k") {
			return i + 1;
		}
		if (option != "/d" && option != "/q" && option != "/a" && option != "/u" &&
			option != "/e:on" && option != "/e:off" && option != "/f:on" && option != "/f:off" &&
			option != "/v:on" && option != "/v:off") {
			break;
		}
	}
	return std::numeric_limits<size_t>::max();
}

bool IsProhibitedLauncher(const wxString &name)
{
	return name == "cmd" || name == "command" || name == "powershell" || name == "pwsh" ||
	       name == "wscript" || name == "cscript" || name == "mshta" || name == "rundll32" ||
	       name == "regsvr32" || name == "env" || name == "busybox";
}

// How many leading arguments must remain fixed? Use deliberately narrow forms
// for interpreters: a fixed script filename, or POSIX shell -c with fixed code.
// Recognized wrappers can hide another interpreter; reject expansion through them.
size_t ProtectedArguments(const wxArrayString &args, Platform platform)
{
	const wxString name = ProgramName(args[0], platform);
	const wxString executable = NormalizedProgramPath(args[0], platform).Lower();
	if (platform == Platform::Windows && name == "cmd") {
		const size_t commandIndex = CmdCommandIndex(args);
		if (commandIndex >= args.size()) {
			return std::numeric_limits<size_t>::max();
		}
		const wxString target = ProgramName(args[commandIndex], platform);
		// Nested interpreters can turn a later data argument into their code
		// argument. cmd dispatch builtins can also select or evaluate another
		// command from later arguments. Invoke supported programs directly.
		if (IsInterpreter(target) || IsProhibitedLauncher(target) || target == "call" ||
			target == "start" || target == "for" || target == "if") {
			return std::numeric_limits<size_t>::max();
		}
		return commandIndex + 1;
	}
	if (platform == Platform::Windows && (executable.EndsWith(".bat") || executable.EndsWith(".cmd"))) {
		return 1;
	}
	if (IsProhibitedLauncher(name)) {
		return std::numeric_limits<size_t>::max();
	}
	if (!IsInterpreter(name)) {
		return 1;
	}
	if (platform == Platform::Posix && IsPosixShell(name) && args.size() >= 3 && args[1] == "-c") {
		return 3;
	}
	if (args.size() >= 2 && !args[1].empty() && !args[1].StartsWith("-") && !args[1].StartsWith("+")) {
		return 2; // fixed script file; later arguments are data
	}
	return std::numeric_limits<size_t>::max(); // unsupported interpreter options: fail closed
}
RejectionReason ValidateValue(const wxString &value, bool commandShell)
{
	if (value.find(wxChar(0)) != wxString::npos) {
		return RejectionReason::EmbeddedNul;
	}
	if (commandShell && value.find_first_of("\"%!\r\n") != wxString::npos) {
		return RejectionReason::UnsafeWindowsShellValue;
	}
	return RejectionReason::None;
}
} // namespace

wxString DescribeRejection(RejectionReason reason)
{
	switch (reason) {
	case RejectionReason::None:
		return {};
	case RejectionReason::EmptyCommand:
		return _("The command has no executable.");
	case RejectionReason::EmbeddedNul:
		return _("The command template or an argument contains a NUL character.");
	case RejectionReason::ExecutableSubstitution:
		return _("An event value cannot select the executable.");
	case RejectionReason::InterpreterSubstitution:
		return _("An event value would be used as interpreter code, a script name, or an unsupported "
			 "launcher argument.");
	case RejectionReason::UnsafeWindowsShellValue:
		return _("A command-shell argument contains a double quote, percent sign, exclamation mark, "
			 "or line break.");
	case RejectionReason::UnsupportedWindowsCommand:
		return _("cmd.exe requires a fixed builtin or an explicit .exe, .com, .bat, or .cmd target "
			 "followed by separate data arguments.");
	}
	return {};
}

wxArrayString Build(const wxString &command,
	const std::vector<std::pair<wxString, wxString>> &values,
	bool *substituted,
	Platform platform,
	const wxString *fallbackArgument,
	RejectionReason *rejection)
{
	if (rejection != nullptr) {
		*rejection = RejectionReason::None;
	}
	const auto reject = [rejection](RejectionReason reason) {
		if (rejection != nullptr) {
			*rejection = reason;
		}
		return wxArrayString{};
	};
	if (substituted != nullptr) {
		*substituted = false;
	}
	if (command.find(wxChar(0)) != wxString::npos) {
		return reject(RejectionReason::EmbeddedNul);
	}
	wxArrayString args = wxCmdLineParser::ConvertStringToArgs(
		command, platform == Platform::Windows ? wxCMD_LINE_SPLIT_DOS : wxCMD_LINE_SPLIT_UNIX);
	if (args.IsEmpty() || args[0].empty()) {
		return reject(RejectionReason::EmptyCommand);
	}
	const size_t protectedArgs = ProtectedArguments(args, platform);
	const bool commandShell = IsWindowsCommandShell(args[0], platform);
	bool didSubstitute = false;
	for (size_t index = 0; index < args.size(); ++index) {
		wxString &arg = args[index];
		wxString expanded;
		for (size_t i = 0; i < arg.length();) {
			bool replaced = false;
			if (arg[i] == '%' || arg[i] == '$') {
				for (const auto &value : values) {
					if (!value.first.empty() &&
						arg.Mid(i, value.first.length()) == value.first) {
						if (index < protectedArgs) {
							return reject(
								index == 0 ? RejectionReason::
										     ExecutableSubstitution
									   : RejectionReason::
										     InterpreterSubstitution);
						}
						const RejectionReason reason =
							ValidateValue(value.second, commandShell);
						if (reason != RejectionReason::None) {
							return reject(reason);
						}
						didSubstitute = true;
						expanded += value.second;
						i += value.first.length();
						replaced = true;
						break;
					}
				}
			}
			if (!replaced) {
				expanded += arg[i++];
			}
		}
		// A literal prefix/suffix containing a quote or expansion marker would
		// undermine cmd quoting even if the inserted value itself is harmless.
		if (commandShell && expanded != arg) {
			const RejectionReason reason = ValidateValue(expanded, true);
			if (reason != RejectionReason::None) {
				return reject(reason);
			}
		}
		arg = expanded;
	}
	const bool appendFallback = !didSubstitute && fallbackArgument != nullptr;
	if ((didSubstitute || appendFallback) && commandShell) {
		for (const wxString &arg : args) {
			const RejectionReason reason = ValidateValue(arg, true);
			if (reason != RejectionReason::None) {
				return reject(reason);
			}
		}
	}
	if ((didSubstitute || appendFallback) && commandShell && ProgramName(args[0], platform) == "cmd") {
		const size_t index = CmdCommandIndex(args);
		// A quoted fixed executable path may contain spaces and parentheses.
		// Require an explicit extension for external targets so PATHEXT cannot
		// silently select a batch file while we serialize arguments for the CRT.
		if (index >= args.size() || args[index].empty() || args[index].StartsWith("/") ||
			args[index].StartsWith("-") ||
			(!IsWindowsBuiltin(args[index]) &&
				!HasExplicitWindowsProgramExtension(args[index])) ||
			args[index].find_first_of("\"%!\r\n&|<>^") != wxString::npos) {
			return reject(RejectionReason::UnsupportedWindowsCommand);
		}
	}
	if (appendFallback) {
		if (args.size() < protectedArgs) {
			return reject(RejectionReason::InterpreterSubstitution);
		}
		const RejectionReason reason = ValidateValue(*fallbackArgument, commandShell);
		if (reason != RejectionReason::None) {
			return reject(reason);
		}
		args.Add(*fallbackArgument);
	}
	if (substituted != nullptr) {
		*substituted = didSubstitute;
	}
	return args;
}

// Windows CreateProcess accepts a string, and wxWidgets 3.2/3.3.1's argv overload
// does not double backslashes before embedded quotes or the closing quote.
// Quote each argument using the Windows C runtime rules instead.
static wxString QuoteCrtArguments(const wxArrayString &args)
{
	wxString command;
	for (const wxString &arg : args) {
		if (!command.empty()) {
			command += ' ';
		}
		command += '"';
		size_t backslashes = 0;
		for (const wxUniChar ch : arg) {
			if (ch == '\\') {
				++backslashes;
				continue;
			}
			const size_t count = ch == '"' ? backslashes * 2 + 1 : backslashes;
			command += wxString('\\', count);
			command += ch;
			backslashes = 0;
		}
		command += wxString('\\', backslashes * 2);
		command += '"';
	}
	return command;
}

// cmd strips its outermost quotes. Give it a sacrificial outer pair so each
// argument's quotes survive, and use /s to make that stripping deterministic.
wxString BuildWindowsCommandLine(const wxArrayString &args, const wxString *unchangedTemplate)
{
	if (unchangedTemplate != nullptr) {
		return unchangedTemplate->find(wxChar(0)) == wxString::npos ? *unchangedTemplate : wxString{};
	}
	if (args.IsEmpty() || !IsWindowsCommandShell(args[0], Platform::Windows)) {
		return QuoteCrtArguments(args);
	}
	for (const wxString &arg : args) {
		if (ValidateValue(arg, true) != RejectionReason::None) {
			// Only fixed, user-authored templates can reach this path through Build.
			return QuoteCrtArguments(args);
		}
	}
	const bool cmd = ProgramName(args[0], Platform::Windows) == "cmd";
	const size_t first = cmd ? CmdCommandIndex(args) : 0;
	if (first >= args.size()) {
		return QuoteCrtArguments(args);
	}
	wxString shell;
	if (cmd) {
		shell = QuoteCrtArguments(wxArrayString{ args[0] }) + " /d /s";
		for (size_t i = 1; i < first; ++i) {
			shell += " " + args[i];
		}
	} else {
#ifdef __WINDOWS__
		// Avoid implicit batch dispatch and PATH/COMSPEC lookup: select the OS shell.
		shell = QuoteCrtArguments(wxArrayString{ wxGetOSDirectory() + "\\System32\\cmd.exe" });
#else
		shell = "\"cmd.exe\""; // portable serializer regression coverage
#endif
		shell += " /d /s /c";
	}
	const bool builtin = IsWindowsBuiltin(args[first]);
	const bool rawQuotes = IsWindowsCommandShell(args[first], Platform::Windows) || builtin;
	shell += " \"";
	for (size_t i = first; i < args.size(); ++i) {
		if (i != first) {
			shell += ' ';
		}
		// cmd/batch arguments use literal backslashes; native children still
		// need CRT escaping of backslashes before their closing quotes.
		if (i == first && builtin) {
			shell += args[i]; // fixed builtin token; cmd does not use CRT parsing
		} else {
			shell += rawQuotes ? "\"" + args[i] + "\""
					   : QuoteCrtArguments(wxArrayString{ args[i] });
		}
	}
	shell += '\"';
	return shell;
}

wxString ReplaceProgram(const wxString &commandLine, const wxString &program)
{
	size_t start = 0;
	while (start < commandLine.length() && (commandLine[start] == ' ' || commandLine[start] == '\t')) {
		++start;
	}
	size_t end = start;
	for (bool quoted = false; end < commandLine.length(); ++end) {
		const wxUniChar ch = commandLine[end];
		if (ch == '"') {
			quoted = !quoted;
		} else if (!quoted && (ch == ' ' || ch == '\t')) {
			break;
		}
	}
	return commandLine.Left(start) + "\"" + program + "\"" + commandLine.Mid(end);
}

#ifdef __WINDOWS__
wxString WithAbsoluteProgram(const wxString &commandLine)
{
	const wxArrayString args = wxCmdLineParser::ConvertStringToArgs(commandLine, wxCMD_LINE_SPLIT_DOS);
	const wxString program = args.IsEmpty() ? wxString() : FindProgram(args[0]);
	if (program.IsEmpty()) {
		return wxEmptyString;
	}
	return program == args[0] ? commandLine : ReplaceProgram(commandLine, program);
}
#endif

// Preserve non-ASCII arguments and use host libraries when running inside an AppImage.
bool RunDetached(const wxString &description, const wxArrayString &args, const wxString *unchangedTemplate)
{
	if (args.IsEmpty() || args[0].empty() ||
		(unchangedTemplate != nullptr && unchangedTemplate->find(wxChar(0)) != wxString::npos)) {
		return false;
	}
	// Embedded NULs would silently truncate argv entries or the Windows command
	// string, invalidating every argument-boundary check performed above.
	for (const wxString &arg : args) {
		if (arg.find(wxChar(0)) != wxString::npos) {
			return false;
		}
	}
#ifndef __WINDOWS__
	std::vector<wxWCharBuffer> buffers;
	std::vector<const wchar_t *> argv;
	buffers.reserve(args.size()); // no reallocation, so the pointers below stay valid
	argv.reserve(args.size() + 1);
	for (const wxString &arg : args) {
		buffers.emplace_back(arg.wc_str());
		argv.push_back(buffers.back().data());
	}
	argv.push_back(nullptr);
#else
	// CreateProcess would try a bare name in the current directory before PATH.
	const wxString program = FindProgram(args[0]);
	if (program.IsEmpty()) {
		return false;
	}
	wxArrayString resolved = args;
	resolved[0] = program;
	wxString resolvedTemplate;
	if (unchangedTemplate != nullptr && program != args[0]) {
		resolvedTemplate = ReplaceProgram(*unchangedTemplate, program);
		unchangedTemplate = &resolvedTemplate;
	}
#endif

	wxExecuteEnv execEnv;
	const bool sanitized = AppImageEnv::GetSanitizedExecEnv(execEnv);
	CTerminationProcess *process = new CTerminationProcess(description);
	long ret = 0;
	try {
#ifdef __WINDOWS__
		ret = wxExecute(BuildWindowsCommandLine(resolved, unchangedTemplate),
			wxEXEC_ASYNC,
			process,
			sanitized ? &execEnv : nullptr);
#else
		ret = wxExecute(argv.data(), wxEXEC_ASYNC, process, sanitized ? &execEnv : nullptr);
#endif
	} catch (...) {
		// wxExecute throws, rather than returning an error, when the environment it has to
		// hand the child is unusable -- a working directory that no longer exists, for one.
		// Uncaught it unwinds out of the menu handler and wx terminates the application
		// cleanly: no signal, no backtrace, no crash report.
		delete process;
		return false;
	}
	if (ret <= 0) {
		delete process;
		return false;
	}
	// True means the child was spawned, not that it did anything useful: an async wxExecute
	// returns the pid as soon as the fork succeeds, so a failed exec (no xdg-open installed,
	// say) is not reported here. macOS and Windows do surface a failure, since neither reaches
	// the desktop opener this way.
	return true;
}

} // namespace ExternalCommand
