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
// Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
//

#include "AppImageIntegration.h"

#include "amule.h"
#include "AppImageEnv.h" // Needed for GetSanitizedExecEnv
#include "Logger.h"
#include "Preferences.h"

#include <wx/arrstr.h>
#include <wx/dir.h>
#include <wx/file.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/intl.h>
#include <wx/msgdlg.h>
#include <wx/richmsgdlg.h>
#include <wx/stdpaths.h>
#include <wx/string.h>
#include <wx/textfile.h>
#include <wx/utils.h>

#include <vector> // Needed for the argv vector in RunHelper

#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

namespace
{

// AppImage's AppRun exports both env vars: APPIMAGE = the original .AppImage
// path the user invoked; APPDIR = the squashfs mount point. We need both —
// APPIMAGE for the rewritten Exec= line so launcher-clicks invoke the image
// at its real on-disk path, APPDIR to find the bundled .desktop and icons.
wxString GetAppImagePath()
{
	const char *env = getenv("APPIMAGE");
	return env ? wxString::FromUTF8(env) : wxString();
}

wxString GetAppDir()
{
	const char *env = getenv("APPDIR");
	return env ? wxString::FromUTF8(env) : wxString();
}

// Resolve the XDG user data dir per the basedir spec. Don't derive from
// wxStandardPaths::GetUserDataDir() — wx returns "$HOME/.<appname>" for
// aMule (legacy dot-prefixed dir), not the canonical XDG location, so
// any path arithmetic on top of it lands in the wrong tree.
wxString GetUserDataHome()
{
	const char *xdg = getenv("XDG_DATA_HOME");
	if (xdg && *xdg) {
		return wxString::FromUTF8(xdg);
	}
	return wxFileName::GetHomeDir() + wxT("/.local/share");
}

wxString GetUserApplicationsDir()
{
	return GetUserDataHome() + wxT("/applications");
}

wxString GetUserIconsDir()
{
	return GetUserDataHome() + wxT("/icons");
}

wxString GetUserMimePackagesDir()
{
	return GetUserDataHome() + wxT("/mime/packages");
}

// Read the bundled .desktop, swap Exec= and TryExec= to point at $APPIMAGE,
// and write the result to ~/.local/share/applications/org.amule.aMule.desktop.
// Returns true on success.
bool InstallDesktopFile(
	const wxString &appimagePath, const wxString &sourceDesktop, const wxString &destDesktop)
{
	wxTextFile in(sourceDesktop);
	if (!in.Open()) {
		AddDebugLogLineC(logGeneral, wxT("AppImageIntegration: failed to open ") + sourceDesktop);
		return false;
	}

	wxTextFile out(destDesktop);
	if (out.Exists()) {
		out.Open();
		out.Clear();
	} else if (!out.Create()) {
		AddDebugLogLineC(logGeneral, wxT("AppImageIntegration: failed to create ") + destDesktop);
		return false;
	}

	for (size_t i = 0; i < in.GetLineCount(); ++i) {
		wxString line = in[i];
		if (line.StartsWith(wxT("Exec="))) {
			// Quote the AppImage path so spaces survive. %U (URL list)
			// matches the shipped org.amule.aMule.desktop; aMule accepts
			// ed2k:// / magnet: URLs (via scheme handler clicks) plus
			// .emulecollection paths and file:// URLs, and takes any
			// number of them. Must stay in step with the shipped file:
			// it was %F once, which silently swallowed URL clicks.
			line = wxT("Exec=\"") + appimagePath + wxT("\" %U");
		} else if (line.StartsWith(wxT("TryExec="))) {
			line = wxT("TryExec=") + appimagePath;
		}
		out.AddLine(line);
	}

	bool ok = out.Write();
	in.Close();
	out.Close();
	return ok;
}

// Walk $APPDIR/usr/share/icons/hicolor and mirror the org.amule.aMule.* PNG
// files into ~/.local/share/icons/hicolor preserving the size subdirs.
// Best-effort: any single copy failure is logged but doesn't abort the rest.
bool InstallIcons(const wxString &appdir, const wxString &userIconsDir)
{
	const wxString sourceHicolor = appdir + wxT("/usr/share/icons/hicolor");
	if (!wxDirExists(sourceHicolor)) {
		AddDebugLogLineC(
			logGeneral, wxT("AppImageIntegration: hicolor tree missing at ") + sourceHicolor);
		return false;
	}

	// Two name shapes to collect: the application icons (org.amule.aMule.*
	// under apps/) and the icon for the collection file type, whose name
	// is dictated by the icon naming spec (the MIME type with '/' replaced
	// by '-') and so does not carry the app id.
	wxArrayString found;
	wxDir::GetAllFiles(sourceHicolor, &found, wxT("org.amule.aMule.*"), wxDIR_FILES | wxDIR_DIRS);
	wxDir::GetAllFiles(
		sourceHicolor, &found, wxT("application-x-emule-collection.*"), wxDIR_FILES | wxDIR_DIRS);
	if (found.IsEmpty()) {
		AddDebugLogLineC(
			logGeneral, wxT("AppImageIntegration: no aMule icons under ") + sourceHicolor);
		return false;
	}

	bool anyOk = false;
	for (size_t i = 0; i < found.GetCount(); ++i) {
		const wxString &src = found[i];
		wxString relative = src.Mid(sourceHicolor.length());
		wxString dest = userIconsDir + wxT("/hicolor") + relative;

		wxFileName destPath(dest);
		if (!destPath.DirExists()) {
			destPath.Mkdir(0755, wxPATH_MKDIR_FULL);
		}

		if (wxCopyFile(src, dest, true)) {
			anyOk = true;
		} else {
			AddDebugLogLineC(logGeneral,
				wxT("AppImageIntegration: copy failed: ") + src + wxT(" -> ") + dest);
		}
	}
	return anyOk;
}

// Copy the bundled shared-mime-info package into
// ~/.local/share/mime/packages so the desktop learns what a
// .emulecollection is. Without it the file is sniffed as text/plain or
// application/octet-stream, the MimeType= line in the installed .desktop
// never matches, and aMule is absent from the file manager's "Open With".
// A system package install gets this from CMake instead; an AppImage has
// no packager, so we do it ourselves.
bool InstallMimePackage(const wxString &appdir, const wxString &userMimePackagesDir)
{
	const wxString source = appdir + wxT("/usr/share/mime/packages/org.amule.aMule.xml");
	if (!wxFileExists(source)) {
		AddDebugLogLineC(logGeneral, wxT("AppImageIntegration: mime package missing at ") + source);
		return false;
	}

	wxFileName destPath(userMimePackagesDir + wxT("/org.amule.aMule.xml"));
	if (!destPath.DirExists()) {
		destPath.Mkdir(0755, wxPATH_MKDIR_FULL);
	}

	const wxString dest = destPath.GetFullPath();
	if (!wxCopyFile(source, dest, true)) {
		AddDebugLogLineC(
			logGeneral, wxT("AppImageIntegration: copy failed: ") + source + wxT(" -> ") + dest);
		return false;
	}
	return true;
}

// Run a helper with its arguments passed as an argv vector rather than as
// one command string.
//
// The string form of wxExecute does its own tokenising and hands the quote
// characters through to the program, so `cmd "/home/me/dir"` arrives as a
// path that literally starts with a quote and the helper fails with
// "directory does not exist". Quoting was there to survive spaces in $HOME;
// argv gives us that for free and without a shell.
static void RunHelper(const wxString &program, const wxArrayString &args)
{
	std::vector<wxCharBuffer> storage;
	std::vector<const char *> argv;
	storage.reserve(args.GetCount() + 1);

	storage.emplace_back(program.mb_str(wxConvUTF8));
	argv.push_back(storage.back().data());
	for (size_t i = 0; i < args.GetCount(); ++i) {
		storage.emplace_back(args[i].mb_str(wxConvUTF8));
		argv.push_back(storage.back().data());
	}
	argv.push_back(nullptr);

	// These are system binaries, and AppRun has put the AppImage's own
	// library directory at the front of LD_LIBRARY_PATH for our sake. A
	// child inheriting that loads our bundled glib rather than the host's
	// and dies before doing any work: update-mime-database exits with
	// "undefined symbol: g_string_free_and_steal" against a host glib newer
	// than the bundled copy. Same class of failure as #334, so reuse the
	// helper written for it rather than stripping the paths again here.
	wxExecuteEnv execEnv;
	const bool sanitized = AppImageEnv::GetSanitizedExecEnv(execEnv);
	const int flags = wxEXEC_SYNC | wxEXEC_NODISABLE | wxEXEC_NOEVENTS;

	wxExecute(argv.data(), flags, nullptr, sanitized ? &execEnv : nullptr);
}

// update-desktop-database, gtk-update-icon-cache and update-mime-database
// exist on every desktop distro that ships a .desktop file system, and we
// don't fail integration if they're missing.
//
// The .desktop and icon caches have a safety net - desktop environments
// watch those directories and rebuild on their own within seconds - which
// is exactly why the broken quoting above went unnoticed for so long. The
// shared-mime-info database has no such watcher: if update-mime-database
// does not run, the collection type stays unknown and a double-click opens
// whatever handles text/plain.
void RefreshSystemCaches(
	const wxString &userAppsDir, const wxString &userIconsDir, const wxString &userMimeDir)
{
	wxArrayString args;

	args.Add(userAppsDir);
	RunHelper(wxT("update-desktop-database"), args);

	args.Clear();
	args.Add(wxT("-f"));
	args.Add(wxT("-t"));
	args.Add(userIconsDir + wxT("/hicolor"));
	RunHelper(wxT("gtk-update-icon-cache"), args);

	args.Clear();
	// Takes the mime dir itself, not the packages/ subdir inside it.
	args.Add(userMimeDir);
	RunHelper(wxT("update-mime-database"), args);
}

bool DesktopFileAlreadyInstalled()
{
	return wxFileExists(GetUserApplicationsDir() + wxT("/org.amule.aMule.desktop"));
}

wxString GetUserBinDir()
{
	return wxGetUserHome() + wxT("/.local/bin");
}

// True if `path` names an existing symlink (broken or intact). Distinct
// from wxFileExists which returns true for a symlink pointing at a
// non-existent target BUT ALSO returns true for a regular file - we
// need to know the difference before deciding whether it's safe to
// replace with our own symlink. POSIX-only; MinGW-w64 (Windows) has
// neither lstat nor S_ISLNK, and the whole AppImage integration path
// is a no-op there anyway (ShouldPrompt returns false on non-GTK).
#ifdef __WXGTK__
bool IsSymlink(const wxString &path)
{
	struct stat st;
	return lstat((const char *)path.mb_str(wxConvUTF8), &st) == 0 && S_ISLNK(st.st_mode);
}
#endif

// The names AppRun's argv[0]-dispatch case knows about. Any name that
// AppRun doesn't recognize falls back to `amule`, so a stale symlink
// to a since-removed binary still launches the monolithic GUI —
// never a hard failure. Order matches the AppRun case statement.
const wxString kAppRunNames[] = {
	wxT("amule"),
	wxT("amuled"),
	wxT("amulegui"),
	wxT("amulecmd"),
	wxT("amuleweb"),
	wxT("amuleapi"),
	wxT("ed2k"),
	wxT("cas"),
	wxT("wxcas"),
	wxT("alc"),
	wxT("alcc"),
};

// Create ~/.local/bin/<name> symlinks for each AppRun-dispatched
// binary that this AppImage actually bundles. Returns the count
// created. Refuses to clobber a pre-existing regular file at any of
// these paths (protects e.g. a user's distro-packaged amule binary if
// it somehow lives under ~/.local/bin); existing symlinks ARE replaced
// so a re-install of a new AppImage refreshes the targets.
// POSIX-only (uses symlink(2) and IsSymlink() which use lstat/S_ISLNK).
// The AppImage integration flow is inherently Linux-only; ShouldPrompt
// returns false on Windows / macOS, so this function is never actually
// called on those platforms - but it still needs to compile.
#ifdef __WXGTK__
int InstallCommandSymlinks(const wxString &appdir, const wxString &appimagePath)
{
	const wxString binDir = GetUserBinDir();
	if (!wxDirExists(binDir) && !wxFileName::Mkdir(binDir, 0755, wxPATH_MKDIR_FULL)) {
		AddDebugLogLineC(logGeneral, wxT("AppImageIntegration: failed to create ") + binDir);
		return 0;
	}

	int created = 0;
	for (const wxString &name : kAppRunNames) {
		// Only if this AppImage actually bundles the binary.
		const wxString bundled = appdir + wxT("/usr/bin/") + name;
		if (!wxFileExists(bundled)) {
			continue;
		}
		const wxString linkPath = binDir + wxT("/") + name;
		if (wxFileExists(linkPath) && !IsSymlink(linkPath)) {
			AddDebugLogLineC(logGeneral,
				wxT("AppImageIntegration: not clobbering existing non-symlink ") + linkPath);
			continue;
		}
		if (IsSymlink(linkPath)) {
			wxRemoveFile(linkPath);
		}
		if (symlink((const char *)appimagePath.mb_str(wxConvUTF8),
			    (const char *)linkPath.mb_str(wxConvUTF8)) == 0) {
			++created;
		} else {
			AddDebugLogLineC(
				logGeneral, wxT("AppImageIntegration: symlink failed for ") + linkPath);
		}
	}
	return created;
}
#else
// Non-GTK stub - never called at runtime (PromptAndInstall bails
// early via ShouldPrompt on !__WXGTK__), but needs to link.
int InstallCommandSymlinks(const wxString &, const wxString &)
{
	return 0;
}
#endif // __WXGTK__

} // anonymous namespace

namespace AppImageIntegration
{

bool ShouldPrompt()
{
#ifdef __WXGTK__
	if (GetAppImagePath().IsEmpty()) {
		// Not running from an AppImage — distro install or dev build.
		return false;
	}
	if (thePrefs::IsAppImageIntegrationDeclined()) {
		// User picked "Don't ask again" on a previous launch.
		return false;
	}
	if (DesktopFileAlreadyInstalled()) {
		// Already installed — most likely from a previous "Yes" click.
		return false;
	}
	return true;
#else
	return false;
#endif
}

void PromptAndInstall(wxWindow *parent)
{
	if (!ShouldPrompt()) {
		return;
	}

	wxRichMessageDialog dlg(parent,
		_("aMule can install desktop launchers, icons, and shell command "
		  "shortcuts into your home directory so it appears in your application "
		  "menu like a regularly installed app.\n\n"
		  "This is reversible - the files live under ~/.local/share/applications, "
		  "~/.local/share/icons, and ~/.local/bin, and can be removed at any time.\n\n"
		  "Install desktop integration now?"),
		_("Add aMule to your application menu?"),
		wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
	dlg.SetYesNoLabels(_("Install"), _("Not now"));
	dlg.ShowCheckBox(_("Don't ask again"));

	const int answer = dlg.ShowModal();
	const bool dontAskAgain = dlg.IsCheckBoxChecked();

	if (dontAskAgain) {
		thePrefs::SetAppImageIntegrationDeclined(true);
		// Persist immediately so a crash before normal shutdown still
		// remembers the user's choice.
		theApp->glob_prefs->Save();
	}

	if (answer != wxID_YES) {
		return;
	}

	const wxString appimagePath = GetAppImagePath();
	const wxString appdir = GetAppDir();
	if (appdir.IsEmpty()) {
		const wxString msg =
			_("Cannot install desktop integration: the APPDIR environment variable is not set. "
			  "This usually means the AppImage was launched in a non-standard way.");
		AddLogLineC(msg);
		wxMessageBox(msg, _("aMule integration failed"), wxOK | wxICON_ERROR, parent);
		return;
	}

	const wxString userAppsDir = GetUserApplicationsDir();
	const wxString userIconsDir = GetUserIconsDir();

	if (!wxDirExists(userAppsDir) && !wxFileName::Mkdir(userAppsDir, 0755, wxPATH_MKDIR_FULL)) {
		const wxString msg =
			wxString::Format(_("Cannot install desktop integration: failed to create %s. Check "
					   "that your home directory is writable."),
				userAppsDir);
		AddLogLineC(msg);
		wxMessageBox(msg, _("aMule integration failed"), wxOK | wxICON_ERROR, parent);
		return;
	}

	// Shell-command shortcuts first: the amulegui .desktop file
	// installed below points at ~/.local/bin/amulegui to route
	// clicks to the correct AppRun dispatch. If that symlink doesn't
	// materialise the .gui .desktop's Exec= still resolves via PATH
	// lookup, but a fresh installation with an untouched
	// ~/.local/bin/ path is the happy case we design for here.
	const int symlinkCount = InstallCommandSymlinks(appdir, appimagePath);

	// Install org.amule.aMule.desktop (monolithic amule). Exec= points
	// straight at the AppImage - default AppRun dispatch is amule.
	const wxString sourceAmuleDesktop = appdir + wxT("/usr/share/applications/org.amule.aMule.desktop");
	const wxString destAmuleDesktop = userAppsDir + wxT("/org.amule.aMule.desktop");
	if (!InstallDesktopFile(appimagePath, sourceAmuleDesktop, destAmuleDesktop)) {
		const wxString msg = wxString::Format(_("Cannot install desktop integration: failed to write "
							"%s. Check that your home directory is writable."),
			destAmuleDesktop);
		AddLogLineC(msg);
		wxMessageBox(msg, _("aMule integration failed"), wxOK | wxICON_ERROR, parent);
		return;
	}

	// Install org.amule.aMule.gui.desktop (remote-GUI amulegui). Exec=
	// points at the ~/.local/bin/amulegui symlink so AppRun's basename
	// dispatch picks amulegui rather than the default amule. Only
	// attempt if the AppImage actually bundles amulegui AND the
	// symlink was created; otherwise silently skip (a
	// TESTING=OFF-only build has no amulegui).
	const wxString guiSource = appdir + wxT("/usr/share/applications/org.amule.aMule.gui.desktop");
	const wxString guiSymlink = GetUserBinDir() + wxT("/amulegui");
	if (wxFileExists(guiSource) && wxFileExists(guiSymlink)) {
		const wxString destGuiDesktop = userAppsDir + wxT("/org.amule.aMule.gui.desktop");
		if (!InstallDesktopFile(guiSymlink, guiSource, destGuiDesktop)) {
			AddDebugLogLineC(logGeneral,
				wxT("AppImageIntegration: failed to install amulegui .desktop; "
				    "amulegui menu entry and scheme-handler association will not "
				    "work until re-integrated"));
		}
	}

	InstallIcons(appdir, userIconsDir);
	// Best-effort, like the icons: no mime package just means
	// .emulecollection files aren't offered to aMule in the file manager.
	InstallMimePackage(appdir, GetUserMimePackagesDir());
	RefreshSystemCaches(userAppsDir, userIconsDir, GetUserDataHome() + wxT("/mime"));

	AddLogLineN(wxString::Format(
		_("AppImage integration: aMule added to your application menu (%d shell shortcuts under "
		  "~/.local/bin)."),
		symlinkCount));

	wxMessageDialog success(parent,
		_("aMule has been added to your application menu. You can now launch it from your "
		  "applications list, and the launcher will run this same AppImage.\n\n"
		  "On some desktops, aMule may need to restart for the dock / taskbar icon to bind to the "
		  "new launcher entry. Modern Wayland compositors (GNOME, KDE) pick this up live, but a "
		  "restart is the safe fallback if the icon stays generic.\n\n"
		  "Restart aMule now?"),
		_("aMule installed"),
		wxYES_NO | wxNO_DEFAULT | wxICON_INFORMATION);
	success.SetYesNoLabels(_("Restart now"), _("Later"));

	if (success.ShowModal() != wxID_YES) {
		return;
	}

	// Spawn a detached shell that polls our PID and re-execs the AppImage
	// once we fully exit. This gives aMule's normal shutdown path time to
	// save partfiles, release the EC port, etc., before the new instance
	// tries to grab the same locks.
	const wxString relaunchCmd =
		wxString::Format(wxT("sh -c 'while kill -0 %d 2>/dev/null; do sleep 0.2; done; exec \"%s\"'"),
			static_cast<int>(getpid()),
			appimagePath);
	wxExecute(relaunchCmd, wxEXEC_ASYNC | wxEXEC_MAKE_GROUP_LEADER);

	// Trigger normal close on the main dialog — same path as red X / File>Quit.
	if (parent) {
		parent->Close(true);
	}
}

} // namespace AppImageIntegration
