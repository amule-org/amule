//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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

#ifndef AMULE_H
#define AMULE_H

#include <wx/app.h>      // Needed for wxApp
#include <wx/datetime.h> // Needed for wxDateTime (ED2K/Kad "Connected since")
#include <wx/intl.h>     // Needed for wxLocale
#include <wx/timer.h>    // Needed for wxTimer (startup splash poll)

#include "Types.h"    // Needed for int32, uint16 and uint64
#include <functional> // Needed for std::function (DeferShutDownToOuterLoop)
#include <map>
#ifndef __WINDOWS__
#include <signal.h>
#endif // __WINDOWS__

#include "config.h" // Needed for ENABLE_UPNP

// GeoIP country DISPLAY + remote-config capability, as opposed to the resolver.
// True for a resolver-owning build (ENABLE_IP2COUNTRY, needs libmaxminddb) OR
// any remote GUI (amulegui / CLIENT_GUI, which receives country codes over EC
// and links no library). Use GEOIP_GUI to gate flag rendering, the country
// columns and the IP2Country preferences page; keep ENABLE_IP2COUNTRY for the
// resolver itself (CIP2Country) and the monolithic local IP→code lookup.
#if defined(ENABLE_IP2COUNTRY) || defined(CLIENT_GUI)
#define GEOIP_GUI 1
#endif

class CAbstractFile;
class CKnownFile;
class CSplashScreen;
class ExternalConn;
class CamuleDlg;
class CIP2Country;
class CCountryFlags;
class CPreferences;
class CDownloadQueue;
class CUploadQueue;
class CPartFileWriteThread;
class CPartFileHashThread;
class CMediaProbeThread;
class CFreeSpaceThread;
class CPartFileHashResultEvent;
class CServerConnect;
class CSharedFileList;
class CServer;
class CFriend;
class CMD4Hash;
class CServerList;
class CListenSocket;
class CClientList;
class CKnownFileList;
class CCanceledFileList;
class CSearchList;
class CBrowseManager;
class CClientCreditsList;
class CFriendList;
class CChatSessionStore;
class CClientUDPSocket;
class CIPFilter;
class UploadBandwidthThrottler;
class CUploadDiskIOThread;
class CAsioService;
#ifdef ENABLE_UPNP
class CUPnPControlPoint;
class CUPnPPortMapping;
#endif
class CStatistics;
class wxCommandEvent;
class wxCloseEvent;
class wxIdleEvent;
class wxFFileOutputStream;
class CTimer;
class CTimerEvent;
class InstanceLock;
class CHashingEvent;
class CMediaProbeEvent;
class CMuleInternalEvent;
class CCompletionEvent;
class CAllocFinishedEvent;
class wxExecuteData;
class CLoggingEvent;

namespace MuleNotify
{
class CMuleGUIEvent;
}

using MuleNotify::CMuleGUIEvent;

namespace Kademlia
{
class CUInt128;
}

#ifdef AMULE_DAEMON
#define AMULE_APP_BASE wxAppConsole
#define CORE_TIMER_PERIOD 300
#else
#define AMULE_APP_BASE wxApp
#define CORE_TIMER_PERIOD 100
#endif

// How long the amuleapi EC token file may sit on disk before the core
// removes it whether or not the child read it. Long enough to cover a
// slow exec on a loaded machine, short enough that a secret is not
// left at rest for anything a person would notice.
#define EC_TOKEN_FILE_TTL_MS 10000

#define CONNECTED_ED2K (1 << 0)
#define CONNECTED_KAD_NOT (1 << 1)
#define CONNECTED_KAD_OK (1 << 2)
#define CONNECTED_KAD_FIREWALLED (1 << 3)

void OnShutdownSignal(int /* sig */);

// Base class common to amule, aamuled and amulegui
class CamuleAppCommon
{
private:
	// Used to detect a previous running instance of aMule.
	// InstanceLock replaces wxSingleInstanceChecker on POSIX so single-
	// instance detection survives PID-namespace boundaries (Flatpak).
	InstanceLock *m_singleInstance;

	bool CheckPassedLink(const wxString &in, wxString &out, int cat);

public:
	// Outcome of trying to read a path as a collection.
	enum CollectionExpansion
	{
		// The path isn't a collection; a command-line caller treats it
		// as a link instead.
		kNotACollection,
		// The path named a collection and its links were emitted.
		kCollectionExpanded,
		// The path named a collection we could not read. Already
		// logged, and deliberately not retried as a link - doing so
		// would emit a second, misleading "invalid eD2k link" error.
		kCollectionFailed
	};

	/**
	 * Expands a .emulecollection into its eD2k links.
	 *
	 * Reads the file from this host's filesystem, so a caller holding a
	 * remote core's path must resolve it first (FileLaunch::ResolvePath).
	 *
	 * Accepts a plain path or a file:// URL; file managers pass either
	 * depending on the platform and the .desktop Exec field used.
	 * Each link is validated through CheckPassedLink(), so the caller
	 * gets the same canonicalisation and category suffix as a link typed
	 * on the command line. Missing, malformed and empty collections are
	 * reported by this function; the caller only has to stop.
	 */
	CollectionExpansion ExpandPassedCollection(const wxString &in, wxArrayString &out, int cat);

	/**
	 * Queues every collection among @a fileNames into the ED2KLinks file.
	 *
	 * This is the macOS "open document" path: the OS hands us the files
	 * the user double-clicked (or dropped on the Dock icon), which is why
	 * anything that isn't a collection is skipped without complaint.
	 *
	 * Goes through the ED2KLinks file rather than touching downloadqueue,
	 * which on amulegui does not exist until the EC connection is up.
	 */
	void OpenCollectionFiles(const wxArrayString &fileNames);

protected:
	wxString FullMuleVersion;
	wxString OSDescription;
	wxString OSType;
	bool enable_daemon_fork;
	bool ec_config;
	bool m_skipConnectionDialog;
	bool m_geometryEnabled;
	bool m_disableFatal;
	wxString m_geometryString;
	wxString m_logFile;
	wxString m_appName;
	wxString m_PidFile;

	bool InitCommon(int argc, wxChar **argv);

	/**
	 * Logs an assertion, with a backtrace, and applies --disable-fatal.
	 *
	 * Every app wants this identically -- the log copy is often the only
	 * record that survives, since the wxWidgets dialog dies with the
	 * process if nobody presses Continue -- but only the app itself knows
	 * which wxWidgets base to hand the assert on to afterwards, so the
	 * decision is returned rather than acted on here.
	 *
	 * @param dialogUsable whether wxWidgets can still put its assert dialog
	 *        on screen: false off the main thread, and false for an app
	 *        that is not running yet or is already tearing down.
	 * @return true when the caller should call its base OnAssertFailure().
	 *         Does not return at all when --disable-fatal is set, or when
	 *         the dialog is unusable: both abort so a supervisor sees a
	 *         non-zero exit and a debugger catches the assert in place.
	 */
	bool ReportAssertFailure(const wxChar *file,
		int line,
		const wxChar *func,
		const wxChar *cond,
		const wxChar *msg,
		bool dialogUsable);

	void RefreshSingleInstanceChecker();
	// Drop the single-instance lock (and unlink its file). OnExit() calls
	// std::_Exit() to dodge the wxWebSession teardown crash, which bypasses
	// ~CamuleAppCommon — so this must be called explicitly before the exit,
	// or the muleLock / muleLockRGUI file lingers between runs.
	void ReleaseSingleInstance();

	/**
	 * Postpone a shutdown that was requested from inside a nested event loop.
	 *
	 * amuledlg->Destroy() only queues the frame on wxPendingDelete, and the
	 * queue is drained from the idle handler of whichever event loop happens
	 * to be running. A modal dialog runs its own loop, so a shutdown started
	 * while one is open destroys the main window from under the handlers that
	 * opened the dialog: ~CamuleDlg walks down to the list control, and
	 * DestroyChildren() deletes the (stack-allocated) dialog those frames
	 * still own, which aborts on a free of stack memory.
	 *
	 * Ending the nested loop makes its ShowModal() return, so those frames
	 * unwind normally. @a retry then runs the teardown from the loop we land
	 * in; several levels of nesting peel off one per pass.
	 *
	 * The retry is driven by idle events rather than CallAfter() because
	 * wxEvtHandler::ProcessPendingEvents() re-drains whatever is queued while
	 * it runs: a pending-event retry gets picked up by the very loop it is
	 * waiting to leave, and Exit/re-queue then live-locks. Idle events are
	 * generated by the loop itself, so they let it unwind first.
	 *
	 * @return true if the shutdown was postponed and the caller must return
	 *         without tearing anything down.
	 */
	bool DeferShutDownToOuterLoop(const std::function<void()> &retry);

	void OnDeferredShutDownIdle(wxIdleEvent &evt);

	// Teardown queued by DeferShutDownToOuterLoop(), run once the nested
	// event loops have unwound. Non-empty only while one is outstanding.
	std::function<void()> m_deferredShutDown;

	bool CheckMuleDirectory(const wxString &desc,
		const class CPath &directory,
		const wxString &alternative,
		class CPath &outDir);

public:
	wxString m_configFile;

	CamuleAppCommon();
	~CamuleAppCommon();

	// GeoIP country resolver (headless: DB + download + ISO-code lookup).
	// The resolver lives on CamuleApp (amuled + monolithic amule); amulegui
	// has none and receives country codes over EC, so the base returns
	// nullptr. This lets shared model code (CServer / CUpDownClient::
	// GetCountryCode) resolve locally where a resolver exists and use the
	// EC-provided value where it doesn't.
	virtual class CIP2Country *GetIP2Country() { return nullptr; }

	// Apply the current GeoIP preference at runtime (creating the resolver on
	// first enable, disabling it when turned off). No-op on amulegui, which has
	// no local resolver and configures the daemon's GeoIP over EC. startup=true
	// (OnInit / a local enable toggle) also kicks the auto-update refresh; it is
	// false on a remote prefs-apply so an amulegui OK doesn't trigger a download
	// on every save — an explicit "Update now" (EC_TAG_IP2COUNTRY_UPDATE_NOW)
	// carries that intent instead (otherwise both fire → duplicate download).
	virtual void EnableIP2Country(bool startup) {}

	void AddLinksFromFile();
	// URL functions
	wxString CreateMagnetLink(const CAbstractFile *f);
	wxString CreateED2kLink(const CAbstractFile *f,
		bool add_source = false,
		bool use_hostname = false,
		bool add_cryptoptions = false,
		bool add_AICH = false);
	// Who am I ?
#ifdef AMULE_DAEMON
	bool IsDaemon() const { return true; }
#else
	bool IsDaemon() const { return false; }
#endif

#ifdef CLIENT_GUI
	bool IsRemoteGui() const { return true; }
#else
	bool IsRemoteGui() const { return false; }
#endif

	const wxString &GetMuleAppName() const { return m_appName; }
	const wxString GetFullMuleVersion() const;

#ifdef __WXGTK__
	// True when the process is running under a Wayland session (any
	// distro, any compositor). xdg-shell intentionally doesn't deliver
	// iconify-state notifications to clients, so several tray-icon
	// features that rely on detecting "user just minimized the
	// window" cannot work there - the call sites use this flag to
	// disable / grey out the relevant prefs and skip the broken paths.
	static bool IsWaylandSession();
#endif

	/**
	 * Forces the tray-dependent preferences off where the tray can't
	 * deliver what they promise.
	 *
	 * Both settings hide the main window and leave the tray icon as the
	 * only way back, so each one has to be switched off wherever that
	 * icon won't be there: no SNI backend on Linux (the legacy
	 * GtkStatusIcon is invisible on modern desktops), a Wayland session
	 * (no iconify notification to hide on), or simply the tray icon
	 * turned off. Called by every GUI app before it builds its window --
	 * amuled inherits it harmlessly, having no window to hide.
	 */
	static void SanitiseTrayPreferences();

	// Set when a quit was requested out-of-band of the main-window
	// close button (Cmd+Q, Dock right-click → Quit, tray-icon Exit).
	// CamuleDlg::OnClose checks this so HideOnClose only hides the
	// window for the actual red close-button gesture and never blocks
	// an explicit quit request. Lives on the common base so both the
	// monolithic CamuleApp and the remote-GUI CamuleRemoteGuiApp
	// expose the same accessors.
	bool IsQuitting() const { return m_isQuitting; }
	void SetQuitting() { m_isQuitting = true; }
	void ResetQuitting() { m_isQuitting = false; }

private:
	bool m_isQuitting = false;
};

class CamuleApp : public AMULE_APP_BASE, public CamuleAppCommon
{
private:
	enum APPState
	{
		APP_STATE_RUNNING = 0,
		APP_STATE_SHUTTINGDOWN,
		APP_STATE_STARTING
	};

public:
	CamuleApp();
	virtual ~CamuleApp();

	bool OnInit() override;
	int OnExit() override;
#if wxUSE_ON_FATAL_EXCEPTION
	void OnFatalException() override;
#endif
	bool ReinitializeNetwork(wxString *msg);

	// The core owns the GeoIP resolver (created in OnInit, guarded by
	// ENABLE_IP2COUNTRY + thePrefs::IsGeoIPEnabled). Serves the daemon
	// (serialising the country EC tag) and monolithic amule (local display).
	CIP2Country *GetIP2Country() override { return m_IP2Country; }
	void EnableIP2Country(bool startup) override;

	// derived classes may override those
	virtual int InitGui(bool geometry_enable, wxString &geometry_string);

	/**
	 * Create a search tab for every search restored from StoredSearches.met.
	 *
	 * Separate from InitGui() because the two cannot share a slot in
	 * startup order: the tabs need the restored results, and the restored
	 * results need the download queue loaded before their status is
	 * computed -- which happens well after the GUI is up. Called from
	 * OnInit() right after CSearchList::LoadSearches(), and a no-op on
	 * every build without a search dialog.
	 */
	virtual void RestoreSearchTabs() {}

	virtual int ShowAlert(wxString msg, wxString title, int flags) = 0;

	// Barry - To find out if app is running or shutting/shut down
	bool IsRunning() const { return (m_app_state == APP_STATE_RUNNING); }
	bool IsOnShutDown() const { return (m_app_state == APP_STATE_SHUTTINGDOWN); }

	// Check ED2K and Kademlia state
	bool IsFirewalled() const;
	// Are we connected to at least one network?
	bool IsConnected() const;
	// Connection to ED2K
	bool IsConnectedED2K() const;

	// What about Kad? Is it running?
	bool IsKadRunning() const;
	// Connection to Kad
	bool IsConnectedKad() const;
	// Check Kad state (TCP)
	bool IsFirewalledKad() const;
	// Check Kad state (UDP)
	bool IsFirewalledKadUDP() const;
	// Check Kad state (LAN mode)
	bool IsKadRunningInLanMode() const;
	// Kad stats
	uint32 GetKadUsers() const;
	uint32 GetKadFiles() const;
	uint32 GetKadIndexedSources() const;
	uint32 GetKadIndexedKeywords() const;
	uint32 GetKadIndexedNotes() const;
	uint32 GetKadIndexedLoad() const;
	// True IP of machine
	uint32 GetKadIPAddress() const;
	// Buddy status
	uint8 GetBuddyStatus() const;
	uint32 GetBuddyIP() const;
	uint32 GetBuddyPort() const;
	// Kad ID
	const Kademlia::CUInt128 &GetKadID() const;

	// Check if we should callback this client
	bool CanDoCallback(uint32 clientServerIP, uint16 clientServerPort);

	// Misc functions
	void OnlineSig(bool zero = false);
	void Localize_mule();
	void Trigger_New_version(wxString newMule);

	// shakraw - new EC code using wxSocketBase
	ExternalConn *ECServerHandler;

	// return current (valid) public IP or 0 if unknown
	// If ignorelocal is true, don't use m_localip
	uint32 GetPublicIP(bool ignorelocal = false) const;
	void SetPublicIP(const uint32 dwIP);

	uint32 GetED2KID() const;
	uint32 GetID() const;

	// Other parts of the interface and such
	CPreferences *glob_prefs;
	CDownloadQueue *downloadqueue;
	CUploadQueue *uploadqueue;
	CPartFileWriteThread *partFileWriteThread;
	CPartFileHashThread *partFileHashThread;
	CMediaProbeThread *mediaProbeThread;
	CFreeSpaceThread *freeSpaceThread;
	CServerConnect *serverconnect;
	CSharedFileList *sharedfiles;
	CServerList *serverlist;
	CListenSocket *listensocket;
	CClientList *clientlist;
	CKnownFileList *knownfiles;
	CCanceledFileList *canceledfiles;
	CSearchList *searchlist;
	//! Every "View Files" browse in flight. Owns their whole lifecycle; see
	//! CBrowseManager.
	CBrowseManager *browsemanager;
	CClientCreditsList *clientcredits;
	CFriendList *friendlist;
	// The core's chat transcript, shared by the local GUI and every EC
	// client so all three see one conversation rather than three private
	// ones. In-memory; emptied by a restart.
	CChatSessionStore *chatsessions;
	CClientUDPSocket *clientudp;
	CStatistics *m_statistics;
	CIPFilter *ipfilter;
	UploadBandwidthThrottler *uploadBandwidthThrottler;
	CUploadDiskIOThread *uploadDiskIOThread; // eMule ref: emule.h:92
	CAsioService *m_AsioService;
#ifdef ENABLE_UPNP
	CUPnPControlPoint *m_upnp;
	std::vector<CUPnPPortMapping> m_upnpMappings;
	// Build the port mappings from the current preferences and start the
	// UPnP control point. Safe to call more than once: it is a no-op when
	// UPnP is disabled or already running.
	void StartUPnP();
#endif
	wxLocale m_locale;

	void ShutDown();

	wxString GetLog(bool reset = false);
	wxString GetServerLog(bool reset = false);
	wxString GetDebugLog(bool reset = false);

	bool AddServer(CServer *srv, bool fromUser = false);
	void AddServerMessageLine(wxString &msg);
#ifdef __DEBUG__
	void AddSocketDeleteDebug(uint32 socket_pointer, uint32 creation_time);
#endif
	void SetOSFiles(const wxString &new_path);

	const wxString &GetOSType() const { return OSType; }

	void ShowUserCount();

	void ShowConnectionState(bool forceUpdate = false);

	// Wall-clock timestamp of the most recent ed2k/Kad connect, set by
	// ShowConnectionState() on the false->true transition it already
	// detects. Invalid (wxDateTime::IsValid() == false) while
	// disconnected. Feeds the "Connected since" row in the desktop
	// ED2K/Kad Info panes (CServerWnd::UpdateED2KInfo/UpdateKadInfo) --
	// not wired over EC, so amulegui doesn't show it (amule-org/amule#174).
	const wxDateTime &GetED2KConnectedSince() const { return m_ed2kConnectedSince; }
	const wxDateTime &GetKadConnectedSince() const { return m_kadConnectedSince; }

	void StartKad();
	void StopKad();

	/** Bootstraps kad from the specified IP (must be in hostorder). */
	void BootstrapKad(uint32 ip, uint16 port);
	/** Updates the nodes.dat file from the specified url. */
	void UpdateNotesDat(const wxString &str);

	void DisconnectED2K();

	bool CryptoAvailable() const;

protected:
	/**
	 * Handles asserts in a thread-safe manner.
	 *
	 * Compiled unconditionally (not just in __WXDEBUG__) so the
	 * --disable-fatal short-circuit also covers wxWidgets' own
	 * release-build assertions: Debian / Ubuntu's libwx packages
	 * leave wxDEBUG_LEVEL=1, which keeps wxASSERT live even in
	 * release builds and otherwise routes here through wxApp's
	 * default dialog.
	 */
	void OnAssertFailure(const wxChar *file,
		int line,
		const wxChar *func,
		const wxChar *cond,
		const wxChar *msg) override;

	void OnUDPDnsDone(CMuleInternalEvent &evt);
	void OnSourceDnsDone(CMuleInternalEvent &evt);
	void OnServerDnsDone(CMuleInternalEvent &evt);

	void OnTCPTimer(CTimerEvent &evt);
	void OnCoreTimer(CTimerEvent &evt);

	void OnFinishedHashing(CHashingEvent &evt);
	void OnPartFileHashResult(CPartFileHashResultEvent &evt);
	void OnFinishedAICHHashing(CHashingEvent &evt);

#if !defined(CLIENT_GUI) && !defined(AMULE_DAEMON)
	// Monolithic only: the daemon has no window to put a splash on, and
	// amulegui is a separate process that does not run this startup at all.
	//
	// Startup splash, kept alive past OnInit because the slowest part of a
	// first run is the hashing of everything the scan found unknown, and
	// that drains asynchronously long after OnInit has returned. Null once
	// the window is up and the splash has closed -- which is now the end of
	// the scan, not the end of hashing (#853).
	CSplashScreen *m_splash = nullptr;
	// Drives the hashing drain: refreshes the count on the shared-files label
	// and sorts the rows that arrived since the last tick. Polled rather than
	// driven from the completion handlers: the worker clears its current task
	// after the completion event has been posted, so a handler-driven update
	// can see the last task still pending and then never run again, leaving
	// the batch open for good. Polling also covers tasks that finish without
	// reaching a handler here, such as an aborted one.
	wxTimer m_splashPollTimer;

	// One tick of the drain: label, sort, and finish when the queue empties.
	void UpdateStartupHashProgress();
	void OnSplashPollTimer(wxTimerEvent &evt);
	// Ends the download list's batch, sorts and thaws the shared list, shows
	// the main window and closes the splash. Called when the scan finishes,
	// with hashing still running behind it.
	void ShowMainWindowAfterScan();
	// Ends the shared list's batch with its final sort and stops the poll
	// timer. Safe to call when nothing needed hashing.
	void FinishStartupHashing();
#endif // monolithic only

	// #140 - CMediaProbeTask marshals results back here so we can
	// attach FT_MEDIA_* tags on the main thread (the worker never
	// touches CKnownFile state).
	void OnMediaProbeFinished(CMediaProbeEvent &evt);
	void OnFinishedCompletion(CCompletionEvent &evt);
	void OnFinishedAllocation(CAllocFinishedEvent &evt);
	void OnFinishedHTTPDownload(CMuleInternalEvent &evt);
	void OnHashingShutdown(CMuleInternalEvent &);
	void OnNotifyEvent(CMuleGUIEvent &evt);

	void SetTimeOnTransfer();

	APPState m_app_state;

	// Media-probe tag writes coalesce into one known.met save: every
	// OnMediaProbeFinished stamps this (uptime ms), and OnCoreTimer flushes a
	// single Save() once probing has been idle for 30 s -- avoids the O(N^2)
	// full-file rewrite when the whole library is probed at startup (#616).
	// 0 = nothing pending.
	uint64 m_mediaTagsDirtiedMs = 0;

	// Headless GeoIP resolver, owned by the core (created in OnInit under
	// ENABLE_IP2COUNTRY). NULL when GeoIP is disabled/unsupported.
	CIP2Country *m_IP2Country;

	wxString m_emulesig_path;
	wxString m_amulesig_path;

	uint32 m_dwPublicIP;

	// PID type: `int` is wide enough for any OS pid we run on -- POSIX
	// pid_t is typically `int`, and Windows process IDs are 32-bit
	// DWORDs but always fit in a positive int. `long` was wrong on
	// LLP64 only by happy accident (long is also 32-bit there), but
	// using `int` makes the intent explicit and avoids %ld for what
	// is really an int-sized value.
	int webserver_pid;
	int amuleapi_pid;

	// Ephemeral EC credential for the amuleapi we spawn. Generated fresh
	// every start, handed over through a 0600 file in the config dir that
	// the child deletes as soon as it has read it, and accepted by the EC
	// server alongside the configured password for the rest of this run.
	//
	// The point is that amuleapi never needs the value in amule.conf. That
	// stored value IS the credential -- the challenge hashes it directly --
	// so a compromised network-facing daemon holding it would hold something
	// equivalent to the user's EC password. This holds a secret that dies
	// with the process instead.
	//
	// Empty when we did not spawn amuleapi, in which case no token is
	// accepted at all.
	wxString m_ecToken;

	// Uptime-ms deadline after which the token file is removed whether or
	// not the child read it. Without it a child that dies before reading
	// leaves a live secret at rest, and "no token on disk after startup"
	// stops being true. Same stamp-and-sweep shape as m_mediaTagsDirtiedMs.
	// 0 = nothing pending.
	uint64 m_ecTokenFileExpiryMs = 0;

	wxString server_msg;

	CTimer *core_timer;

public:
	// The ephemeral EC credential handed to the spawned amuleapi, or empty
	// when none was issued. Read by the EC server's authentication path,
	// which accepts it alongside the configured password.
	const wxString &GetEcToken() const { return m_ecToken; }

#ifdef ENABLE_VERSION_CHECK
	// Result of the last completed version check, relayed over EC to
	// amuleapi (the /version "update" object) and read by the UIs.
	bool IsVersionCheckDone() const { return m_versionCheckDone; }
	bool IsVersionCheckOutdated() const { return m_versionCheckOutdated; }
	const wxString &GetVersionCheckLatest() const { return m_versionCheckLatest; }
	// Unix time the last check completed (0 if never). Relayed so clients
	// can show how stale the result is (checks are startup-only).
	time_t GetVersionCheckTimestamp() const { return m_versionCheckTimestamp; }

	// Kick off an async GitHub /releases/latest fetch; CheckNewVersion()
	// stores the outcome when it completes. Reused by OnInit (startup) and
	// the EC_OP_VERSION_CHECK trigger. Returns false when throttled (a
	// check ran within the cooldown window) so the trigger caller can
	// report "try again later" instead of hammering GitHub's rate limit.
	bool StartVersionCheck();
#endif

private:
	void OnUnhandledException() override;

#ifdef ENABLE_VERSION_CHECK
	void CheckNewVersion(uint32 result);

	bool m_versionCheckDone = false;
	bool m_versionCheckOutdated = false;
	wxString m_versionCheckLatest;
	// Unix time the last check completed (0 = never).
	time_t m_versionCheckTimestamp = 0;
	// Wall-clock of the last StartVersionCheck() attempt, for throttling
	// the EC trigger against GitHub's unauthenticated rate limit.
	time_t m_versionCheckLastAttempt = 0;
#endif

	uint32 m_localip;

	// Set by ShowConnectionState(); see the GetED2KConnectedSince() /
	// GetKadConnectedSince() accessors above.
	wxDateTime m_ed2kConnectedSince;
	wxDateTime m_kadConnectedSince;
};

#ifndef AMULE_DAEMON

class CamuleGuiBase
{
public:
	CamuleGuiBase();
	virtual ~CamuleGuiBase();

	wxString m_FrameTitle;
	CamuleDlg *amuledlg;

#ifdef GEOIP_GUI
	// Country flag images (ISO code -> wxImage), shared by the monolithic
	// and remote GUIs. The country *code* comes from the core resolver
	// (monolithic) or the EC tag (amulegui); this turns it into a flag.
	// Held by pointer so the core header stays free of <wx/image.h>.
	CCountryFlags *GetCountryFlags() { return m_countryFlags; }
#endif

	bool CopyTextToClipboard(wxString strText);
	void ResetTitle();

	/**
	 * Asks the platform to follow the desktop's light/dark setting.
	 *
	 * Must be called from OnInit() before the first window is created: wx
	 * answers CannotChange once one exists, so anywhere later silently does
	 * nothing.
	 */
	static void FollowSystemAppearance();

	virtual int InitGui(bool geometry_enable, wxString &geometry_string);
	//! See CamuleApp::RestoreSearchTabs().
	void CreateRestoredSearchTabs();
	virtual int ShowAlert(wxString msg, wxString title, int flags);

	void AddGuiLogLine(const wxString &line);

protected:
	/**
	 * This list is used to contain log messages that are to be displayed
	 * on the GUI, when it is currently impossible to do so. This is in order
	 * to allows us to queue messages till after the dialog has been created.
	 */
	std::list<wxString> m_logLines;

#ifdef GEOIP_GUI
	CCountryFlags *m_countryFlags;
#endif
};

#ifndef CLIENT_GUI

class CamuleGuiApp : public CamuleApp, public CamuleGuiBase
{

	virtual int InitGui(bool geometry_enable, wxString &geometry_string);
	// No `override`: this class declares none, and adding the first one
	// turns -Winconsistent-missing-override into errors on every sibling.
	virtual void RestoreSearchTabs() { CreateRestoredSearchTabs(); }

	int OnExit();
	bool OnInit();

	// Catch alternate quit paths (macOS Dock right-click → Quit)
	// so we can run ShutDown cleanup even when wxApp skips OnExit.
	void OnEndSession(wxCloseEvent &evt);
	void OnQueryEndSession(wxCloseEvent &evt);

#ifdef __WXMAC__
	// Restore the main window when the user clicks the Dock icon
	// while no aMule windows are visible. Default wxApp::MacReopenApp
	// behaviour is to do nothing when the frame is hidden, so a
	// window hidden via the close button (HideOnClose pref) stays
	// permanently hidden - the app appears stuck.
	virtual void MacReopenApp();

	// Finder "Open With" / double-click on a .emulecollection, and Dock
	// drops. wx defers the launch event until after OnInit returns, so
	// the config dir is always set by the time we run.
	virtual void MacOpenFiles(const wxArrayString &fileNames);

	// ed2k:// and magnet: clicks. A safety net, not the usual path: in
	// practice the kAEGetURL handler in ProtocolHandlerManager_mac.mm
	// receives these. wxNSAppController registers for the same event in
	// applicationWillFinishLaunching, and -setEventHandler: replaces per
	// (class, id), so which one is live is a question of load order we
	// don't control. Whichever wins, the URL is queued exactly once.
	virtual void MacOpenURL(const wxString &url);
#endif

public:
	virtual int ShowAlert(wxString msg, wxString title, int flags);

	void ShutDown(wxCloseEvent &evt);

	wxString GetLog(bool reset = false);
	wxString GetServerLog(bool reset = false);
	void AddServerMessageLine(wxString &msg);
	wxDECLARE_EVENT_TABLE();
};

DECLARE_APP(CamuleGuiApp)
extern CamuleGuiApp *theApp;

#else /* !CLIENT_GUI */

#include "amule-remote-gui.h"

#endif // CLIENT_GUI

#define CALL_APP_DATA_LOCK

#else /* ! AMULE_DAEMON */

class CamuleDaemonApp : public CamuleApp
{
private:
	bool OnInit();
	int OnRun();
	int OnExit();

	virtual int InitGui(bool geometry_enable, wxString &geometry_string);
	// The GTK wxApps sets its file name conversion properly
	// in wxApp::Initialize(), while wxAppConsole::Initialize()
	// does not, leaving wxConvFileName being set to wxConvLibc. File
	// name conversion should be set otherwise amuled will abort to
	// handle non-ASCII file names which monolithic amule can handle.
	// This function are overridden to perform this.
	virtual bool Initialize(int &argc_, wxChar **argv_);

public:
	bool CopyTextToClipboard(wxString strText);

	virtual int ShowAlert(wxString msg, wxString title, int flags);

	wxDECLARE_EVENT_TABLE();
};

DECLARE_APP(CamuleDaemonApp)
extern CamuleDaemonApp *theApp;

#endif /* ! AMULE_DAEMON */

#endif // AMULE_H
// File_checked_for_headers
