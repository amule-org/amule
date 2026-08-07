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

#include "FileLaunch.h" // Interface declarations

#include <vector> // Needed for std::vector

#include <wx/cmdline.h>  // Needed for wxCmdLineParser::ConvertStringToArgs
#include <wx/filename.h> // Needed for wxFileName::IsFileExecutable
#include <wx/msgdlg.h>   // Needed for wxMessageBox
#include <wx/utils.h>    // Needed for wxExecute, wxLaunchDefaultApplication

#include <common/Format.h> // Needed for CFormat

#ifdef __WXMAC__
#include "MacAppHelper.h" // Needed for mac_reveal_in_finder
#endif

#include "AppImageEnv.h"        // Needed for GetSanitizedExecEnv
#include "KnownFile.h"          // Needed for CKnownFile
#include "Logger.h"             // Needed for AddLogLineC
#include "OtherFunctions.h"     // Needed for GetFiletype
#include "PartFile.h"           // Needed for CPartFile
#include "Preferences.h"        // Needed for thePrefs
#include "TerminationProcess.h" // Needed for CTerminationProcess

namespace
{

// Hand an argument vector to the desktop, with the AppImage-safe environment
// when we are inside a bundle: AppRun prepends the bundle's lib/bin directories
// to the child's search paths, and a host program that inherits them loads our
// older bundled libraries instead of the system ones and dies on an undefined
// symbol (#334). Outside an AppImage this is a no-op and the child inherits as
// usual.
//
// A vector rather than a command string on purpose: the path crosses untouched,
// so a name with spaces or quotes needs no escaping and cannot be re-split by
// the shell-style parser wxExecute applies to strings. The wide overload keeps
// non-ASCII names intact, which mb_str() would mangle under a C locale.
bool RunDetached(const wxString &description, const std::vector<wxString> &args)
{
	std::vector<wxWCharBuffer> buffers;
	std::vector<const wchar_t *> argv;
	buffers.reserve(args.size()); // no reallocation, so the pointers below stay valid
	argv.reserve(args.size() + 1);
	for (const wxString &arg : args) {
		buffers.emplace_back(arg.wc_str());
		argv.push_back(buffers.back().data());
	}
	argv.push_back(nullptr);

	CTerminationProcess *process = new CTerminationProcess(description);
	wxExecuteEnv execEnv;
	const bool sanitized = AppImageEnv::GetSanitizedExecEnv(execEnv);
	long ret = 0;
	try {
		ret = wxExecute(argv.data(), wxEXEC_ASYNC, process, sanitized ? &execEnv : nullptr);
	} catch (...) {
		// wxExecute throws, rather than returning an error, when the environment
		// it has to hand the child is unusable -- a working directory that no
		// longer exists, for one. Uncaught it unwinds out of the menu handler and
		// wx terminates the application: a *clean* exit, so no signal, no
		// backtrace and no crash report, which makes it near-undiagnosable. A
		// failed launch is not worth losing the session over.
		delete process;
		return false;
	}
	if (ret <= 0) {
		delete process;
		return false;
	}
	// True means the child was spawned, not that it did anything useful: an
	// async wxExecute returns the pid as soon as the fork succeeds, so a failed
	// exec (no xdg-open installed, say) is not reported here. macOS and Windows
	// do surface a failure, because neither goes through this path for the
	// desktop opener.
	return true;
}

// Log a launch failure, and put it in front of the user when the caller has no
// second option to fall through to.
void Fail(const wxString &message, wxWindow *parent, bool reportModally)
{
	AddLogLineC(message);
	if (reportModally && parent != nullptr) {
		wxMessageBox(message, _("File preview"), wxOK | wxICON_EXCLAMATION, parent);
	}
}

// Hand the path to the user's configured player.
//
// The template is split into arguments ONCE, with wxCmdLineParser, and the
// placeholders are then substituted inside the resulting arguments -- so the
// path and the bare name each cross as a single argv entry no matter what they
// contain. The old code appended the path to a command string and let wxExecute
// re-split it, which meant a remote-supplied filename could inject arguments:
// `x\' --script=/tmp/evil.lua \'.avi` becomes a separate --script argument, and
// players like mpv and vlc execute scripts given that way. Escaping the quotes
// was the previous defence and was itself bypassable with a backslash; not
// building a string in the first place removes the class.
// `reportModally` is for the caller that has nothing to fall back to: a failure
// it only logs is, from the user's side, the same silent nothing this is meant
// to remove. The caller that can fall back leaves it false and stays quiet.
bool LaunchWithPlayer(const wxString &player, const CPath &path, wxWindow *parent, bool reportModally)
{
	const wxString target = path.GetRaw();
	const wxString name = path.GetFullName().GetRaw();

	// ConvertStringToArgs defaults to DOS rules, which is wrong everywhere but
	// Windows: it leaves single quotes literal and does not honour backslash
	// escapes, so an existing `mpv -fs '%PARTFILE'` or a path with an escaped
	// space silently stops working. Windows genuinely needs DOS, since UNIX
	// rules would eat the backslashes in C:\Program Files\...
#ifdef __WINDOWS__
	wxArrayString parts = wxCmdLineParser::ConvertStringToArgs(player, wxCMD_LINE_SPLIT_DOS);
#else
	wxArrayString parts = wxCmdLineParser::ConvertStringToArgs(player, wxCMD_LINE_SPLIT_UNIX);
#endif
	if (parts.IsEmpty()) {
		Fail(CFormat(_("ERROR: Failed to execute external media-player! Command: `%s'")) % player,
			parent,
			reportModally);
		return false;
	}

	// An absolute program path that is not a runnable file is worth catching
	// here. IsFileExecutable covers both halves: a path that is absent, and one
	// that exists without the exec bit -- a data file, or a wrapper script that
	// was never chmod +x -- which would otherwise fail the same way.
	// wxExecute cannot report it: the async form returns the pid as soon as the
	// fork succeeds and execvp then fails in the child, so the caller sees
	// success while the user sees only wx's "execvp ... failed with error 2".
	// That is the normal state of affairs inside a Flatpak, where a host player
	// such as /usr/bin/vlc is simply not on the sandbox's filesystem.
	//
	// A bare command name is left alone: resolving it would mean reimplementing
	// the PATH search that the exec does anyway.
	//
	// Windows paths never match this test, and do not need to: there wxExecute
	// goes through CreateProcess, which fails synchronously, so the launch
	// returns 0 and the error path below already reports it. Only the POSIX
	// fork/exec pair can succeed at the fork and fail out of sight in the child.
	if (parts[0].StartsWith("/") && !wxFileName::IsFileExecutable(parts[0])) {
		Fail(CFormat(_("The configured video player was not found: %s")) % parts[0],
			parent,
			reportModally);
		return false;
	}

	std::vector<wxString> argv;
	argv.reserve(parts.GetCount() + 1);
	bool substituted = false;
	for (const wxString &part : parts) {
		// One pass: a value that happens to contain a placeholder (a file named
		// "x%PARTNAME.avi") must not have it expanded again by a later rule.
		wxString arg;
		arg.reserve(part.length());
		for (size_t i = 0; i < part.length();) {
			if (part.compare(i, 9, "%PARTFILE") == 0) {
				arg += target;
				i += 9;
				substituted = true;
			} else if (part.compare(i, 9, "%PARTNAME") == 0) {
				arg += name;
				i += 9;
				substituted = true;
			} else if (part.compare(i, 5, "$file") == 0) {
				// The historic spelling of %PARTFILE.
				arg += target;
				i += 5;
				substituted = true;
			} else {
				arg += part[i];
				++i;
			}
		}
		argv.push_back(arg);
	}
	// No placeholder anywhere: the player takes the file as its last argument.
	if (!substituted) {
		argv.push_back(target);
	}

	if (!RunDetached(player, argv)) {
		Fail(CFormat(_("ERROR: Failed to execute external media-player! Command: `%s'")) % player,
			parent,
			reportModally);
		return false;
	}
	return true;
}

// The platform's "open this with whatever handles it" action.
bool OpenWithDesktop(const CPath &path)
{
	const wxString target = path.GetRaw();
#if defined(__WXMSW__) || defined(__WXMAC__)
	// No AppImage on these platforms, so there is no environment to sanitize
	// and the portable call is both simpler and better behaved than spawning
	// a shell (it uses ShellExecute / LaunchServices directly).
	try {
		return wxLaunchDefaultApplication(target);
	} catch (...) {
		// See RunDetached: an escaping exception would end the session.
		return false;
	}
#else
	// Linux/BSD: xdg-open, via wxExecute so the AppImage-safe environment can
	// be passed. Inside Flatpak this is the portal shim, which forwards to the
	// host's handler.
	return RunDetached(CFormat("xdg-open %s") % target, { "xdg-open", target });
#endif
}

} // namespace

namespace FileLaunch
{

bool ResolvePath(const CKnownFile *file, CPath &out)
{
	if (file == nullptr) {
		return false;
	}

	const CPath &directory = file->GetFilePath();
	if (!directory.IsOk()) {
		// amulegui before the daemon has streamed EC_TAG_KNOWNFILE_PATH.
		return false;
	}

	// IsPartFile() is `status != PS_COMPLETE` on CPartFile and a constant false
	// on CKnownFile, so a true answer also establishes the dynamic type -- the
	// downcast below is guarded by the same test that selects this branch.
	CPath name;
	if (file->IsPartFile()) {
		name = static_cast<const CPartFile *>(file)->GetPartMetFileName().RemoveExt();
	} else {
		name = file->GetFileName();
	}
	if (!name.IsOk()) {
		return false;
	}

	out = directory.JoinPaths(name);
	return true;
}

bool CanOpen(const CKnownFile *file)
{
	CPath path;
	return ResolvePath(file, path) && path.FileExists();
}

void GetAvailability(const CKnownFile *file, bool &canOpen, bool &canReveal)
{
	CPath path;
	canOpen = ResolvePath(file, path) && path.FileExists();
	// Revealing a ".part" in the temp directory is not what the entry means.
	canReveal = canOpen && !file->IsPartFile();
}

bool CanReveal(const CKnownFile *file)
{
	// A ".part" in the temp directory is not what "show in file manager" means.
	if (file == nullptr || file->IsPartFile()) {
		return false;
	}
	return CanOpen(file);
}

void Open(CKnownFile *file, wxWindow *parent)
{
	CPath path;
	if (!ResolvePath(file, path) || !path.FileExists()) {
		AddLogLineC(CFormat(_("Cannot open '%s': the file is not present on this computer.")) %
			    (file != nullptr ? file->GetFileName().GetPrintable() : wxString()));
		return;
	}

	const bool incomplete = file->IsPartFile();
	const FileType type = GetFiletype(file->GetFileName());
	const bool media = (type == ftVideo || type == ftAudio);
	const wxString &player = thePrefs::GetVideoPlayer();

	// An unfinished download is "NNNN.part" on disk. No desktop registers a
	// handler for that extension, so the platform opener cannot do anything
	// with it -- it would answer with its own "no application set to open this
	// document" dialog. Previewing one only works by handing the path to a
	// player, so when there is none, say so instead of failing twice.
	if (incomplete) {
		if (player.IsEmpty()) {
			if (parent != nullptr) {
				wxMessageBox(
					_("Previewing an unfinished download needs a video player: the "
					  "partly-downloaded file has no type the system can open on its "
					  "own. One can be set in Preferences -> General."),
					_("File preview"),
					wxOK | wxICON_INFORMATION,
					parent);
			}
			return;
		}
		// Nothing to fall back to for a .part, so this one has to be seen.
		LaunchWithPlayer(player, path, parent, true);
		return;
	}

	// Completed: the configured player is for watching media, so it is used for
	// media and deliberately not for anything else -- which is what used to send
	// an archive to the video player.
	// A player that could not be launched should not cost the user the action:
	// for a finished file the desktop handler is a reasonable second choice, and
	// inside a Flatpak that is the portal, which reaches the host's application.
	if (media && !player.IsEmpty() && LaunchWithPlayer(player, path, parent, false)) {
		return;
	}

	if (!OpenWithDesktop(path)) {
		AddLogLineC(CFormat(_("ERROR: Failed to open '%s' with the default application.")) %
			    path.GetPrintable());
	}
}

void Reveal(const CKnownFile *file, wxWindow *WXUNUSED(parent))
{
	CPath path;
	if (!ResolvePath(file, path)) {
		return;
	}
	if (!CanReveal(file)) {
		// Gone between the menu being built and this click, or never eligible.
		// Open's equivalent path logs, so this one does too.
		AddLogLineC(
			CFormat(_("ERROR: Failed to show '%s' in the file manager.")) % path.GetPrintable());
		return;
	}

	bool ok = false;
#if defined(__WXMAC__)
	// NSWorkspace rather than spawning `open -R`: it selects the file in its
	// folder, which is what "Show in Finder" means, and it needs no subprocess
	// -- so nothing about the caller's environment (an unreadable working
	// directory, for one) can perturb it.
	ok = mac_reveal_in_finder(path.GetRaw().utf8_str());
#elif defined(__WXMSW__)
	// Explorer wants the path glued to the switch by a comma, and it exits
	// non-zero even on success -- so the return code says nothing, and this
	// stays a quoted string rather than an argument vector.
	const wxString command = "explorer /select,\"" + path.GetRaw() + "\"";
	CTerminationProcess *process = new CTerminationProcess(command);
	try {
		// Explorer's own exit status is meaningless (non-zero even on success),
		// so it is ignored -- but a failure to spawn it at all is reportable.
		if (wxExecute(command, wxEXEC_ASYNC, process) <= 0) {
			delete process;
			ok = false;
		} else {
			ok = true;
		}
	} catch (...) {
		// see RunDetached
		delete process;
		ok = false;
	}
#else
	// No portable way to select the file; opening the containing folder is the
	// part every desktop agrees on. Selecting it would mean the FileManager1
	// D-Bus interface, which not every file manager implements.
	const wxString directory = path.GetPath().GetRaw();
	ok = RunDetached(CFormat("xdg-open %s") % directory, { "xdg-open", directory });
#endif

	if (!ok) {
		// Quiet beyond the log: the user is looking at the window that did not
		// appear, so a modal adds nothing.
		AddLogLineC(
			CFormat(_("ERROR: Failed to show '%s' in the file manager.")) % path.GetPrintable());
	}
}

} // namespace FileLaunch
