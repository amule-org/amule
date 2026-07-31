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

//
// This file is for functions common to all three apps (amule, amuled, amulegui),
// but preprocessor-dependent (using theApp, thePrefs), so it is compiled separately for each app.
//

#include <wx/wx.h>
#include <wx/cmdline.h>  // Needed for wxCmdLineParser
#include <wx/evtloop.h>  // Needed for wxEventLoopBase
#include <wx/filename.h> // Needed for wxFileName
#include <wx/filesys.h>  // Needed for wxFileSystem::URLToFileName

#include "InstanceLock.h" // Needed for InstanceLock (replaces wxSingleInstanceChecker on POSIX)
#include <wx/textfile.h>  // Needed for wxTextFile
#include <wx/config.h>    // Do_not_auto_remove (win32)
#include <wx/fileconf.h>

#ifdef __WXGTK__
#include <stdlib.h> // Needed for getenv (IsWaylandSession)
#include <string.h> // Needed for strcmp (IsWaylandSession)
#endif

#include "amule.h"                  // Interface declarations.
#include "AutostartManager.h"       // Needed for --configure-autostart handling
#include "ProtocolHandlerManager.h" // Needed for --configure-protocols handling
#include "CamuleFileConfig.h"       // CamuleFileConfig + CCtypeAsciiScope (#852)
#include <common/FileFunctions.h>   // Needed for RestrictToOwner
#include <common/Format.h>          // Needed for CFormat
#include "CFile.h"                  // Needed for CFile
#include "ED2KLink.h"               // Needed for command line passing of links
#include "FileLock.h"               // Needed for CFileLock
#include "GuiEvents.h"              // Needed for Notify_*
#include "KnownFile.h"
#include "Logger.h"
#include "MagnetURI.h"      // Needed for CMagnetURI
#include "MuleCollection.h" // Needed for expanding .emulecollection arguments
#include "Preferences.h"
#include "ScopedPtr.h"
#include "MuleVersion.h" // Needed for GetMuleVersion()

#ifndef CLIENT_GUI
#include "DownloadQueue.h"
#endif

CamuleAppCommon::CamuleAppCommon()
{
	m_singleInstance = NULL;
	ec_config = false;
	m_geometryEnabled = false;
	m_disableFatal = false;
	if (IsRemoteGui()) {
		m_appName = "aMuleGUI";
		m_configFile = "remote.conf";
		m_logFile = "remotelogfile";
	} else {
		m_configFile = "amule.conf";
		m_logFile = "logfile";

		if (IsDaemon()) {
			m_appName = "aMuleD";
		} else {
			m_appName = "aMule";
		}
	}
}

CamuleAppCommon::~CamuleAppCommon()
{
	delete m_singleInstance;
}

#ifdef __WXGTK__
bool CamuleAppCommon::IsWaylandSession()
{
	// Explicit GDK_BACKEND=x11 forces the app onto XWayland - OS
	// minimize events come through reliably, so treat it as X11.
	// This is the documented user workaround for "I want MinToTray
	// on Wayland": launch with `GDK_BACKEND=x11 amule`. Same trick
	// Discord users adopt to get their tray icon back on Wayland.
	if (const char *gb = getenv("GDK_BACKEND")) {
		if (strncmp(gb, "x11", 3) == 0) {
			return false;
		}
	}
	// WAYLAND_DISPLAY is set by Wayland servers to the socket name
	// (e.g. "wayland-0") for any client running under that session.
	// XDG_SESSION_TYPE is the systemd-logind hint and is also set
	// to "wayland" on every common distro. Either non-empty match
	// is treated as a Wayland session.
	if (const char *wd = getenv("WAYLAND_DISPLAY")) {
		if (wd[0] != '\0') {
			return true;
		}
	}
	if (const char *st = getenv("XDG_SESSION_TYPE")) {
		if (strcmp(st, "wayland") == 0) {
			return true;
		}
	}
	return false;
}
#endif

void CamuleAppCommon::RefreshSingleInstanceChecker()
{
	// Reacquire after daemonization fork(). POSIX advisory locks are
	// owned by the parent process, not inherited by the child, so the
	// forked daemon needs to relock the same file. Historically this
	// path was disabled on __WXMAC__ + AMULE_DAEMON because linking
	// wxSingleInstanceChecker into a wxAppConsole (headless) binary
	// pulled in Cocoa framework symbols. InstanceLock has no wx-GUI
	// dependencies on POSIX (fcntl only), so amuled/Mac now has
	// working single-instance detection too.
	delete m_singleInstance;
	m_singleInstance = new InstanceLock();
	m_singleInstance->Acquire("muleLock", thePrefs::GetConfigDir());
}

void CamuleAppCommon::ReleaseSingleInstance()
{
	// ~InstanceLock() -> Release() unlinks the lock file and drops the
	// fcntl lock. Needed because OnExit() terminates via std::_Exit(),
	// which skips ~CamuleAppCommon and so would leave the file behind.
	delete m_singleInstance;
	m_singleInstance = nullptr;
}

bool CamuleAppCommon::DeferShutDownToOuterLoop(const std::function<void()> &retry)
{
	wxEventLoopBase *active = wxEventLoopBase::GetActive();
	if (!active || active->IsMain()) {
		return false;
	}

	// Exit() only asks the loop to stop, so the frames between here and the
	// ShowModal() that started it still have to unwind before the teardown
	// can safely run -- hence the retry rather than falling through.
	active->Exit();

	if (!m_deferredShutDown) {
		wxTheApp->Bind(wxEVT_IDLE, &CamuleAppCommon::OnDeferredShutDownIdle, this);
	}
	m_deferredShutDown = retry;
	return true;
}

void CamuleAppCommon::OnDeferredShutDownIdle(wxIdleEvent &evt)
{
	evt.Skip();

	wxEventLoopBase *active = wxEventLoopBase::GetActive();
	if (active && !active->IsMain()) {
		// A further loop was entered (or this one has not unwound yet).
		// Keep asking it to stop and look again on the next idle.
		active->Exit();
		evt.RequestMore();
		return;
	}

	wxTheApp->Unbind(wxEVT_IDLE, &CamuleAppCommon::OnDeferredShutDownIdle, this);

	// Clear before running: the teardown re-enters ShutDown(), which must see
	// no outstanding retry or it would bind a second idle handler.
	std::function<void()> retry;
	retry.swap(m_deferredShutDown);
	retry();
}

void CamuleAppCommon::AddLinksFromFile()
{
	const wxString fullPath = thePrefs::GetConfigDir() + "ED2KLinks";
	if (!wxFile::Exists(fullPath)) {
		return;
	}

	// Attempt to lock the ED2KLinks file.
	CFileLock lock((const char *)unicode2char(fullPath));

	wxTextFile file(fullPath);
	if (file.Open()) {
		// Group the links by category and hand each group to AddLinks() in
		// one call. A collection expands to hundreds of lines, and the
		// per-link AddLink() path costs one EC round trip - and potentially
		// one error dialog - per link on the remote GUI. Order within a
		// category is preserved; categories run in first-seen order.
		std::vector<std::pair<uint8, wxArrayString>> batches;

		for (unsigned int i = 0; i < file.GetLineCount(); i++) {
			wxString line = file.GetLine(i).Strip(wxString::both);

			if (line.IsEmpty()) {
				continue;
			}
			// Special case! used by a secondary running mule to raise this one.
			if (line == "RAISE_DIALOG") {
				Notify_ShowGUI();
				continue;
			}
			unsigned long category = 0;
			if (line.AfterLast(':').ToULong(&category) == true) {
				line = line.BeforeLast(':');
			} else { // If ToULong returns false the category still can have been changed!
				 // This is fixed in wx 2.9
				category = 0;
			}

			const uint8 cat = static_cast<uint8>(category);
			size_t slot = 0;
			while (slot < batches.size() && batches[slot].first != cat) {
				++slot;
			}
			if (slot == batches.size()) {
				batches.emplace_back(cat, wxArrayString());
			}
			batches[slot].second.Add(line);
		}

		file.Close();

		// AddLinks() logs each failure and raises a single aggregated alert
		// for the batch, so there is nothing to count or report here.
		for (const auto &batch : batches) {
			theApp->downloadqueue->AddLinks(batch.second, batch.first);
		}
	} else {
		AddLogLineNS(_("Failed to open ED2KLinks file."));
	}

	// Delete the file.
	wxRemoveFile(thePrefs::GetConfigDir() + "ED2KLinks");
}

// Returns a magnet ed2k URI
wxString CamuleAppCommon::CreateMagnetLink(const CAbstractFile *f)
{
	CMagnetURI uri;

	uri.AddField("dn", f->GetFileName().Cleanup(false).GetPrintable());
	uri.AddField("xt", wxString("urn:ed2k:") + f->GetFileHash().Encode().Lower());
	uri.AddField("xt", wxString("urn:ed2khash:") + f->GetFileHash().Encode().Lower());
	uri.AddField("xl", CFormat("%d") % f->GetFileSize());

	// AICH, when we have one -- same source and condition CreateED2kLink()
	// uses for the ed2k link's "h=" field (amule-org/amule#331).
	const CKnownFile *kf = dynamic_cast<const CKnownFile *>(f);
	if (kf && kf->HasProperAICHHashSet()) {
		uri.AddField("xt", wxString("urn:aich:") + kf->GetAICHMasterHash());
	}

	return uri.GetLink();
}

// Returns a ed2k file URL
wxString CamuleAppCommon::CreateED2kLink(
	const CAbstractFile *f, bool add_source, bool use_hostname, bool add_cryptoptions, bool add_AICH)
{
	wxASSERT(!(!add_source && (use_hostname || add_cryptoptions)));
	// Construct URL like this: ed2k://|file|<filename>|<size>|<hash>|/
	wxString strURL = CFormat("ed2k://|file|%s|%i|%s|") % f->GetFileName().Cleanup(false) %
			  f->GetFileSize() % f->GetFileHash().Encode();

	// Append the AICH info
	if (add_AICH) {
		const CKnownFile *kf = dynamic_cast<const CKnownFile *>(f);
		if (kf && kf->HasProperAICHHashSet()) {
			strURL << "h=" << kf->GetAICHMasterHash() << "|";
		}
	}

	strURL << "/";

	if (add_source && theApp->IsConnected() && !theApp->IsFirewalled()) {
		// Create the first part of the URL
		strURL << "|sources,";
		if (use_hostname) {
			strURL << thePrefs::GetYourHostname();
		} else {
			uint32 clientID = theApp->GetID();
			strURL = CFormat("%s%u.%u.%u.%u") % strURL % (clientID & 0xff) %
				 ((clientID >> 8) & 0xff) % ((clientID >> 16) & 0xff) %
				 ((clientID >> 24) & 0xff);
		}

		strURL << ":" << thePrefs::GetPort();

		if (add_cryptoptions) {
			uint8 uSupportsCryptLayer = thePrefs::IsClientCryptLayerSupported() ? 1 : 0;
			uint8 uRequestsCryptLayer = thePrefs::IsClientCryptLayerRequested() ? 1 : 0;
			uint8 uRequiresCryptLayer = thePrefs::IsClientCryptLayerRequired() ? 1 : 0;
			uint16 byCryptOptions = (uRequiresCryptLayer << 2) | (uRequestsCryptLayer << 1) |
						(uSupportsCryptLayer << 0) |
						(uSupportsCryptLayer ? 0x80 : 0x00);

			strURL << ":" << byCryptOptions;

			if (byCryptOptions & 0x80) {
				strURL << ":" << thePrefs::GetUserHash().Encode();
			}
		}
		strURL << "|/";
	} else if (add_source) {
		AddLogLineC(_("WARNING: You can't add yourself as a source for an eD2k link while having a "
			      "lowid."));
	}

	// Result is "ed2k://|file|<filename>|<size>|<hash>|[h=<AICH master
	// hash>|]/|sources,[(<ip>|<hostname>):<port>[:cryptoptions[:hash]]]|/"
	return strURL;
}

bool CamuleAppCommon::InitCommon(int argc, wxChar **argv)
{
	theApp->SetAppName("aMule");
	FullMuleVersion = GetFullMuleVersion();
	OSDescription = wxGetOsDescription();
	OSType = OSDescription.BeforeFirst(' ');
	if (OSType.IsEmpty()) {
		OSType = "Unknown";
	}

	// Parse cmdline arguments.
	wxCmdLineParser cmdline(argc, argv);

	// Handle these arguments.
	cmdline.AddSwitch("v", "version", "Displays the current version number.");
	cmdline.AddSwitch("h", "help", "Displays this information.");
	cmdline.AddOption("c", "config-dir", "read config from <dir> instead of home");
	// One-shot autostart toggle. Called by the Windows installer's
	// Components-page checkbox and by the Preferences UI; lives in
	// AutostartManager so the OS-specific store (Windows registry,
	// macOS LaunchAgent, Linux XDG .desktop) is hidden from callers.
	cmdline.AddOption("",
		"configure-autostart",
		"Enable or disable starting this binary on user login (on|off), then exit.");
	// One-shot ed2k:// + magnet: URL-scheme handler toggle. Called by
	// the Windows installer's Components-page checkboxes and by the
	// Preferences UI / first-run wizard; lives in ProtocolHandlerManager
	// so the OS-specific store (Windows registry, Linux mimeapps.list,
	// macOS LaunchServices) is hidden from callers.
	cmdline.AddOption("",
		"configure-protocols",
		"Register/unregister aMule as the default handler for URL schemes, then exit. "
		"Values: on|off (both schemes) or ed2k:on|ed2k:off|magnet:on|magnet:off "
		"(per-scheme). The Windows installer invokes the per-scheme form.");
	// Same one-shot shape as the two above. Called by the Windows
	// installer's Components-page checkbox and by the Preferences UI /
	// first-run wizard; also the way a portable or self-built copy can
	// register itself without an installer.
	cmdline.AddOption("",
		"configure-file-assoc",
		"Register/unregister aMule as the handler for .emulecollection files, then exit. "
		"Values: on|off.");
#ifdef AMULE_DAEMON
	cmdline.AddSwitch("f", "full-daemon", "Fork to background.");
	cmdline.AddOption("p", "pid-file", "After fork, create a pid-file in the given fullname file.");
	cmdline.AddSwitch("e", "ec-config", "Configure EC (External Connections).");
#else

#ifdef __WINDOWS__
	// MSW shows help options in a dialog box, and the formatting doesn't fit there
#define HELPTAB "\t"
#else
#define HELPTAB "\t\t\t"
#endif

	cmdline.AddOption("geometry",
		"",
		"Sets the geometry of the app.\n" HELPTAB
		"<str> uses the same format as standard X11 apps:\n" HELPTAB
		"[=][<width>{xX}<height>][{+-}<xoffset>{+-}<yoffset>]");
#endif // !AMULE_DAEMON

	cmdline.AddSwitch("o", "log-stdout", "Print log messages to stdout.");
	cmdline.AddSwitch("r", "reset-config", "Resets config to default values.");

#ifdef CLIENT_GUI
	cmdline.AddSwitch("s", "skip", "Skip connection dialog.");
#else
	// Change webserver path. This is also a config option, so this switch will go at some time.
	cmdline.AddOption("w", "use-amuleweb", "Specify location of amuleweb binary.");
#endif
#ifndef __WINDOWS__
	cmdline.AddSwitch("d",
		"disable-fatal",
		"Don't catch fatal exceptions or block exit on assertions "
		"(useful under systemd / watchdog scripts).");
	// Keep stdin open to run valgrind --gen_suppressions
	cmdline.AddSwitch("i", "enable-stdin", "Do not disable stdin.");
#endif

	// Allow passing of links to the app
	cmdline.AddOption("t", "category", "Set category for passed ED2K links.", wxCMD_LINE_VAL_NUMBER);
	cmdline.AddParam(
		"ED2K link", wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL | wxCMD_LINE_PARAM_MULTIPLE);

	// wx asserts in debug mode if there is a check for an option that wasn't added.
	// So we have to wrap around the same #ifdefs as above. >:(

	// Show help on --help or invalid commands
	if (cmdline.Parse()) {
		return false;
	} else if (cmdline.Found("help")) {
		cmdline.Usage();
		return false;
	}

	if (cmdline.Found("version")) {
		// This looks silly with logging macros that add a timestamp.
		printf("%s\n",
			(const char *)unicode2char(
				wxString(CFormat("%s (OS: %s)") % FullMuleVersion % OSType)));
		return false;
	}

	wxString autostart_arg;
	if (cmdline.Found("configure-autostart", &autostart_arg)) {
		autostart_arg.MakeLower();
		bool ok = false;
		if (autostart_arg == wxT("on") || autostart_arg == wxT("yes") ||
			autostart_arg == wxT("true") || autostart_arg == wxT("1")) {
			ok = AutostartManager::Enable();
			printf(ok ? "autostart enabled\n" : "autostart enable FAILED\n");
		} else if (autostart_arg == wxT("off") || autostart_arg == wxT("no") ||
			   autostart_arg == wxT("false") || autostart_arg == wxT("0")) {
			ok = AutostartManager::Disable();
			printf(ok ? "autostart disabled\n" : "autostart disable FAILED\n");
		} else {
			fprintf(stderr,
				"configure-autostart expects 'on' or 'off' (got '%s')\n",
				(const char *)unicode2char(autostart_arg));
		}
		// Exit either way - this flag is a one-shot toggle, not a
		// "run aMule WITH autostart enabled" combo. (Caller can chain:
		// `amule --configure-autostart on && amule`.)
		// Return-false here propagates to OnInit returning false, which
		// makes wxApp terminate cleanly with exit code 0/1 per `ok`.
		// Using exit() directly would skip wx destructors.
		return false;
	}

	wxString protocols_arg;
	if (cmdline.Found("configure-protocols", &protocols_arg)) {
		protocols_arg.MakeLower();
		// Parse both the legacy bare on|off (applies to both schemes)
		// and the per-scheme ed2k:on|ed2k:off|magnet:on|magnet:off form
		// the Windows installer's two SecProto* sections now use.
		bool doEd2k = false, doMagnet = false;
		bool wantEnable = false;
		bool parsed = true;
		if (protocols_arg == wxT("on") || protocols_arg == wxT("yes") ||
			protocols_arg == wxT("true") || protocols_arg == wxT("1")) {
			doEd2k = doMagnet = true;
			wantEnable = true;
		} else if (protocols_arg == wxT("off") || protocols_arg == wxT("no") ||
			   protocols_arg == wxT("false") || protocols_arg == wxT("0")) {
			doEd2k = doMagnet = true;
			wantEnable = false;
		} else if (protocols_arg == wxT("ed2k:on")) {
			doEd2k = true;
			wantEnable = true;
		} else if (protocols_arg == wxT("ed2k:off")) {
			doEd2k = true;
			wantEnable = false;
		} else if (protocols_arg == wxT("magnet:on")) {
			doMagnet = true;
			wantEnable = true;
		} else if (protocols_arg == wxT("magnet:off")) {
			doMagnet = true;
			wantEnable = false;
		} else {
			parsed = false;
		}
		if (!parsed) {
			fprintf(stderr,
				"configure-protocols expects 'on', 'off', 'ed2k:on', 'ed2k:off', "
				"'magnet:on' or 'magnet:off' (got '%s')\n",
				(const char *)unicode2char(protocols_arg));
			return false;
		}
		bool ok = true;
		if (doEd2k) {
			ok &= (wantEnable ? ProtocolHandlerManager::Enable(HandlerTarget::Ed2kScheme)
					  : ProtocolHandlerManager::Disable(HandlerTarget::Ed2kScheme));
		}
		if (doMagnet) {
			ok &= (wantEnable ? ProtocolHandlerManager::Enable(HandlerTarget::MagnetScheme)
					  : ProtocolHandlerManager::Disable(HandlerTarget::MagnetScheme));
		}
		printf("%s: %s%s\n",
			ok ? (wantEnable ? "protocols enabled" : "protocols disabled")
			   : (wantEnable ? "protocols enable FAILED" : "protocols disable FAILED"),
			doEd2k ? "ed2k" : "",
			doMagnet ? (doEd2k ? ", magnet" : "magnet") : "");
		// Same one-shot exit semantics as --configure-autostart above.
		return false;
	}

	wxString fileassoc_arg;
	if (cmdline.Found("configure-file-assoc", &fileassoc_arg)) {
		fileassoc_arg.MakeLower();
		bool ok = false;
		if (fileassoc_arg == wxT("on") || fileassoc_arg == wxT("yes") ||
			fileassoc_arg == wxT("true") || fileassoc_arg == wxT("1")) {
			ok = ProtocolHandlerManager::Enable(HandlerTarget::CollectionFile);
			printf(ok ? "file association enabled\n" : "file association enable FAILED\n");
		} else if (fileassoc_arg == wxT("off") || fileassoc_arg == wxT("no") ||
			   fileassoc_arg == wxT("false") || fileassoc_arg == wxT("0")) {
			ok = ProtocolHandlerManager::Disable(HandlerTarget::CollectionFile);
			printf(ok ? "file association disabled\n" : "file association disable FAILED\n");
		} else {
			fprintf(stderr,
				"configure-file-assoc expects 'on' or 'off' (got '%s')\n",
				(const char *)unicode2char(fileassoc_arg));
		}
		// Same one-shot exit semantics as --configure-autostart above.
		return false;
	}

	wxString configdir;
	if (cmdline.Found("config-dir", &configdir)) {
		// Make an absolute path from the config dir
		wxFileName fn(configdir);
		fn.MakeAbsolute();
		configdir = fn.GetFullPath();
		if (configdir.Last() != wxFileName::GetPathSeparator()) {
			configdir += wxFileName::GetPathSeparator();
		}
		thePrefs::SetConfigDir(configdir);
	} else {
		thePrefs::SetConfigDir(/*OtherFunctions::*/ GetConfigDir(m_configFile));
	}

	// Backtracing works in MSW.
	// Problem is just that the backtraces are useless, because apparently the context gets lost
	// in the try/catch somewhere.
	// So leave it out.
#ifndef __WINDOWS__
	m_disableFatal = cmdline.Found("disable-fatal");
#if wxUSE_ON_FATAL_EXCEPTION
	if (!m_disableFatal) {
		// catch fatal exceptions
		wxHandleFatalExceptions(true);
	}
#endif
#endif

	theLogger.SetEnabledStdoutLog(cmdline.Found("log-stdout"));
#ifdef AMULE_DAEMON
	enable_daemon_fork = cmdline.Found("full-daemon");
	if (cmdline.Found("pid-file", &m_PidFile)) {
		// Remove any existing PidFile
		if (wxFileExists(m_PidFile))
			wxRemoveFile(m_PidFile);
	}
	ec_config = cmdline.Found("ec-config");
#else
	enable_daemon_fork = false;

	// Default geometry of the GUI. Can be changed with a cmdline argument...
	if (cmdline.Found("geometry", &m_geometryString)) {
		m_geometryEnabled = true;
	}
#endif

	if (theLogger.IsEnabledStdoutLog()) {
		if (enable_daemon_fork) {
			AddLogLineNS(
				"Daemon will fork to background - log to stdout disabled"); // localization
											    // not active yet
			theLogger.SetEnabledStdoutLog(false);
		} else {
			AddLogLineNS("Logging to stdout enabled");
		}
	}

	AddLogLineNS("Initialising " + FullMuleVersion);

	// Ensure that "~/.aMule/" is accessible.
	CPath outDir;
	if (!CheckMuleDirectory("configuration", CPath(thePrefs::GetConfigDir()), "", outDir)) {
		return false;
	}

	if (cmdline.Found("reset-config")) {
		// Make a backup first.
		wxRemoveFile(thePrefs::GetConfigDir() + m_configFile + ".backup");
		wxRenameFile(thePrefs::GetConfigDir() + m_configFile,
			thePrefs::GetConfigDir() + m_configFile + ".backup");
		AddLogLineNS(CFormat("Your settings have been reset to default values.\nThe old config file "
				     "has been saved as %s.backup\n") %
			     m_configFile);
	}

	size_t linksPassed = cmdline.GetParamCount(); // number of links from the command line
	// cppcheck-suppress variableScope
	int linksActuallyPassed = 0; // number of links that pass the syntax check
	if (linksPassed) {
		long catArg = 0;
		if (!cmdline.Found("t", &catArg)) {
			catArg = 0;
		}
		// wxCmdLineParser hands back a long; both consumers take an int.
		const int cat = static_cast<int>(catArg);

		wxTextFile ed2kFile(thePrefs::GetConfigDir() + "ED2KLinks");
		if (!ed2kFile.Exists()) {
			ed2kFile.Create();
		}
		if (ed2kFile.Open()) {
			for (size_t i = 0; i < linksPassed; i++) {
				const wxString param = cmdline.GetParam(i);

				// A .emulecollection argument stands for every link
				// inside it. This is what makes a double-click in a
				// file manager work on Linux and Windows, where the
				// path arrives as an ordinary argument.
				wxArrayString expanded;
				const CollectionExpansion expansion =
					ExpandPassedCollection(param, expanded, cat);
				if (expansion == kCollectionFailed) {
					continue;
				}
				if (expansion == kCollectionExpanded) {
					for (size_t e = 0; e < expanded.GetCount(); ++e) {
						ed2kFile.AddLine(expanded[e]);
						linksActuallyPassed++;
					}
					continue;
				}

				wxString link;
				if (CheckPassedLink(param, link, cat)) {
					ed2kFile.AddLine(link);
					linksActuallyPassed++;
				}
			}
			ed2kFile.Write();
		} else {
			AddLogLineCS("Failed to open 'ED2KLinks', cannot add links.");
		}
	}

	AddLogLineNS("Checking if there is an instance already running...");

	m_singleInstance = new InstanceLock();
	wxString lockfile = IsRemoteGui() ? "muleLockRGUI" : "muleLock";
	InstanceLock::Result lockResult = m_singleInstance->Acquire(lockfile, thePrefs::GetConfigDir());
	if (lockResult == InstanceLock::LOCK_HELD) {
		AddLogLineCS(CFormat("There is an instance of %s already running") % m_appName);
		AddLogLineNS(CFormat("(lock file: %s%s)") % thePrefs::GetConfigDir() % lockfile);
		if (linksPassed) {
			AddLogLineNS(CFormat("passed %d %s to it, finished") % linksActuallyPassed %
				     (linksPassed == 1 ? "link" : "links"));
			// Nothing was handed over, so don't pull the running
			// instance to the front on account of a bad argument.
			if (linksActuallyPassed == 0) {
				return false;
			}
		}

		// This is very tricky. The most secure way to communicate is via ED2K links file
		//
		// Raise the window even when we did hand links over. It matters most
		// for a file-manager double-click on a collection: the links land in
		// the running instance either way, but without this the user gets no
		// visible response at all and assumes nothing happened.
		wxTextFile ed2kFile(thePrefs::GetConfigDir() + "ED2KLinks");
		if (!ed2kFile.Exists()) {
			ed2kFile.Create();
		}

		if (ed2kFile.Open()) {
			ed2kFile.AddLine("RAISE_DIALOG");
			ed2kFile.Write();

			AddLogLineNS("Raising current running instance.");
		} else {
			AddLogLineCS("Failed to open 'ED2KFile', cannot signal running instance.");
		}

		return false;
	} else if (lockResult == InstanceLock::LOCK_ERROR) {
		AddLogLineCS(CFormat("Could not access lock file (%s%s); continuing without single-instance "
				     "protection.") %
			     thePrefs::GetConfigDir() % lockfile);
	} else {
		AddLogLineNS("No other instances are running.");
	}

#ifndef __WINDOWS__
	// Close standard-input
	if (!cmdline.Found("enable-stdin")) {
		// The full daemon will close all std file-descriptors by itself,
		// so closing it here would lead to the closing on the first open
		// file, which is the logfile opened below
		if (!enable_daemon_fork) {
			close(0);
		}
	}
#endif

	// Create the CFG file we shall use and set the config object as the
	// global cfg file. CamuleFileConfig is a wxFileConfig subclass that
	// wraps every entry / group lookup in an LC_CTYPE="C" scope so the
	// case-insensitive sorted-array binary searches are locale-
	// deterministic. The initial parse below also has to run under
	// LC_CTYPE="C" so the in-memory sort order matches what subsequent
	// Read / Write calls will use - otherwise a session that runs under
	// a non-C locale silently accumulates duplicate key=value lines in
	// amule.conf (#852).
	{
		CCtypeAsciiScope scope;
		wxConfig::Set(new CamuleFileConfig("", "", thePrefs::GetConfigDir() + m_configFile));
	}

	// The config holds the EC password, which is the credential itself rather
	// than a verifier -- anything that can read this file can drive the daemon.
	// It is created with whatever the umask allows, which on Debian and Ubuntu
	// defaults to group-writable. Tighten it here rather than at creation so
	// configs that already exist are covered too. The .bak is a full copy and
	// needs the same treatment.
	if (RestrictToOwner(CPath(thePrefs::GetConfigDir() + m_configFile))) {
		AddLogLineN(CFormat(_("Restricted permissions on %s to owner-only.")) % m_configFile);
	}
	RestrictToOwner(CPath(thePrefs::GetConfigDir() + m_configFile + ".bak"));

	// Make a backup of the log file
	CPath logfileName = CPath(thePrefs::GetConfigDir() + m_logFile);
	if (logfileName.FileExists()) {
		CPath::BackupFile(logfileName, ".bak");
	}

	// Open the log file
	if (!theLogger.OpenLogfile(logfileName.GetRaw())) {
		// use std err as last resolt to indicate problem
		fputs("ERROR: unable to open log file\n", stderr);
		// failure to open log is serious problem
		return false;
	}

	// Load Preferences
	CPreferences::BuildItemList(thePrefs::GetConfigDir());
	CPreferences::LoadAllItems(wxConfigBase::Get());

#ifdef CLIENT_GUI
	m_skipConnectionDialog = cmdline.Found("skip");
#else
	wxString amulewebPath;
	if (cmdline.Found("use-amuleweb", &amulewebPath)) {
		thePrefs::SetWSPath(amulewebPath);
		AddLogLineNS(CFormat("Using amuleweb in '%s'.") % amulewebPath);
	}
#endif

	// If an autostart entry exists pointing at a stale path (user
	// moved the AppImage / .app / install dir since they enabled the
	// toggle), silently rewrite it to the current canonical path so
	// the next login launches the right binary. No-op if no entry
	// exists - disabling autostart is always a deliberate choice we
	// don't second-guess.
	AutostartManager::SelfHealOnStartup();

	// Same idea for the URL-scheme handler registration: if we're the
	// current handler for ed2k:/magnet: but the registered path drifted
	// (user moved the AppImage / .app / install dir), rewrite it. No-op
	// if we aren't the current handler for a given scheme.
	ProtocolHandlerManager::SelfHealOnStartup();

	return true;
}

/**
 * Returns a description of the version of aMule being used.
 *
 * @return A detailed description of the aMule version, including application
 *         name and wx information.
 */
const wxString CamuleAppCommon::GetFullMuleVersion() const
{
	return GetMuleAppName() + " " + GetMuleVersion();
}

void CamuleAppCommon::OpenCollectionFiles(const wxArrayString &fileNames)
{
	wxArrayString links;

	for (size_t i = 0; i < fileNames.GetCount(); ++i) {
		wxArrayString expanded;
		// Category 0: the OS gives us no way to say which one, same as
		// a link clicked in a browser.
		if (ExpandPassedCollection(fileNames[i], expanded, 0) == kCollectionExpanded) {
			for (size_t e = 0; e < expanded.GetCount(); ++e) {
				links.Add(expanded[e]);
			}
		}
	}

	ProtocolHandler_QueueLinks(links);
}

CamuleAppCommon::CollectionExpansion CamuleAppCommon::ExpandPassedCollection(
	const wxString &in, wxArrayString &out, int cat)
{
	wxString path(in);

	// File managers hand over either a plain path or a file:// URL,
	// depending on the platform and on the .desktop Exec field in use.
	if (path.StartsWith("file://")) {
		const wxFileName fn = wxFileSystem::URLToFileName(path);
		path = fn.GetFullPath();
	}

	if (path.IsEmpty() || !wxFileName(path).GetExt().IsSameAs("emulecollection", false)) {
		return kNotACollection;
	}
	if (!wxFile::Exists(path)) {
		// The extension says collection, so a missing file is a real
		// error rather than something to retry as a link.
		AddLogLineCS(CFormat("Collection file not found: %s") % path);
		return kCollectionFailed;
	}

	CMuleCollection collection;
	if (!collection.Open(path)) {
		AddLogLineCS(CFormat("Invalid or empty collection file: %s") % path);
		return kCollectionFailed;
	}

	unsigned skipped = 0;
	for (size_t i = 0; i < collection.size(); ++i) {
		// eMule stores collection strings as UTF-8. Fall back to raw
		// bytes rather than dropping the entry, since FromUTF8 yields
		// an empty string on invalid input.
		wxString link = wxString::FromUTF8(collection[i].c_str());
		if (link.IsEmpty()) {
			link = wxString::From8BitData(collection[i].c_str());
		}

		// Route through CheckPassedLink so a collection link gets the
		// same validation, canonicalisation and category suffix as one
		// typed on the command line.
		wxString checked;
		if (CheckPassedLink(link, checked, cat)) {
			out.Add(checked);
		} else {
			++skipped;
		}
	}

	if (out.IsEmpty()) {
		AddLogLineCS(CFormat("No usable links in collection: %s") % path);
		return kCollectionFailed;
	}
	if (skipped > 0) {
		AddLogLineCS(CFormat("Skipped %u unusable link(s) in collection: %s") % skipped % path);
	}

	return kCollectionExpanded;
}

bool CamuleAppCommon::CheckPassedLink(const wxString &in, wxString &out, int cat)
{
	wxString link(in);

	// restore ASCII-encoded pipes
	link.Replace("%7C", "|");
	link.Replace("%7c", "|");

	if (link.compare(0, 7, "magnet:") == 0) {
		link = CMagnetED2KConverter(link);
		if (link.empty()) {
			AddLogLineCS(CFormat("Cannot convert magnet link to eD2k: %s") % in);
			return false;
		}
	}

	try {
		CScopedPtr<CED2KLink> uri(CED2KLink::CreateLinkFromUrl(link));
		out = uri.get()->GetLink();
		if (cat && uri.get()->GetKind() == CED2KLink::kFile) {
			out += CFormat(":%d") % cat;
		}
		return true;
	} catch (const wxString &err) {
		AddLogLineCS(CFormat("Invalid eD2k link \"%s\" - ERROR: %s") % link % err);
	}
	return false;
}

/**
 * Checks permissions on a aMule directory, creating if needed.
 *
 * @param desc A description of the directory in question, used for error messages.
 * @param directory The directory in question.
 * @param alternative If the dir specified with 'directory' could not be created, try this instead.
 * @param outDir Returns the used path.
 * @return False on error.
 */
bool CamuleAppCommon::CheckMuleDirectory(
	const wxString &desc, const CPath &directory, const wxString &alternative, CPath &outDir)
{
	wxString msg;

	if (directory.IsDir(CPath::readwritable)) {
		outDir = directory;
		return true;
	} else if (directory.DirExists()) {
		// Strings are not translated here because translation isn't up yet.
		msg = CFormat("Permissions on the %s directory too strict!\n"
			      "aMule cannot proceed. To fix this, you must set read/write/exec\n"
			      "permissions for the folder '%s'") %
		      desc % directory;
	} else if (CPath::MakeDir(directory)) {
		outDir = directory;
		return true;
	} else {
		msg << CFormat("Could not create the %s directory at '%s'.") % desc % directory;
	}

	// Attempt to use fallback directory.
	const CPath fallback(alternative);
	if (fallback.IsOk() && (directory != fallback)) {
		msg << "\nAttempting to use default directory at location \n'" << alternative << "'.";
		if (theApp->ShowAlert(msg, "Error accessing directory.", wxICON_ERROR | wxOK | wxCANCEL) ==
			wxCANCEL) {
			outDir = CPath("");
			return false;
		}

		return CheckMuleDirectory(desc, fallback, "", outDir);
	}

	theApp->ShowAlert(msg, "Fatal error.", wxICON_ERROR | wxOK);
	outDir = CPath("");
	return false;
}
