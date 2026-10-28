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

#ifndef EXTERNALCOMMAND_H
#define EXTERNALCOMMAND_H

#include <wx/arrstr.h>
#include <functional>
#include <utility>
#include <vector>

namespace ExternalCommand
{
enum class Platform
{
	Posix,
	Windows
};
#ifdef __WINDOWS__
constexpr Platform NativePlatform = Platform::Windows;
#else
constexpr Platform NativePlatform = Platform::Posix;
#endif

enum class RejectionReason
{
	None,
	EmptyCommand,
	EmbeddedNul,
	ExecutableSubstitution,
	InterpreterSubstitution,
	UnsafeWindowsShellValue,
	UnsupportedWindowsCommand
};

// Human-readable explanation for a validation refusal, separate from spawn errors.
wxString DescribeRejection(RejectionReason reason);

// Parse before expansion. Reject substituted executables, embedded NULs, unsafe
// interpreter positions, and unsafe values for Windows command-shell targets. The platform can
// be selected explicitly to validate both policies on either build host. If no
// placeholder is used, fallbackArgument is validated as data and appended.
wxArrayString Build(const wxString &command,
	const std::vector<std::pair<wxString, wxString>> &values,
	bool *substituted = nullptr,
	Platform platform = NativePlatform,
	const wxString *fallbackArgument = nullptr,
	RejectionReason *rejection = nullptr);

// Use CRT quoting for native programs, and explicit outer quoting for cmd/batch.
// Supply unchangedTemplate only when Build inserted no values or fallback argument.
// Such fixed Windows commands preserve their original quoting and shell syntax.
wxString BuildWindowsCommandLine(const wxArrayString &args, const wxString *unchangedTemplate = nullptr);

// `commandLine` with its first token, a Windows command line's program, replaced by `program`,
// quoted. The rest is kept as written.
wxString ReplaceProgram(const wxString &commandLine, const wxString &program);

// `command` with an unquoted absolute Windows program path that contains blanks put in quotes.
// Like CreateProcess, the program is the shortest run of words that names a file, ".exe" implied.
// Unchanged if the first word is already a file, or no run is.
wxString QuoteUnquotedProgram(const wxString &command, const std::function<bool(const wxString &)> &isFile);

#ifdef __WINDOWS__
// `commandLine` with a bare program name replaced by its absolute path (see FindProgram), or empty
// if it is not found.
wxString WithAbsoluteProgram(const wxString &commandLine);
#endif

// Spawn asynchronously using the AppImage-safe environment. False means no child
// was spawned; successful spawning does not establish successful child execution.
bool RunDetached(
	const wxString &description, const wxArrayString &args, const wxString *unchangedTemplate = nullptr);
} // namespace ExternalCommand

#endif // EXTERNALCOMMAND_H
