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

#include "AutostartManager.h"

#include <wx/app.h>      // Needed for wxTheApp
#include <wx/filename.h> // Needed for wxFileName
#include <wx/log.h>      // Needed for wxLogDebug
#include <wx/regex.h>    // Needed for wxRegEx (macOS plist parsing)
#include <wx/stdpaths.h> // Needed for wxStandardPaths

#ifdef __WXMSW__
#include <windows.h>
#include <cwchar> // Needed for wcslen
#include <vector> // Needed for std::vector buffer in BackendReadTargetPath
#else
#include <climits>        // Needed for PATH_MAX
#include <stdlib.h>       // Needed for realpath
#include <wx/utils.h>     // Needed for wxGetEnv / wxGetUserHome
#include <wx/file.h>      // Needed for wxFile
#include <wx/filefn.h>    // Needed for wxRemoveFile
#include <wx/tokenzr.h>   // Needed for wxStringTokenizer
#include "XdgConfigDir.h" // Needed for XdgConfigDir
#ifdef HAVE_GIO
#include <gio/gio.h> // GDBus, for the Flatpak background portal
#endif
#endif

// Per-backend low-level helpers (return raw OS state, no policy).
// Defined in the platform-specific section near the bottom of this file.
namespace
{
// Reads the autostart entry's target path from the OS store. Empty when the entry does not exist or
// cannot be parsed. Does not validate the path against the filesystem.
wxString BackendReadTargetPath();

// Writes/overwrites the autostart entry to point at `executable`, leaving it switched off in the
// OS's own startup settings when `switchedOff`. Returns true on success.
bool BackendWrite(const wxString &executable, bool switchedOff);

// True when the entry exists but the user turned it off from the OS (Task Manager's Startup tab,
// the desktop's startup settings) rather than through aMule.
bool BackendIsSwitchedOff();

// Removes the autostart entry. Idempotent -- returns true if the
// entry didn't exist either.
bool BackendRemove();
} // namespace

wxString AutostartManager::GetCanonicalExecutablePath()
{
	// wxStandardPaths::GetExecutablePath() wraps the OS native call; on POSIX we then resolve
	// intermediate symlinks via realpath(), so a moved install or .app bundle is detected.
	wxString raw = wxStandardPaths::Get().GetExecutablePath();

#ifndef __WXMSW__
	// realpath() rejects empty input; guard.
	if (raw.empty()) {
		return raw;
	}
	char resolved[PATH_MAX];
	if (realpath(raw.mb_str(wxConvUTF8), resolved) != NULL) {
		return wxString::FromUTF8(resolved);
	}
	// realpath failed (binary unlinked? permission?) -- fall through to the raw
	// path, which still works as long as the OS can resolve it.
#endif

	return raw;
}

// The aMule program a registered target starts, by the rule AppRun applies to an AppImage name:
// amuled and amulegui by name, anything else the monolithic amule. Covers .exe, .app and .AppImage
// names alike, so each OS store can tell whose entry it holds.
static wxString ProgramOf(const wxString &target)
{
	wxString name = wxFileName(target).GetFullName().Lower();
	for (const char *suffix : { ".exe", ".app", ".appimage" }) {
		name.EndsWith(suffix, &name);
	}
	for (const char *suffix : { "-x64", "-arm64", "-x86_64", "-aarch64" }) {
		name.EndsWith(suffix, &name);
	}
	name.EndsWith("-linux", &name);
	if (name == "amuled" || name == "amulegui") {
		return name;
	}
	return "amule";
}

static bool SameTarget(const wxString &a, const wxString &b)
{
#ifdef __WXMSW__
	// Registry values can round-trip with a different case.
	return a.IsSameAs(b, false);
#else
	return a == b;
#endif
}

wxString AutostartManager::GetTarget()
{
	// Resolved once: Enable() can run long after startup, when the working directory an AppImage's
	// relative ARGV0 was given against may have changed.
	static const wxString target = [] {
		const wxString exe = GetCanonicalExecutablePath();
#if defined(__WXMAC__) || defined(__WXOSX__)
#ifndef AMULE_DAEMON
		// The GUIs start through their .app, which keeps Gatekeeper quiet at login. amuled lives
		// inside aMule.app too, but opening the bundle would start the GUI instead.
		const int idx = exe.Find(".app/");
		if (idx != wxNOT_FOUND) {
			return exe.Mid(0, idx + 4);
		}
#endif
#elif !defined(__WXMSW__)
		// Under an AppImage the executable sits in a mount that goes away on exit. Register what
		// the AppImage was started as: AppRun picks the binary from that name, so a symlink named
		// amuled stays amuled.
		wxString appImage;
		if (wxGetEnv("APPIMAGE", &appImage) && !appImage.empty()) {
			wxString argv0;
			if (wxGetEnv("ARGV0", &argv0) && !argv0.empty()) {
				wxFileName invoked(argv0);
				if (!argv0.Contains("/")) {
					wxPathList path;
					path.AddEnvList("PATH");
					invoked.Assign(path.FindAbsoluteValidPath(argv0));
				}
				invoked.MakeAbsolute();
				if (invoked.FileExists() &&
					ProgramOf(invoked.GetFullPath()) == ProgramOf(exe)) {
					return invoked.GetFullPath();
				}
			}
			if (ProgramOf(appImage) == ProgramOf(exe)) {
				return appImage;
			}
			// Nothing on disk starts this program; registering the mount would break at the next
			// login, and the AppImage itself would start another program.
			return wxString();
		}
#endif
		return exe;
	}();
	return target;
}

bool AutostartManager::IsEnabled()
{
	const wxString registered = BackendReadTargetPath();
	return !registered.empty() && ProgramOf(registered) == ProgramOf(GetTarget()) &&
	       !BackendIsSwitchedOff();
}

bool AutostartManager::Enable()
{
	const wxString target = GetTarget();
	if (target.empty()) {
		wxLogDebug(wxT("AutostartManager::Enable: no startable path for this program, refusing to "
			       "write a broken entry"));
		return false;
	}
	return BackendWrite(target, false);
}

bool AutostartManager::Disable()
{
	const wxString registered = BackendReadTargetPath();
	if (registered.empty() || ProgramOf(registered) != ProgramOf(GetTarget())) {
		// Nothing registered for this program. The one entry per user may belong to another
		// aMule program, and turning this one off must not remove that.
		return true;
	}
	return BackendRemove();
}

void AutostartManager::SelfHealOnStartup()
{
#if !defined(__WXMSW__) && !defined(__WXMAC__) && !defined(__WXOSX__)
	if (wxGetEnv(wxT("FLATPAK_ID"), nullptr)) {
		// The portal's entry names a program inside the app, never a path: nothing drifts.
		return;
	}
#endif
	const wxString registered = BackendReadTargetPath();
	if (registered.empty()) {
		// No entry -- user chose not to autostart, or never enabled it.
		// Don't second-guess.
		return;
	}

	const wxString target = GetTarget();
	if (target.empty()) {
		// Couldn't resolve a startable path; best to leave
		// the existing entry alone rather than blow it away.
		return;
	}

	if (ProgramOf(registered) != ProgramOf(target)) {
		// Another aMule program's entry: amule, amuled and amulegui share the one per-user slot,
		// and running one of them must not take over what the user set up for another.
		return;
	}

	if (SameTarget(registered, target)) {
		// Already pointing at us, nothing to do.
		return;
	}

	// Path drifted (moved AppImage / .app / install dir, or an upgrade tool that
	// did not rewrite the entry), so rewrite it to the current canonical path.
	wxLogDebug(wxT("AutostartManager::SelfHealOnStartup: rewriting autostart entry from '%s' to '%s'"),
		registered.c_str(),
		target.c_str());
	BackendWrite(target, BackendIsSwitchedOff());
}

// --------------------------------------------------------------------
// Platform backends
// --------------------------------------------------------------------

namespace
{

#if defined(__WXMSW__)

// Windows: per-user "Run on login" registry key. HKCU rather than HKLM, so autostart is a per-user
// choice on shared machines and toggling never needs elevation. The same key Task Manager's Startup
// tab reads.
static const wchar_t *RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t *RUN_VALUE_NAME = L"aMule";
// Task Manager's Startup tab records its enabled/disabled switch here, per Run value name, and
// leaves the Run value itself alone.
static const wchar_t *STARTUP_APPROVED_KEY =
	L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

static void ClearStartupApproved()
{
	HKEY hKey;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, STARTUP_APPROVED_KEY, 0, KEY_SET_VALUE, &hKey) ==
		ERROR_SUCCESS) {
		RegDeleteValueW(hKey, RUN_VALUE_NAME);
		RegCloseKey(hKey);
	}
}

bool BackendIsSwitchedOff()
{
	HKEY hKey;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, STARTUP_APPROVED_KEY, 0, KEY_QUERY_VALUE, &hKey) !=
		ERROR_SUCCESS) {
		return false;
	}
	BYTE data[12] = {};
	DWORD cb = sizeof(data);
	DWORD type = 0;
	const LSTATUS rc = RegQueryValueExW(hKey, RUN_VALUE_NAME, NULL, &type, data, &cb);
	RegCloseKey(hKey);
	// Undocumented but stable: the first byte is even (2, 6) when enabled and odd (3, 7) when
	// disabled.
	return rc == ERROR_SUCCESS && type == REG_BINARY && cb > 0 && (data[0] & 1) != 0;
}

wxString BackendReadTargetPath()
{
	HKEY hKey;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
		return wxEmptyString;
	}

	// First call returns the required buffer size; second call
	// reads the value. Sized in bytes, includes the trailing NUL.
	DWORD type = 0;
	DWORD cb = 0;
	LSTATUS rc = RegQueryValueExW(hKey, RUN_VALUE_NAME, NULL, &type, NULL, &cb);
	if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || cb == 0) {
		RegCloseKey(hKey);
		return wxEmptyString;
	}

	// cb is in bytes; convert to wide-character count, rounding up.
	size_t wlen = (cb + sizeof(wchar_t) - 1) / sizeof(wchar_t);
	std::vector<wchar_t> buf(wlen + 1, L'\0');
	rc = RegQueryValueExW(hKey, RUN_VALUE_NAME, NULL, &type, reinterpret_cast<LPBYTE>(buf.data()), &cb);
	RegCloseKey(hKey);
	if (rc != ERROR_SUCCESS) {
		return wxEmptyString;
	}

	wxString raw(buf.data());

	// Windows stores Run entries either bare or quoted with an argument tail, so strip
	// surrounding quotes and discard any arguments -- the path comparison in SelfHealOnStartup
	// matches the unadorned canonical path.
	if (!raw.empty() && raw[0] == wxT('"')) {
		size_t closing = raw.find(wxT('"'), 1);
		if (closing != wxString::npos) {
			return raw.SubString(1, closing - 1);
		}
	}
	// Unquoted: take the leading non-whitespace run as the path.
	size_t sp = raw.find_first_of(wxT(" \t"));
	if (sp != wxString::npos) {
		return raw.SubString(0, sp - 1);
	}
	return raw;
}

bool BackendWrite(const wxString &executable, bool switchedOff)
{
	if (!switchedOff) {
		ClearStartupApproved();
	}
	HKEY hKey;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, NULL) !=
		ERROR_SUCCESS) {
		return false;
	}

	// Quote the path so spaces in "Program Files" don't get parsed
	// as argument separators by Windows' Run-key handler.
	wxString quoted = wxT("\"") + executable + wxT("\"");
	const wchar_t *wstr = quoted.wc_str();
	// cb counts the trailing NUL too, per RegSetValueExW contract.
	DWORD cb = static_cast<DWORD>((wcslen(wstr) + 1) * sizeof(wchar_t));
	LSTATUS rc =
		RegSetValueExW(hKey, RUN_VALUE_NAME, 0, REG_SZ, reinterpret_cast<const BYTE *>(wstr), cb);
	RegCloseKey(hKey);
	return rc == ERROR_SUCCESS;
}

bool BackendRemove()
{
	HKEY hKey;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
		// Key itself doesn't exist → nothing to remove, success.
		return true;
	}
	LSTATUS rc = RegDeleteValueW(hKey, RUN_VALUE_NAME);
	RegCloseKey(hKey);
	ClearStartupApproved();
	// ERROR_FILE_NOT_FOUND = value already absent, also success.
	return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
}

#elif defined(__WXMAC__) || defined(__WXOSX__)

// macOS: per-user LaunchAgent. The GUIs register `/usr/bin/open -a <aMule.app>` rather than the bare
// Mach-O, which sidesteps Gatekeeper / quarantine warnings when launchd activates us at login. amuled
// registers its own binary: opening the bundle it ships in would start the GUI.
static const wxString PLIST_LABEL = wxT("org.amule.amule");

static wxString PlistPath()
{
	return wxGetUserHome() + wxT("/Library/LaunchAgents/") + PLIST_LABEL + wxT(".plist");
}

wxString BackendReadTargetPath()
{
	wxString path = PlistPath();
	if (!wxFileName::FileExists(path)) {
		return wxEmptyString;
	}

	wxFile f(path, wxFile::read);
	if (!f.IsOpened()) {
		return wxEmptyString;
	}
	wxString content;
	f.ReadAll(&content, wxConvUTF8);
	f.Close();

	// The target is the last ProgramArguments string: the .app after `/usr/bin/open -a`, or the
	// daemon binary on its own. Plist XML is simple enough that regex extraction beats a full
	// parser dependency.
	wxRegEx re(wxT("<key>ProgramArguments</key>\\s*<array>(.*?)</array>"), wxRE_ADVANCED);
	if (!re.IsValid() || !re.Matches(content)) {
		return wxEmptyString;
	}
	wxString args = re.GetMatch(content, 1);
	wxRegEx str(wxT("<string>([^<]+)</string>"), wxRE_ADVANCED);
	wxString last;
	while (str.IsValid() && str.Matches(args)) {
		size_t start = 0, len = 0;
		str.GetMatch(&start, &len, 0);
		last = str.GetMatch(args, 1);
		args = args.Mid(start + len);
	}
	return last;
}

// Login Items keeps its switch in a store with no public API to read it.
bool BackendIsSwitchedOff()
{
	return false;
}

bool BackendWrite(const wxString &target, bool /*switchedOff*/)
{
	wxString dir = wxGetUserHome() + wxT("/Library/LaunchAgents");
	if (!wxFileName::DirExists(dir)) {
		if (!wxFileName::Mkdir(dir, 0755, wxPATH_MKDIR_FULL)) {
			return false;
		}
	}

	wxString xml;
	xml << wxT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
	xml << wxT("<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" ");
	xml << wxT("\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n");
	xml << wxT("<plist version=\"1.0\">\n");
	xml << wxT("<dict>\n");
	xml << wxT("    <key>Label</key>\n");
	xml << wxT("    <string>") << PLIST_LABEL << wxT("</string>\n");
	xml << wxT("    <key>ProgramArguments</key>\n");
	xml << wxT("    <array>\n");
	if (target.EndsWith(".app")) {
		xml << wxT("        <string>/usr/bin/open</string>\n");
		xml << wxT("        <string>-a</string>\n");
	}
	xml << wxT("        <string>") << target << wxT("</string>\n");
	xml << wxT("    </array>\n");
	xml << wxT("    <key>RunAtLoad</key>\n");
	xml << wxT("    <true/>\n");
	// KeepAlive=false so a user quit means quit: launchd would otherwise treat us
	// as a service to be respawned, fighting the user's deliberate close.
	xml << wxT("    <key>KeepAlive</key>\n");
	xml << wxT("    <false/>\n");
	xml << wxT("</dict>\n");
	xml << wxT("</plist>\n");

	wxFile f;
	if (!f.Create(PlistPath(), true /* overwrite */, 0644)) {
		return false;
	}
	bool ok = f.Write(xml, wxConvUTF8);
	f.Close();
	return ok;
}

bool BackendRemove()
{
	wxString path = PlistPath();
	if (!wxFileName::FileExists(path)) {
		return true;
	}
	return wxRemoveFile(path);
}

#else // assumed Linux / *BSD with XDG-compliant desktop env

// Linux: XDG Autostart spec. $XDG_CONFIG_HOME (falling back to ~/.config) is where the DE's
// "Startup Applications" GUI looks, so users can see and toggle the entry without a terminal --
// systemd user units do not show up there. https://specifications.freedesktop.org/autostart-
// spec/latest/
//
// A Flatpak cannot write the host's autostart directory, so the Background portal writes the entry
// for it, named after the app id, and the app reads it back from the host's config dir.

static bool InFlatpak()
{
	return wxGetEnv(wxT("FLATPAK_ID"), nullptr);
}

static wxString XdgAutostartDir()
{
	return XdgConfigDir() + wxT("/autostart");
}

static wxString DesktopFilePath()
{
	wxString appId;
	if (wxGetEnv(wxT("FLATPAK_ID"), &appId) && !appId.empty()) {
		return XdgAutostartDir() + wxT("/") + appId + wxT(".desktop");
	}
	return XdgAutostartDir() + wxT("/amule.desktop");
}

#ifdef HAVE_GIO
// Asks the Background portal for permission to run with no window, and sets the app's autostart
// entry to start `program` -- or removes it when `program` is empty: every request sets it.
static bool RequestPortalBackground(const wxString &program)
{
	GError *error = nullptr;
	GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
	if (conn == nullptr) {
		g_clear_error(&error);
		return false;
	}

	GVariantBuilder options;
	g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&options,
		"{sv}",
		"reason",
		g_variant_new_string("aMule keeps running in the background to continue your transfers."));
	g_variant_builder_add(&options, "{sv}", "autostart", g_variant_new_boolean(!program.empty()));
	const wxScopedCharBuffer utf8 = program.utf8_str();
	if (!program.empty()) {
		const char *const commandline[] = { utf8.data(), nullptr };
		g_variant_builder_add(&options, "{sv}", "commandline", g_variant_new_strv(commandline, -1));
	}

	// Returns once the request exists; a permission prompt, if any, follows on its own.
	GVariant *reply = g_dbus_connection_call_sync(conn,
		"org.freedesktop.portal.Desktop",
		"/org/freedesktop/portal/desktop",
		"org.freedesktop.portal.Background",
		"RequestBackground",
		g_variant_new("(sa{sv})", "", &options),
		nullptr,
		G_DBUS_CALL_FLAGS_NONE,
		5000,
		nullptr,
		&error);
	g_object_unref(conn);
	if (reply == nullptr) {
		g_clear_error(&error);
		return false;
	}
	g_variant_unref(reply);
	return true;
}
#endif

// The portal writes or removes the entry after it has replied.
template <typename Done> static bool WaitForPortal(Done done)
{
	for (int i = 0; i < 50 && !done(); ++i) {
		wxMilliSleep(100);
	}
	return done();
}

static wxString ReadDesktopFile()
{
	wxString path = DesktopFilePath();
	if (!wxFileName::FileExists(path)) {
		return wxEmptyString;
	}

	wxFile f(path, wxFile::read);
	if (!f.IsOpened()) {
		return wxEmptyString;
	}
	wxString content;
	f.ReadAll(&content, wxConvUTF8);
	f.Close();
	return content;
}

// The two lines a desktop switches an entry off with, rather than deleting the file: KDE sets
// Hidden, GNOME the other key, and each reads Hidden=true differently. Defaults when absent.
static void ReadSwitchLines(wxString &enabledLine, wxString &hiddenLine)
{
	enabledLine = wxT("X-GNOME-Autostart-enabled=true");
	hiddenLine = wxT("Hidden=false");
	wxStringTokenizer lines(ReadDesktopFile(), wxT("\n"));
	while (lines.HasMoreTokens()) {
		const wxString line = lines.GetNextToken().Trim(false).Trim(true);
		if (line.Lower().StartsWith(wxT("x-gnome-autostart-enabled="))) {
			enabledLine = line;
		} else if (line.Lower().StartsWith(wxT("hidden="))) {
			hiddenLine = line;
		}
	}
}

bool BackendIsSwitchedOff()
{
	wxString enabledLine;
	wxString hiddenLine;
	ReadSwitchLines(enabledLine, hiddenLine);
	return enabledLine.IsSameAs(wxT("X-GNOME-Autostart-enabled=false"), false) ||
	       hiddenLine.IsSameAs(wxT("Hidden=true"), false);
}

wxString BackendReadTargetPath()
{
	const wxString content = ReadDesktopFile();

	// Parse the Exec= line. .desktop syntax allows field-code expansion (%U, %f) after the
	// executable; the path is always the first whitespace-delimited token, and may be double-
	// quoted for paths containing spaces.
	wxStringTokenizer lines(content, wxT("\n"));
	while (lines.HasMoreTokens()) {
		wxString line = lines.GetNextToken().Trim(false).Trim(true);
		if (!line.StartsWith(wxT("Exec="))) {
			continue;
		}
		wxString value = line.Mid(5).Trim(false);
		if (value.empty()) {
			return wxEmptyString;
		}
		// The portal's entry reads `flatpak run --command=<program> <app id>`.
		const int command = value.Find(wxT("--command="));
		if (command != wxNOT_FOUND) {
			wxString program = value.Mid(command + 10).BeforeFirst(wxT(' '));
			program.Replace(wxT("'"), wxEmptyString);
			return program;
		}
		if (value[0] == wxT('"')) {
			size_t closing = value.find(wxT('"'), 1);
			if (closing != wxString::npos) {
				return value.SubString(1, closing - 1);
			}
		}
		size_t sp = value.find_first_of(wxT(" \t"));
		if (sp != wxString::npos) {
			return value.SubString(0, sp - 1);
		}
		return value;
	}
	return wxEmptyString;
}

bool BackendWrite(const wxString &executable, bool switchedOff)
{
	if (InFlatpak()) {
#ifdef HAVE_GIO
		// By program name: the portal starts it inside the app.
		const wxString program = wxFileName(executable).GetName();
		return RequestPortalBackground(program) &&
		       WaitForPortal([&] { return BackendReadTargetPath() == program; });
#else
		return false;
#endif
	}

	wxString dir = XdgAutostartDir();
	if (!wxFileName::DirExists(dir)) {
		// Mkdir -p: the XDG dir may not exist yet on a fresh install or on minimal
		// DEs that ship nothing there.
		if (!wxFileName::Mkdir(dir, 0755, wxPATH_MKDIR_FULL)) {
			return false;
		}
	}

	// Quote the path so embedded spaces (e.g. "/home/user/My Apps/amule")
	// survive the .desktop Exec= parser's tokenisation.
	wxString quotedExec = wxT("\"") + executable + wxT("\"");

	// A switched-off entry keeps the lines the desktop set.
	wxString enabledLine = wxT("X-GNOME-Autostart-enabled=true");
	wxString hiddenLine = wxT("Hidden=false");
	if (switchedOff) {
		ReadSwitchLines(enabledLine, hiddenLine);
	}

	// Standard XDG Autostart fields. X-GNOME-Autostart-enabled is widely recognised even
	// outside GNOME and makes the entry toggleable from the DE settings GUI without us
	// rewriting the file; a rewrite keeps whatever the user set there.
	wxString content;
	content << wxT("[Desktop Entry]\n");
	content << wxT("Type=Application\n");
	content << wxT("Name=aMule\n");
	content << wxT("Comment=Start aMule when the user logs in\n");
	content << wxT("Exec=") << quotedExec << wxT("\n");
	content << wxT("Terminal=false\n");
	content << enabledLine << wxT("\n");
	content << hiddenLine << wxT("\n");

	wxFile f;
	if (!f.Create(DesktopFilePath(), true /* overwrite */, 0644)) {
		return false;
	}
	bool ok = f.Write(content, wxConvUTF8);
	f.Close();
	return ok;
}

bool BackendRemove()
{
	wxString path = DesktopFilePath();
	if (!wxFileName::FileExists(path)) {
		return true; // already absent → success
	}
	if (InFlatpak()) {
#ifdef HAVE_GIO
		return RequestPortalBackground(wxEmptyString) &&
		       WaitForPortal([&] { return !wxFileName::FileExists(path); });
#else
		return false;
#endif
	}
	return wxRemoveFile(path);
}

#endif

} // namespace

void AutostartManager::RequestFlatpakBackground()
{
#if defined(HAVE_GIO) && !defined(__WXMSW__) && !defined(__WXMAC__) && !defined(__WXOSX__)
	// With an entry, the permission came with it, and a request would rewrite the entry.
	if (InFlatpak() && BackendReadTargetPath().empty()) {
		RequestPortalBackground(wxEmptyString);
	}
#endif
}
