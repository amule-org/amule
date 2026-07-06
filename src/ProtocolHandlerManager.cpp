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

#include "ProtocolHandlerManager.h"

#include <wx/app.h>      // Needed for wxTheApp
#include <wx/filename.h> // Needed for wxFileName
#include <wx/log.h>      // Needed for wxLogDebug
#include <wx/stdpaths.h> // Needed for wxStandardPaths
#include <wx/string.h>   // Needed for wxString
#include <wx/textfile.h> // Needed for wxTextFile in ProtocolHandler_QueueSchemeLink
#include <wx/uri.h>      // Needed for wxURI::Unescape on browser-delivered URLs

#include "Preferences.h" // Needed for thePrefs::GetConfigDir in QueueSchemeLink

#include <vector> // Needed for std::vector (Windows registry buffer +
		  // Linux mimeapps.list line vector)

#ifdef __WXMSW__
#include <windows.h>
#include <cwchar> // Needed for wcslen
#else
#include <climits>      // Needed for PATH_MAX
#include <stdlib.h>     // Needed for realpath
#include <wx/file.h>    // Needed for wxFile
#include <wx/filefn.h>  // Needed for wxRemoveFile
#include <wx/tokenzr.h> // Needed for wxStringTokenizer
#include <wx/utils.h>   // Needed for wxGetEnv / wxGetUserHome
#endif

// Forward declarations for the macOS backend entry points implemented
// in ProtocolHandlerManager_mac.mm. Declared at file scope (external
// linkage, global namespace) so the .mm's plain-C++ definitions link
// against them. Keeping them out of the anonymous namespace below is
// what actually makes the linker resolution work — a static-linkage
// declaration cannot bind to a global-scope definition.
#if defined(__WXMAC__) || defined(__WXOSX__)
wxString MacReadHandler(UriScheme scheme);
bool MacWrite(UriScheme scheme, const wxString &canonicalExe);
bool MacRemove(UriScheme scheme);
wxString MacOwnBundleId();
#endif

namespace
{
// Short lowercase scheme name — used in every backend's key path
// (registry, mimeapps.list entry, LaunchServices scheme argument).
const wchar_t *SchemeName(UriScheme scheme)
{
	switch (scheme) {
	case UriScheme::Ed2k:
		return L"ed2k";
	case UriScheme::Magnet:
		return L"magnet";
	}
	return L"";
}

// UTF-8 flavour for POSIX backends.
const char *SchemeNameUtf8(UriScheme scheme)
{
	switch (scheme) {
	case UriScheme::Ed2k:
		return "ed2k";
	case UriScheme::Magnet:
		return "magnet";
	}
	return "";
}

// Per-backend low-level helpers. Return raw OS state without applying
// aMule-specific policy (identity comparison etc.). Platform-specific
// definitions live near the bottom of this file.
//
// Reads the currently-registered handler's identifier for `scheme`.
// The identifier shape differs by OS: registry command string on
// Windows, .desktop id / Exec= path on Linux, bundle id on macOS.
// Empty on "no handler set".
wxString BackendReadHandler(UriScheme scheme);

// Sets aMule as the default handler for `scheme`. `canonicalExe` is
// the resolved absolute path of the running binary. Silent overwrite —
// the caller owns the "another app is currently the default" UX.
bool BackendWrite(UriScheme scheme, const wxString &canonicalExe);

// Removes aMule as the default handler for `scheme` if we're the
// current handler. Idempotent — returns true if we weren't the
// current handler either.
bool BackendRemove(UriScheme scheme);

// True iff `raw` (the return value of BackendReadHandler) refers to
// the aMule binary/bundle at `canonicalExe`. Encapsulates the per-OS
// identity check (registry command → executable path, .desktop id →
// our desktop file, bundle id → our bundle id).
bool BackendIdentityMatches(const wxString &raw, const wxString &canonicalExe);

// True iff `raw` refers to *any* aMule binary/bundle (identity check
// ignoring path drift). Used by SelfHealOnStartup: if the registered
// handler is us-but-at-a-stale-path we rewrite; if it's a third-party
// handler we leave it alone.
bool BackendIsUs(const wxString &raw);
} // namespace

wxString ProtocolHandlerManager::GetCanonicalExecutablePath()
{
	// wxStandardPaths::GetExecutablePath() wraps the OS native call
	// (GetModuleFileNameW on Windows, _NSGetExecutablePath on macOS,
	// /proc/self/exe readlink on Linux). On POSIX we resolve any
	// intermediate symlinks via realpath() so AppImage / .app bundle
	// moves are detected correctly by SelfHealOnStartup. Same helper
	// as AutostartManager — duplicated here so this class stays
	// self-contained.
	wxString raw = wxStandardPaths::Get().GetExecutablePath();

#ifndef __WXMSW__
	if (raw.empty()) {
		return raw;
	}
	char resolved[PATH_MAX];
	if (realpath(raw.mb_str(wxConvUTF8), resolved) != nullptr) {
		return wxString::FromUTF8(resolved);
	}
	// realpath failed — fall through to the raw path.
#endif

	return raw;
}

#if defined(__WXMAC__)
// C shim defined in ProtocolHandlerManager_mac.mm — NSLog with the
// [amuleurl] prefix so all diagnostics land under one Console.app filter.
extern "C" void amule_url_log(const char *msg);
#define AMULE_URL_LOG(fmtwx, ...) \
	do { \
		wxString _s = wxString::Format((fmtwx), ##__VA_ARGS__); \
		amule_url_log((const char *)_s.mb_str(wxConvUTF8)); \
	} while (0)
#else
#define AMULE_URL_LOG(fmtwx, ...) ((void)0)
#endif

void ProtocolHandler_QueueSchemeLink(const wxString &url)
{
	// Called via wxTheApp->CallAfter from the mac URL AE handler.
	// Cannot use AddLogLineNS: on amulegui cold-launch this may run
	// before amuledlg is up and the GUI log path would deref null.
	if (url.empty()) {
		return;
	}
	// Browsers percent-encode ed2k:// pipes; wxURI::Unescape restores
	// the literals CMagnetED2KConverter / the eD2k parser expect.
	wxString decoded = wxURI::Unescape(url);
	const wxString &cfgDir = thePrefs::GetConfigDir();
	AMULE_URL_LOG(wxT("queue: '%s' → '%s'"), url, decoded);

	wxTextFile ed2kFile(cfgDir + wxT("ED2KLinks"));
	if (!ed2kFile.Exists()) {
		ed2kFile.Create();
	}
	if (ed2kFile.Open()) {
		ed2kFile.AddLine(decoded);
		ed2kFile.AddLine(wxT("RAISE_DIALOG"));
		ed2kFile.Write();
		ed2kFile.Close();
		AMULE_URL_LOG(wxT("wrote ED2KLinks OK"));
	} else {
		AMULE_URL_LOG(wxT("failed to open ED2KLinks for write"));
	}
	// Do NOT call AddLinksFromFile here — this function is invoked
	// via wxTheApp->CallAfter from the Apple Event handler which may
	// fire before theApp->downloadqueue is fully wired at cold launch.
	// Instead we rely on the ~1 s polling loop in both
	// CDownloadQueue::Process (monolithic + daemon) and
	// CamuleRemoteGuiApp::UpdateStats (remote GUI) to drain the file
	// through AddLinksFromFile → AddLink on the next tick.
}

bool ProtocolHandlerManager::IsEnabled(UriScheme scheme)
{
	wxString raw = BackendReadHandler(scheme);
	if (raw.empty()) {
		return false;
	}
	return BackendIsUs(raw);
}

bool ProtocolHandlerManager::Enable(UriScheme scheme)
{
	wxString exe = GetCanonicalExecutablePath();
	if (exe.empty()) {
		wxLogDebug(wxT("ProtocolHandlerManager::Enable: no executable path resolved, refusing to "
			       "write a broken handler entry"));
		return false;
	}
	return BackendWrite(scheme, exe);
}

bool ProtocolHandlerManager::Disable(UriScheme scheme)
{
	return BackendRemove(scheme);
}

wxString ProtocolHandlerManager::GetCurrentHandler(UriScheme scheme)
{
	wxString raw = BackendReadHandler(scheme);
	if (raw.empty() || BackendIsUs(raw)) {
		return wxEmptyString;
	}
	return raw;
}

void ProtocolHandlerManager::SelfHealOnStartup()
{
	wxString canonical = GetCanonicalExecutablePath();
	if (canonical.empty()) {
		return;
	}

	const UriScheme schemes[] = { UriScheme::Ed2k, UriScheme::Magnet };
	for (UriScheme scheme : schemes) {
		wxString raw = BackendReadHandler(scheme);
		if (raw.empty()) {
			// No handler set — disabling is always a deliberate
			// user choice we don't second-guess.
			continue;
		}
		if (!BackendIsUs(raw)) {
			// A third-party app currently owns this scheme —
			// leave it alone.
			continue;
		}
		if (BackendIdentityMatches(raw, canonical)) {
			// We own it AND the registered path already matches
			// the running binary. Nothing to do.
			continue;
		}
		// We own it but the registered path drifted (user moved
		// AppImage / .app / install dir). Rewrite.
		wxLogDebug(wxT("ProtocolHandlerManager::SelfHealOnStartup: rewriting %s handler from '%s' to "
			       "'%s'"),
			SchemeNameUtf8(scheme),
			raw.c_str(),
			canonical.c_str());
		BackendWrite(scheme, canonical);
	}
}

// --------------------------------------------------------------------
// Platform backends
// --------------------------------------------------------------------

namespace
{

#if defined(__WXMSW__)

// Windows: per-user URL Protocol under HKCU\Software\Classes\<scheme>.
// The layout LaunchServices-equivalent Windows shell uses:
//
//   HKCU\Software\Classes\ed2k\                     (default) = "URL:eD2k Protocol"
//                              \                   URL Protocol   = ""
//                              \DefaultIcon\        (default) = "<amule.exe>,0"
//                              \shell\open\command\ (default) = "\"<amule.exe>\" \"%1\""
//
// Per-user (HKCU not HKLM) so toggling never needs elevation.

static wxString SubKey(UriScheme scheme)
{
	return wxString::Format(wxT("Software\\Classes\\%s"), SchemeName(scheme));
}

static bool WriteStringValue(
	HKEY root, const wchar_t *subKey, const wchar_t *valueName, const wxString &value)
{
	HKEY hKey;
	if (RegCreateKeyExW(root, subKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr) !=
		ERROR_SUCCESS) {
		return false;
	}
	const wchar_t *wstr = value.wc_str();
	DWORD cb = static_cast<DWORD>((wcslen(wstr) + 1) * sizeof(wchar_t));
	LSTATUS rc = RegSetValueExW(hKey, valueName, 0, REG_SZ, reinterpret_cast<const BYTE *>(wstr), cb);
	RegCloseKey(hKey);
	return rc == ERROR_SUCCESS;
}

static wxString ReadStringValue(HKEY root, const wchar_t *subKey, const wchar_t *valueName)
{
	HKEY hKey;
	if (RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
		return wxEmptyString;
	}

	DWORD type = 0;
	DWORD cb = 0;
	LSTATUS rc = RegQueryValueExW(hKey, valueName, nullptr, &type, nullptr, &cb);
	if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || cb == 0) {
		RegCloseKey(hKey);
		return wxEmptyString;
	}

	size_t wlen = (cb + sizeof(wchar_t) - 1) / sizeof(wchar_t);
	std::vector<wchar_t> buf(wlen + 1, L'\0');
	rc = RegQueryValueExW(hKey, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(buf.data()), &cb);
	RegCloseKey(hKey);
	if (rc != ERROR_SUCCESS) {
		return wxEmptyString;
	}
	return wxString(buf.data());
}

// Extract the executable path from a Windows "shell\open\command"
// value: `"C:\...\amule.exe" "%1"` → `C:\...\amule.exe`. Also handles
// the unquoted `C:\...\amule.exe %1` form for defensiveness.
static wxString ExtractExeFromCommand(const wxString &command)
{
	if (command.empty()) {
		return wxEmptyString;
	}
	if (command[0] == wxT('"')) {
		size_t closing = command.find(wxT('"'), 1);
		if (closing != wxString::npos) {
			return command.SubString(1, closing - 1);
		}
	}
	size_t sp = command.find_first_of(wxT(" \t"));
	if (sp != wxString::npos) {
		return command.SubString(0, sp - 1);
	}
	return command;
}

wxString BackendReadHandler(UriScheme scheme)
{
	wxString cmd = ReadStringValue(HKEY_CURRENT_USER,
		(SubKey(scheme) + wxT("\\shell\\open\\command")).wc_str(),
		nullptr /* default value */);
	return ExtractExeFromCommand(cmd);
}

bool BackendWrite(UriScheme scheme, const wxString &canonicalExe)
{
	wxString base = SubKey(scheme);

	// URL Protocol scheme entry — the sentinel value that tells the
	// Windows shell "this is a URL protocol, not a filetype".
	wxString schemeDescription = wxString::Format(
		wxT("URL:%s Protocol"), scheme == UriScheme::Ed2k ? wxT("eD2k") : wxT("Magnet"));
	if (!WriteStringValue(HKEY_CURRENT_USER, base.wc_str(), nullptr, schemeDescription)) {
		return false;
	}
	if (!WriteStringValue(HKEY_CURRENT_USER, base.wc_str(), L"URL Protocol", wxEmptyString)) {
		return false;
	}

	// DefaultIcon: the icon Explorer / Edge show next to the "Open
	// with aMule?" prompt. "<exe>,0" = first icon resource in the
	// executable.
	wxString iconRef = wxString::Format(wxT("\"%s\",0"), canonicalExe);
	if (!WriteStringValue(HKEY_CURRENT_USER, (base + wxT("\\DefaultIcon")).wc_str(), nullptr, iconRef)) {
		return false;
	}

	// The actual command — quote both the executable and %1 so paths
	// with spaces and URIs with query strings survive Windows' command
	// tokeniser intact.
	wxString command = wxString::Format(wxT("\"%s\" \"%%1\""), canonicalExe);
	return WriteStringValue(
		HKEY_CURRENT_USER, (base + wxT("\\shell\\open\\command")).wc_str(), nullptr, command);
}

// Recursively delete a registry key and everything under it. Registry
// APIs don't offer a one-call recursive delete on all Windows versions
// we support, so implement it explicitly.
static LSTATUS DeleteKeyRecursive(HKEY root, const wchar_t *subKey)
{
	HKEY hKey;
	LSTATUS rc = RegOpenKeyExW(root, subKey, 0, KEY_READ | KEY_WRITE, &hKey);
	if (rc == ERROR_FILE_NOT_FOUND) {
		return ERROR_SUCCESS;
	}
	if (rc != ERROR_SUCCESS) {
		return rc;
	}

	// Enumerate + delete subkeys first. Registry enumeration is
	// invalidated by deletion, so we re-enumerate from index 0 each
	// pass until no subkeys remain.
	for (;;) {
		wchar_t name[256];
		DWORD cch = 256;
		rc = RegEnumKeyExW(hKey, 0, name, &cch, nullptr, nullptr, nullptr, nullptr);
		if (rc == ERROR_NO_MORE_ITEMS) {
			break;
		}
		if (rc != ERROR_SUCCESS) {
			RegCloseKey(hKey);
			return rc;
		}
		wxString childPath = wxString(subKey) + wxT("\\") + wxString(name);
		LSTATUS childRc = DeleteKeyRecursive(root, childPath.wc_str());
		if (childRc != ERROR_SUCCESS) {
			RegCloseKey(hKey);
			return childRc;
		}
	}
	RegCloseKey(hKey);

	return RegDeleteKeyW(root, subKey);
}

bool BackendRemove(UriScheme scheme)
{
	// Only remove if we're the current handler — protects a
	// user's manual override or a third-party handler that happens
	// to have written under the same key later.
	wxString current = BackendReadHandler(scheme);
	if (current.empty()) {
		return true; // already absent
	}
	if (!BackendIsUs(current)) {
		return true; // not ours to remove
	}

	LSTATUS rc = DeleteKeyRecursive(HKEY_CURRENT_USER, SubKey(scheme).wc_str());
	return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
}

bool BackendIdentityMatches(const wxString &raw, const wxString &canonicalExe)
{
	// Registry values on Windows can round-trip with different case
	// on paths (C:\Program Files\ vs c:\program files\). Compare
	// case-insensitively to match Windows filesystem semantics.
	return raw.IsSameAs(canonicalExe, false);
}

bool BackendIsUs(const wxString &raw)
{
	if (raw.empty()) {
		return false;
	}
	// "us" == the currently-running binary, keyed by basename so
	// path drift (user moved the install dir since registering) still
	// counts as our own registration — SelfHealOnStartup rewrites
	// the full path in that case. Per-binary comparison is what makes
	// the amule/amulegui differentiation work: in a remote-GUI setup
	// the checkbox in amulegui's prefs correctly reads "unchecked"
	// when amule.exe is the current handler, and vice versa.
	wxString ownExe = ProtocolHandlerManager::GetCanonicalExecutablePath();
	if (ownExe.empty()) {
		return false;
	}
	wxFileName rawFn(raw);
	wxFileName ownFn(ownExe);
	return rawFn.GetFullName().IsSameAs(ownFn.GetFullName(), false);
}

#elif defined(__WXMAC__) || defined(__WXOSX__)

// The macOS backend is Objective-C++ (needs LSCopyDefaultHandlerForURLScheme
// / LSSetDefaultHandlerForURLScheme from ApplicationServices).
// Implementation lives in ProtocolHandlerManager_mac.mm; the per-scheme
// entry points have external linkage at file scope (declared just
// before this anonymous namespace opens) so the linker can resolve
// them against the .mm's global-scope definitions.

wxString BackendReadHandler(UriScheme scheme)
{
	return ::MacReadHandler(scheme);
}

bool BackendWrite(UriScheme scheme, const wxString &canonicalExe)
{
	return ::MacWrite(scheme, canonicalExe);
}

bool BackendRemove(UriScheme scheme)
{
	// LaunchServices has no "remove default" call — the model is
	// "some app is always the default". Best we can do on Disable
	// is check that we're currently the default and, if so, no-op
	// (the user has to pick another app from the OS's "Open With"
	// prompt to actually stop us from receiving clicks). Never
	// silently reassign to a third-party app — that would be a
	// worse UX than leaving us bound.
	//
	// Return true so the prefs toggle reads as "disable succeeded"
	// from the user's perspective; the checkbox flipping off is the
	// visible signal that we've stepped back. On next click of a
	// scheme URL macOS will show the "no default handler picked
	// yet" chooser and the user can select another app.
	//
	// The reality is that on Sequoia+ this is unavoidable: Apple
	// blocked programmatic clearing of scheme handlers to prevent
	// malicious deregistration. Documented in the class comment.
	return true;
}

bool BackendIdentityMatches(const wxString &raw, const wxString & /*canonicalExe*/)
{
	// On macOS the identifier is a bundle id string (e.g.
	// "org.amule.amule"), not a path. Path drift doesn't apply —
	// LaunchServices tracks the bundle by id and finds the current
	// location automatically. Identity match is bundle-id equality.
	return raw.IsSameAs(::MacOwnBundleId(), false);
}

bool BackendIsUs(const wxString &raw)
{
	return BackendIdentityMatches(raw, wxEmptyString);
}

#else // assumed Linux / *BSD with XDG-compliant desktop env

// Linux: per-user default scheme handler in
// $XDG_CONFIG_HOME/mimeapps.list (falls back to ~/.config/mimeapps.list).
// [Default Applications] section maps x-scheme-handler/<scheme> to a
// .desktop file id. The .desktop file itself must also declare
// MimeType= including x-scheme-handler/<scheme> so file managers /
// browsers consider it a valid handler in the first place; that is
// shipped statically in packaging/... /org.amule.aMule.desktop.
// https://specifications.freedesktop.org/mime-apps-spec/latest/
//
// We do NOT depend on xdg-mime (part of xdg-utils) being installed —
// several minimal distros / containers ship without it. Direct ini
// write covers those.

// Which .desktop id represents the currently-running binary.
// amule (monolithic + daemon) → org.amule.aMule.desktop; amulegui
// (remote GUI) → org.amule.aMule.gui.desktop. The daemon is not
// user-facing but we register it against the monolithic desktop id
// too so `amuled --configure-protocols on` still points clicks at a
// user-visible entry that the DE can open. Basename lookup on the
// running exe keeps this working across the install/AppImage variants.
static wxString OwnDesktopId()
{
	wxString ownExe = ProtocolHandlerManager::GetCanonicalExecutablePath();
	wxFileName fn(ownExe);
	wxString base = fn.GetFullName();
	if (base.IsSameAs(wxT("amulegui"), false)) {
		return wxT("org.amule.aMule.gui.desktop");
	}
	return wxT("org.amule.aMule.desktop");
}

static wxString MimeAppsPath()
{
	wxString xdg;
	if (wxGetEnv(wxT("XDG_CONFIG_HOME"), &xdg) && !xdg.empty()) {
		return xdg + wxT("/mimeapps.list");
	}
	return wxGetUserHome() + wxT("/.config/mimeapps.list");
}

static wxString SchemeKey(UriScheme scheme)
{
	return wxString::Format(wxT("x-scheme-handler/%s"), SchemeNameUtf8(scheme));
}

// Parse an ini-style file into (section, key, value) triples for
// straightforward editing. Preserves original line order via
// vector-of-lines so a round-trip write doesn't reorder unrelated
// entries. Comments and blank lines are preserved as-is.
struct IniLine
{
	wxString section; // empty for pre-first-section lines (rare)
	wxString raw;     // original line text (for comments / blanks)
	bool isEntry = false;
	wxString key;
	wxString value;
};

static std::vector<IniLine> ReadIniLines(const wxString &path)
{
	std::vector<IniLine> lines;
	wxFile f(path, wxFile::read);
	if (!f.IsOpened()) {
		return lines;
	}
	wxString content;
	f.ReadAll(&content, wxConvUTF8);
	f.Close();

	wxString section;
	wxStringTokenizer tok(content, wxT("\n"), wxTOKEN_RET_EMPTY_ALL);
	while (tok.HasMoreTokens()) {
		wxString line = tok.GetNextToken();
		wxString trimmed = line;
		trimmed.Trim(false).Trim(true);
		IniLine il;
		il.section = section;
		il.raw = line;
		if (trimmed.StartsWith(wxT("[")) && trimmed.EndsWith(wxT("]"))) {
			section = trimmed.Mid(1, trimmed.length() - 2);
			il.section = section;
		} else if (!trimmed.empty() && !trimmed.StartsWith(wxT("#"))) {
			int eq = trimmed.Find(wxT('='));
			if (eq != wxNOT_FOUND) {
				il.isEntry = true;
				il.key = trimmed.Mid(0, eq);
				il.value = trimmed.Mid(eq + 1);
			}
		}
		lines.push_back(il);
	}
	return lines;
}

static bool WriteIniLines(const wxString &path, const std::vector<IniLine> &lines)
{
	// Ensure parent directory exists.
	wxFileName fn(path);
	if (!fn.DirExists()) {
		if (!wxFileName::Mkdir(fn.GetPath(), 0755, wxPATH_MKDIR_FULL)) {
			return false;
		}
	}

	wxString out;
	for (size_t i = 0; i < lines.size(); ++i) {
		const IniLine &il = lines[i];
		if (il.isEntry) {
			out << il.key << wxT("=") << il.value;
		} else {
			out << il.raw;
		}
		if (i + 1 != lines.size()) {
			out << wxT("\n");
		}
	}

	wxFile f;
	if (!f.Create(path, true /* overwrite */, 0644)) {
		return false;
	}
	bool ok = f.Write(out, wxConvUTF8);
	f.Close();
	return ok;
}

wxString BackendReadHandler(UriScheme scheme)
{
	std::vector<IniLine> lines = ReadIniLines(MimeAppsPath());
	wxString needle = SchemeKey(scheme);
	for (const IniLine &il : lines) {
		if (il.section != wxT("Default Applications")) {
			continue;
		}
		if (!il.isEntry) {
			continue;
		}
		if (il.key == needle) {
			// mimeapps.list allows semicolon-separated fallback
			// chains ("foo.desktop;bar.desktop;"). The first
			// entry is the effective default.
			wxString first = il.value.BeforeFirst(wxT(';')).Trim(false).Trim(true);
			return first;
		}
	}
	return wxEmptyString;
}

bool BackendWrite(UriScheme scheme, const wxString &canonicalExe)
{
	// canonicalExe is unused on Linux — the mimeapps.list entry
	// references a .desktop file id (org.amule.aMule.desktop), not
	// an executable path. Path drift is handled at a different
	// layer: the .desktop's Exec= line is resolved through PATH /
	// AppImage integration by the DE at click time.
	(void)canonicalExe;

	wxString path = MimeAppsPath();
	std::vector<IniLine> lines = ReadIniLines(path);
	wxString needle = SchemeKey(scheme);

	// Update in place if the entry exists in [Default Applications];
	// otherwise append.
	bool sectionSeen = false;
	int lastLineOfSection = -1;
	for (size_t i = 0; i < lines.size(); ++i) {
		if (lines[i].section == wxT("Default Applications")) {
			sectionSeen = true;
			lastLineOfSection = static_cast<int>(i);
			if (lines[i].isEntry && lines[i].key == needle) {
				lines[i].value = OwnDesktopId();
				return WriteIniLines(path, lines);
			}
		}
	}

	// Not found — insert. If the section exists, insert as its last
	// entry; otherwise append the section header + entry to the end.
	IniLine entry;
	entry.section = wxT("Default Applications");
	entry.isEntry = true;
	entry.key = needle;
	entry.value = OwnDesktopId();

	if (sectionSeen) {
		lines.insert(lines.begin() + lastLineOfSection + 1, entry);
	} else {
		if (!lines.empty()) {
			IniLine blank;
			lines.push_back(blank);
		}
		IniLine header;
		header.section = wxT("Default Applications");
		header.raw = wxT("[Default Applications]");
		lines.push_back(header);
		lines.push_back(entry);
	}
	return WriteIniLines(path, lines);
}

bool BackendRemove(UriScheme scheme)
{
	wxString path = MimeAppsPath();
	if (!wxFileName::FileExists(path)) {
		return true;
	}

	std::vector<IniLine> lines = ReadIniLines(path);
	wxString needle = SchemeKey(scheme);

	bool changed = false;
	for (auto it = lines.begin(); it != lines.end();) {
		if (it->section == wxT("Default Applications") && it->isEntry && it->key == needle &&
			it->value == OwnDesktopId()) {
			it = lines.erase(it);
			changed = true;
		} else {
			++it;
		}
	}
	if (!changed) {
		return true;
	}
	return WriteIniLines(path, lines);
}

bool BackendIdentityMatches(const wxString &raw, const wxString & /*canonicalExe*/)
{
	// On Linux the identifier is a .desktop file id, not a path.
	// Path drift is handled by the DE's .desktop lookup (XDG
	// data-dirs walk), not by us rewriting mimeapps.list.
	return raw == OwnDesktopId();
}

bool BackendIsUs(const wxString &raw)
{
	return raw == OwnDesktopId();
}

#endif

} // namespace
// File_checked_for_headers
