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

#include <wx/msgdlg.h> // Needed for wxMessageBox
#include <wx/utils.h>  // Needed for wxExecute, wxLaunchDefaultApplication

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
	const long ret = wxExecute(argv.data(), wxEXEC_ASYNC, process, sanitized ? &execEnv : nullptr);
	if (ret <= 0) {
		delete process;
		return false;
	}
	return true;
}

// Hand the path to the user's configured player. The command is a template
// from preferences, so unlike our own launches it stays a string and keeps the
// historic %PARTFILE / %PARTNAME / $file placeholders and quoting.
void LaunchWithPlayer(const wxString &player, const CPath &path, wxWindow *WXUNUSED(parent))
{
	wxString command = player;
	const wxString name = path.GetFullName().GetRaw();
	if (!command.Replace("$file", "%PARTFILE")) {
		if ((command.Find("%PARTFILE") == wxNOT_FOUND) &&
			(command.Find("%PARTNAME") == wxNOT_FOUND)) {
#ifdef __WINDOWS__
			command << " \"" << path.GetRaw() << "\"";
#else
			command << " '" << path.GetRaw() << "'";
#endif
		}
	}
	command.Replace("%PARTFILE", path.GetRaw());
	command.Replace("%PARTNAME", name);

	CTerminationProcess *process = new CTerminationProcess(command);
	wxExecuteEnv execEnv;
	const bool sanitized = AppImageEnv::GetSanitizedExecEnv(execEnv);
	if (wxExecute(command, wxEXEC_ASYNC, process, sanitized ? &execEnv : nullptr) <= 0) {
		delete process;
		AddLogLineC(CFormat(_("ERROR: Failed to execute external media-player! Command: `%s'")) %
			    command);
	}
}

// The platform's "open this with whatever handles it" action.
bool OpenWithDesktop(const CPath &path)
{
	const wxString target = path.GetRaw();
#if defined(__WXMSW__) || defined(__WXMAC__)
	// No AppImage on these platforms, so there is no environment to sanitize
	// and the portable call is both simpler and better behaved than spawning
	// a shell (it uses ShellExecute / LaunchServices directly).
	return wxLaunchDefaultApplication(target);
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
	const wxString player = thePrefs::GetVideoPlayer();

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
		LaunchWithPlayer(player, path, parent);
		return;
	}

	// Completed: the configured player is for watching media, so it is used for
	// media and deliberately not for anything else -- which is what used to send
	// an archive to the video player.
	if (media && !player.IsEmpty()) {
		LaunchWithPlayer(player, path, parent);
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
	if (!CanReveal(file) || !ResolvePath(file, path)) {
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
	if (wxExecute(command, wxEXEC_ASYNC, process) <= 0) {
		delete process;
	}
	ok = true;
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
