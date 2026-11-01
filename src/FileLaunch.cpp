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

#include <wx/filename.h> // Needed for wxFileName::IsFileExecutable
#include <wx/msgdlg.h>   // Needed for wxMessageBox
#include <wx/utils.h>    // Needed for wxExecute, wxLaunchDefaultApplication

#include <common/Format.h> // Needed for CFormat

#ifdef __WXMAC__
#include "MacAppHelper.h" // Needed for mac_reveal_in_finder
#endif

#include "Flatpak.h"
#include "PlayerExecutable.h"
#include "ExternalCommand.h" // Needed for template expansion and RunDetached
#ifdef CLIENT_GUI
#include "amule.h" // Needed for theApp->glob_prefs (ApplyPathMapping)
#endif
#include "KnownFile.h"          // Needed for CKnownFile
#include "Logger.h"             // Needed for AddLogLineC
#include "OtherFunctions.h"     // Needed for GetFiletype
#include "PartFile.h"           // Needed for CPartFile
#include "Preferences.h"        // Needed for thePrefs
#include "TerminationProcess.h" // Needed for CTerminationProcess

namespace
{

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
// The shared helper parses the template before inserting file values. The full path,
// bare name, and historic $file alias each stay in one argument, and placeholder
// text inside a filename is not expanded again. Explicit shell scripts still need
// positional arguments, as described for event commands in docs/README.md.
//
// `reportModally` is for the caller that has nothing to fall back to: a failure it only logs is,
// from the user's side, the same silent nothing this removes.
bool LaunchWithPlayer(const wxString &player, const CPath &path, wxWindow *parent, bool reportModally)
{
	const wxString target = path.GetRaw();
	const wxString name = path.GetFullName().GetRaw();

	ExternalCommand::RejectionReason rejection;
	wxArrayString parts = ExternalCommand::Build(player,
		{ { "%PARTFILE", target }, { "%PARTNAME", name }, { "$file", target } },
		nullptr,
		ExternalCommand::NativePlatform,
		&target,
		&rejection);
	if (parts.IsEmpty()) {
		Fail(CFormat(_("Media-player command '%s' was not run: %s")) % player %
				ExternalCommand::DescribeRejection(rejection),
			parent,
			reportModally);
		return false;
	}

	// An absolute program path that is not a runnable file is worth catching here.
	// IsFileExecutable covers both halves: a path that is absent, and one that exists without
	// the exec bit. wxExecute cannot report it -- the async form returns the pid as soon as the
	// fork succeeds and execvp then fails in the child, so the caller sees success while the
	// user sees only wx's "execvp ... failed with error 2". That is the normal state of affairs
	// inside a Flatpak, where a host player is simply not on the sandbox's filesystem.
	//
	// In Flatpak, also check bare names so a missing host player gets a useful
	// explanation before wxExecute forks. Other builds retain their usual PATH lookup.
	const bool inFlatpak = InFlatpak();
#ifdef __WXMAC__
	// A folder, which wxExecute opens with Launch Services.
	const bool appBundle = IsAppBundle(parts[0]);
#else
	const bool appBundle = false;
#endif
	const bool unavailable =
		inFlatpak ? !IsPlayerAvailable(parts[0])
			  : parts[0].StartsWith("/") && !appBundle && !wxFileName::IsFileExecutable(parts[0]);
	if (unavailable) {
		const wxString message =
			inFlatpak
				? CFormat(_("The configured video player is unavailable inside aMule's "
					    "Flatpak sandbox: %s. "
					    "Programs installed on the host are not directly available "
					    "here. Completed files can still open with the desktop's default "
					    "application; previewing an unfinished download needs a "
					    "player the sandbox can run, such as a self-contained player "
					    "saved in your home folder.")) %
					  parts[0]
				: CFormat(_("The configured video player was not found: %s")) % parts[0];
		Fail(message, parent, reportModally);
		return false;
	}

	if (!ExternalCommand::RunDetached(player, parts)) {
		Fail(CFormat(_("ERROR: Failed to execute external media-player! Command: '%s'")) % player,
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
	// No AppImage on these platforms, so there is no environment to sanitize, and the portable
	// call is both simpler and better behaved than spawning a shell -- it uses ShellExecute /
	// LaunchServices directly.
	try {
		return wxLaunchDefaultApplication(target);
	} catch (...) {
		// See RunDetached: an escaping exception would end the session.
		return false;
	}
#else
	// Linux/BSD: xdg-open, via wxExecute so the AppImage-safe environment can be passed. Inside
	// Flatpak this is the portal shim, which forwards to the host's handler.
	wxArrayString args;
	args.Add("xdg-open");
	args.Add(target);
	return ExternalCommand::RunDetached(CFormat("xdg-open %s") % target, args);
#endif
}

// Rewrite a remote daemon's path into this machine's, per the user's configured mappings
// (issue #843) -- the daemon's /downloads/incoming reachable here as
// D:\Downloads\aMule\incoming over a Samba mount, say. String substitution on the raw
// daemon path, before it becomes part of a CPath, since the remote side need not use this
// host's separator convention. Returns the input unchanged when no mapping's prefix
// matches, the empty mapping list included.
CPath LocalDirectory(const CPath &directory)
{
#ifdef CLIENT_GUI
	return CPath(theApp->glob_prefs->ApplyPathMapping(directory.GetRaw()));
#else
	// The monolithic app is its own core: never remapped.
	return directory;
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

	const CPath &rawDirectory = file->GetFilePath();
	if (!rawDirectory.IsOk()) {
		// amulegui before the daemon has streamed EC_TAG_KNOWNFILE_PATH.
		return false;
	}

	// IsPartFile() is `status != PS_COMPLETE` on CPartFile and a constant false on CKnownFile,
	// so a true answer also establishes the dynamic type -- the downcast below is guarded by
	// the same test that selects this branch.
	CPath name;
	if (file->IsPartFile()) {
		name = static_cast<const CPartFile *>(file)->GetPartMetFileName().RemoveExt();
	} else {
		name = file->GetFileName();
	}
	if (!name.IsOk()) {
		return false;
	}

	out = LocalDirectory(rawDirectory).JoinPaths(name);
	return true;
}

bool CanOpenDirectory(const CPath &directory)
{
	return directory.IsOk() && LocalDirectory(directory).DirExists();
}

void OpenDirectory(const CPath &directory)
{
	const CPath local = LocalDirectory(directory);
	// Rechecked: the folder may have gone since the menu was built.
	if (!CanOpenDirectory(directory) || !OpenWithDesktop(local)) {
		AddLogLineC(
			CFormat(_("ERROR: Failed to show '%s' in the file manager.")) % local.GetPrintable());
	}
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

	// An unfinished download is "NNNN.part" on disk, and no desktop registers a handler for
	// that extension, so the platform opener would answer with its own "no application set to
	// open this document" dialog. Previewing one only works by handing the path to a player, so
	// when there is none, say so.
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

	// Completed: the configured player is for watching media, so it is used for media and
	// deliberately not for anything else -- which is what used to send an archive to the video
	// player. A player that could not be launched should not cost the user the action: for a
	// finished file the desktop handler is a reasonable second choice, and inside a Flatpak
	// that is the portal.
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
	// NSWorkspace rather than spawning `open -R`: it selects the file in its folder, which is
	// what "Show in Finder" means, and needs no subprocess, so nothing about the caller's
	// environment can perturb it.
	ok = mac_reveal_in_finder(path.GetRaw().utf8_str());
#elif defined(__WXMSW__)
	// Explorer wants the path glued to the switch by a comma, and it exits non-zero even on
	// success -- so the return code says nothing, and this stays a quoted string rather than an
	// argument vector. Its full path, because CreateProcess would try a bare name in the current
	// directory first.
	const wxString command =
		"\"" + wxGetOSDirectory() + "\\explorer.exe\" /select,\"" + path.GetRaw() + "\"";
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
	// No portable way to select the file; opening the containing folder is the part every
	// desktop agrees on. Selecting it would mean the FileManager1 D-Bus interface, which not
	// every file manager implements.
	ok = OpenWithDesktop(path.GetPath());
#endif

	if (!ok) {
		// Quiet beyond the log: the user is looking at the window that did not
		// appear, so a modal adds nothing.
		AddLogLineC(
			CFormat(_("ERROR: Failed to show '%s' in the file manager.")) % path.GetPrintable());
	}
}

} // namespace FileLaunch
