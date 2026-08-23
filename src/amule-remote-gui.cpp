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

#include <algorithm>             // Needed for std::min
#include "libs/ec/cpp/ECCrypt.h" // Needed for ECCrypt::CipherName

#include <wx/ipc.h>
#include <wx/cmdline.h>  // Needed for wxCmdLineParser
#include <wx/config.h>   // Do_not_auto_remove (win32)
#include <wx/fileconf.h> // Needed for wxFileConfig
#include <wx/socket.h>   // Needed for wxSocketBase

#if defined(__WXGTK__) && !defined(__APPLE__)
#include <glib.h> // g_set_prgname() — wl_app_id / WM_CLASS binding
#endif

#include <common/Format.h>
#include <common/StringFunctions.h>
#include <common/MD5Sum.h>

#include <include/common/EventIDs.h>

#include "amule.h"                  // Interface declarations.
#include "ProtocolHandlerManager.h" // Needed for ProtocolHandler_QueueSchemeLink
#include "CamuleArtProvider.h"      // Needed for wxArtProvider::Push() in OnInit
#include "amuleDlg.h"               // Needed for CamuleDlg
#include "PrefsUnifiedDlg.h"        // Needed for the shared-dirs editor refresh hook

#include <wx/sizer.h>    // CReconnectDialog layout (issue #444)
#include <wx/stattext.h> // CReconnectDialog status label
#include <wx/button.h>   // CReconnectDialog abort button
#include "ClientCredits.h"
#include "SourceListCtrl.h"
#include "ChatWnd.h"
#include "ClientsWnd.h"       // Needed for CClientsWnd
#include "DataToText.h"       // Needed for GetSoftName()
#include "DownloadListCtrl.h" // Needed for CDownloadListCtrl
#include "Friend.h"
#include "GetTickCount.h" // Needed for GetTickCount64
#include "GuiEvents.h"
#ifdef GEOIP_GUI
#include "IP2Country.h" // Needed for IP2Country
#endif
#include "InternalEvents.h" // Needed for wxEVT_CORE_FINISHED_HTTP_DOWNLOAD
#include "Logger.h"
#include "muuli_wdr.h"       // Needed for IDs
#include "PartFile.h"        // Needed for CPartFile
#include <tags/FileTags.h>   // Needed for FT_MEDIA_* metadata tag names
#include "SearchDlg.h"       // Needed for CSearchDlg
#include "Server.h"          // Needed for GetListName
#include "ServerWnd.h"       // Needed for CServerWnd
#include "SharedFilesCtrl.h" // Needed for CSharedFilesCtrl
#include "SharedFilesWnd.h"  // Needed for CSharedFilesWnd
#include "TransferWnd.h"     // Needed for CTransferWnd
#include "UpDownClientEC.h"  // Needed for CUpDownClient
#include "ServerListCtrl.h"  // Needed for CServerListCtrl
#include "ScopedPtr.h"
#include "StatisticsDlg.h" // Needed for CStatisticsDlg
#include "KadDlg.h"        // Needed for CKadDlg::UpdateGraph
#include "ArchSpecific.h"  // Needed for ENDIAN_NTOHL

CEConnectDlg::CEConnectDlg()
: wxDialog(theApp->amuledlg, -1, _("Connect to remote amule"), wxDefaultPosition)
{
	CoreConnect(this, true);

	wxString pref_host, pref_port;
	// Use the literal loopback address rather than "localhost":
	// on Windows, "localhost" lookups can fail intermittently
	// (IPv4 vs IPv6 stack ordering, hosts-file shape, ...).
	// 127.0.0.1 is portable across every supported OS. Same default
	// as amulecmd / amuleweb (CaMuleExternalConnector). (#822)
	wxConfig::Get()->Read("/EC/Host", &pref_host, "127.0.0.1");
	wxConfig::Get()->Read("/EC/Port", &pref_port, "4712");
	wxConfig::Get()->Read("/EC/Password", &pwd_hash);
	long pref_force_zlib;
	wxConfig::Get()->Read("/EC/ForceZLIB", &pref_force_zlib, 0);

	CastChild(ID_REMOTE_HOST, wxTextCtrl)->SetValue(pref_host);
	CastChild(ID_REMOTE_PORT, wxTextCtrl)->SetValue(pref_port);
	CastChild(ID_EC_PASSWD, wxTextCtrl)->SetValue(pwd_hash);
	CastChild(ID_EC_FORCE_ZLIB, wxCheckBox)->SetValue(pref_force_zlib != 0);
	// Default 1: a config predating this key gets encryption.
	long pref_encryption;
	wxConfig::Get()->Read("/EC/Encryption", &pref_encryption, 1);
	CastChild(ID_EC_ENCRYPTION, wxCheckBox)->SetValue(pref_encryption != 0);

	CentreOnParent();
}

wxString CEConnectDlg::PassHash()
{
	return pwd_hash;
}

wxBEGIN_EVENT_TABLE(CEConnectDlg, wxDialog)
	EVT_BUTTON(wxID_OK, CEConnectDlg::OnOK)
wxEND_EVENT_TABLE()

void CEConnectDlg::OnOK(wxCommandEvent &evt)
{
	wxString s_port = CastChild(ID_REMOTE_PORT, wxTextCtrl)->GetValue();
	port = StrToLong(s_port);

	host = CastChild(ID_REMOTE_HOST, wxTextCtrl)->GetValue();
	passwd = CastChild(ID_EC_PASSWD, wxTextCtrl)->GetValue();

	if (passwd != pwd_hash) {
		pwd_hash = MD5Sum(passwd).GetHash();
	}
	m_save_user_pass = CastChild(ID_EC_SAVE, wxCheckBox)->IsChecked();
	m_force_zlib = CastChild(ID_EC_FORCE_ZLIB, wxCheckBox)->IsChecked();
	m_encryption = CastChild(ID_EC_ENCRYPTION, wxCheckBox)->IsChecked();
	evt.Skip();
}

wxDEFINE_EVENT(wxEVT_EC_INIT_DONE, wxEvent);

// ----------------------------------------------------------------------
// Reconnect-after-loss dialog (issue #444). Shown modally while amulegui
// re-establishes a dropped EC connection: modal so the main window is
// frozen (the user must not act on stale data or queue EC commands at a
// dead socket), while the retry timer + socket events still pump in the
// modal loop. The wxID_CANCEL "Abort and exit" button ends the modal
// with wxID_CANCEL; a successful reconnect ends it with wxID_OK from
// CamuleRemoteGuiApp::OnECConnection.
// ----------------------------------------------------------------------
class CReconnectDialog : public wxDialog
{
public:
	CReconnectDialog(wxWindow *parent, const wxString &target)
	: wxDialog(parent, wxID_ANY, _("Connection lost"), wxDefaultPosition, wxDefaultSize, wxCAPTION)
	, m_label(nullptr)
	, m_target(target)
	{
		wxBoxSizer *top = new wxBoxSizer(wxVERTICAL);
		m_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
		top->Add(m_label, 0, wxALL, 15);
		top->Add(new wxButton(this, wxID_CANCEL, _("Abort and exit")),
			0,
			static_cast<int>(wxALIGN_CENTER) | wxLEFT | wxRIGHT | wxBOTTOM,
			15);
		SetAttempt(1);
		SetSizerAndFit(top);
		CentreOnParent();
	}

	// Shown while a connect attempt is in flight.
	void SetAttempt(int n) { SetStatus(CFormat(_("Reconnecting... (attempt %d)")) % n); }

	// Shown counting down to the next attempt after a failure.
	void SetCountdown(int seconds) { SetStatus(CFormat(_("Next attempt in %d s...")) % seconds); }

private:
	// Only the middle line changes; the (widest) "paused" line is constant,
	// so the dialog keeps its size and doesn't jump on each update.
	void SetStatus(const wxString &middle)
	{
		m_label->SetLabel(CFormat(_("Connection to %s lost.\n%s\n\nThe interface is paused "
					    "until the connection is restored.")) %
				  m_target % middle);
	}

	wxStaticText *m_label;
	wxString m_target;
};

wxBEGIN_EVENT_TABLE(CamuleRemoteGuiApp, wxApp)
	// macOS Dock right-click -> Quit ends the session without going through
	// the red-X / Cmd+Q paths; catch it so ShutDown + OnExit cleanup still runs.
	EVT_QUERY_END_SESSION(CamuleRemoteGuiApp::OnQueryEndSession)
	EVT_END_SESSION(CamuleRemoteGuiApp::OnEndSession)

	// Core timer
	EVT_TIMER(ID_CORE_TIMER_EVENT, CamuleRemoteGuiApp::OnPollTimer)
	// Watchdog on the initial EC connect attempt
	EVT_TIMER(ID_REMOTE_CONNECT_TIMEOUT_TIMER, CamuleRemoteGuiApp::OnConnectTimeout)
	// Spacing between reconnect attempts after a post-startup loss (#444)
	EVT_TIMER(ID_REMOTE_RECONNECT_TIMER, CamuleRemoteGuiApp::OnReconnectTimer)

	EVT_CUSTOM(wxEVT_EC_CONNECTION, -1, CamuleRemoteGuiApp::OnECConnection)
	EVT_CUSTOM(wxEVT_EC_INIT_DONE, -1, CamuleRemoteGuiApp::OnECInitDone)

	EVT_MULE_NOTIFY(CamuleRemoteGuiApp::OnNotifyEvent)

#ifdef GEOIP_GUI
	// HTTPDownload finished
	EVT_MULE_INTERNAL(wxEVT_CORE_FINISHED_HTTP_DOWNLOAD, -1, CamuleRemoteGuiApp::OnFinishedHTTPDownload)
#endif
wxEND_EVENT_TABLE()

IMPLEMENT_APP(CamuleRemoteGuiApp)

int CamuleRemoteGuiApp::OnExit()
{
	StopTickTimer();

	wxSocketBase::Shutdown(); // needed because we also called Initialize() manually

	// Mirror CamuleApp::OnExit (#141): drain the pending-delete queue (where
	// CamuleDlg::OnClose -> ShutDown parked amuledlg->Destroy()) so the frame's
	// lazily-scheduled destructor runs, then tear down wxConfig to flush it.
	// Otherwise the red-X + confirm path reaches here with the destroy still
	// queued and anything that chain persists is lost (CMD+Q happens to drain
	// naturally). The _Exit(0) below skips wx's own cleanup that normally does
	// both, so we do it by hand here. Column widths and sort orders no longer
	// depend on this: CMuleDataViewCtrl writes those eagerly.
	DeletePendingObjects();
	delete wxConfigBase::Set(nullptr);

	// Skip wx's static-destructor / module cleanup, exactly as the monolithic
	// app does in CamuleGuiApp::OnExit (amule.cpp). wx's WebRequestModule
	// teardown destroys the platform wxWebSession, whose dtor dereferences
	// already-freed state and raise(SIGABRT)s on quit (amule-org/amule#18,
	// PR #159). amulegui links wxWebRequest too, so it hits the identical
	// crash. By this point our own cleanup (timer, sockets) has run; _Exit
	// bypasses atexit + static destructors so the buggy wx dtor never runs.
	// Remove once the upstream wx fix lands in a release we depend on.
	//
	// _Exit also skips ~CamuleAppCommon, which would release the
	// single-instance lock; drop it here so muleLockRGUI is unlinked.
	ReleaseSingleInstance();
	std::_Exit(0);

	return wxApp::OnExit();
}

void CamuleRemoteGuiApp::OnQueryEndSession(wxCloseEvent &evt)
{
	// Flag the quit so CamuleDlg::OnClose skips its HideOnClose-veto branch
	// and actually quits on a Dock right-click -> Quit (as for Cmd+Q).
	SetQuitting();
	evt.Skip();
}

void CamuleRemoteGuiApp::OnEndSession(wxCloseEvent &evt)
{
	// The Dock-Quit path can bypass OnExit, so run ShutDown (unless OnClose
	// already did -- it nulls amuledlg) and then OnExit explicitly, so the
	// list-control destructors + wxConfig flush run and column widths / sort
	// orders persist. Mirrors CamuleGuiApp::OnEndSession (amule-gui.cpp).
	if (amuledlg) {
		ShutDown(evt);
	}
	OnExit();
	evt.Skip();
}

#ifdef __WXMAC__
void CamuleRemoteGuiApp::MacReopenApp()
{
	// Dock-icon click (and re-launch from Finder / Launchpad) while no
	// window is visible. wxApp's default handler only de-iconizes; a frame
	// hidden with Show(false) -- the close-button HideOnClose path and the
	// minimize-to-tray path both end there -- is not a candidate, so
	// without this amulegui simply never came back. Mirrors CamuleGuiApp.
	if (amuledlg) {
		amuledlg->RestoreMainWindow();
	}
}

void CamuleRemoteGuiApp::MacOpenFiles(const wxArrayString &fileNames)
{
	// Also fires for Dock drops of arbitrary files; anything that is not
	// a collection is ignored without complaint. Mirrors CamuleGuiApp.
	OpenCollectionFiles(fileNames);
}

void CamuleRemoteGuiApp::MacOpenURL(const wxString &url)
{
	ProtocolHandler_QueueSchemeLink(url);
}
#endif

#if wxUSE_ON_FATAL_EXCEPTION
// Gracefully handle fatal exceptions and print backtrace if possible.
// Mirrors CamuleApp::OnFatalException (amule.cpp) -- without this
// override amulegui crashes produce no symbolicated amule frames,
// which makes diagnosing GTK-callback-into-stale-widget bugs like
// #692 a guessing game.
void CamuleRemoteGuiApp::OnFatalException()
{
	/* Print the backtrace */
	wxString msg;
	msg << "\n--------------------------------------------------------------------------------\n"
	    << "A fatal error has occurred and amulegui has crashed.\n"
	    << "Please assist us in fixing this problem by reporting the backtrace below as a\n"
	    << "GitHub issue, including as much information as possible regarding the\n"
	    << "circumstances of this crash. Issue tracker:\n"
	    << "    https://github.com/amule-org/amule/issues\n"
	    << "If possible, please try to generate a real backtrace of this crash:\n"
	    << "    https://amule-org.github.io/docs/contributing/bug-report\n\n"
	    << "----------------------------=| BACKTRACE FOLLOWS: |=----------------------------\n"
	    << "Current version is: " << FullMuleVersion << "\nRunning on: " << OSDescription << "\n\n"
	    << get_backtrace(1) // 1 == skip this function.
	    << "\n--------------------------------------------------------------------------------\n";

	theLogger.EmergencyLog(msg, true);
}
#endif

void CamuleRemoteGuiApp::OnAssertFailure(
	const wxChar *file, int line, const wxChar *func, const wxChar *cond, const wxChar *msg)
{
	// Unlike CamuleApp there is no app-state gate here: the remote GUI has
	// no equivalent of IsRunning(), and its window is either up or the
	// assert came from a thread that cannot show a dialog anyway.
	if (ReportAssertFailure(file, line, func, cond, msg, wxThread::IsMain())) {
		wxApp::OnAssertFailure(file, line, func, cond, msg);
	}
}

void CamuleRemoteGuiApp::OnPollTimer(wxTimerEvent &)
{
	static int request_step = 0;
	static uint32 msPrevStats = 0;

	// Reply watchdog. EC has no application-level keepalive, and the daemon
	// always answers, so requests outstanding with nothing coming back means
	// the transport has gone quiet -- an SSH tunnel with no ServerAliveInterval,
	// a NAT dropping an idle mapping, a proxy that stopped relaying. None of
	// those close the socket: it stays ESTABLISHED with every queue empty and
	// no error is ever raised, so neither OnLost nor OnError fires.
	//
	// Without this the failure is permanent AND silent. Once m_req_count passes
	// m_req_fifo_thr the early-return below fires on every tick, so the client
	// stops sending too -- and recovery would need a reply, which needs a
	// request. The back-pressure brake becomes a deadlock the client can never
	// leave, with a frozen UI and no message.
	//
	// Gated on the fifo rather than on the threshold so it trips on the real
	// symptom (nothing answering) rather than waiting for the queue to fill.
	if (m_connect->GetReqFifoSize() > 0 &&
		m_connect->MillisecondsSinceLastReply() > EC_REPLY_TIMEOUT_MS) {
		// Untranslated and debug-level on purpose: the user-facing messaging
		// already comes from the path this drops into -- OnLost posts
		// "Connection failure" and BeginReconnect announces the retry, both
		// long since translated. Adding a new msgid here would have bought
		// translators work for something nobody but a developer reads.
		AddDebugLogLineN(logEC,
			CFormat(wxT("EC reply watchdog: no reply for %u ms with %u requests "
				    "pending -- treating the connection as dead")) %
				(unsigned)m_connect->MillisecondsSinceLastReply() %
				(unsigned)m_connect->GetReqFifoSize());
		// Close ourselves and dispatch the loss: a self-close suppresses the
		// asio lost-event path, so without the explicit dispatch nothing would
		// tell the GUI. Lands in OnECConnection(false) -> BeginReconnect().
		m_connect->CloseAndDispatchLost();
		return;
	}
	if (m_connect->RequestFifoFull()) {
		return;
	}

	switch (request_step) {
	case 0: {
		CECPacket stats_req(EC_OP_STAT_REQ, EC_DETAIL_INC_UPDATE);
		m_connect->SendRequest(&m_stats_updater, &stats_req);
		request_step++;
		break;
	}
	case 1:
		if (amuledlg->m_sharedfileswnd->IsShown() || amuledlg->m_chatwnd->IsShown() ||
			amuledlg->m_serverwnd->IsShown()) {
			// update downloads, shared files and servers
			knownfiles->DoRequery(EC_OP_GET_UPDATE, EC_TAG_KNOWNFILE);
			// Server-message log mirror: pull the cumulative
			// server_msg buffer while the Network tab is up so the
			// "Server Info" sub-panel reaches feature parity with
			// the monolithic build. ed2k server messages are bursty
			// (one-off on connect, the occasional ID-change notice,
			// disconnect) so the natural cadence of the page step is
			// plenty.
			if (amuledlg->m_serverwnd->IsShown()) {
				CECPacket srvinfo_req(EC_OP_GET_SERVERINFO);
				m_connect->SendRequest(&m_serverinfo_handler, &srvinfo_req);
			}
		} else if (amuledlg->m_transferwnd->IsShown()) {
			// update both downloads and shared files
			knownfiles->DoRequery(EC_OP_GET_UPDATE, EC_TAG_KNOWNFILE);
		} else if (amuledlg->m_clientswnd->IsShown()) {
			// Same request, for the peers rather than the files: the
			// EC_TAG_CLIENT tags the clients list lives on ride along
			// in this reply (CKnownFilesRem::ProcessUpdate) and are
			// sent by nothing else. Without this the page shows
			// whatever the peers looked like when the user left the
			// last tab that did ask, and never moves again.
			knownfiles->DoRequery(EC_OP_GET_UPDATE, EC_TAG_KNOWNFILE);
		} else if (amuledlg->m_searchwnd->IsShown()) {
			// Reachability fix (#641): ask what searches the daemon
			// currently holds -- independent of m_curr_search, which only
			// ever reflects a search THIS client itself started -- so a
			// search opened by another client, or one day restored from
			// disk, gets a tab created here (CSearchListRem::HandlePacket).
			// Search entries are near-static (id/name/kind barely change),
			// so unlike the results union poll below this isn't asked every
			// tick: once on (re)connect, and again only when a result turns
			// up bearing a search ID with no tab yet (CreateItem sets
			// m_needSearchListRequery) -- the daemon-side registry this
			// reads never changes without a result also arriving for it.
			// RequestSearchList (not DoRequery) on purpose: HandlePacket's
			// EC_OP_SEARCH_LIST branch never reaches the base class's
			// STATUS_REQ_SENT -> IDLE transition, so going through
			// DoRequery would wedge this container's request state machine
			// forever and silently drop every later EC_OP_SEARCH_RESULTS
			// poll (got3nks, PR #680 review).
			if (searchlist->m_needSearchListRequery) {
				searchlist->RequestSearchList();
				searchlist->m_needSearchListRequery = false;
			}
			// The union poll below already returns every active search's
			// results regardless of m_curr_search (Get_EC_Response_Search_
			// Results_Union iterates the daemon's own registry) -- the old
			// m_curr_search-only gate just meant a client that never
			// started a search of its own never asked at all, even once a
			// tab existed for one it learned about above.
			searchlist->DoRequery(EC_OP_SEARCH_RESULTS, EC_TAG_SEARCHFILE);
		}
		// Stats polling is always on, even when the Statistics dialog
		// isn't the active tab. statgraphs->HandlePacket() also feeds
		// the Kad node-count graph on the Network -> Kad sub-tab via
		// m_kademliawnd->UpdateGraph(); gating on m_statisticswnd left
		// the Kad graph empty whenever the user wasn't sitting on
		// Statistics, and produced a visible gap in the Statistics
		// graph itself across any tab switch. Both requests are cheap
		// deltas: the graph sends m_lastTimestamp and the daemon
		// returns only points newer than that (or EC_OP_FAILED "No
		// points for graph."); the tree request honors
		// thePrefs::GetStatsInterval().
		{
			int sStatsUpdate = thePrefs::GetStatsInterval();
			uint32 msCur = theStats::GetUptimeMillis();
			if ((sStatsUpdate > 0) && ((int)(msCur - msPrevStats) > sStatsUpdate * 1000)) {
				msPrevStats = msCur;
				stattree->DoRequery();
			}
			statgraphs->DoRequery();
		}
		// Chat sessions + every message newer than our cursor, in one
		// roundtrip. Poll unconditionally — like stats above, and unlike
		// the per-tab data — so a message that arrives while the user is
		// on another tab still triggers the new-message blink. Cheap: an
		// idle daemon answers with the session list and no message
		// children. Gated on the capability; a daemon that never echoes
		// EC_TAG_CAN_CHAT does not know these opcodes, and sending one
		// asserts on the daemon before it can answer EC_OP_FAILED.
		if (m_connect->ServerSupportsChatSessions()) {
			CECPacket chat_req(EC_OP_GET_CHAT_SESSIONS);
			if (m_chatmsg_handler.Cursor()) {
				chat_req.AddTag(CECTag(EC_TAG_CHAT_MSG_ID, m_chatmsg_handler.Cursor()));
			}
			m_connect->SendRequest(&m_chatmsg_handler, &chat_req);
		}
		// Back to the roots
		request_step = 0;
		break;
	default:
		wxFAIL;
		request_step = 0;
	}

	// Check for new links once per second.
	static uint64 lastED2KLinkCheck = 0;
	uint64 now = GetTickCount64();
	if (now - lastED2KLinkCheck >= 1000) {
		AddLinksFromFile();
		lastED2KLinkCheck = now;
	}
}

void CamuleRemoteGuiApp::OnFinishedHTTPDownload(CMuleInternalEvent &WXUNUSED(event))
{
	// amulegui has no local GeoIP resolver — country codes arrive over EC from
	// the daemon (#439 / #440) — so it never starts a GeoIP download and there
	// is nothing to finish here.
}

void CamuleRemoteGuiApp::ShutDown(wxCloseEvent &WXUNUSED(evt))
{
	// A modal dialog (comments/ratings, file details, ...) runs its own event
	// loop, and an EC drop is dispatched from whichever loop is current — so a
	// shutdown can start with one of those dialogs still on the stack. Unwind
	// to the outer loop first; Quit() resumes the teardown from there.
	if (DeferShutDownToOuterLoop([this] { Quit(); })) {
		return;
	}

	// Stop the Core Timer
	delete poll_timer;
	poll_timer = NULL;

	delete connect_timeout_timer;
	connect_timeout_timer = NULL;

	m_AsioService->Stop();
	delete m_AsioService;
	m_AsioService = NULL;

	// Destroy the EC socket
	m_connect->Destroy();
	m_connect = NULL;

	//
	if (amuledlg) {
		amuledlg->DlgShutDown();
		amuledlg->Destroy();
		amuledlg = NULL;
	}
	delete m_allUploadingKnownFile;
	delete stattree;

	m_tornDown = true;
}

void CamuleRemoteGuiApp::Quit()
{
	if (m_tornDown) {
		return;
	}

	wxCloseEvent ev;
	ShutDown(ev);

	// Still unset means ShutDown() postponed itself to the outer event loop;
	// leaving the main loop now would drop the retry it queued.
	if (m_tornDown) {
		ExitMainLoop();
	}
}

bool CamuleRemoteGuiApp::OnInit()
{
	StartTickTimer();
	amuledlg = NULL;
	connect_timeout_timer = NULL;
	// ShutDown() unconditionally deletes these, but Startup() — where they
	// are allocated — only runs after a successful EC connect. Null them so
	// a connect-timeout teardown doesn't delete an indeterminate pointer.
	stattree = NULL;
	m_allUploadingKnownFile = NULL;

#if defined(__WXGTK__) && !defined(__APPLE__)
	// Set the GTK program name to the canonical app id. On Wayland,
	// GTK derives wl_app_id (xdg_toplevel.set_app_id) from
	// g_get_prgname(); compositors match wl_app_id against the
	// .desktop filename to bind windows to launcher icons. Without
	// this the binding falls back to argv[0], which differs across
	// packaging formats (AppImage's argv[0] is "aMuleGUI", distro
	// installs use "amulegui", Flatpak renames the .desktop entirely).
	// On X11 the same value also feeds into WM_CLASS, matching
	// StartupWMClass=org.amule.aMule.gui in the .desktop file. Must run
	// before any GTK window is created — same fix the monolithic amule
	// has in CamuleApp::OnInit; amulegui shipped without it, so on
	// GNOME / wlroots the taskbar icon never bound to the launcher
	// and showed the generic fallback. (#562 follow-up.)
	// Skipped on macOS even under wxGTK (MacPorts): no Wayland or
	// .desktop binding exists, and app identity is set via Info.plist
	// in the .app bundle. Dropping the call lets that build skip the
	// glib2 dep entirely (#641).
	g_set_prgname("org.amule.aMule.gui");
#endif

	// Register the embedded-PNG art provider before any UI work.
	// wxArtProvider::Push takes ownership of the pointer; wx tears
	// the providers down at app exit.
	wxArtProvider::Push(new CamuleArtProvider());

	// Must happen before any window exists; the connect dialog below is the
	// first one amulegui creates. Without this amulegui stayed light on
	// Windows while the monolithic amule, which has always asked, went dark.
	FollowSystemAppearance();

	// Get theApp
	theApp = &wxGetApp();

	// Handle uncaught exceptions
	InstallMuleExceptionHandler();

	// Parse cmdline arguments.
	if (!InitCommon(AMULE_APP_BASE::argc, AMULE_APP_BASE::argv)) {
		return false;
	}

	// Initialize wx sockets (needed for http download in background with Asio sockets)
	wxSocketBase::Initialize();

	// Create the polling timer
	poll_timer = new wxTimer(this, ID_CORE_TIMER_EVENT);
	if (!poll_timer) {
		AddLogLineCS(_("Fatal Error: Failed to create Poll Timer"));
		OnExit();
	}

	m_connect = new CRemoteConnect(this);

	m_AsioService = new CAsioService;

	glob_prefs = new CPreferencesRem(m_connect);
	long enableZLIB;
	wxConfig::Get()->Read("/EC/ZLIB", &enableZLIB, 1);
	m_connect->SetCapabilities(enableZLIB != 0, true, false); // ZLIB, UTF8 numbers, notification
	// amulegui addresses searches by daemon-allocated ID (per-tab, several at
	// once); advertise the multi-search capability. An old daemon won't echo
	// it and amulegui stays single-search (ServerSupportsMultiSearch()).
	m_connect->SetCanMultiSearch(true);
	// amulegui shows incoming friend/chat messages read-only; ask the daemon
	// to relay them (polled via EC_OP_GET_CHAT_MESSAGES). An old daemon won't
	// echo the capability and amulegui simply never polls (ServerSupportsChatSessions()).
	m_connect->SetCanChatSessions(true);
	// The ForceZLIB override is read from the connection dialog
	// (see ShowConnectionDialog) so the user's checkbox choice in this
	// session overrides the persisted /EC/ForceZLIB value.

	InitCustomLanguages();
	InitLocale(m_locale, StrLang2wx(thePrefs::GetLanguageID()));

	if (ShowConnectionDialog()) {
		// The watchdog timer is armed inside ShowConnectionDialog right
		// before each ConnectToCore call — the retry loop re-arms it on
		// every attempt, so OnInit doesn't need to touch it here.
		AddLogLineNS(_("Going to event loop..."));
		return true;
	}

	// User cancelled (or ShowConnectionDialog failed before reaching the
	// connect step). Tear down the partial init so the Asio thread pool,
	// poll timer and remote-connect socket don't leak — wx will never
	// call ShutDown() / OnExit() because the main loop isn't entered when
	// OnInit() returns false, so we have to unwind manually here. Without
	// this, wx reports "4 threads were not terminated by the application".
	if (m_AsioService) {
		m_AsioService->Stop();
		delete m_AsioService;
		m_AsioService = NULL;
	}
	if (m_connect) {
		m_connect->Destroy();
		m_connect = NULL;
	}
	if (poll_timer) {
		delete poll_timer;
		poll_timer = NULL;
	}
	return false;
}

bool CamuleRemoteGuiApp::CryptoAvailable() const
{
	return thePrefs::IsSecureIdentEnabled(); // good enough
}

bool CamuleRemoteGuiApp::ShowConnectionDialog()
{
	// The dialog is kept alive across retry attempts so the values the
	// user typed (host / port / password / Force-ZLIB) survive a wrong
	// guess — they only need to fix the field that was wrong instead of
	// re-typing everything. Destroyed in Startup() on success or below
	// when the user cancels.
	if (!dialog) {
		dialog = new CEConnectDlg;
	}

	while (true) {
		if (m_skipConnectionDialog) {
			wxCommandEvent evt;
			dialog->OnOK(evt);
			// --skip is a one-shot: on retry the user must see the
			// dialog so they can correct the bad values.
			m_skipConnectionDialog = false;
		} else if (dialog->ShowModal() != wxID_OK) {
			dialog->Destroy();
			dialog = NULL;
			return false;
		}

		AddLogLineNS(_("Connecting..."));
		// Watchdog on the EC connect. When the host is unreachable the
		// TCP SYN can silently time out over several minutes while the
		// main loop is running with no visible window, which the OS
		// reports as "not responding". Fire a shorter timeout so we
		// can show an error and re-prompt instead. Re-armed on every
		// retry attempt so the user gets the same 15s budget each time.
		delete connect_timeout_timer;
		connect_timeout_timer = new wxTimer(this, ID_REMOTE_CONNECT_TIMEOUT_TIMER);
		connect_timeout_timer->StartOnce(15000);

		// Apply the dialog's checkbox states to the EC client before each
		// ConnectToCore attempt (re-applied per retry so the user can
		// toggle them between attempts if they need to). ResetEcConnect()
		// recreates m_connect on a failed handshake and deliberately does
		// not re-apply these two: the dialog owns them and sets them again
		// on the fresh object right here.
		m_connect->SetForceZlib(dialog->ForceZlib());
		m_connect->SetCanAEAD(dialog->Encryption());
		if (m_connect->ConnectToCore(
			    dialog->Host(), dialog->Port(), dialog->PassHash(), "amule-remote", "0x0001")) {
			// Sync part succeeded; async OnECConnection will
			// resolve the auth outcome.
			return true;
		}

		// Sync failure (DNS / immediate connect-refused). The async
		// path won't fire — cancel the watchdog ourselves, show the
		// error, recreate the EC client so its half-baked socket
		// state is gone, and loop back to the dialog.
		connect_timeout_timer->Stop();
		delete connect_timeout_timer;
		connect_timeout_timer = NULL;
		wxMessageBox(_("Connection failed. Please check the host, port, and password."),
			_("ERROR"),
			wxOK | wxICON_ERROR);
		ResetEcConnect();
	}
}

void CamuleRemoteGuiApp::ResetEcConnect()
{
	// Tear down the busted EC client and recreate a fresh one. The
	// CRemoteConnect's socket / auth state isn't safe to reuse after
	// a failed handshake. glob_prefs holds a reference to m_connect
	// so it's reborn alongside. Both objects are only fully wired
	// into the rest of the app by Startup(), which doesn't run until
	// a successful connect — recreating them here is safe.
	delete glob_prefs;
	glob_prefs = NULL;
	if (m_connect) {
		m_connect->Destroy();
		m_connect = NULL;
	}
	m_connect = new CRemoteConnect(this);
	glob_prefs = new CPreferencesRem(m_connect);
	long enableZLIB;
	wxConfig::Get()->Read("/EC/ZLIB", &enableZLIB, 1);
	m_connect->SetCapabilities(enableZLIB != 0, true, false);
	m_connect->SetCanMultiSearch(true);
	m_connect->SetCanChatSessions(true);
}

void CamuleRemoteGuiApp::OnECConnection(wxEvent &event)
{
	// Connect attempt resolved one way or the other — kill the watchdog.
	if (connect_timeout_timer) {
		connect_timeout_timer->Stop();
		delete connect_timeout_timer;
		connect_timeout_timer = NULL;
	}
	wxECSocketEvent &evt = *((wxECSocketEvent *)&event);
	AddLogLineNS(_("Remote GUI EC event handler"));
	wxString reply = evt.GetServerReply();
	AddLogLineC(reply);
	if (evt.GetResult() == true) {
		if (m_reconnecting) {
			// Reconnected: close the modal reconnect dialog. Execution
			// resumes right after ShowModal() in ShowReconnectDialog(),
			// which re-arms the reconcile prune and restarts polling.
			if (m_reconnectDlg) {
				m_reconnectDlg->EndModal(wxID_OK);
			} else {
				// Reconnected behind a minimised window, so there is no
				// modal loop to unwind and nothing to close -- finish
				// here instead (issue #806).
				FinishReconnect(wxID_OK);
			}
		} else {
			// Connected - go to next init step
			glob_prefs->LoadRemote();
		}
	} else if (m_reconnecting) {
		// A reconnect attempt failed. Space out the next one; the modal
		// dialog stays up with the attempt count and an Abort button.
		ScheduleNextReconnect();
	} else if (dialog) {
		// Connect failed during the initial attempt or a previous
		// retry (dialog is still alive — it's only destroyed in
		// Startup() after a successful connect). Show the error,
		// reset the EC client, and reopen the dialog with the
		// previous values still in place so the user can fix the
		// wrong field and try again. If the user cancels the retry,
		// then we shut down.
		wxMessageBox((CFormat(_("Connection Failed. Unable to connect to %s:%d\n")) % dialog->Host() %
				     dialog->Port()) +
				     reply,
			_("ERROR"),
			wxOK | wxICON_ERROR);
		ResetEcConnect();
		if (!ShowConnectionDialog()) {
			AddLogLineNS(_("Going down"));
			Quit();
		}
	} else {
		// Connection lost after startup (e.g. the machine slept and the
		// EC socket dropped). Don't quit — freeze the UI and reconnect in
		// the background until we're back or the user aborts (issue #444).
		BeginReconnect();
	}
}

void CamuleRemoteGuiApp::OnConnectTimeout(wxTimerEvent &)
{
	delete connect_timeout_timer;
	connect_timeout_timer = NULL;

	if (m_reconnecting) {
		// This reconnect attempt hung (host unreachable, SYN black-holed).
		// Treat it as a failed attempt and space out the next one; the
		// modal reconnect dialog stays up with its Abort button (#444).
		AddLogLineCS(_("Reconnect attempt timed out; retrying."));
		ScheduleNextReconnect();
		return;
	}

	wxString host = dialog ? dialog->Host() : wxString();
	long port = dialog ? dialog->Port() : 0;
	wxMessageBox(
		CFormat(_(
			"Connection timed out. Unable to reach %s:%d within the allotted time.\nPlease check "
			"the host, port and that aMule is running with External Connections enabled.")) %
			host % port,
		_("ERROR"),
		wxOK | wxICON_ERROR);

	// Reset the EC client and reopen the dialog so the user can
	// correct the host / port / etc. If they cancel, then quit.
	ResetEcConnect();
	if (!ShowConnectionDialog()) {
		Quit();
	}
}

void CamuleRemoteGuiApp::BeginReconnect()
{
	if (m_reconnecting) {
		return;
	}
	m_reconnecting = true;
	m_reconnectAttempt = 0;

	AddLogLineCS(_("Connection to the remote core was lost. Trying to reconnect..."));

	// Freeze polling while disconnected: the poll timer would fire
	// GET_UPDATE at a dead socket, and the GUI timer animates stale data.
	if (poll_timer) {
		poll_timer->Stop();
	}
	if (amuledlg) {
		amuledlg->StopGuiTimer();
	}

	if (!m_reconnectTimer) {
		m_reconnectTimer = new wxTimer(this, ID_REMOTE_RECONNECT_TIMER);
	}

	// Kick off the first attempt before deciding about the dialog, so the
	// common case -- a blip that reconnects on the first try -- can be over
	// with before anything is drawn.
	AttemptReconnect();

	// The dialog earns its intrusion by explaining a frozen window. With the
	// window minimised or hidden to tray there is nothing on screen to
	// explain, and a modal appearing over whatever the user is actually doing
	// is worse than silence (issue #806). Retry quietly instead; the log still
	// carries every attempt, and OnMainWindowRestored() puts the dialog up if
	// the user comes back while this is still going.
	//
	// "Visible" has to mean both halves: minimized to Dock/taskbar keeps
	// IsShown() true with nothing on screen, and hidden to tray leaves the
	// iconized bit clear while the frame is gone. CamuleDlg tracks the
	// iconized half from wxIconizeEvent rather than wxFrame::IsIconized(),
	// which lies on wxGTK mid-transition. Compositors that never report
	// iconize at all (Wayland xdg-shell has no such notification) keep the
	// old behaviour for the minimize case.
	if (amuledlg && !amuledlg->IsVisibleToUser()) {
		return;
	}

	ShowReconnectDialog();
}

void CamuleRemoteGuiApp::ShowReconnectDialog()
{
	if (!m_reconnecting || m_reconnectDlg) {
		return;
	}

	m_reconnectDlg = new CReconnectDialog(amuledlg, CFormat(wxT("%s:%d")) % m_ecHost % m_ecPort);
	// The retry loop has been running without us, so open on what it is
	// actually doing rather than on "attempt 1": mid-countdown after a failed
	// attempt, or in the middle of one.
	if (m_reconnectCountdown > 0) {
		m_reconnectDlg->SetCountdown(m_reconnectCountdown);
	} else {
		m_reconnectDlg->SetAttempt(m_reconnectAttempt);
	}

	// Run it modally: the retry timer and OnECConnection pump inside
	// ShowModal(). A success calls EndModal(wxID_OK); the Abort button ends it
	// with wxID_CANCEL.
	const int result = m_reconnectDlg->ShowModal();
	m_reconnectDlg->Destroy();
	m_reconnectDlg = nullptr;
	FinishReconnect(result);
}

void CamuleRemoteGuiApp::UpdateCoreVersionIndicator()
{
	if (!amuledlg || !m_connect) {
		return;
	}
	// Cipher names are protocol identifiers ("AES-128-GCM"), not prose, so
	// only the not-encrypted case needs the catalog.
	const bool encrypted = m_connect->IsAEADEnabled();
	const wxString encryption =
		encrypted ? wxString::FromAscii(ECCrypt::CipherName(m_connect->GetAEADCipher()))
			  : _("Disabled");

	// Empty against a daemon too old to send EC_TAG_SERVER_VERSION; the
	// dialog hides the field in that case rather than showing a blank.
	amuledlg->ShowCoreVersion(m_connect->GetServerVersion(),
		CFormat(wxT("%s:%d")) % m_ecHost % m_ecPort,
		encryption,
		encrypted);
}

void CamuleRemoteGuiApp::FinishReconnect(int result)
{
	m_reconnecting = false;
	m_reconnectCountdown = 0;
	if (m_reconnectTimer) {
		m_reconnectTimer->Stop();
	}
	delete connect_timeout_timer;
	connect_timeout_timer = nullptr;

	if (result == wxID_OK) {
		AddLogLineCS(_("Reconnected to the remote core."));

		// The daemon may have been upgraded while we were away, so re-read
		// the version rather than leaving the pre-drop one on screen.
		UpdateCoreVersionIndicator();

		// Everything we hold is keyed by ECID, and an ECID only means
		// something within one daemon process: CECID hands them out from a
		// counter that restarts with the process, so a restarted daemon
		// reissues the same numbers in whatever order it loads files this
		// time. Reconciling in place across that pairs our objects with
		// whatever now happens to share their number.
		//
		// EC_TAG_SESSION_ID says which process we're talking to. Same value
		// means the socket dropped but the daemon lived (a sleeping laptop,
		// a dead tunnel), so the in-place reconcile below is right and keeps
		// scroll and selection. Anything else -- a different value, or none
		// at all because the daemon predates the tag -- means we cannot
		// trust a single ID we hold, and starting over is the only correct
		// answer even though it costs the user their scroll position.
		const uint64 sessionId = m_connect ? m_connect->GetServerSessionId() : 0;
		const bool sameSession = sessionId != 0 && sessionId == m_ecSessionId;
		m_ecSessionId = sessionId;

		if (!sameSession) {
			AddLogLineNS(_("The remote core was restarted; reloading."));
			if (knownfiles) {
				knownfiles->ResetForNewDaemonSession();
			}
			if (clientlist) {
				clientlist->ResetForNewSession();
			}
			if (serverlist) {
				serverlist->ResetForNewSession();
			}
			if (friendlist) {
				friendlist->ResetForNewSession();
			}
			if (searchlist) {
				// Searches survive a restart -- amuled persists them and
				// re-registers each restored one, so the daemon hands the
				// same results back under their original search ids. What
				// does NOT survive is the per-result ECID: the counter
				// restarts with the process, so every restored result
				// arrives as an id this client has never seen and is added
				// alongside the copy it already holds.
				//
				// Nothing reaps the stale copy on its own. The search list
				// deletes only on an explicit EC_TAG_FILE_REMOVED tombstone
				// (it opted out of the base class's absence sweep so an idle
				// search costs no traffic), and a daemon that has just
				// started never emits a tombstone for an ECID it never knew
				// about. Both copies therefore stay -- issue #1101, where
				// every result appears twice after a restart.
				searchlist->ResetForNewSession();
				if (amuledlg && amuledlg->m_searchwnd) {
					// Frees every result, so the tabs must drop their rows
					// before anything repaints through them.
					amuledlg->m_searchwnd->ResetResultViews();
				}
			}
		} else if (knownfiles) {
			// Same daemon: the next full poll reconciles every list against
			// the fresh snapshot in place (update / add / prune) — no wipe,
			// so scroll and selection survive.
			knownfiles->ArmReconnectReconcile();
		}
		if (poll_timer) {
			poll_timer->Start(EC_POLL_INTERVAL_MS);
		}
		if (amuledlg) {
			amuledlg->StartGuiTimer();
		}
	} else {
		// User aborted the reconnect.
		AddLogLineNS(_("Going down"));
		Quit();
	}
}

void CamuleRemoteGuiApp::OnMainWindowRestored()
{
	if (!m_reconnecting || m_reconnectDlg) {
		return;
	}
	// Not straight from here: this runs inside the iconize event handler, and
	// ShowReconnectDialog() ends in either a nested modal loop or Quit(). Let
	// the handler return first (#738 is what tearing the main window down from
	// a nested loop costs).
	CallAfter(&CamuleRemoteGuiApp::ShowReconnectDialog);
}

void CamuleRemoteGuiApp::AttemptReconnect()
{
	m_reconnectAttempt++;
	if (m_reconnectDlg) {
		m_reconnectDlg->SetAttempt(m_reconnectAttempt);
	}

	AddLogLineCS(CFormat(_("Reconnect attempt %d: connecting to %s:%d")) % m_reconnectAttempt % m_ecHost %
		     m_ecPort);

	// Reset ALL layers of the reused connection: the pending-request FIFO
	// (orphaned handlers from requests the dropped socket left unanswered
	// would otherwise mis-pair with the new session's replies — a stats reply
	// routed to the file-list handler wipes the download / shared lists, #444),
	// the EC packet-reassembly state (a stale mid-packet read left over from
	// the drop would otherwise misparse the new session's first bytes and the
	// login would fail) and a fresh asio socket on the SAME CRemoteConnect
	// object (keeps every remote container's m_conn pointer valid). Locally
	// chosen capability flags persist deliberately -- SetCapabilities is not
	// called again here, and what this end can do has not changed -- so
	// ConnectToCore re-runs the login handshake as on first connect. Flags
	// agreed with the previous daemon are a different matter: the next one may
	// not support them, so they are dropped here rather than inside
	// ResetProtocolState, which other CECSocket users share.
	m_connect->DiscardRequestQueue();
	m_connect->ResetProtocolState();
	m_connect->ClearPeerNegotiatedFlags();
	m_connect->ResetForReconnect();

	// Per-attempt watchdog so an attempt that hangs (host unreachable, SYN
	// black-holed) doesn't stall the retry loop; OnConnectTimeout treats a
	// fire as a failed attempt. Cancelled by OnECConnection when the attempt
	// resolves either way.
	delete connect_timeout_timer;
	connect_timeout_timer = new wxTimer(this, ID_REMOTE_CONNECT_TIMEOUT_TIMER);
	connect_timeout_timer->StartOnce(15000);

	if (!m_connect->ConnectToCore(m_ecHost, m_ecPort, m_ecPass, "amule-remote", "0x0001")) {
		// Couldn't even initiate the connect — space out the next attempt.
		AddLogLineCS(_("Reconnect could not start; retrying shortly."));
		delete connect_timeout_timer;
		connect_timeout_timer = nullptr;
		ScheduleNextReconnect();
	}
	// else: async — OnECConnection resolves success/failure.
}

void CamuleRemoteGuiApp::ScheduleNextReconnect()
{
	// Drop any pending per-attempt watchdog, then count down 5 s to the next
	// attempt via a repeating 1 s tick that updates the dialog, so the user
	// sees when the retry will fire and an unreachable daemon isn't hammered.
	delete connect_timeout_timer;
	connect_timeout_timer = nullptr;
	m_reconnectCountdown = 5;
	if (m_reconnectDlg) {
		m_reconnectDlg->SetCountdown(m_reconnectCountdown);
	}
	if (m_reconnectTimer) {
		m_reconnectTimer->Start(1000);
	}
}

void CamuleRemoteGuiApp::OnReconnectTimer(wxTimerEvent &)
{
	if (!m_reconnecting) {
		return;
	}
	if (--m_reconnectCountdown > 0) {
		if (m_reconnectDlg) {
			m_reconnectDlg->SetCountdown(m_reconnectCountdown);
		}
		return;
	}
	// Countdown elapsed — fire the next attempt.
	m_reconnectTimer->Stop();
	AttemptReconnect();
}

void CamuleRemoteGuiApp::OnECInitDone(wxEvent &)
{
	Startup();
}

void CamuleRemoteGuiApp::OnNotifyEvent(CMuleGUIEvent &evt)
{
	// Two kinds of notification arrive here and they need opposite treatment.
	//
	// DoNotify() drives what the GUI displays. Its handlers read
	// theApp->amuledlg->m_transferwnd and friends without checking the dialog
	// itself, so one delivered after the window is gone dereferences null
	// (EXC_BAD_ACCESS at 0x3c0 -- m_transferwnd's offset in CamuleDlg).
	// amulegui destroys its dialog whenever the EC link drops, a remote core
	// restart being the everyday way that happens, and the pending-event
	// queue is not drained first. HandleNotification() already declines to
	// run one of these inline when there is no window; this is the same test
	// for the queued half.
	//
	// DoNotifyAlways() is how the socket layer reaches the main thread
	// (CoreNotify_LibSocketConnect and friends, posted from asio callbacks).
	// Those must run whatever the GUI is doing -- amulegui has no main window
	// until an EC connection has been made, so dropping them means the
	// connection never completes and every attempt ends at the 15 s watchdog.
	if (evt.IsAlways() || amuledlg) {
		evt.Notify();
	}
}

void CamuleRemoteGuiApp::Startup()
{

	if (dialog->SaveUserPass()) {
		wxConfig::Get()->Write("/EC/Host", dialog->Host());
		wxConfig::Get()->Write("/EC/Port", dialog->Port());
		wxConfig::Get()->Write("/EC/Password", dialog->PassHash());
		wxConfig::Get()->Write("/EC/ForceZLIB", dialog->ForceZlib() ? 1l : 0l);
		wxConfig::Get()->Write("/EC/Encryption", dialog->Encryption() ? 1l : 0l);
	}
	// Capture the EC connection params so a post-startup reconnect
	// (issue #444) can re-dial without the (about-to-be-destroyed) dialog.
	m_ecHost = dialog->Host();
	m_ecPort = dialog->Port();
	m_ecPass = dialog->PassHash();
	// Baseline for the reconnect comparison: which daemon process this
	// first connection reached. Stays 0 against a daemon that doesn't send
	// EC_TAG_SESSION_ID, which makes every later reconnect take the
	// start-over path.
	//
	// Not null-guarded, deliberately: Startup() only runs on a successful
	// connect and the lines below dereference m_connect unconditionally, so
	// a guard here would protect nothing while claiming the pointer is
	// optional -- which is also how the static analyser reads it, and it
	// then reports the dereference below as reachable with null.
	m_ecSessionId = m_connect->GetServerSessionId();

	dialog->Destroy();
	dialog = NULL;

	m_ConnState = 0;
	m_clientID = 0;

	serverconnect = new CServerConnectRem(m_connect);
	m_statistics = new CStatistics(*m_connect);
	stattree = new CStatTreeRem(m_connect);
	statgraphs = new CStatGraphRem(m_connect);

	clientlist = new CUpDownClientListRem(m_connect);
	searchlist = new CSearchListRem(m_connect);
	serverlist = new CServerListRem(m_connect);
	friendlist = new CFriendListRem(m_connect);

	sharedfiles = new CSharedFilesRem(m_connect);
	knownfiles = new CKnownFilesRem(m_connect);

	downloadqueue = new CDownQueueRem(m_connect);
	ipfilter = new CIPFilterRem(m_connect);

	m_allUploadingKnownFile = new CKnownFile;

	// Must run before InitGui() below: the CamuleDlg constructor reads
	// UseTrayIcon() to decide whether to create the tray icon at all, so a
	// guard applied after it would leave the icon built for a backend that
	// cannot show it. Nothing here waits on the EC connection -- these are
	// amulegui's own local preferences, not packed by CEC_Prefs_Packet and
	// with no category in the EC_PREFS_* mask -- so OnInit() would have
	// worked equally well; this sits next to the call it has to precede.
	SanitiseTrayPreferences();

	// Create main dialog
	InitGui(m_geometryEnabled, m_geometryString);

	// Needs the dialog to exist, so it cannot happen with the rest of the
	// handshake bookkeeping above.
	UpdateCoreVersionIndicator();

	// Forward wxLog events to CLogger
	wxLog::SetActiveTarget(new CLoggerTarget);
	knownfiles->DoRequery(EC_OP_GET_UPDATE, EC_TAG_KNOWNFILE);

	// Start the Poll Timer
	poll_timer->Start(EC_POLL_INTERVAL_MS);
	amuledlg->StartGuiTimer();

	// Drain any pre-connect URL queued by ProtocolHandler_QueueSchemeLink
	// (cold-launch: click → wrote ED2KLinks → user then connects). Saves
	// a ~1 s wait for OnPollTimer to notice the file. No-op if empty.
	AddLinksFromFile();

	// amulegui does no local GeoIP resolution: the daemon resolves country
	// codes and sends them over EC (#439 / #440), and amulegui only renders the
	// flags. GeoIP settings are managed against the daemon, not locally.
}

int CamuleRemoteGuiApp::ShowAlert(wxString msg, wxString title, int flags)
{
	return CamuleGuiBase::ShowAlert(msg, title, flags);
}

void CamuleRemoteGuiApp::AddRemoteLogLine(const wxString &line)
{
	amuledlg->AddLogLine(line);
}

void CamuleRemoteGuiApp::BeginRemoteLogBatch()
{
	amuledlg->BeginLogBatch();
}

void CamuleRemoteGuiApp::EndRemoteLogBatch()
{
	amuledlg->EndLogBatch();
}

int CamuleRemoteGuiApp::InitGui(bool geometry_enabled, wxString &geom_string)
{
	CamuleGuiBase::InitGui(geometry_enabled, geom_string);
	SetTopWindow(amuledlg);
	AddLogLineN(_(
		"Ready")); // The first log line after the window is up triggers output of all the ones before
	return 0;
}

bool CamuleRemoteGuiApp::CopyTextToClipboard(wxString strText)
{
	return CamuleGuiBase::CopyTextToClipboard(strText);
}

uint32 CamuleRemoteGuiApp::GetPublicIP()
{
	return 0;
}

wxString CamuleRemoteGuiApp::GetLog(bool reset)
{
	if (reset) {
		amuledlg->ResetLog(ID_LOGVIEW);
		CECPacket req(EC_OP_RESET_LOG);
		m_connect->SendPacket(&req);
	}
	return "";
}

wxString CamuleRemoteGuiApp::GetServerLog(bool reset)
{
	if (reset) {
		// Mirror the GetLog reset path: clear the remote buffer, the
		// local snapshot we diff against, and the on-screen text ctrl.
		CECPacket req(EC_OP_CLEAR_SERVERINFO);
		m_connect->SendPacket(&req);
		m_serverinfo_handler.m_seenSoFar.clear();
		amuledlg->ResetLog(ID_SERVERINFO);
	}
	return "";
}

void CamuleRemoteGuiApp::AddServerMessageLine(wxString &msg)
{
	// Drives the same CamuleDlg::AddServerMessageLine the monolithic
	// build calls -- ID_SERVERINFO text ctrl + 500-char truncation +
	// auto-scroll. Cumulative-log diffing happens in
	// CServerInfoHandlerRem::HandlePacket; by the time we land here
	// `msg` is a single new line, ready to append.
	amuledlg->AddServerMessageLine(msg);
}

void CServerInfoHandlerRem::HandlePacket(const CECPacket *packet)
{
	// amuled answers EC_OP_GET_SERVERINFO with one EC_TAG_STRING
	// carrying the full cumulative server_msg buffer. Diff against
	// what we've already shown so the text ctrl receives only new
	// lines (preserves scroll position and avoids re-rendering tens
	// of KB on every poll).
	const CECTag *tag = packet->GetFirstTagSafe();
	if (!tag || !tag->IsString()) {
		return;
	}
	const wxString fullLog = tag->GetStringData();

	wxString delta;
	if (fullLog.StartsWith(m_seenSoFar)) {
		delta = fullLog.Mid(m_seenSoFar.length());
	} else {
		// amuled was restarted, or someone else issued a clear, so the
		// remote cumulative buffer is shorter or unrelated. Wipe the
		// local view and start fresh from the current snapshot.
		// Guarded like every other view call reached from an EC reply: a
		// reply can land while amulegui has no dialog -- it is destroyed
		// whenever the link drops and rebuilt by Startup() after the next
		// connect, and replies already in flight are not drained. The
		// bookkeeping around these calls still has to run, so only the call
		// into the view is skipped.
		if (theApp->amuledlg) {
			theApp->amuledlg->ResetLog(ID_SERVERINFO);
		}
		delta = fullLog;
	}
	m_seenSoFar = fullLog;

	while (!delta.IsEmpty()) {
		wxString line = delta.BeforeFirst('\n');
		delta = delta.AfterFirst('\n');
		if (!line.IsEmpty()) {
			theApp->AddServerMessageLine(line);
		}
	}
}

void CChatMsgHandlerRem::HandlePacket(const CECPacket *packet)
{
	// EC_OP_CHAT_SESSIONS: a top-level EC_TAG_CHAT_MSG_ID carrying the store's
	// current last id, then one EC_TAG_CHAT_SESSION per session with only the
	// messages newer than the cursor we sent.
	if (!theApp->amuledlg || !theApp->amuledlg->m_chatwnd) {
		return;
	}
	CChatWnd *chatwnd = theApp->amuledlg->m_chatwnd;

	// First reply of a connection backfills history rather than announcing
	// it: replay must not light the Messages toolbar button up for messages
	// the user already read on another client. Only what arrives after we
	// have a cursor is genuinely new.
	const bool backfill = (m_cursor == 0);

	std::set<uint64> present;
	for (const CECTag &sessionTag : *packet) {
		if (sessionTag.GetTagName() != EC_TAG_CHAT_SESSION) {
			continue;
		}
		const uint64 gui_id = sessionTag.GetInt();
		present.insert(gui_id);

		wxString name;
		if (const CECTag *nameTag = sessionTag.GetTagByName(EC_TAG_CHAT_PEER_NAME)) {
			name = nameTag->GetStringData();
		}
		if (name.IsEmpty()) {
			name = ChatPeerFallbackName(gui_id);
		}

		// Open the tab even when the session has no new messages: that is
		// how a session started before we connected becomes visible at all.
		// Unfocused, so a session appearing on its own cannot steal the
		// selection from whatever the user is doing.
		chatwnd->StartSessionByID(gui_id, name);

		for (const CECTag &msgTag : sessionTag) {
			if (msgTag.GetTagName() != EC_TAG_CHAT_MESSAGE) {
				continue;
			}
			const CECTag *dirTag = msgTag.GetTagByName(EC_TAG_CHAT_DIRECTION);
			const bool outgoing = dirTag && dirTag->GetInt() != 0;
			chatwnd->AppendStoredMessage(
				gui_id, name, msgTag.GetStringData(), outgoing, /*blink=*/!backfill);
		}
	}

	// A session we are tracking that is absent from the reply was closed by
	// another client (or evicted). Drop the tab without sending a close of
	// our own — the core has already forgotten it. Snapshot first: closing a
	// tab mutates the set being iterated.
	const std::set<uint64> tracked = m_sessions;
	for (uint64 gui_id : tracked) {
		if (!present.count(gui_id)) {
			chatwnd->EndSessionFromCore(gui_id);
		}
	}
	m_sessions = present;

	if (const CECTag *cursorTag = packet->GetTagByName(EC_TAG_CHAT_MSG_ID)) {
		// Advance even when nothing came back, so evicted ids are not asked
		// for forever.
		m_cursor = cursorTag->GetInt();
	}
}

bool CamuleRemoteGuiApp::AddServer(CServer *server, bool)
{
	CECPacket req(EC_OP_SERVER_ADD);
	req.AddTag(
		CECTag(EC_TAG_SERVER_ADDRESS, CFormat("%s:%d") % server->GetAddress() % server->GetPort()));
	req.AddTag(CECTag(EC_TAG_SERVER_NAME, server->GetListName()));
	m_connect->SendPacket(&req);

	return true;
}

bool CamuleRemoteGuiApp::IsFirewalled() const
{
	if (IsConnectedED2K() && !serverconnect->IsLowID()) {
		return false;
	}

	return IsFirewalledKad();
}

bool CamuleRemoteGuiApp::IsConnectedED2K() const
{
	return serverconnect && serverconnect->IsConnected();
}

void CamuleRemoteGuiApp::StartKad()
{
	m_connect->StartKad();
}

void CamuleRemoteGuiApp::StopKad()
{
	m_connect->StopKad();
}

void CamuleRemoteGuiApp::BootstrapKad(uint32 ip, uint16 port)
{
	CECPacket req(EC_OP_KAD_BOOTSTRAP_FROM_IP);
	req.AddTag(CECTag(EC_TAG_BOOTSTRAP_IP, ip));
	req.AddTag(CECTag(EC_TAG_BOOTSTRAP_PORT, port));

	m_connect->SendPacket(&req);
}

void CamuleRemoteGuiApp::UpdateNotesDat(const wxString &url)
{
	CECPacket req(EC_OP_KAD_UPDATE_FROM_URL);
	req.AddTag(CECTag(EC_TAG_KADEMLIA_UPDATE_URL, url));

	m_connect->SendPacket(&req);
}

void CamuleRemoteGuiApp::DisconnectED2K()
{
	if (IsConnectedED2K()) {
		m_connect->DisconnectED2K();
	}
}

uint32 CamuleRemoteGuiApp::GetED2KID() const
{
	return serverconnect ? serverconnect->GetClientID() : 0;
}

uint32 CamuleRemoteGuiApp::GetID() const
{
	return m_clientID;
}

void CamuleRemoteGuiApp::ShowUserCount()
{
	wxString buffer;

	static const wxString s_singlenetstatusformat = _("Users: %s | Files: %s");
	static const wxString s_bothnetstatusformat = _("Users: E: %s K: %s | Files: E: %s K: %s");

	if (thePrefs::GetNetworkED2K() && thePrefs::GetNetworkKademlia()) {
		buffer = CFormat(s_bothnetstatusformat) % CastItoIShort(theStats::GetED2KUsers()) %
			 CastItoIShort(theStats::GetKadUsers()) % CastItoIShort(theStats::GetED2KFiles()) %
			 CastItoIShort(theStats::GetKadFiles());
	} else if (thePrefs::GetNetworkED2K()) {
		buffer = CFormat(s_singlenetstatusformat) % CastItoIShort(theStats::GetED2KUsers()) %
			 CastItoIShort(theStats::GetED2KFiles());
	} else if (thePrefs::GetNetworkKademlia()) {
		buffer = CFormat(s_singlenetstatusformat) % CastItoIShort(theStats::GetKadUsers()) %
			 CastItoIShort(theStats::GetKadFiles());
	} else {
		buffer = _("No networks selected");
	}

	Notify_ShowUserCount(buffer);
}

/*
 * Preferences: holds both local and remote settings.
 *
 * First, everything is loaded from local config file. Later, settings
 * that are relevant on remote side only are loaded thru EC
 */
CPreferencesRem::CPreferencesRem(CRemoteConnect *conn)
{
	m_conn = conn;

	//
	// Settings queried from remote side
	//
	m_exchange_send_selected_prefs = EC_PREFS_GENERAL | EC_PREFS_CONNECTIONS | EC_PREFS_MESSAGEFILTER |
					 EC_PREFS_ONLINESIG | EC_PREFS_SERVERS | EC_PREFS_FILES |
					 EC_PREFS_DIRECTORIES | EC_PREFS_SECURITY | EC_PREFS_CORETWEAKS |
					 EC_PREFS_REMOTECONTROLS | EC_PREFS_KADEMLIA | EC_PREFS_IP2COUNTRY;
	m_exchange_recv_selected_prefs = m_exchange_send_selected_prefs | EC_PREFS_CATEGORIES;
}

void CPreferencesRem::HandlePacket(const CECPacket *packet)
{
	static_cast<const CEC_Prefs_Packet *>(packet)->Apply();

	const CECTag *cat_tags = packet->GetTagByName(EC_TAG_PREFS_CATEGORIES);
	if (cat_tags) {
		for (CECTag::const_iterator it = cat_tags->begin(); it != cat_tags->end(); ++it) {
			const CECTag &cat_tag = *it;
			Category_Struct *cat = new Category_Struct;
			cat->title = cat_tag.GetTagByName(EC_TAG_CATEGORY_TITLE)->GetStringData();
			cat->path = CPath(cat_tag.GetTagByName(EC_TAG_CATEGORY_PATH)->GetStringData());
			cat->comment = cat_tag.GetTagByName(EC_TAG_CATEGORY_COMMENT)->GetStringData();
			cat->color = cat_tag.GetTagByName(EC_TAG_CATEGORY_COLOR)->GetInt();
			cat->prio = cat_tag.GetTagByName(EC_TAG_CATEGORY_PRIO)->GetInt();
			theApp->glob_prefs->AddCat(cat);
		}
	} else {
		Category_Struct *cat = new Category_Struct;
		cat->title = _("All");
		cat->color = 0;
		cat->prio = PR_NORMAL;
		theApp->glob_prefs->AddCat(cat);
	}
	wxECInitDoneEvent event;
	theApp->AddPendingEvent(event);
}

bool CPreferencesRem::LoadRemote()
{
#ifdef GEOIP_GUI
	// Assume the core has NO GeoIP support until it says otherwise. A 3.1+ core
	// built with ENABLE_IP2COUNTRY answers with EC_TAG_IP2COUNTRY_SUPPORTED (see
	// CEC_Prefs_Packet::Apply); a pre-3.1 core (e.g. 3.0.1) omits the whole
	// category, so leaving this default false correctly hides the IP2Country
	// page rather than offering settings the core can't honour.
	thePrefs::SetGeoIPSupported(false);
#endif
	//
	// override local settings with remote
	CECPacket req(EC_OP_GET_PREFERENCES, EC_DETAIL_UPDATE);

	// bring categories too
	req.AddTag(CECTag(EC_TAG_SELECT_PREFS, m_exchange_recv_selected_prefs));

	m_conn->SendRequest(this, &req);

	return true;
}

void CPreferencesRem::SendToRemote()
{
	CEC_Prefs_Packet pref_packet(m_exchange_send_selected_prefs, EC_DETAIL_UPDATE, EC_DETAIL_FULL);
	m_conn->SendPacket(&pref_packet);
}

// Reply handler for the shared-directory ops. Deliberately separate from
// CPreferencesRem::HandlePacket, which static_casts every packet it is given to
// CEC_Prefs_Packet — routing a non-prefs reply through it would be undefined
// behaviour. Results are stored on glob_prefs rather than on the Preferences
// dialog, so a reply that arrives after the user closed the dialog is harmless.
class CSharedDirsHandler : public CECPacketHandlerBase
{
	void HandlePacket(const CECPacket *packet) override;
};

void CSharedDirsHandler::HandlePacket(const CECPacket *packet)
{
	if (packet->GetOpCode() == EC_OP_GET_SHARED_DIRS) {
		CPreferences::PathList explicitDirs;
		CPreferences::PathList recursiveDirs;
		for (const CECTag &tag : *packet) {
			if (tag.GetTagName() != EC_TAG_SHAREDDIR) {
				continue;
			}
			const CPath path(tag.GetStringData());
			const CECTag *recursiveTag = tag.GetTagByName(EC_TAG_SHAREDDIR_RECURSIVE);
			if (recursiveTag != nullptr && recursiveTag->GetInt() != 0) {
				recursiveDirs.push_back(path);
			} else {
				explicitDirs.push_back(path);
			}
		}
		theApp->glob_prefs->shareddir_explicit_list = explicitDirs;
		theApp->glob_prefs->shareddir_recursive_list = recursiveDirs;
		// Refresh the editor if Preferences is open; a no-op otherwise.
		wxTheApp->CallAfter([]() { PrefsUnifiedDlg::RefreshSharedDirsIfOpen(); });
	} else if (packet->GetOpCode() == EC_OP_SET_SHARED_DIRS) {
		// The daemon applied every path that validated and listed the rest.
		// Report them so a typo'd path can't become a silently dead share —
		// the remote user has no way to browse and check it themselves.
		wxString rejected;
		for (const CECTag &tag : *packet) {
			if (tag.GetTagName() != EC_TAG_SHAREDDIR_REJECTED) {
				continue;
			}
			const CECTag *errTag = tag.GetTagByName(EC_TAG_SHAREDDIR_ERROR);
			// Reason codes are translated here, in the user's locale, rather
			// than shipped as text from a daemon with a different one.
			const wxString reason = (errTag != nullptr && errTag->GetInt() == 2)
							? _("not readable")
							: _("not found");
			rejected += CFormat("\n%s (%s)") % tag.GetStringData() % reason;
		}
		if (!rejected.IsEmpty()) {
			const wxString msg =
				CFormat(_("The core rejected these shared folders:%s")) % rejected;
			// Deferred: a modal opened straight from a packet handler spins a
			// nested event loop that re-enters the EC socket and corrupts its
			// receive state (see CAddLinkHandler).
			wxTheApp->CallAfter([msg]() { wxMessageBox(msg, _("ERROR"), wxOK | wxICON_ERROR); });
		}
	}
	delete this;
}

void CPreferencesRem::LoadSharedDirsRemote()
{
	if (!m_conn->ServerSupportsSharedDirsConfig()) {
		return;
	}
	CECPacket req(EC_OP_GET_SHARED_DIRS);
	m_conn->SendRequest(new CSharedDirsHandler(), &req);
}

void CPreferencesRem::SendSharedDirsToRemote()
{
	if (!m_conn->ServerSupportsSharedDirsConfig()) {
		return;
	}
	CECPacket req(EC_OP_SET_SHARED_DIRS);
	for (const CPath &dir : shareddir_explicit_list) {
		req.AddTag(CECTag(EC_TAG_SHAREDDIR, dir.GetRaw()));
	}
	for (const CPath &dir : shareddir_recursive_list) {
		CECTag dirTag(EC_TAG_SHAREDDIR, dir.GetRaw());
		dirTag.AddTag(CECTag(EC_TAG_SHAREDDIR_RECURSIVE, (uint8)1));
		req.AddTag(dirTag);
	}
	m_conn->SendRequest(new CSharedDirsHandler(), &req);
}

// Surfaces the EC_OP_FAILED reply from amuled's EC_OP_ADD_LINK handler
// to the user. CDownQueueRem::AddLink used to drop the reply on the
// floor (fire-and-forget SendPacket), so a malformed ed2k link -- e.g.
// the original #310 reproducer `ed2k::3D366ED505B977FC61C9A6EE01E96329`
// -- silently did nothing. amuled does the right thing now (logs
// "Unknown protocol of link" and returns EC_OP_FAILED + EC_TAG_STRING),
// but the GUI side has to actually show the message.
class CAddLinkHandler : public CECPacketHandlerBase
{
	virtual void HandlePacket(const CECPacket *packet);
};

void CAddLinkHandler::HandlePacket(const CECPacket *packet)
{
	if (packet->GetOpCode() == EC_OP_FAILED) {
		// Daemon-side EC_OP_ADD_LINK always tags the failure response
		// with an EC_TAG_STRING explaining what went wrong. Reuse that
		// string as the fallback too (it's already in the i18n catalog
		// at po/amule.pot:1783, so no new string needs adding here).
		const CECTag *tag = packet->GetFirstTagSafe();
		wxString msg = (tag && tag->IsString())
				       ? wxGetTranslation(tag->GetStringData())
				       : wxGetTranslation(wxTRANSLATE("Invalid link or already on list."));
		// Defer the modal off the OnPacketReceived call stack: wxMessageBox
		// spins a nested wx event loop, which dispatches CoreNotify_LibSocket*
		// events that re-enter CECSocket::OnInput on the same socket and
		// clobber its rx state (m_curr_rx_data / m_bytes_needed / m_in_header).
		// Under heavy notification load — a batched-link add against a
		// big shareset — the corrupted parse trips a protocol-error
		// CloseSocket, which is the desync amuled logs as "External
		// connection closed" right after the AddLink batch (#757 part 1).
		wxTheApp->CallAfter([msg]() { wxMessageBox(msg, _("ERROR"), wxOK | wxICON_ERROR); });
	}
	delete this;
}

class CCatHandler : public CECPacketHandlerBase
{
	virtual void HandlePacket(const CECPacket *packet);
};

// EC_OP_DELETE_CATEGORY reply. Holds the index because a successful delete
// answers a bare EC_OP_NOOP; only the failures name the category.
class CCatDeleteHandler : public CECPacketHandlerBase
{
public:
	explicit CCatDeleteHandler(uint8 cat)
	: m_cat(cat)
	{
	}
	virtual void HandlePacket(const CECPacket *packet);
	// Socket died mid-request: free ourselves (DiscardRequestQueue clears the
	// queue without deleting) and commit nothing -- whether the daemon did the
	// delete before the drop is exactly what is unknown.
	virtual void AbortPendingRequest() { delete this; }

private:
	uint8 m_cat;
};

void CCatDeleteHandler::HandlePacket(const CECPacket *packet)
{
	if (packet->GetOpCode() == EC_OP_FAILED) {
		// Nothing was removed locally, so there is nothing to undo. Report the
		// reason: a rejected index means our list is behind the core's.
		const CECTag *msgTag = packet->GetTagByName(EC_TAG_STRING);
		const wxString reason = msgTag ? wxGetTranslation(msgTag->GetStringData())
					       : wxString(_("The daemon refused the request."));
		const wxString msg = CFormat(_("Could not delete the category: %s")) % reason;
		// Modal off the OnPacketReceived stack, as the handlers above do.
		wxTheApp->CallAfter([msg]() { wxMessageBox(msg, _("ERROR"), wxOK); });
	} else if (theApp->amuledlg && theApp->amuledlg->m_transferwnd) {
		// Confirmed gone core-side: same commit the monolithic build runs inline.
		theApp->amuledlg->m_transferwnd->CommitRemoveCategory(m_cat);
	}
	delete this;
}

void CCatHandler::HandlePacket(const CECPacket *packet)
{
	if (packet->GetOpCode() == EC_OP_FAILED) {
		const CECTag *catTag = packet->GetTagByName(EC_TAG_CATEGORY);
		const CECTag *pathTag = packet->GetTagByName(EC_TAG_CATEGORY_PATH);
		if (catTag && pathTag && catTag->GetInt() < theApp->glob_prefs->GetCatCount()) {
			int cat = catTag->GetInt();
			Category_Struct *cs = theApp->glob_prefs->GetCategory(cat);
			wxString msg = CFormat(_("Can't create directory '%s' for category '%s', keeping "
						 "directory '%s'.")) %
				       cs->path.GetPrintable() % cs->title % pathTag->GetStringData();
			cs->path = CPath(pathTag->GetStringData());
			if (theApp->amuledlg) {
				theApp->amuledlg->m_transferwnd->UpdateCategory(cat);
				theApp->amuledlg->m_transferwnd->downloadlistctrl->Refresh();
			}
			// Same re-entrancy hazard as CAddLinkHandler above: keep the
			// modal off the OnPacketReceived stack so a nested wx event
			// loop doesn't corrupt CECSocket rx state.
			wxTheApp->CallAfter([msg]() { wxMessageBox(msg, _("ERROR"), wxOK); });
		}
	}
	delete this;
}

bool CPreferencesRem::CreateCategory(Category_Struct *&category,
	const wxString &name,
	const CPath &path,
	const wxString &comment,
	uint32 color,
	uint8 prio)
{
	CECPacket req(EC_OP_CREATE_CATEGORY);
	CEC_Category_Tag tag(0xffffffff, name, path.GetRaw(), comment, color, prio);
	req.AddTag(tag);
	m_conn->SendRequest(new CCatHandler, &req);

	category = new Category_Struct();
	category->path = path;
	category->title = name;
	category->comment = comment;
	category->color = color;
	category->prio = prio;

	AddCat(category);

	return true;
}

bool CPreferencesRem::UpdateCategory(
	uint8 cat, const wxString &name, const CPath &path, const wxString &comment, uint32 color, uint8 prio)
{
	CECPacket req(EC_OP_UPDATE_CATEGORY);
	CEC_Category_Tag tag(cat, name, path.GetRaw(), comment, color, prio);
	req.AddTag(tag);
	m_conn->SendRequest(new CCatHandler, &req);

	Category_Struct *category = m_CatList[cat];
	category->path = path;
	category->title = name;
	category->comment = comment;
	category->color = color;
	category->prio = prio;

	return true;
}

bool CPreferencesRem::RequestRemoveCat(size_t cat)
{
	const uint8 cat8 = static_cast<uint8>(cat);
	CECPacket req(EC_OP_DELETE_CATEGORY);
	CEC_Category_Tag tag(cat8, EC_DETAIL_CMD);
	req.AddTag(tag);
	// SendRequest, not SendPacket: the reply decides whether this happened.
	// Fire-and-forget updated the local list regardless, so a discarded delete
	// shifted this client's indices against the core's -- and ids are positional.
	m_conn->SendRequest(new CCatDeleteHandler(cat8), &req);
	return false;
}

//
// Container implementation
//
CServerConnectRem::CServerConnectRem(CRemoteConnect *conn)
{
	m_CurrServer = 0;
	m_Conn = conn;
}

void CServerConnectRem::ConnectToAnyServer()
{
	CECPacket req(EC_OP_SERVER_CONNECT);
	m_Conn->SendPacket(&req);
}

void CServerConnectRem::StopConnectionTry()
{
	// lfroen: isn't Disconnect the same ?
}

void CServerConnectRem::Disconnect()
{
	CECPacket req(EC_OP_SERVER_DISCONNECT);
	m_Conn->SendPacket(&req);
}

void CServerConnectRem::ConnectToServer(CServer *server)
{
	m_Conn->ConnectED2K(server->GetIP(), server->GetPort());
}

void CServerConnectRem::HandlePacket(const CECPacket *packet)
{
	const CEC_ConnState_Tag *tag =
		static_cast<const CEC_ConnState_Tag *>(packet->GetTagByName(EC_TAG_CONNSTATE));
	if (!tag) {
		return;
	}

	theApp->m_ConnState = 0;
	CServer *server;
	m_ID = tag->GetEd2kId();
	theApp->m_clientID = tag->GetClientId();
	tag->GetKadID(theApp->m_kadID);

	if (tag->IsConnectedED2K()) {
		const CECTag *srvtag = tag->GetTagByName(EC_TAG_SERVER);
		if (srvtag) {
			server = theApp->serverlist->GetByID(srvtag->GetInt());
			if (server != m_CurrServer) {
				if (theApp->amuledlg) {
					theApp->amuledlg->m_serverwnd->serverlistctrl->HighlightServer(
						server, true);
				}
				m_CurrServer = server;
			}
		}
		theApp->m_ConnState |= CONNECTED_ED2K;
		uint32 ed2kSince = 0;
		theApp->m_ed2kConnectedSince =
			tag->GetED2KConnectedSince(ed2kSince) ? wxDateTime((time_t)ed2kSince) : wxDateTime();
	} else {
		theApp->m_ed2kConnectedSince = wxDateTime();
		if (m_CurrServer) {
			if (theApp->amuledlg) {
				theApp->amuledlg->m_serverwnd->serverlistctrl->HighlightServer(
					m_CurrServer, false);
			}
			m_CurrServer = nullptr;
		}
	}

	if (tag->IsConnectedKademlia()) {
		if (tag->IsKadFirewalled()) {
			theApp->m_ConnState |= CONNECTED_KAD_FIREWALLED;
		} else {
			theApp->m_ConnState |= CONNECTED_KAD_OK;
		}
		uint32 kadSince = 0;
		theApp->m_kadConnectedSince =
			tag->GetKadConnectedSince(kadSince) ? wxDateTime((time_t)kadSince) : wxDateTime();
	} else {
		if (tag->IsKadRunning()) {
			theApp->m_ConnState |= CONNECTED_KAD_NOT;
		}
		theApp->m_kadConnectedSince = wxDateTime();
	}

	if (theApp->amuledlg) {
		theApp->amuledlg->ShowConnectionState();
	}
}

/*
 * Server list: host list of ed2k servers.
 */
CServerListRem::CServerListRem(CRemoteConnect *conn)
: CRemoteContainer<CServer, uint32, CEC_Server_Tag>(conn, true)
{
}

void CServerListRem::ProcessUpdate(const CECTag *reply, CECPacket *full_req, int req_type)
{
	CRemoteContainer<CServer, uint32, CEC_Server_Tag>::ProcessUpdate(reply, full_req, req_type);

	// Size the columns once the first list has actually arrived -- see
	// m_columnsFitted. Empty is not "arrived": fitting against no rows would
	// measure only the headers and then never run again.
	if (!m_columnsFitted && !m_items.empty() && theApp->amuledlg != nullptr &&
		theApp->amuledlg->m_serverwnd != nullptr &&
		theApp->amuledlg->m_serverwnd->serverlistctrl != nullptr) {
		m_columnsFitted = true;
		theApp->amuledlg->m_serverwnd->serverlistctrl->FitColumnsToContent();
	}
}

void CServerListRem::HandlePacket(const CECPacket *)
{
	// There is no packet for the server list, it is part of the general update packet
	wxFAIL;
	// CRemoteContainer<CServer, uint32, CEC_Server_Tag>::HandlePacket(packet);
}

void CServerListRem::UpdateServerMetFromURL(wxString url)
{
	CECPacket req(EC_OP_SERVER_UPDATE_FROM_URL);
	req.AddTag(CECTag(EC_TAG_SERVERS_UPDATE_URL, url));

	m_conn->SendPacket(&req);
}

void CServerListRem::SetStaticServer(CServer *server, bool isStatic)
{
	// update display right away
	server->SetIsStaticMember(isStatic);
	Notify_ServerRefresh(server);

	CECPacket req(EC_OP_SERVER_SET_STATIC_PRIO);
	req.AddTag(CECTag(EC_TAG_SERVER, server->ECID()));
	req.AddTag(CECTag(EC_TAG_SERVER_STATIC, isStatic));

	m_conn->SendPacket(&req);
}

void CServerListRem::SetServerPrio(CServer *server, uint32 prio)
{
	// update display right away
	server->SetPreference(prio);
	Notify_ServerRefresh(server);

	CECPacket req(EC_OP_SERVER_SET_STATIC_PRIO);
	req.AddTag(CECTag(EC_TAG_SERVER, server->ECID()));
	req.AddTag(CECTag(EC_TAG_SERVER_PRIO, prio));

	m_conn->SendPacket(&req);
}

void CServerListRem::RemoveServer(CServer *server)
{
	m_conn->RemoveServer(server->GetIP(), server->GetPort());
}

void CServerListRem::UpdateUserFileStatus(CServer *server)
{
	if (server) {
		m_TotalUser = server->GetUsers();
		m_TotalFile = server->GetFiles();
	}
}

CServer *CServerListRem::GetServerByAddress(const wxString &WXUNUSED(address), uint16 WXUNUSED(port)) const
{
	// It's ok to return 0 for context where this code is used in remote gui
	return 0;
}

CServer *CServerListRem::GetServerByIPTCP(uint32 WXUNUSED(nIP), uint16 WXUNUSED(nPort)) const
{
	// It's ok to return 0 for context where this code is used in remote gui
	return 0;
}

CServer *CServerListRem::CreateItem(const CEC_Server_Tag *tag)
{
	CServer *server = new CServer(tag);
	ProcessItemUpdate(tag, server);
	return server;
}

void CServerListRem::DeleteItem(CServer *in_srv)
{
	CScopedPtr<CServer> srv(in_srv);
	if (theApp->amuledlg) {
		theApp->amuledlg->m_serverwnd->serverlistctrl->RemoveServer(srv.get());
	}
}

uint32 CServerListRem::GetItemID(CServer *server)
{
	return server->ECID();
}

void CServerListRem::ProcessItemUpdate(const CEC_Server_Tag *tag, CServer *server)
{
	if (!tag->HasChildTags()) {
		return;
	}
	tag->ServerName(&server->listname);
	tag->ServerDesc(&server->description);
	tag->ServerVersion(&server->m_strVersion);
#ifdef GEOIP_GUI
	// Server host country from the daemon's GeoIP (#440); check tag presence
	// so an authoritative empty code isn't mistaken for an absent tag.
	if (tag->GetTagByName(EC_TAG_SERVER_COUNTRY)) {
		server->SetCountryCode(tag->Country());
	}
#endif
	tag->GetMaxUsers(&server->maxusers);
	tag->GetSoftFiles(&server->softfiles);
	tag->GetHardFiles(&server->hardfiles);
	// Read through the pointer, as the fields above do. The valuemap builder
	// omits a tag whose value has not changed since the last update, and the
	// return form yields 0 for an absent tag -- so assigning it would wipe the
	// value on every update that did not resend it. AssignIfExist() leaves the
	// current value alone instead.
	uint32 tcpFlags = server->GetTCPFlags();
	uint32 udpFlags = server->GetUDPFlags();
	tag->GetTCPFlags(&tcpFlags);
	tag->GetUDPFlags(&udpFlags);
	server->SetTCPFlags(tcpFlags);
	server->SetUDPFlags(udpFlags);

	tag->GetFiles(&server->files);
	tag->GetUsers(&server->users);

	tag->GetPrio(&server->preferences); // SRV_PR_NORMAL = 0, so it's ok
	tag->GetStatic(&server->staticservermember);

	tag->GetPing(&server->ping);
	tag->GetFailed(&server->failedcount);

	if (theApp->amuledlg) {
		theApp->amuledlg->m_serverwnd->serverlistctrl->RefreshServer(server);
	}
}

CServer::CServer(const CEC_Server_Tag *tag)
: CECID(tag->GetInt())
{
	ip = tag->GetTagByNameSafe(EC_TAG_SERVER_IP)->GetInt();
	port = tag->GetTagByNameSafe(EC_TAG_SERVER_PORT)->GetInt();

	Init();
}

/*
 * IP filter
 */
CIPFilterRem::CIPFilterRem(CRemoteConnect *conn)
{
	m_conn = conn;
}

void CIPFilterRem::Reload()
{
	CECPacket req(EC_OP_IPFILTER_RELOAD);
	m_conn->SendPacket(&req);
}

void CIPFilterRem::Update(wxString url)
{
	CECPacket req(EC_OP_IPFILTER_UPDATE);
	req.AddTag(CECTag(EC_TAG_STRING, url));

	m_conn->SendPacket(&req);
}

/*
 * Shared files list
 */
CSharedFilesRem::CSharedFilesRem(CRemoteConnect *conn)
{
	m_conn = conn;
}

void CSharedFilesRem::Reload(bool, bool)
{
	CECPacket req(EC_OP_SHAREDFILES_RELOAD);

	m_conn->SendPacket(&req);
}

bool CSharedFilesRem::RenameFile(CKnownFile *file, const CPath &newName)
{
	// We use the printable name, as the filename originated from user input,
	// and the filesystem name might not be valid on the remote host.
	const wxString strNewName = newName.GetPrintable();

	CECPacket request(EC_OP_RENAME_FILE);
	request.AddTag(CECTag(EC_TAG_KNOWNFILE, file->GetFileHash()));
	request.AddTag(CECTag(EC_TAG_PARTFILE_NAME, strNewName));

	m_conn->SendPacket(&request);

	return true;
}

void CSharedFilesRem::VerifyLocalData(const CKnownFile *file) const
{
	CECPacket request(EC_OP_VERIFY_LOCAL_DATA);
	request.AddTag(CECTag(EC_TAG_KNOWNFILE, file->GetFileHash()));
	m_conn->SendPacket(&request);
}

unsigned CSharedFilesRem::RefreshMediaMetadata(const std::vector<CMD4Hash> &hashes)
{
	if (hashes.empty()) {
		return 0;
	}
	// ONE packet carrying every hash. One request per file would push the
	// selection through m_req_fifo a packet at a time, and OnPollTimer stops
	// updating the GUI while that fifo is full -- on a large selection the
	// window would go quiet for as long as it took to drain.
	CECPacket request(EC_OP_REFRESH_MEDIA_METADATA);
	for (const CMD4Hash &hash : hashes) {
		// A FRESH tag per hash, deliberately. CECTag::AddTag swaps the
		// argument's contents into the child list and leaves the original
		// empty, so adding the same tag object twice appends one real child
		// and one blank -- silently sending fewer hashes than intended.
		request.AddTag(CECTag(EC_TAG_KNOWNFILE, hash));
	}
	m_conn->SendPacket(&request);
	// Sent, not probed: the daemon decides eligibility and reports what it did
	// in its own log, which this GUI displays.
	return hashes.size();
}

bool CSharedFilesRem::RefreshMediaMetadata(const CMD4Hash &hash)
{
	CECPacket request(EC_OP_REFRESH_MEDIA_METADATA);
	request.AddTag(CECTag(EC_TAG_KNOWNFILE, hash));
	m_conn->SendPacket(&request);
	// Sent, not probed. A daemon that predates the opcode answers EC_OP_FAILED
	// and the reply is dropped here as it is for every other fire-and-forget
	// request on this class; the user sees nothing happen, which is the same
	// outcome as the action not existing.
	return true;
}

void CSharedFilesRem::SetFileCommentRating(CKnownFile *file, const wxString &newComment, int8 newRating)
{
	CECPacket request(EC_OP_SHARED_FILE_SET_COMMENT);
	request.AddTag(CECTag(EC_TAG_KNOWNFILE, file->GetFileHash()));
	request.AddTag(CECTag(EC_TAG_KNOWNFILE_COMMENT, newComment));
	request.AddTag(CECTag(EC_TAG_KNOWNFILE_RATING, newRating));

	m_conn->SendPacket(&request);
}

void CSharedFilesRem::SearchKadNotes(CAbstractFile *file)
{
	// The daemon owns Kad; ask it to run the on-demand NOTES lookup for this
	// file (a download, a shared file, or a search result — the request is keyed
	// purely by hash). Retrieved notes flow back through the comments channel.
	CECPacket request(EC_OP_SHARED_FILE_SEARCH_KAD_NOTES);
	request.AddTag(CECTag(EC_TAG_KNOWNFILE, file->GetFileHash()));

	m_conn->SendPacket(&request);
}

void CSharedFilesRem::CopyFileList(std::vector<CKnownFile *> &out_list) const
{
	out_list.reserve(size());
	for (const_iterator it = begin(); it != end(); ++it) {
		out_list.push_back(it->second);
	}
}

void CKnownFilesRem::DeleteItem(CKnownFile *file)
{
	uint32 id = file->ECID();
	// Broadcast to every subscriber that holds a raw CKnownFile* to
	// this object — CUpDownClient::m_uploadingfile / m_reqfile (the
	// #748 crash flow), CGenericClientListCtrl::m_knownfiles (#755),
	// and the open-dialog registry. Subscribers strip their refs
	// using pointer-value comparison only. See
	// MuleNotify::KnownFileBeingDestroyed (GuiEvents.cpp).
	Notify_KnownFileBeingDestroyed(file);

	if (theApp->sharedfiles->count(id)) {
		if (theApp->amuledlg) {
			theApp->amuledlg->m_sharedfileswnd->sharedfilesctrl->RemoveFile(file);
		}
		theApp->sharedfiles->erase(id);
	}
	if (theApp->downloadqueue->count(id)) {
		if (theApp->amuledlg) {
			theApp->amuledlg->m_transferwnd->downloadlistctrl->RemoveFile(
				static_cast<CPartFile *>(file));
		}
		theApp->downloadqueue->erase(id);
	}
	delete file;
}

void CUpDownClientListRem::DropReferencesTo(const CKnownFile *file)
{
	// Null out CUpDownClient::m_uploadingfile / m_reqfile on every
	// client still pointing at `file`. Called by
	// MuleNotify::KnownFileBeingDestroyed (the broadcast handler in
	// GuiEvents.cpp) which is itself fired from CKnownFilesRem::
	// DeleteItem above before the file is freed. Pointer-value
	// comparison only — `file` may already be freed by the time
	// this runs on the main thread.
	for (iterator it = begin(); it != end(); ++it) {
		CUpDownClient *client = (*it)->GetClient();
		if (!client) {
			continue;
		}
		if (client->m_uploadingfile == file) {
			client->m_uploadingfile = NULL;
		}
		if (client->m_reqfile == file) {
			client->m_reqfile = NULL;
		}
	}
}

uint32 CKnownFilesRem::GetItemID(CKnownFile *file)
{
	return file->ECID();
}

namespace
{

// Copy the six FT_MEDIA_* fields off an EC tag onto the local proxy object, so
// the identical GetIntTagValue / GetStrTagValue / GetMetaDataVer calls in the
// File Details dialog work unchanged in the remote build.
//
// A zero / empty value is the daemon saying the field is GONE, not a value
// worth storing -- it only sends one for a field it previously sent a real
// value for (see AddMediaTagsPresent). Storing it would leave the dialog
// showing 0:00 or a blank Artist where N/A is the honest answer.
//
// Shared by the known-file walker and the search-result one because the daemon
// emits these from ONE place for both: CEC_SharedFile_Tag's base ctor, which
// CEC_SearchFile_Tag derives from. Two copies of this loop would drift the
// moment a seventh field appeared, and the search half is only being written
// now because the first copy was missing there entirely.
void DecodeMediaTags(const CECTag *tag, CAbstractFile *file)
{
	static const struct
	{
		ec_tagname_t ecId;
		uint8 ftId;
		bool isInt;
	} kMedia[] = { { EC_TAG_KNOWNFILE_MEDIA_LENGTH, FT_MEDIA_LENGTH, true },
		{ EC_TAG_KNOWNFILE_MEDIA_BITRATE, FT_MEDIA_BITRATE, true },
		{ EC_TAG_KNOWNFILE_MEDIA_CODEC, FT_MEDIA_CODEC, false },
		{ EC_TAG_KNOWNFILE_MEDIA_ARTIST, FT_MEDIA_ARTIST, false },
		{ EC_TAG_KNOWNFILE_MEDIA_ALBUM, FT_MEDIA_ALBUM, false },
		{ EC_TAG_KNOWNFILE_MEDIA_TITLE, FT_MEDIA_TITLE, false } };
	for (const auto &entry : kMedia) {
		const CECTag *m = tag->GetTagByName(entry.ecId);
		if (!m) {
			// Absent means UNCHANGED, per the CValueMap contract -- not
			// cleared. Leave whatever is already stored.
			continue;
		}
		if (entry.isInt) {
			const uint32 v = m->GetInt();
			if (v) {
				file->AddTagUnique(CTagInt32(entry.ftId, v));
			} else {
				file->RemoveTag(entry.ftId);
			}
		} else {
			const wxString v = m->GetStringData();
			if (!v.IsEmpty()) {
				file->AddTagUnique(CTagString(entry.ftId, v));
			} else {
				file->RemoveTag(entry.ftId);
			}
		}
	}
}

} // namespace

void CKnownFilesRem::ProcessItemUpdate(const CEC_SharedFile_Tag *tag, CKnownFile *file)
{
	const CECTag *parttag = tag->GetTagByName(EC_TAG_PARTFILE_PART_STATUS);
	if (parttag) {
		const uint8 *data =
			file->m_partStatus.Decode((uint8 *)parttag->GetTagData(), parttag->GetTagDataLen());
		// The two bounds come from different places and are not guaranteed to
		// agree: the buffer is as long as the decoder made it from what the
		// wire carried, while GetPartCount() is this object's own, derived
		// from the size it currently believes the file to be. They match for
		// as long as an ECID keeps meaning the same file -- which a daemon
		// restart ends, because CECID hands IDs out from a counter that starts
		// again with the process while amulegui deliberately keeps its objects
		// across a reconnect. An ID that comes back naming a different file
		// then pairs a short buffer with a long part count, and this loop
		// reads off the end of the heap allocation.
		//
		// Copy what actually arrived and clear the rest, rather than trusting
		// either bound alone: leaving the tail would show the previous file's
		// availability under the new one's name.
		const int arrived = file->m_partStatus.Size();
		const int parts = file->GetPartCount();
		const int copied = std::min(arrived, parts);
		if (arrived != parts && !m_loggedPartStatusMismatch) {
			// Says the tag and the object disagree about which file this
			// is. The reconnect path is supposed to make that impossible
			// by discarding everything keyed by ECID when the daemon
			// changes underneath us, so this firing means that went
			// wrong. Critical rather than debug: clamping the copy leaves
			// no other trace, the rest of this update goes on applying the
			// same mismatched tag to the same object, and what the user
			// ends up looking at is a row describing the wrong file --
			// which they can act on, and would otherwise have no reason to
			// suspect.
			//
			// Once per session, not once per file: if the ECID space has
			// gone bad it has gone bad for everything, and this sits in a
			// loop that runs over the whole library on every poll.
			m_loggedPartStatusMismatch = true;
			AddLogLineC(CFormat(_("The remote core sent inconsistent data for \"%s\" "
					      "(part status %d, expected %d). The file list may be "
					      "showing the wrong information; reconnect to clear it.")) %
				    file->GetFileName().GetPrintable() % arrived % parts);
		}
		for (int i = 0; i < copied; ++i) {
			file->m_AvailPartFrequency[i] = data[i];
		}
		for (int i = copied; i < parts; ++i) {
			file->m_AvailPartFrequency[i] = 0;
		}
	}
	wxString fileName;
	if (tag->FileName(fileName)) {
		file->SetFileName(CPath(fileName));
	}
	// The status-agnostic directory (EC_TAG_KNOWNFILE_PATH): the Temp dir for a
	// partfile, the destination once completed. FilePath()/_FILENAME carries the
	// .part basename for partfiles, so decode the dedicated PATH tag here to keep
	// GetFilePath() meaning "the folder" in amulegui, as it does in the daemon.
	if (tag->DirectoryPath(fileName)) {
		file->m_filePath = CPath(fileName);
	}
	tag->UpPrio(&file->m_iUpPriorityEC);
	tag->GetAICHHash(file->m_AICHMasterHash);
	// Bad thing - direct writing another class' members
	tag->GetRequests(&file->statistic.requested);
	tag->GetAllRequests(&file->statistic.alltimerequested);
	tag->GetAccepts(&file->statistic.accepted);
	tag->GetAllAccepts(&file->statistic.alltimeaccepted);
	tag->GetXferred(&file->statistic.transferred);
	tag->GetAllXferred(&file->statistic.alltimetransferred);
	tag->UpPrio(&file->m_iUpPriorityEC);
	if (file->m_iUpPriorityEC >= 10) {
		file->m_iUpPriority = file->m_iUpPriorityEC - 10;
		file->m_bAutoUpPriority = true;
	} else {
		file->m_iUpPriority = file->m_iUpPriorityEC;
		file->m_bAutoUpPriority = false;
	}
	tag->GetCompleteSourcesLow(&file->m_nCompleteSourcesCountLo);
	tag->GetCompleteSourcesHigh(&file->m_nCompleteSourcesCountHi);
	tag->GetCompleteSources(&file->m_nCompleteSourcesCount);
	uint16 hashingProgress = 0;
	if (tag->HashingProgress(hashingProgress)) {
		file->SetHashingProgress(hashingProgress);
	}

	tag->GetOnQueue(&file->m_queuedCount);

	// Live upload activity (issue #466): the daemon summarises these from its
	// m_ClientUploadList and ships them every update tick. Decoded into the EC
	// mirror members so GetUploadDatarate()/GetTransferringClientCount() work
	// in amulegui (e.g. the file-details Sharing box).
	tag->GetUploadSpeed(&file->m_uploadDatarateEC);
	tag->GetUploadingCount(&file->m_transferringClientCountEC);
	// Share timestamps ride only in the full-detail section, not every tick, so
	// guard on a real (non-zero) value to avoid a bare update tick zeroing them.
	time_t sharedSince = 0;
	tag->GetSharedSince(&sharedSince);
	if (sharedSince) {
		file->SetDateShared(sharedSince);
	}
	time_t lastUpload = 0;
	tag->GetLastUpload(&lastUpload);
	if (lastUpload) {
		file->SetLastUpload(lastUpload);
	}

	tag->GetComment(file->m_strComment);
	tag->GetRating(file->m_iRating);

	// Community ratings/comments + the on-demand Kad-notes running flag. The
	// daemon serializes these on the CEC_SharedFile_Tag base, so this one decode
	// serves both shared files and downloads (it runs for every known file
	// before the partfile-specific pass). The container is present only when it
	// changed (valuemap-suppressed), so an absent tag keeps the prior list.
	const CECTag *commenttag = tag->GetTagByName(EC_TAG_PARTFILE_COMMENTS);
	if (commenttag) {
		file->ClearFileRatingList();
		for (CECTag::const_iterator it = commenttag->begin(); it != commenttag->end();) {
			wxString u = (it++)->GetStringData();
			wxString f = (it++)->GetStringData();
			sint16 r = static_cast<sint16>(static_cast<sint64>((it++)->GetInt()));
			wxString c = (it++)->GetStringData();
			file->AddFileRatingList(u, f, r, c);
		}
		file->UpdateFileRatingCommentAvail();
	}
	if (const CECTag *kadSearchTag = tag->GetTagByName(EC_TAG_PARTFILE_KAD_COMMENT_SEARCHING)) {
		file->SetKadCommentSearchRunning(kadSearchTag->GetInt() != 0);
	}

	requested += file->statistic.requested;
	transferred += file->statistic.transferred;
	accepted += file->statistic.transferred;

	if (!m_initialUpdate && theApp->amuledlg) {
		theApp->amuledlg->m_sharedfileswnd->sharedfilesctrl->UpdateItem(file);
	}

	// Media metadata rides the SHARED-FILE base tag, so it is decoded here for
	// every known file rather than inside the partfile branch below. It used
	// to live there, which meant a COMPLETED shared file -- the only kind the
	// completion re-probe produces -- had all six tags dropped on the floor,
	// and the File Details dialog showed N/A for anything not currently
	// downloading. The same mistake the comments/ratings decode already
	// avoids for the same reason.
	DecodeMediaTags(tag, file);

	if (file->IsPartFile()) {
		ProcessItemUpdatePartfile(
			static_cast<const CEC_PartFile_Tag *>(tag), static_cast<CPartFile *>(file));
	}
}

void CSharedFilesRem::SetFilePrio(CKnownFile *file, uint8 prio)
{
	CECPacket req(EC_OP_SHARED_SET_PRIO);

	CECTag hashtag(EC_TAG_PARTFILE, file->GetFileHash());
	hashtag.AddTag(CECTag(EC_TAG_PARTFILE_PRIO, prio));

	req.AddTag(hashtag);

	m_conn->SendPacket(&req);
}

void CKnownFilesRem::ArmReconnectReconcile()
{
	m_reconnectReconcile = true;

	// A reconnect opens a fresh EC session. The daemon keeps its RLE gap/part/
	// req-status encoders per connection (CFileEncoderMap in ExternalConn.cpp),
	// so the new session's encoders restart from an empty baseline. Our reused
	// CKnownFile / CPartFile objects still carry the previous session's
	// *decoder* state; left alone, the first differential status update would
	// XOR the daemon's from-empty diff against that stale buffer and paint
	// garbage — all-red download progress bars and wrong shared-file
	// availability shading (issue #444). Reset every decoder now so both ends
	// share the same empty baseline before the first post-reconnect update.
	for (CKnownFile *file : *this) {
		file->m_partStatus.ResetEncoder();
		if (file->IsPartFile()) {
			static_cast<CPartFile *>(file)->m_PartFileEncoderData.ResetDecoder();
		}
	}
}

void CKnownFilesRem::ResetForNewDaemonSession()
{
	CRemoteContainer<CKnownFile, uint32, CEC_SharedFile_Tag>::ResetForNewSession();

	// Back to the state a cold boot starts in: the next reply is a full
	// library snapshot and gets the batched ShowFileList() treatment, not
	// the per-item show-and-sort that a steady-state poll uses.
	m_initialUpdate = true;
	// Nothing left to reconcile against, so the one-shot absence-prune has
	// no work and must not fire on a list it would find empty anyway.
	m_reconnectReconcile = false;
	// New session, new chance to complain if the ECIDs go bad again.
	m_loggedPartStatusMismatch = false;
}

void CKnownFilesRem::ProcessUpdate(const CECTag *reply, CECPacket *, int)
{
	requested = 0;
	transferred = 0;
	accepted = 0;

	// The first poll after a reconnect re-processes the whole (potentially
	// 10k+) library in one go (update-in-place + add + prune) — that is the
	// reconcile case (issue #444), which drives the one-shot absence-prune
	// below. The cold-boot m_initialUpdate path has its own batching
	// (ShowFileList) and never overlaps — m_initialUpdate is false whenever
	// reconcile is true.
	const bool reconcile = m_reconnectReconcile;

	// Batch the download list on every steady-state poll, not just a reconnect
	// resync: an ordinary poll can surface a whole burst of freshly-added
	// downloads at once (e.g. a hundred search results dropped into a large
	// queue), and CDownloadListCtrl::AddFile() re-sorts the entire list on
	// every insert. On a 10k queue that is O(n^2 log n) and freezes the GUI for
	// seconds (issue #615). BeginBatchUpdate() suppresses the per-item sort;
	// the single SortList() runs once at the end, and only if we actually added
	// a file (downloadListGrew) — a pure in-place stat poll stays sort-free.
	// The cold-boot m_initialUpdate path has its own batching (ShowFileList),
	// so leave it alone to avoid a redundant second sort. Captured now because
	// ProcessItemUpdate/AddFile below and the final prune all touch the ctrl.
	// The dialog is part of the condition: these Begin/End pairs and the
	// per-item calls between them are view work, and an EC reply can arrive
	// with no dialog at all (destroyed on link loss, rebuilt by Startup()).
	const bool batchDownloadList = !m_initialUpdate && theApp->amuledlg != nullptr;
	bool downloadListGrew = false;
	if (batchDownloadList) {
		theApp->amuledlg->m_transferwnd->downloadlistctrl->BeginBatchUpdate();
	}

	// Same reasoning for the shared-files list: a steady-state poll can surface
	// a burst of freshly-shared files (many downloads completing, or a shared
	// folder added/rescanned daemon-side), and ShowFile() -> AddItemData()
	// rebuilds the whole virtual-list row index on every insert -- O(n) each,
	// O(n^2) on a large share. Batch every non-initial poll and sort once at the
	// end, only if the poll actually added a file (sharedListGrew). Reconnect
	// reconcile is one such non-initial poll, so it is covered too.
	const bool batchSharedList = !m_initialUpdate && theApp->amuledlg != nullptr;
	bool sharedListGrew = false;
	if (batchSharedList) {
		theApp->amuledlg->m_sharedfileswnd->sharedfilesctrl->BeginBatchUpdate();
	}
	// Set below if the reconnect reconcile reply is too empty to trust as a
	// full snapshot: keep the one-shot armed instead of pruning (see #444).
	bool deferReconcile = false;

	// Partial-update protocol negotiated at auth time (see
	// CRemoteConnect::ServerSupportsPartialUpdate). When true, the server
	// only sends tags for files that actually changed and signals
	// deletions explicitly via top-level `EC_TAG_FILE_REMOVED` markers —
	// the bulk "anything missing == deleted" loop below would silently
	// wipe most of the library every cycle (#713). When false (old
	// server), the server emits alive-marker tags for unchanged files
	// so the bulk-deletion path stays correct.
	const bool partial_update = m_conn->ServerSupportsPartialUpdate();

	std::set<uint32> core_files;
	std::set<uint32> removed_files;
	for (CECPacket::const_iterator it = reply->begin(); it != reply->end(); ++it) {
		const CECTag *curTag = &*it;
		ec_tagname_t tagname = curTag->GetTagName();
		if (tagname == EC_TAG_CLIENT) {
			theApp->clientlist->ProcessUpdate(curTag, NULL, EC_TAG_CLIENT);
		} else if (tagname == EC_TAG_SERVER) {
			theApp->serverlist->ProcessUpdate(curTag, NULL, EC_TAG_SERVER);
		} else if (tagname == EC_TAG_FRIEND) {
			theApp->friendlist->ProcessUpdate(curTag, NULL, EC_TAG_FRIEND);
		} else if (tagname == EC_TAG_FILE_REMOVED) {
			// Explicit deletion marker from a partial-update-capable
			// server. Buffered and processed in one pass below so we
			// only iterate the live list when there's actually
			// something to remove. Outside the partial-update path
			// the server never emits this tag.
			removed_files.insert(curTag->GetInt());
		} else if (tagname == EC_TAG_KNOWNFILE || tagname == EC_TAG_PARTFILE) {
			const CEC_SharedFile_Tag *tag = static_cast<const CEC_SharedFile_Tag *>(curTag);
			uint32 id = tag->ID();
			bool isNew = true;
			if (!m_initialUpdate) {
				// Collect the full ID set whenever we mean to prune by
				// absence below: a legacy server always (it emits alive
				// markers for every file), or once right after a reconnect
				// (m_reconnectReconcile — the fresh session sends a full
				// snapshot we reconcile against).
				if (!partial_update || m_reconnectReconcile) {
					core_files.insert(id);
				}
				std::map<uint32, CKnownFile *>::iterator it2 = m_items_hash.find(id);
				if (it2 != m_items_hash.end()) {
					// Item already known: update it
					if (tag->HasChildTags()) {
						ProcessItemUpdate(tag, it2->second);
					}
					isNew = false;
				}
			}
			if (isNew) {
				// An alive-marker tag carries only the ECID with no
				// child tags; it's the compat-mode "this file still
				// exists" signal and is only meaningful for files we
				// already know. If we receive one for an ID we've never
				// seen (race during bulk Reload, server diff baseline
				// briefly out of step, etc.), constructing
				// CKnownFile(tag) on an empty tag yields a ghost entry
				// with no name and zero size (#808). Skip the marker
				// and rely on a subsequent full-tag update to introduce
				// the file.
				if (!tag->HasChildTags()) {
					AddDebugLogLineN(logEC,
						CFormat(wxT("EC: alive-marker for unknown file ID %u; "
							    "ignoring.")) %
							id);
					continue;
				}
				// Second variant of the same #808 ghost-entry pattern.
				// CEC_SharedFile_Tag runs each metadata field through
				// a per-connection CValueMap (ECSpecialTags.h) which
				// suppresses tags whose value hasn't changed since the
				// last cached send.  On an EC_DETAIL_INC_UPDATE for a
				// file ID we've never seen, the server may already
				// have cached + suppressed EC_TAG_PARTFILE_HASH /
				// _NAME / _SIZE_FULL, in which case the tag has plenty
				// of statistical child tags (request count, accepts,
				// transferred, AICH masterhash, priority...) but no
				// identifying metadata.  CKnownFile(tag) would then
				// read empty hash + 0 size via GetTagByNameSafe and
				// produce the same 0-byte unnamed ghost entry that the
				// HasChildTags() guard above is shaped against -- with
				// children but no identity.  Detect by the absence of
				// the file hash (the canonical identity tag) and skip
				// the same way; the next full-state poll picks the
				// file up correctly once the server-side cache
				// invalidation puts it back on the wire.
				if (tag->GetTagByName(EC_TAG_PARTFILE_HASH) == NULL) {
					AddDebugLogLineN(logEC,
						CFormat(wxT(
							"EC: incomplete INC_UPDATE tag (no PARTFILE_HASH) "
							"for unknown file ID %u; ignoring.")) %
							id);
					continue;
				}
				CKnownFile *newFile;
				if (tag->GetTagName() == EC_TAG_PARTFILE) {
					CPartFile *file =
						new CPartFile(static_cast<const CEC_PartFile_Tag *>(tag));
					ProcessItemUpdate(tag, file);
					(*theApp->downloadqueue)[id] = file;
					// On the initial full sync, defer the per-item show +
					// sort; the whole list is shown and sorted once below
					// via ShowFileList() (issue #414 — O(n^2) otherwise).
					if (theApp->amuledlg) {
						theApp->amuledlg->m_transferwnd->downloadlistctrl->AddFile(
							file, /*deferView=*/m_initialUpdate);
					}
					downloadListGrew = true;
					newFile = file;
				} else {
					newFile = new CKnownFile(tag);
					ProcessItemUpdate(tag, newFile);
					(*theApp->sharedfiles)[id] = newFile;
					if (!m_initialUpdate) {
						if (theApp->amuledlg) {
							theApp->amuledlg->m_sharedfileswnd->sharedfilesctrl
								->ShowFile(newFile);
						}
						sharedListGrew = true;
					}
				}
				AddItem(newFile);
			}
		}
	}

	if (m_initialUpdate) {
		if (theApp->amuledlg) {
			theApp->amuledlg->m_sharedfileswnd->sharedfilesctrl->ShowFileList();
			theApp->amuledlg->m_transferwnd->downloadlistctrl->ShowFileList();
		}
		m_initialUpdate = false;
	} else if (partial_update && !m_reconnectReconcile) {
		// Normal partial-update poll: apply explicit removals from
		// `EC_TAG_FILE_REMOVED` markers. One linear pass over the live
		// list, only when there's actually something to remove.
		if (!removed_files.empty()) {
			for (iterator it = begin(); it != end();) {
				iterator it2 = it++;
				if (removed_files.count(GetItemID(*it2))) {
					RemoveItem(it2);
				}
			}
		}
	} else if (reconcile && core_files.empty() && GetCount() != 0) {
		// Defensive guard (#444): the first poll after a reconnect should
		// carry the full library snapshot. If it came back with no file tags
		// at all while we still hold a populated list, that reply is not a
		// trustworthy baseline — pruning by absence here would wipe every
		// download and shared file. Skip the prune and leave the one-shot
		// armed so the next poll reconciles against a real snapshot instead.
		AddDebugLogLineN(logEC,
			wxT("EC: post-reconnect reconcile reply carried no files; "
			    "deferring the absence-prune to the next poll."));
		deferReconcile = true;
	} else {
		// Legacy server (alive-marker protocol), OR the first poll after a
		// reconnect (m_reconnectReconcile): anything missing from the
		// response is deleted. On reconnect the fresh partial-update server
		// sends a full snapshot but never re-emits FILE_REMOVED for files
		// deleted while we were disconnected, so this one-shot prune against
		// the full ID set is how those stale entries get cleared.
		for (iterator it = begin(); it != end();) {
			iterator it2 = it++;
			if (!core_files.count(GetItemID(*it2))) {
				RemoveItem(it2); // This calls DeleteItem, where it is removed from lists and
						 // views.
			}
		}
	}
	if (batchDownloadList) {
		// Sort once if the poll added a file (or on a reconnect resync, where
		// the whole list was reconciled); otherwise the order is unchanged and
		// EndBatchUpdate() just Thaws without a needless re-sort.
		theApp->amuledlg->m_transferwnd->downloadlistctrl->EndBatchUpdate(
			reconcile || downloadListGrew);
	}
	if (batchSharedList) {
		// Sort once if the poll added a shared file (or on a reconnect resync,
		// where the whole list was reconciled); otherwise the order is
		// unchanged and EndBatchUpdate() just Thaws without a needless re-sort.
		theApp->amuledlg->m_sharedfileswnd->sharedfilesctrl->EndBatchUpdate(
			reconcile || sharedListGrew);
	}
	// One-shot: consumed by the first post-reconnect poll above, unless the
	// reply was too empty to trust and we left the flag armed for the next.
	if (!deferReconcile) {
		m_reconnectReconcile = false;
	}
}

CKnownFilesRem::CKnownFilesRem(CRemoteConnect *conn)
: CRemoteContainer<CKnownFile, uint32, CEC_SharedFile_Tag>(conn, true)
{
	requested = 0;
	transferred = 0;
	accepted = 0;
	m_initialUpdate = true;
}

/*
 * List of uploading and waiting clients.
 */
CUpDownClientListRem::CUpDownClientListRem(CRemoteConnect *conn)
: CRemoteContainer<CClientRef, uint32, CEC_UpDownClient_Tag>(conn, true)
{
}

CClientRef::CClientRef(const CEC_UpDownClient_Tag *tag)
{
	m_client = new CUpDownClient(tag);
#ifdef DEBUG_ZOMBIE_CLIENTS
	m_client->Link("TAG");
#else
	m_client->Link();
#endif
}

CUpDownClient::CUpDownClient(const CEC_UpDownClient_Tag *tag)
: CECID(tag->ID())
{
	m_linked = 0;
#ifdef DEBUG_ZOMBIE_CLIENTS
	m_linkedDebug = false;
#endif
	// Clients start up empty, then get asked for their data.
	// So all data here is processed in ProcessItemUpdate and thus updatable.
	m_bEmuleProtocol = false;
	m_AvailPartCount = 0;
	m_clientSoft = 0;
	m_nDownloadState = 0;
	m_Friend = NULL;
	m_bFriendSlot = false;
	m_nKadPort = 0;
	m_kBpsDown = 0;
	m_dwUserIP = 0;
	m_lastDownloadingPart = 0xffff;
	m_nextRequestedPart = 0xffff;
	m_obfuscationStatus = 0;
	m_modCapabilities.Reset();
	m_nOldRemoteQueueRank = 0;
	m_nRemoteQueueRank = 0;
	m_reqfile = NULL;
	m_score = 0;
	m_dwServerIP = 0;
	m_nServerPort = 0;
	m_nSourceFrom = SF_NONE;
	m_nTransferredDown = 0;
	m_nTransferredUp = 0;
	m_nUpDatarate = 0;
	m_uploadingfile = NULL;
	m_waitingPosition = 0;
	m_nUploadState = 0;
	m_nUserIDHybrid = 0;
	m_nUserPort = 0;
	m_nClientVersion = 0;
	m_fNoViewSharedFiles = true;
	m_identState = IS_NOTAVAILABLE;
	m_bRemoteQueueFull = false;

	credits = new CClientCredits(new CreditStruct());
}

#ifdef DEBUG_ZOMBIE_CLIENTS
void CUpDownClient::Unlink(const wxString &from)
{
	std::multiset<wxString>::iterator it = m_linkedFrom.find(from);
	if (it != m_linkedFrom.end()) {
		m_linkedFrom.erase(it);
	}
	m_linked--;
	if (!m_linked) {
		if (m_linkedDebug) {
			AddLogLineN(CFormat("Last reference to client %d %p unlinked, delete it.") % ECID() %
				    this);
		}
		delete this;
	}
}

#else

void CUpDownClient::Unlink()
{
	m_linked--;
	if (!m_linked) {
		delete this;
	}
}
#endif

uint64 CUpDownClient::GetDownloadedTotal() const
{
	return credits->GetDownloadedTotal();
}

uint64 CUpDownClient::GetUploadedTotal() const
{
	return credits->GetUploadedTotal();
}

double CUpDownClient::GetScoreRatio() const
{
	return credits->GetScoreRatio(GetIP(), theApp->CryptoAvailable());
}

/* End Warning */

CUpDownClient::~CUpDownClient()
{
	delete credits;
}

CClientRef *CUpDownClientListRem::CreateItem(const CEC_UpDownClient_Tag *tag)
{
	CClientRef *client = new CClientRef(tag);
	ProcessItemUpdate(tag, client);

	return client;
}

void CUpDownClientListRem::DeleteItem(CClientRef *clientref)
{
	CUpDownClient *client = clientref->GetClient();

	if (client->m_reqfile) {
		client->m_reqfile->DelSource(client);
		client->m_reqfile = NULL;
	}
	Notify_SourceCtrlRemoveSource(client->ECID(), (CPartFile *)NULL);

	if (client->m_uploadingfile) {
		client->m_uploadingfile->RemoveUploadingClient(client); // this notifies
		client->m_uploadingfile = NULL;
	}
	theApp->m_allUploadingKnownFile->RemoveUploadingClient(
		client); // in case it vanished directly while uploading
	Notify_SharedCtrlRemoveClient(client->ECID(), (CKnownFile *)NULL);

	if (client->m_Friend) {
		client->m_Friend->UnLinkClient(); // this notifies
		client->m_Friend = NULL;
	}

#ifdef DEBUG_ZOMBIE_CLIENTS
	if (client->m_linked > 1) {
		// Same level and wording as the core-side twin in BaseClient.cpp.
		AddLogLineN(CFormat("Client %d: deletion deferred, still referenced in %d place(s): %s") %
			    client->ECID() % (client->m_linked - 1) % client->GetLinkedFrom());
		client->m_linkedDebug = true;
	}
#endif

	delete clientref;
}

uint32 CUpDownClientListRem::GetItemID(CClientRef *client)
{
	return client->ECID();
}

void CUpDownClientListRem::ProcessItemUpdate(const CEC_UpDownClient_Tag *tag, CClientRef *clientref)
{
	if (!tag->HasChildTags()) {
		return; // speed exit for clients without any change
	}
	CUpDownClient *client = clientref->GetClient();

	tag->UserID(&client->m_nUserIDHybrid);
	tag->ClientName(&client->m_Username);
#ifdef GEOIP_GUI
	// Peer country from the daemon's GeoIP (#439). Check tag *presence*, not
	// its value: a present-but-empty tag is an authoritative "unknown" and
	// must still mark the code as core-provided so the client list doesn't
	// try a (non-existent) local fallback.
	if (tag->GetTagByName(EC_TAG_CLIENT_COUNTRY)) {
		client->SetCountryCode(tag->Country());
	}
#endif
	// Client Software
	bool sw_updated = false;
	if (tag->ClientSoftware(client->m_clientSoft)) {
		client->m_clientSoftString = GetSoftName(client->m_clientSoft);
		sw_updated = true;
	}
	if (tag->SoftVerStr(client->m_clientVerString) || sw_updated) {
		if (client->m_clientSoftString == _("Unknown")) {
			client->m_fullClientVerString = client->m_clientSoftString;
		} else {
			client->m_fullClientVerString =
				client->m_clientSoftString + " " + client->m_clientVerString;
		}
	}
	// User hash
	tag->UserHash(&client->m_UserHash);

	// User IP:Port
	tag->UserIP(client->m_dwUserIP);
	tag->UserPort(&client->m_nUserPort);

	// Server IP:Port
	tag->ServerIP(&client->m_dwServerIP);
	tag->ServerPort(&client->m_nServerPort);
	tag->ServerName(&client->m_ServerName);

	tag->KadPort(client->m_nKadPort);
	tag->FriendSlot(client->m_bFriendSlot);

	tag->GetCurrentIdentState(&client->m_identState);
	tag->ObfuscationStatus(client->m_obfuscationStatus);
	{
		// Additive tag: an older daemon sends nothing and the mirror keeps
		// whatever it had, which for a fresh client is the empty word.
		uint32 modCapabilities = 0;
		if (tag->ModCapabilities(modCapabilities)) {
			client->m_modCapabilities.SetFromWire(modCapabilities);
		}
	}
	tag->HasExtendedProtocol(&client->m_bEmuleProtocol);

	tag->WaitingPosition(&client->m_waitingPosition);
	tag->RemoteQueueRank(&client->m_nRemoteQueueRank);
	client->m_bRemoteQueueFull = client->m_nRemoteQueueRank == 0xffff;
	tag->OldRemoteQueueRank(&client->m_nOldRemoteQueueRank);

	tag->ClientDownloadState(client->m_nDownloadState);
	if (tag->ClientUploadState(client->m_nUploadState)) {
		if (client->m_nUploadState == US_UPLOADING) {
			theApp->m_allUploadingKnownFile->AddUploadingClient(client);
		} else {
			theApp->m_allUploadingKnownFile->RemoveUploadingClient(client);
		}
	}

	tag->SpeedUp(&client->m_nUpDatarate);
	if (client->m_nDownloadState == DS_DOWNLOADING) {
		tag->SpeedDown(&client->m_kBpsDown);
	} else {
		client->m_kBpsDown = 0;
	}

	// tag->WaitTime(&client->m_WaitTime);
	// tag->XferTime(&client->m_UpStartTimeDelay);
	// tag->LastReqTime(&client->m_dwLastUpRequest);
	// tag->QueueTime(&client->m_WaitStartTime);

	CreditStruct *credit_struct = const_cast<CreditStruct *>(client->credits->GetDataStruct());
	tag->XferUp(&credit_struct->uploaded);
	tag->XferUpSession(&client->m_nTransferredUp);

	tag->XferDown(&credit_struct->downloaded);
	tag->XferDownSession(&client->m_nTransferredDown);

	tag->Score(&client->m_score);

	tag->NextRequestedPart(client->m_nextRequestedPart);
	tag->LastDownloadingPart(client->m_lastDownloadingPart);

	uint8 sourceFrom = 0;
	if (tag->GetSourceFrom(sourceFrom)) {
		client->m_nSourceFrom = (ESourceFrom)sourceFrom;
	}

	tag->RemoteFilename(client->m_clientFilename);
	tag->DisableViewShared(client->m_fNoViewSharedFiles);
	tag->Version(client->m_nClientVersion);
	tag->ModVersion(client->m_strModVersion);
	tag->OSInfo(client->m_sClientOSInfo);
	tag->AvailableParts(client->m_AvailPartCount);

	// Download client
	uint32 fileID;
	bool notified = false;
	if (tag->RequestFile(fileID)) {
		if (client->m_reqfile) {
			Notify_SourceCtrlRemoveSource(client->ECID(), client->m_reqfile);
			client->m_reqfile->DelSource(client);
			client->m_reqfile = NULL;
			client->m_downPartStatus.clear();
		}
		CKnownFile *kf = theApp->knownfiles->GetByID(fileID);
		if (kf && kf->IsCPartFile()) {
			client->m_reqfile = static_cast<CPartFile *>(kf);
			client->m_reqfile->AddSource(client);
			client->m_downPartStatus.setsize(kf->GetPartCount(), 0);
			Notify_SourceCtrlAddSource(
				client->m_reqfile, CCLIENTREF(client, "AddSource"), A4AF_SOURCE);
			notified = true;
		}
	}

	// Part status
	const CECTag *partStatusTag = tag->GetTagByName(EC_TAG_CLIENT_PART_STATUS);
	if (partStatusTag) {
		if (partStatusTag->GetTagDataLen() == 0) {
			// empty tag means full source
			client->m_downPartStatus.SetAllTrue();
		} else if (partStatusTag->GetTagDataLen() == client->m_downPartStatus.SizeBuffer()) {
			client->m_downPartStatus.SetBuffer(partStatusTag->GetTagData());
		}
		notified = false;
	}

	if (!notified && client->m_reqfile && client->m_reqfile->ShowSources()) {
		SourceItemType type;
		switch (client->GetDownloadState()) {
		case DS_DOWNLOADING:
		case DS_ONQUEUE:
			// We will send A4AF, which will be checked.
			type = A4AF_SOURCE;
			break;
		default:
			type = UNAVAILABLE_SOURCE;
			break;
		}

		Notify_SourceCtrlUpdateSource(client->ECID(), type);
	}

	// Upload client
	notified = false;
	if (tag->UploadFile(fileID)) {
		if (client->m_uploadingfile) {
			client->m_uploadingfile->RemoveUploadingClient(client); // this notifies
			notified = true;
			client->m_uploadingfile = NULL;
		}
		CKnownFile *kf = theApp->knownfiles->GetByID(fileID);
		if (kf) {
			client->m_uploadingfile = kf;
			client->m_upPartStatus.setsize(kf->GetPartCount(), 0);
			client->m_uploadingfile->AddUploadingClient(client); // this notifies
			notified = true;
		}
	}

	// Part status
	partStatusTag = tag->GetTagByName(EC_TAG_CLIENT_UPLOAD_PART_STATUS);
	if (partStatusTag) {
		if (partStatusTag->GetTagDataLen() == client->m_upPartStatus.SizeBuffer()) {
			client->m_upPartStatus.SetBuffer(partStatusTag->GetTagData());
		}
		notified = false;
	}

	if (!notified && client->m_uploadingfile &&
		(client->m_uploadingfile->ShowPeers() || (client->m_nUploadState == US_UPLOADING))) {
		// notify if KnowFile is selected, or if it's uploading (in case clients are in show uploading
		// mode)
		SourceItemType type;
		switch (client->GetUploadState()) {
		case US_UPLOADING:
		case US_ONUPLOADQUEUE:
			type = AVAILABLE_SOURCE;
			break;
		default:
			type = UNAVAILABLE_SOURCE;
			break;
		}
		Notify_SharedCtrlRefreshClient(client->ECID(), type);
	}
}

/*
 * Download queue container: hold PartFiles with progress status
 *
 */

bool CDownQueueRem::AddLink(const wxString &link, uint8 cat)
{
	CECPacket req(EC_OP_ADD_LINK);
	CECTag link_tag(EC_TAG_STRING, link);
	link_tag.AddTag(CECTag(EC_TAG_PARTFILE_CAT, cat));
	req.AddTag(link_tag);

	// SendRequest registers the handler; on EC_OP_FAILED the GUI shows
	// the wxMessageBox set up in CAddLinkHandler. Previously this was
	// fire-and-forget and silently dropped errors (#310 follow-up).
	m_conn->SendRequest(new CAddLinkHandler, &req);
	return true;
}

void CDownQueueRem::AddLinks(const wxArrayString &links, uint8 cat)
{
	// Pack the whole batch into one EC_OP_ADD_LINK packet. The daemon-side
	// handler (PR #551) already iterates over every child tag and emits a
	// single aggregated response, so CAddLinkHandler fires once for the
	// whole batch — no client-side N popups for N invalid links.
	if (links.IsEmpty()) {
		return;
	}
	CECPacket req(EC_OP_ADD_LINK);
	for (size_t i = 0; i < links.GetCount(); ++i) {
		CECTag link_tag(EC_TAG_STRING, links[i]);
		link_tag.AddTag(CECTag(EC_TAG_PARTFILE_CAT, cat));
		req.AddTag(link_tag);
	}
	m_conn->SendRequest(new CAddLinkHandler, &req);
}

void CDownQueueRem::ResetCatParts(int cat)
{
	// Called when category is deleted. Command will be performed on the remote side,
	// but files still should be updated here right away, or drawing errors (colour not available)
	// will happen.
	for (iterator it = begin(); it != end(); ++it) {
		CPartFile *file = it->second;
		file->RemoveCategory(cat);
	}
}

void CKnownFilesRem::ProcessItemUpdatePartfile(const CEC_PartFile_Tag *tag, CPartFile *file)
{
	//
	// update status
	//
	tag->Speed(&file->m_kbpsDown);
	file->kBpsDown = file->m_kbpsDown / 1024.0;

	tag->SizeXfer(&file->transferred);
	tag->SizeDone(&file->completedsize);
	tag->SourceXferCount(&file->transferingsrc);
	tag->SourceNotCurrCount(&file->m_notCurrentSources);
	tag->SourceCount(&file->m_source_count);
	tag->SourceCountA4AF(&file->m_a4af_source_count);
	tag->FileStatus(&file->status);
	tag->Stopped(&file->m_stopped);

	tag->LastSeenComplete(&file->lastseencomplete);
	tag->LastDateChanged(&file->m_lastDateChanged);
	tag->DownloadActiveTime(&file->m_nDlActiveTime);
	tag->AvailablePartCount(&file->m_availablePartsCount);
	tag->Shared(&file->m_isShared);
	tag->A4AFAuto(file->m_is_A4AF_auto);
	tag->HashingProgress(file->m_hashingProgress);

	tag->GetLostDueToCorruption(&file->m_iLostDueToCorruption);
	tag->GetGainDueToCompression(&file->m_iGainDueToCompression);
	tag->TotalPacketsSavedDueToICH(&file->m_iTotalPacketsSavedDueToICH);

	tag->FileCat(&file->m_category);

	tag->DownPrio(&file->m_iDownPriorityEC);
	if (file->m_iDownPriorityEC >= 10) {
		file->m_iDownPriority = file->m_iDownPriorityEC - 10;
		file->m_bAutoDownPriority = true;
	} else {
		file->m_iDownPriority = file->m_iDownPriorityEC;
		file->m_bAutoDownPriority = false;
	}

	file->percentcompleted = (100.0 * file->GetCompletedSize()) / file->GetFileSize();

	//
	// Copy part/gap status
	//
	const CECTag *gaptag = tag->GetTagByName(EC_TAG_PARTFILE_GAP_STATUS);
	const CECTag *parttag = tag->GetTagByName(EC_TAG_PARTFILE_PART_STATUS);
	const CECTag *reqtag = tag->GetTagByName(EC_TAG_PARTFILE_REQ_STATUS);
	if (gaptag || parttag || reqtag) {
		PartFileEncoderData &encoder = file->m_PartFileEncoderData;

		if (gaptag) {
			ArrayOfUInts64 gaps;
			encoder.DecodeGaps(gaptag, gaps);
			int gap_size = gaps.size() / 2;
			// clear gaplist
			file->m_gaplist.Init(file->GetFileSize(), false);

			// and refill it
			for (int j = 0; j < gap_size; j++) {
				file->m_gaplist.AddGap(gaps[2 * j], gaps[2 * j + 1]);
			}
		}
		if (parttag) {
			encoder.DecodeParts(parttag, file->m_SrcpartFrequency);
			// sanity check
			wxASSERT(file->m_SrcpartFrequency.size() == file->GetPartCount());
		}
		if (reqtag) {
			ArrayOfUInts64 reqs;
			encoder.DecodeReqs(reqtag, reqs);
			int req_size = reqs.size() / 2;
			// clear reqlist
			DeleteContents(file->m_requestedblocks_list);

			// and refill it
			for (int j = 0; j < req_size; j++) {
				Requested_Block_Struct *block = new Requested_Block_Struct;
				block->StartOffset = reqs[2 * j];
				block->EndOffset = reqs[2 * j + 1];
				file->m_requestedblocks_list.push_back(block);
			}
		}
	}

	// Get source names and counts
	const CECTag *srcnametag = tag->GetTagByName(EC_TAG_PARTFILE_SOURCE_NAMES);
	if (srcnametag) {
		SourcenameItemMap &map = file->GetSourcenameItemMap();
		for (CECTag::const_iterator it = srcnametag->begin(); it != srcnametag->end(); ++it) {
			uint32 key = it->GetInt();
			int count = it->GetTagByNameSafe(EC_TAG_PARTFILE_SOURCE_NAMES_COUNTS)->GetInt();
			if (count == 0) {
				map.erase(key);
			} else {
				SourcenameItem &item = map[key];
				item.count = count;
				const CECTag *nametag = it->GetTagByName(EC_TAG_PARTFILE_SOURCE_NAMES);
				if (nametag) {
					item.name = nametag->GetStringData();
				}
			}
		}
	}

	// Comments/ratings + the Kad-notes running flag are decoded once for every
	// known file in CKnownFilesRem::ProcessItemUpdate (they ride the shared-file
	// base tag now), so there is nothing partfile-specific to do here.

	// Update A4AF sources
	ListOfUInts32 &clientIDs = file->GetA4AFClientIDs();
	const CECTag *a4aftag = tag->GetTagByName(EC_TAG_PARTFILE_A4AF_SOURCES);
	if (a4aftag) {
		file->ClearA4AFList();
		clientIDs.clear();
		for (CECTag::const_iterator it = a4aftag->begin(); it != a4aftag->end(); ++it) {
			if (it->GetTagName() != EC_TAG_ECID) { // should always be this
				continue;
			}
			uint32 id = it->GetInt();
			CClientRef *src = theApp->clientlist->GetByID(id);
			if (src) {
				file->AddA4AFSource(src->GetClient());
			} else {
				// client wasn't transmitted yet, try it later
				clientIDs.push_back(id);
			}
		}
	} else if (!clientIDs.empty()) {
		// Process clients from the last pass whose ids were still unknown then
		for (ListOfUInts32::iterator it = clientIDs.begin(); it != clientIDs.end();) {
			ListOfUInts32::iterator it1 = it++;
			uint32 id = *it1;
			CClientRef *src = theApp->clientlist->GetByID(id);
			if (src) {
				file->AddA4AFSource(src->GetClient());
				clientIDs.erase(it1);
			}
		}
	}

	if (theApp->amuledlg) {
		theApp->amuledlg->m_transferwnd->downloadlistctrl->UpdateItem(file);
	}

	// If file is shared check if it is already listed in shared files.
	// If not, add it and show it.
	if (file->IsShared() && !theApp->sharedfiles->count(file->ECID())) {
		(*theApp->sharedfiles)[file->ECID()] = file;
		if (theApp->amuledlg) {
			theApp->amuledlg->m_sharedfileswnd->sharedfilesctrl->ShowFile(file);
		}
	}
}

void CDownQueueRem::SendFileCommand(CPartFile *file, ec_tagname_t cmd)
{
	CECPacket req(cmd);
	req.AddTag(CECTag(EC_TAG_PARTFILE, file->GetFileHash()));

	m_conn->SendPacket(&req);
}

void CDownQueueRem::Prio(CPartFile *file, uint8 prio)
{
	CECPacket req(EC_OP_PARTFILE_PRIO_SET);

	CECTag hashtag(EC_TAG_PARTFILE, file->GetFileHash());
	hashtag.AddTag(CECTag(EC_TAG_PARTFILE_PRIO, prio));
	req.AddTag(hashtag);

	m_conn->SendPacket(&req);
}

void CDownQueueRem::AutoPrio(CPartFile *file, bool flag)
{
	CECPacket req(EC_OP_PARTFILE_PRIO_SET);

	CECTag hashtag(EC_TAG_PARTFILE, file->GetFileHash());

	hashtag.AddTag(CECTag(EC_TAG_PARTFILE_PRIO, (uint8)(flag ? PR_AUTO : file->GetDownPriority())));
	req.AddTag(hashtag);

	m_conn->SendPacket(&req);
}

void CDownQueueRem::Category(CPartFile *file, uint8 cat)
{
	CECPacket req(EC_OP_PARTFILE_SET_CAT);
	file->SetCategory(cat);

	CECTag hashtag(EC_TAG_PARTFILE, file->GetFileHash());
	hashtag.AddTag(CECTag(EC_TAG_PARTFILE_CAT, cat));
	req.AddTag(hashtag);

	m_conn->SendPacket(&req);
}

void CDownQueueRem::AddSearchToDownload(CSearchFile *file, uint8 category)
{
	CECPacket req(EC_OP_DOWNLOAD_SEARCH_RESULT);
	CECTag hashtag(EC_TAG_PARTFILE, file->GetFileHash());
	hashtag.AddTag(CECTag(EC_TAG_PARTFILE_CAT, category));
	req.AddTag(hashtag);

	m_conn->SendPacket(&req);
}

void CDownQueueRem::ClearCompleted(const ListOfUInts32 &ecids)
{
	CECPacket req(EC_OP_CLEAR_COMPLETED);
	for (ListOfUInts32::const_iterator it = ecids.begin(); it != ecids.end(); ++it) {
		req.AddTag(CECTag(EC_TAG_ECID, *it));
	}

	m_conn->SendPacket(&req);
}

/*
 * List of friends.
 */
CFriendListRem::CFriendListRem(CRemoteConnect *conn)
: CRemoteContainer<CFriend, uint32, CEC_Friend_Tag>(conn, true)
{
}

void CFriendListRem::HandlePacket(const CECPacket *)
{
	wxFAIL; // not needed
}

CFriend *CFriendListRem::CreateItem(const CEC_Friend_Tag *tag)
{
	CFriend *Friend = new CFriend(tag->ID());
	ProcessItemUpdate(tag, Friend);
	return Friend;
}

void CFriendListRem::DeleteItem(CFriend *Friend)
{
	Friend->UnLinkClient(false);
	Notify_ChatRemoveFriend(Friend);
}

uint32 CFriendListRem::GetItemID(CFriend *Friend)
{
	return Friend->ECID();
}

void CFriendListRem::ProcessItemUpdate(const CEC_Friend_Tag *tag, CFriend *Friend)
{
	if (!tag->HasChildTags()) {
		return;
	}
	tag->Name(Friend->m_strName);
	tag->UserHash(Friend->m_UserHash);
	tag->IP(Friend->m_dwLastUsedIP);
	tag->Port(Friend->m_nLastUsedPort);
	// Read the slot back rather than leaving it at its constructor false: the
	// context menu's check mark is drawn from it, so without this it never
	// showed which friend actually holds the slot -- not even right after
	// this GUI set it.
	bool friendSlot = false;
	if (tag->FriendSlot(friendSlot)) {
		Friend->SetPersistentFriendSlot(friendSlot);
	}
	uint32 clientID;
	bool notified = false;
	if (tag->Client(clientID)) {
		if (clientID) {
			CClientRef *client = theApp->clientlist->GetByID(clientID);
			if (client) {
				Friend->LinkClient(*client); // this notifies
				notified = true;
			}
		} else {
			// Unlink
			Friend->UnLinkClient(false);
		}
	}
	if (!notified) {
		Notify_ChatUpdateFriend(Friend);
	}
}

void CFriendListRem::AddFriend(const CClientRef &toadd)
{
	CECPacket req(EC_OP_FRIEND);

	CECEmptyTag addtag(EC_TAG_FRIEND_ADD);
	addtag.AddTag(CECTag(EC_TAG_CLIENT, toadd.ECID()));
	req.AddTag(addtag);

	m_conn->SendPacket(&req);
}

void CFriendListRem::AddFriend(
	const CMD4Hash &userhash, uint32 lastUsedIP, uint32 lastUsedPort, const wxString &name)
{
	CECPacket req(EC_OP_FRIEND);

	CECEmptyTag addtag(EC_TAG_FRIEND_ADD);
	addtag.AddTag(CECTag(EC_TAG_FRIEND_HASH, userhash));
	addtag.AddTag(CECTag(EC_TAG_FRIEND_IP, lastUsedIP));
	addtag.AddTag(CECTag(EC_TAG_FRIEND_PORT, lastUsedPort));
	addtag.AddTag(CECTag(EC_TAG_FRIEND_NAME, name));
	req.AddTag(addtag);

	m_conn->SendPacket(&req);
}

CFriend *CFriendListRem::LookupFriend(const CMD4Hash &userhash, uint32 dwIP, uint16 nPort) const
{
	for (CFriend *cur_friend : m_items) {
		if (!userhash.IsEmpty() && cur_friend->HasHash()) {
			if (cur_friend->GetUserHash() == userhash) {
				return cur_friend;
			}
		} else if (dwIP != 0 && cur_friend->GetIP() == dwIP && cur_friend->GetPort() == nPort) {
			// A zero address is the absence of one, not a value to match on.
			return cur_friend;
		}
	}
	return nullptr;
}

void CFriendListRem::RemoveFriend(CFriend *toremove)
{
	CECPacket req(EC_OP_FRIEND);

	CECEmptyTag removetag(EC_TAG_FRIEND_REMOVE);
	removetag.AddTag(CECTag(EC_TAG_FRIEND, toremove->ECID()));
	req.AddTag(removetag);

	m_conn->SendPacket(&req);
}

void CFriendListRem::SetFriendSlot(CFriend *Friend, bool new_state)
{
	if (!Friend) {
		return;
	}
	CECPacket req(EC_OP_FRIEND);

	CECTag slottag(EC_TAG_FRIEND_FRIENDSLOT, new_state);
	slottag.AddTag(CECTag(EC_TAG_FRIEND, Friend->ECID()));
	req.AddTag(slottag);

	m_conn->SendPacket(&req);
}

// "View Files" (browse) over EC. On a multi-search daemon, open an optimistic
// browse tab and correlate it via EC_TAG_SEARCH_REF: the daemon allocates the
// browse's search id, echoes the ref, and CSearchListRem::HandlePacket rekeys
// the tab (and starts polling its progress) exactly like a search START reply.
// On a legacy daemon (no multi-search) fall back to the old fire-and-forget
// request with no tab — the daemon can't surface browse results over EC there,
// so opening a tab would just leave a phantom empty page. Behaviour unchanged.
static void SendBrowseRequest(
	CRemoteConnect *conn, CECEmptyTag &sharedtag, uint32 peerEcid, const wxString &peerName)
{
	CECPacket req(EC_OP_FRIEND);
	if (conn->ServerSupportsMultiSearch() && theApp->amuledlg && theApp->amuledlg->m_searchwnd) {
		wxUIntPtr localID = theApp->amuledlg->m_searchwnd->AllocateOptimisticId();
		theApp->amuledlg->m_searchwnd->EnsureBrowseTab(peerEcid, peerName, localID);
		sharedtag.AddTag(CECTag(EC_TAG_SEARCH_REF, (uint32)localID));
		req.AddTag(sharedtag);
		conn->SendRequest(theApp->searchlist, &req);
	} else {
		req.AddTag(sharedtag);
		conn->SendPacket(&req);
	}
}

void CFriendListRem::RequestSharedFileList(CFriend *Friend)
{
	CECEmptyTag sharedtag(EC_TAG_FRIEND_SHARED);
	sharedtag.AddTag(CECTag(EC_TAG_FRIEND, Friend->ECID()));
	SendBrowseRequest(m_conn, sharedtag, Friend->ECID(), Friend->GetName());
}

void CFriendListRem::RequestSharedFileList(CClientRef &client)
{
	CECEmptyTag sharedtag(EC_TAG_FRIEND_SHARED);
	sharedtag.AddTag(CECTag(EC_TAG_CLIENT, client.ECID()));
	SendBrowseRequest(m_conn, sharedtag, client.ECID(), client.GetUserName());
}

/*
 * Search results
 */
CSearchListRem::CSearchListRem(CRemoteConnect *conn)
: CRemoteContainer<CSearchFile, uint32, CEC_SearchFile_Tag>(conn, true)
, m_needSearchListRequery(true)
{
	m_curr_search = 0;
}

wxString CSearchListRem::StartNewSearch(
	uint32 *nSearchID, SearchType search_type, const CSearchList::CSearchParams &params)
{
	CECPacket search_req(EC_OP_SEARCH_START);
	EC_SEARCH_TYPE ec_search_type = EC_SEARCH_LOCAL;
	switch (search_type) {
	case LocalSearch:
		ec_search_type = EC_SEARCH_LOCAL;
		break;
	case GlobalSearch:
		ec_search_type = EC_SEARCH_GLOBAL;
		break;
	case KadSearch:
		ec_search_type = EC_SEARCH_KAD;
		break;
	}
	search_req.AddTag(CEC_Search_Tag(params.searchString,
		ec_search_type,
		params.typeText,
		params.extension,
		params.availability,
		params.minSize,
		params.maxSize));

	if (m_conn->ServerSupportsMultiSearch()) {
		// Multi-search: the daemon allocates the real search ID. Send the
		// optimistic local tab ID as a correlation token (EC_TAG_SEARCH_REF);
		// the daemon echoes it alongside the allocated EC_TAG_SEARCH_ID, and
		// HandlePacket remaps the tab. Sent via SendRequest so the reply is
		// routed back to this handler.
		search_req.AddTag(CECTag(EC_TAG_SEARCH_REF, *nSearchID));
		m_conn->SendRequest(this, &search_req);
		// See m_pendingSearchStarts's declaration: closes the window where an
		// EC_OP_SEARCH_LIST reply could double-tab this not-yet-remapped
		// search before RemapSearch erases this ID again.
		m_pendingSearchStarts.insert(*nSearchID);
	} else {
		// Legacy single-search daemon: no ID negotiation, sentinel bucket.
		m_conn->SendPacket(&search_req);
	}
	m_curr_search = *(nSearchID);

	// Legacy single-search wipes the one result set on the daemon at each new
	// search, so flush the container to match. Multi-search must NOT flush: the
	// container holds every open search's results, and clearing its item hash
	// (without clearing the displayed rows) makes the next poll re-create every
	// result — producing ghost rows (INC_UPDATE) or duplicate rows (FULL).
	if (!m_conn->ServerSupportsMultiSearch()) {
		Flush();
	}

	return ""; // EC reply will have the error mesg is needed.
}

void CSearchListRem::StopSearch(bool globalOnly)
{
	// globalOnly is the new-search path (OnBnClickedStart): monolithic uses it
	// to reset only the single in-flight ed2k slot while sparing running Kad
	// searches. In multi-search each search is independent and the daemon
	// finalizes any in-flight ed2k search itself, so starting a new search must
	// NOT stop the previous one — otherwise a second Kad search cancels the
	// first. On a legacy single-search daemon, fall through (the daemon replaces
	// the one search anyway). globalOnly == false is the explicit Stop button.
	if (globalOnly && m_conn->ServerSupportsMultiSearch()) {
		return;
	}
	StopSearchById(m_curr_search, false);
}

void CSearchListRem::StopSearchById(wxUIntPtr searchID, bool andClose)
{
	if (searchID == 0) {
		return;
	}
	CECPacket search_req(EC_OP_SEARCH_STOP);
	if (m_conn->ServerSupportsMultiSearch()) {
		// Per-ID stop; the close flag also frees the results (tab close).
		search_req.AddTag(CECTag(EC_TAG_SEARCH_ID, (uint32)searchID));
		if (andClose) {
			search_req.AddTag(CECEmptyTag(EC_TAG_SEARCH_CLOSE));
			// Tab closed: stop tracking this search's lifecycle.
			m_activeSearches.erase((uint32)searchID);
			m_kadActive.erase((uint32)searchID);
			// Also the backstop for a START whose reply never attributed
			// itself (EC_OP_FAILED carries no ID -- see HandlePacket's
			// EC_OP_FAILED branch): closing the tab the failed start left
			// behind clears its entry, so the discovery deferral can't be
			// held open for the rest of the session. A no-op for the normal
			// case, where RemapSearch already erased it.
			m_pendingSearchStarts.erase((uint32)searchID);
		}
	}
	// Legacy: parameterless stop of the single current search.
	m_conn->SendPacket(&search_req);
}

bool CSearchListRem::IsKadSearch(uint32_t searchID) const
{
	// The "More" button applies only to a *running* Kad search (it greys out
	// once the search completes). The flag is set from each search's per-id
	// kind + lifecycle state in HandlePacket.
	std::map<uint32, bool>::const_iterator it = m_kadActive.find(searchID);
	return it != m_kadActive.end() && it->second;
}

bool CSearchListRem::RequestMoreResults(uint32_t searchID)
{
	if (searchID == 0) {
		return false;
	}
	// Ask the daemon to widen this Kad search (KADEMLIA_FIND_VALUE_MORE).
	// SendRequest, not SendPacket: the daemon answers with EC_OP_MISC_DATA
	// carrying EC_TAG_SEARCH_MORE_REASKABLE, and HandlePacket below greys the
	// button for that search when it says the reasking is over for good. This
	// used to be fire-and-forget and returned an optimistic true, so a user
	// could press "More" repeatedly against a daemon that was doing nothing.
	//
	// Still returns true here: the verdict arrives asynchronously, and this
	// return only says the request went out. The greying happens in the reply
	// handler.
	CECPacket req(EC_OP_SEARCH_REQUEST_MORE);
	if (m_conn->ServerSupportsMultiSearch()) {
		req.AddTag(CECTag(EC_TAG_SEARCH_ID, (uint32)searchID));
	}
	m_conn->SendRequest(this, &req);
	return true;
}

void CSearchListRem::RemapSearch(uint32 localID, uint32 daemonID)
{
	// Closes this ID's m_pendingSearchStarts window regardless of whether the
	// local and daemon ids happen to match (the early return below is only
	// about whether a rekey is needed, not whether the START round trip this
	// tracks has completed). Keyed by ID, so a *browse* remap -- which also
	// lands here, via BuildBrowseReply's own EC_OP_STRINGS -- erases nothing
	// and can no longer lift the deferral for someone else's in-flight
	// search start (got3nks, PR #680 review).
	m_pendingSearchStarts.erase(localID);
	if (localID == daemonID) {
		return;
	}
	// Rekey the optimistic tab (created with the local ID) to the daemon's
	// ID so the union-poll results (tagged with the daemon ID) route to it.
	// The START reply arrives well before any network results, so no result
	// is misrouted in the interim.
	if (theApp->amuledlg && theApp->amuledlg->m_searchwnd) {
		theApp->amuledlg->m_searchwnd->RekeySearch(localID, daemonID);
	}
	m_curr_search = daemonID;
	// Track this search for per-tab progress polling.
	m_activeSearches.insert(daemonID);
}

void CSearchListRem::RequestSearchList()
{
	// Capability-gated: a daemon predating #680 has no case for
	// EC_OP_SEARCH_LIST, so the request would land in ProcessRequest2's
	// unknown-opcode branch, which logs "invalid opcode received: 0x60" and
	// trips a wxFAIL on the daemon. Sending nothing degrades to the pre-#680
	// behaviour -- only searches this client started itself are listed -- which
	// is exactly what such a daemon can support anyway.
	if (!m_conn->ServerSupportsSearchList()) {
		return;
	}

	CECPacket req(EC_OP_SEARCH_LIST);
	m_conn->SendRequest(this, &req);
}

void CSearchListRem::ProcessUpdate(const CECTag *reply, CECPacket *full_req, int req_type)
{
	if (!m_conn->ServerSupportsPartialSearch()) {
		// Legacy daemon: it still re-sends every live result each poll and
		// expects absence to mean deletion.
		CRemoteContainer<CSearchFile, uint32, CEC_SearchFile_Tag>::ProcessUpdate(
			reply, full_req, req_type);
		return;
	}
	for (const CECTag &entry : *reply) {
		const CECTag *curTag = &entry;
		if (curTag->GetTagName() == EC_TAG_FILE_REMOVED) {
			// The one path that deletes now. Same tombstone the shared-file
			// list uses; the payload is the result's ECID.
			const uint32 ecid = static_cast<uint32>(curTag->GetInt());
			std::map<uint32, CSearchFile *>::iterator hit = m_items_hash.find(ecid);
			if (hit == m_items_hash.end()) {
				continue;
			}
			for (iterator lit = begin(); lit != end(); ++lit) {
				if (GetItemID(*lit) == ecid) {
					RemoveItem(lit);
					break;
				}
			}
			continue;
		}
		if (curTag->GetTagName() != req_type) {
			continue;
		}
		const CEC_SearchFile_Tag *tag = static_cast<const CEC_SearchFile_Tag *>(curTag);
		if (m_items_hash.count(tag->ID())) {
			ProcessItemUpdate(tag, m_items_hash[tag->ID()]);
		} else {
			// A result new to this client always arrives with its owning
			// EC_TAG_SEARCH_ID (the daemon omits that tag only for results it
			// has already attributed), so CreateItem can route it to a tab.
			AddItem(CreateItem(tag));
		}
	}
}

void CSearchListRem::ApplySearchProgress(const CECTag *src)
{
	// Per-search progress: STATUS is the first tag; EC_TAG_SEARCH_ID is
	// echoed so we can update this specific tab's lifecycle. An expired
	// search (evicted on the daemon) reports done so its "!" clears.
	const CECTag *idTag = src->GetTagByName(EC_TAG_SEARCH_ID);
	const CECTag *browseTag = src->GetTagByName(EC_TAG_SEARCH_BROWSE_STATUS);
	if (idTag && theApp->amuledlg && theApp->amuledlg->m_searchwnd) {
		if (browseTag) {
			// A browse ("View Files") tab: update its lifecycle marker
			// (browsing / finished / failed) AND drive the gauge from the
			// bar value (EC_TAG_SEARCH_STATUS) via the same per-tab path a
			// search uses.
			theApp->amuledlg->m_searchwnd->SetBrowseStatus(
				idTag->GetInt(), (uint32)browseTag->GetInt());
			if (const CECTag *barTag = src->GetTagByName(EC_TAG_SEARCH_STATUS)) {
				theApp->amuledlg->m_searchwnd->UpdateSearchProgress(
					idTag->GetInt(), (uint32)barTag->GetInt());
			}
		} else {
			const bool expired = src->GetTagByName(EC_TAG_SEARCH_EXPIRED) != nullptr;
			if (expired) {
				// The daemon has discarded this search -- a deliberate
				// close from another client, or LRU eviction -- so
				// its tab here can only mislead: mapping this to the
				// plain "finished" status (0xfffe) would make it
				// indistinguishable from a search that ended
				// normally, and "Download" on one of its results
				// would silently do nothing (the daemon's m_results
				// no longer has the hash). Close the tab locally
				// instead -- CloseSearchTab does not send
				// EC_OP_SEARCH_STOP, since the daemon already
				// doesn't know this id (got3nks, PR #680 review).
				// CSearchListRem::RemoveResults (called from there)
				// also erases m_kadActive for this id.
				const uint32 sid = (uint32)idTag->GetInt();
				m_activeSearches.erase(sid);
				if (theApp->amuledlg && theApp->amuledlg->m_searchwnd) {
					theApp->amuledlg->m_searchwnd->CloseSearchTab(sid);
				}
			} else {
				// Cache whether this tab is a *running* Kad search so
				// IsKadSearch can gate the "More" button — enabled
				// only while the search runs, disabled once it
				// completes (the progress lifecycle is the gate; no
				// extra status). Update before the progress call,
				// which refreshes that button for the visible tab.
				const CECTag *kindTag = src->GetTagByName(EC_TAG_SEARCH_LIFECYCLE_KIND);
				const CECTag *stateTag = src->GetTagByName(EC_TAG_SEARCH_LIFECYCLE_STATE);
				if (kindTag && stateTag) {
					m_kadActive[(uint32)idTag->GetInt()] =
						(kindTag->GetInt() == KadSearch) &&
						(stateTag->GetInt() == CSearchList::SEARCH_LIFECYCLE_RUNNING);
				}
				theApp->amuledlg->m_searchwnd->UpdateSearchProgress(
					idTag->GetInt(), (uint32)src->GetFirstTagSafe()->GetInt());
			}
		}
	}
}

void CSearchListRem::HandlePacket(const CECPacket *packet)
{
	// Reply to EC_OP_SEARCH_REQUEST_MORE. Claimed by the TAG, not by the
	// opcode alone: EC_OP_MISC_DATA is a generic envelope (search stop and
	// GET_CONNSTATE use it too), and consuming every packet carrying that
	// opcode would silently swallow any future request that both answers
	// MISC_DATA and routes its handler here. Only a packet that actually
	// carries the reaskable bit is ours; anything else falls through.
	//
	// The tag is absent on a daemon older than it, which means "unknown",
	// never "exhausted" -- so that case falls through too and keeps the
	// previous optimistic behaviour. Present and false is terminal for this
	// search: its reask budget is spent, or it is inside the stopping window
	// Kad enters 20 s before a keyword search ends.
	//
	// The id has to come off the packet rather than from the selected tab.
	// The EC FIFO pairs replies to requests positionally, with no request id
	// on the wire, so with two "More" presses in flight on different tabs the
	// arrival order is the only thing distinguishing them -- and the user may
	// have switched tabs meanwhile. The daemon echoes EC_TAG_SEARCH_ID for
	// exactly this.
	if (const CECTag *reaskable = packet->GetTagByName(EC_TAG_SEARCH_MORE_REASKABLE)) {
		const CECTag *idTag = packet->GetTagByName(EC_TAG_SEARCH_ID);
		if (idTag && reaskable->GetInt() == 0 && theApp->amuledlg && theApp->amuledlg->m_searchwnd) {
			theApp->amuledlg->m_searchwnd->MarkMoreExhausted((uint32)idTag->GetInt());
		}
		return;
	}
	if (packet->GetOpCode() == EC_OP_SEARCH_PROGRESS) {
		if (m_conn->ServerSupportsMultiSearch()) {
			if (m_conn->ServerSupportsSearchProgressUnion()) {
				// Union form: one child per search the daemon holds, so a
				// client with N open tabs costs one round trip instead of N.
				// Every child is an EC_TAG_SEARCH_ID entry whose own value is
				// the search id and whose children are that search's progress.
				std::set<uint32> reported;
				for (const CECTag &entry : *packet) {
					if (entry.GetTagName() != EC_TAG_SEARCH_ID) {
						continue;
					}
					reported.insert((uint32)entry.GetInt());
					ApplySearchProgress(&entry);
				}
				// The union carries no EC_TAG_SEARCH_EXPIRED: it reports the
				// daemon's whole set, so a tab whose id is missing from it is
				// one the daemon no longer has -- closed from another client,
				// or evicted by the LRU. Same verdict the per-id poll reaches
				// via the explicit tag, and it lands on the same cycle rather
				// than whenever that particular id next gets polled.
				//
				// Snapshot first: CloseSearchTab erases from m_activeSearches.
				const std::set<uint32> tracked = m_activeSearches;
				for (uint32 id : tracked) {
					if (reported.count(id)) {
						continue;
					}
					m_activeSearches.erase(id);
					if (theApp->amuledlg && theApp->amuledlg->m_searchwnd) {
						theApp->amuledlg->m_searchwnd->CloseSearchTab(id);
					}
				}
			} else {
				ApplySearchProgress(packet);
			}
		} else {
			CoreNotify_Search_Update_Progress(packet->GetFirstTagSafe()->GetInt());
		}
	} else if (packet->GetOpCode() == EC_OP_STRINGS) {
		// Multi-search START reply: remap the optimistic local tab ID to the
		// daemon-allocated ID. Guarded by both tags so non-search string
		// replies are ignored.
		const CECTag *idTag = packet->GetTagByName(EC_TAG_SEARCH_ID);
		const CECTag *refTag = packet->GetTagByName(EC_TAG_SEARCH_REF);
		if (idTag && refTag) {
			RemapSearch(refTag->GetInt(), idTag->GetInt());
		}
	} else if (packet->GetOpCode() == EC_OP_FAILED) {
		// A rejected EC_OP_SEARCH_START or browse (EC_OP_FRIEND, via
		// SendBrowseRequest -- both route their replies here). Both send
		// EC_TAG_SEARCH_REF, the optimistic tab id, and the daemon now echoes
		// it on the failure paths too, so the verdict is attributable: without
		// it, StartNewSearch's unconditional "" return means OnBnClickedStart
		// already took its success branch and created a tab, leaving a phantom
		// tab plus an id stuck in m_pendingSearchStarts -- which disables
		// discovery and costs an EC_OP_SEARCH_LIST round trip every tick until
		// the user happens to close exactly that tab (got3nks, PR #680 review).
		//
		// The branch also has to exist at all: without it an EC_OP_FAILED falls
		// through to CRemoteContainer::HandlePacket, which hits wxFAIL in IDLE
		// and, if a DoRequery happened to be outstanding, consumes that reply's
		// state instead.
		if (const CECTag *refTag = packet->GetTagByName(EC_TAG_SEARCH_REF)) {
			const uint32 localID = static_cast<uint32>(refTag->GetInt());
			m_pendingSearchStarts.erase(localID);
			// Report it and undo the optimistic tab/button state, through the
			// same path the monolithic build uses for a rejected start. The
			// daemon sends the reason as EC_TAG_STRING (a wxTRANSLATE'd
			// literal, so translate it here like CAddLinkHandler does);
			// without this the user clicks Search and nothing at all happens.
			if (theApp->amuledlg && theApp->amuledlg->m_searchwnd) {
				const CECTag *msgTag = packet->GetTagByName(EC_TAG_STRING);
				const wxString reason = (msgTag && msgTag->IsString())
								? wxGetTranslation(msgTag->GetStringData())
								: wxString();
				theApp->amuledlg->m_searchwnd->OnStartRejected(localID, reason);
			}
			// Nothing to prune from m_activeSearches / m_kadActive: both are
			// only ever keyed by a daemon-allocated id (RemapSearch, or the
			// discovery branch), never by an optimistic one.
		}
	} else if (packet->GetOpCode() == EC_OP_SEARCH_LIST) {
		// Reachability fix (#641): one entry per search the daemon currently
		// holds, so a search this client never started locally -- one
		// already open in another amulegui session, or restored from disk
		// once persistence lands -- gets a tab here instead of silently
		// having its results dropped by AddResult/UpdateResult (neither
		// looks up a tab that doesn't exist yet). Skips any ID that already
		// has a tab (a search this client itself started) rather than
		// recreating it.
		//
		// Deferred whole, rather than per-id, while m_pendingSearchStarts
		// is non-empty: this reply's ids reflect the daemon's state at send
		// time, which can already include a search THIS client just
		// started but hasn't been told the id of yet (see that member's
		// declaration) -- and the ids in it are the client's *optimistic*
		// ones, which by construction never match the daemon ids here, so
		// there is nothing to match per-entry against. Re-arming for the
		// next tick costs one extra poll, not correctness, since the set
		// only stays non-empty for one EC round trip.
		if (!m_pendingSearchStarts.empty()) {
			m_needSearchListRequery = true;
		} else if (theApp->amuledlg && theApp->amuledlg->m_searchwnd) {
			for (const CECTag &entry : *packet) {
				uint32 sid = static_cast<uint32>(entry.GetInt());
				if (sid == 0 || theApp->amuledlg->m_searchwnd->GetSearchList(sid)) {
					continue;
				}
				const CECTag *nameTag = entry.GetTagByName(EC_TAG_SEARCH_NAME);
				// "(0)" matches CSearchDlg::CreateNewTab's own callers (e.g.
				// SearchDlg.cpp:1017) -- no leading "!" even for a Kad search:
				// that marker is a live indicator normal callers seed only
				// because they know at creation time they just started a Kad
				// search, and it is only ever cleared (KadSearchEnd), never
				// set, by the progress path below. Making this tab first-class
				// in m_activeSearches is what lets that live path take over
				// from here -- the "!" appears on the next progress poll if
				// this is in fact a running Kad search, same as any other tab.
				// Unselected: this tab appears on its own, driven by another
				// client, so it must not pull the selection away from whatever
				// the local user is looking at -- possibly mid-typing in the
				// search box (got3nks, amule-org/amule#703).
				// A browse the daemon is serving for someone else, or one
				// still running from before this GUI attached, arrives here
				// like any other search -- it shares the id space. Rebuild it
				// as a browse tab rather than a search tab, which is what the
				// kind is reported for. Daemons predating EC_SEARCH_BROWSE
				// never send it, so the test simply fails there and the tab is
				// built the way it always was.
				const CECTag *kindTag = entry.GetTagByName(EC_TAG_SEARCH_LIFECYCLE_KIND);
				const CECTag *peerTag = entry.GetTagByName(EC_TAG_CLIENT);
				const uint32 peerEcid = peerTag ? static_cast<uint32>(peerTag->GetInt()) : 0;
				// Only with a real peer: EnsureBrowseTab keys on the ecid and
				// an ecid of 0 is not "unknown peer", it is what every
				// ordinary search tab carries -- GetBrowseList(0) would match
				// the first of those and repoint it at this browse. A daemon
				// that reports the kind without the peer therefore falls
				// through to the plain tab, which is what it built before.
				if (kindTag && kindTag->GetInt() == BrowseSearch && peerEcid) {
					theApp->amuledlg->m_searchwnd->EnsureBrowseTab(peerEcid,
						nameTag ? nameTag->GetStringData() : wxString(),
						sid,
						false);
				} else {
					theApp->amuledlg->m_searchwnd->CreateNewTab(
						(nameTag ? nameTag->GetStringData() : wxString()) + " (0)",
						sid,
						false);
				}
				// Reachability fix (#641): without this, a discovered tab never
				// gets polled for progress at all (Phase1Done only loops over
				// m_activeSearches), so its hit count, progress bar and "!"
				// marker would stay frozen forever -- exactly like a search
				// this client started itself once RemapSearch runs.
				m_activeSearches.insert(sid);
				// Backfill (got3nks, PR #680 review): a result for this sid may
				// already have arrived and been dropped by CreateItem (no tab
				// existed yet to AddResult into), and the daemon's
				// EC_DETAIL_INC_UPDATE diffing never re-sends an
				// already-delivered result -- so without this, that result is
				// lost permanently rather than merely delayed. The container
				// (m_items) still holds it: CreateItem always keeps every
				// CSearchFile it builds, tab or no tab. Walk it now, once, for
				// this newly-discovered sid -- no extra EC traffic.
				for (CSearchFile *file : m_items) {
					if (file->GetSearchID() == sid) {
						theApp->amuledlg->m_searchwnd->AddResult(file);
					}
				}
			}
		}
	} else {
		CRemoteContainer<CSearchFile, uint32, CEC_SearchFile_Tag>::HandlePacket(packet);
	}
}

CSearchFile::CSearchFile(const CEC_SearchFile_Tag *tag)
: CECID(tag->ID())
, m_parent(NULL)
, m_showChildren(false)
, m_sourceCount(0)
, m_completeSourceCount(0)
, m_kademlia(false)
, m_downloadStatus(NEW)
, m_clientID(0)
, m_clientPort(0)
, m_kadPublishInfo(0)
{
	SetFileName(CPath(tag->FileName()));
	m_abyFileHash = tag->FileHash();
	SetFileSize(tag->SizeFull());

	uint8 rating = 0;
	if (tag->GetRating(rating)) {
		m_iUserRating = rating;
	}

	// Browse ("View Files") results carry their source peer's id/port and the
	// shared folder they live in — the daemon emits these only for browse
	// results, so a remote GUI can render per-source info and folder grouping.
	tag->GetBrowseSource(m_clientID, m_clientPort, m_directory);

	// Multi-search: the daemon's union poll tags every result with its owning
	// search ID, so route by that. 0 => legacy single-search reply, fall back
	// to the one current-search ID (old daemon / old behaviour).
	uint32 tagSearchID = tag->SearchID();
	m_searchID = tagSearchID ? tagSearchID : theApp->searchlist->m_curr_search;
	uint32 parentID = tag->ParentID();
	if (parentID) {
		CSearchFile *parent = theApp->searchlist->GetByID(parentID);
		if (parent) {
			parent->AddChild(this);
		}
	}
}

void CSearchFile::AddChild(CSearchFile *file)
{
	m_children.push_back(file);
	file->m_parent = this;
}

// dtor is virtual - must be implemented
CSearchFile::~CSearchFile()
{
	// Mirror the core dtor: let an open comments dialog drop this result before
	// it is freed (the remote container can remove/recreate a result while the
	// modal is up). The core ~CSearchFile lives in SearchFile.cpp, which the
	// remote GUI does not compile, so the notify has to be fired here too.
	Notify_SearchFileBeingDestroyed(this);
}

CSearchFile *CSearchListRem::CreateItem(const CEC_SearchFile_Tag *tag)
{
	CSearchFile *file = new CSearchFile(tag);
	ProcessItemUpdate(tag, file);

	// Reachability fix (#641): a result bearing a search ID with no tab
	// is exactly the symptom EC_OP_SEARCH_LIST exists to fix -- ask for
	// the list again on the next poll so a tab gets created for it
	// (CSearchListRem::HandlePacket's EC_OP_SEARCH_LIST branch) instead
	// of polling that op every tick regardless of whether anything new
	// showed up.
	if (file->m_searchID != 0 && !theApp->amuledlg->m_searchwnd->GetSearchList(file->m_searchID)) {
		m_needSearchListRequery = true;
	}

	// A result whose tag names a parent this client has not built yet cannot be
	// grouped: the constructor's lookup came back empty, so it stays parentless
	// and is indexed as a top-level row that nothing ever re-parents. The
	// daemon emits a parent before its children, so this should not happen --
	// log it rather than assert, since the trigger would be the wire order
	// rather than a bug on this side, and losing the result would be worse than
	// showing it ungrouped.
	if (tag->ParentID() != 0 && file->GetParent() == nullptr) {
		AddDebugLogLineN(logSearch,
			CFormat("Search result %u names parent %u, which has not arrived; "
				"showing it as a top-level result") %
				file->ECID() % tag->ParentID());
	}

	// Make the result visible to the GUI: the search model builds its rows
	// from GetSearchResults(), so a result that is not indexed here shows up
	// nowhere, however correctly it arrived over EC. Grouped children are
	// dropped by IndexResult() itself, which is where that rule lives.
	IndexResult(file);

	theApp->amuledlg->m_searchwnd->AddResult(file);

	return file;
}

void CSearchListRem::DeleteItem(CSearchFile *file)
{
	// De-index before freeing, so a later GetSearchResults() never hands out
	// this freed pointer.
	UnindexResult(file);
	delete file;
}

uint32 CSearchListRem::GetItemID(CSearchFile *file)
{
	return file->ECID();
}

void CSearchListRem::ProcessItemUpdate(const CEC_SearchFile_Tag *tag, CSearchFile *file)
{
	uint32 sourceCount = file->m_sourceCount;
	uint32 completeSourceCount = file->m_completeSourceCount;
	CSearchFile::DownloadStatus status = file->m_downloadStatus;
	tag->SourceCount(&file->m_sourceCount);
	tag->CompleteSourceCount(&file->m_completeSourceCount);
	tag->DownloadStatus((uint32 *)&file->m_downloadStatus);

	// On-demand Kad community ratings/comments (same positional encoding as a
	// partfile's; see CEC_SearchFile_Tag). The comments dialog polls the running
	// flag + rating list directly, so no explicit UpdateResult is needed here.
	const CECTag *commenttag = tag->GetTagByName(EC_TAG_PARTFILE_COMMENTS);
	if (commenttag) {
		file->ClearFileRatingList();
		for (CECTag::const_iterator it = commenttag->begin(); it != commenttag->end();) {
			wxString u = (it++)->GetStringData();
			wxString f = (it++)->GetStringData();
			sint16 r = static_cast<sint16>(static_cast<sint64>((it++)->GetInt()));
			wxString c = (it++)->GetStringData();
			file->AddFileRatingList(u, f, r, c);
		}
	}
	if (const CECTag *kadSearchTag = tag->GetTagByName(EC_TAG_PARTFILE_KAD_COMMENT_SEARCHING)) {
		file->SetKadCommentSearchRunning(kadSearchTag->GetInt() != 0);
	}

	// The daemon has always sent these for search results -- CEC_SearchFile_Tag
	// derives from CEC_SharedFile_Tag, whose base ctor emits them -- and this
	// walker never read them back. So amulegui showed an empty Length /
	// Bitrate / Codec for every hit while the monolithic client filled those
	// columns in from the same server reply. CreateItem calls this immediately
	// after constructing, so a result is decoded on arrival as well as on
	// update.
	DecodeMediaTags(tag, file);

	if (file->m_sourceCount != sourceCount || file->m_completeSourceCount != completeSourceCount ||
		file->m_downloadStatus != status) {
		if (theApp->amuledlg && theApp->amuledlg->m_searchwnd) {
			theApp->amuledlg->m_searchwnd->UpdateResult(file);
		}
	}
}

bool CSearchListRem::Phase1Done(const CECPacket *WXUNUSED(reply))
{
	// The empty check matters: with no open tabs the per-id path below sends
	// nothing, and an unconditional union request would turn that into a
	// roundtrip every cycle.
	if (m_conn->ServerSupportsSearchProgressUnion() && !m_activeSearches.empty()) {
		// One request covers every open tab: the daemon answers with one
		// child per search. Costs a single round trip no matter how many are
		// open, where the per-id path below sends one each -- and kept sending
		// them for searches that had long finished, since a tab only leaves
		// m_activeSearches when it is closed.
		//
		// The ids ride along so the daemon keeps bumping exactly the searches
		// this client still has open in its LRU, as the per-id poll did. An id
		// missing from the reply is one the daemon no longer holds.
		CECPacket progress_req(EC_OP_SEARCH_PROGRESS);
		for (uint32 id : m_activeSearches) {
			progress_req.AddTag(CECTag(EC_TAG_SEARCH_ID, id));
		}
		m_conn->SendRequest(this, &progress_req);
	} else if (m_conn->ServerSupportsMultiSearch()) {
		// Poll progress for each open search so every tab's lifecycle ("!",
		// progress bar) is tracked independently. Snapshot the set: an expired
		// reply may erase from m_activeSearches while iterating.
		std::set<uint32> ids = m_activeSearches;
		for (uint32 id : ids) {
			CECPacket progress_req(EC_OP_SEARCH_PROGRESS);
			progress_req.AddTag(CECTag(EC_TAG_SEARCH_ID, id));
			m_conn->SendRequest(this, &progress_req);
		}
	} else {
		CECPacket progress_req(EC_OP_SEARCH_PROGRESS);
		m_conn->SendRequest(this, &progress_req);
	}

	return true;
}

void CSearchListRem::RemoveResults(wxUIntPtr nSearchID)
{
	// The index only borrows: the CRemoteContainer owns these results and
	// frees them through DeleteItem(), so dropping the index is all that is
	// needed here -- deleting would double-free.
	DropResultIndex(nSearchID);
	m_kadActive.erase((uint32)nSearchID);
}

void CStatsUpdaterRem::HandlePacket(const CECPacket *packet)
{
	theStats::UpdateStats(packet);
	if (theApp->amuledlg) {
		theApp->amuledlg->ShowTransferRate();
	}
	theApp->ShowUserCount(); // maybe there should be a check if a usercount changed ?
	// handle the connstate tag which is included in the stats packet
	theApp->serverconnect->HandlePacket(packet);
}

void CUpDownClient::RequestSharedFileList()
{
	CClientRef ref = CCLIENTREF(this, "");
	theApp->friendlist->RequestSharedFileList(ref);
}

bool CUpDownClient::SwapToAnotherFile(bool WXUNUSED(bIgnoreNoNeeded),
	bool WXUNUSED(ignoreSuspensions),
	bool WXUNUSED(bRemoveCompletely),
	CPartFile *toFile)
{
	CECPacket req(EC_OP_CLIENT_SWAP_TO_ANOTHER_FILE);
	req.AddTag(CECTag(EC_TAG_CLIENT, ECID()));
	req.AddTag(CECTag(EC_TAG_PARTFILE, toFile->GetFileHash()));
	theApp->m_connect->SendPacket(&req);

	return true;
}

wxString CAICHHash::GetString() const
{
	return EncodeBase32(m_abyBuffer, HASHSIZE);
}

//
// Those functions are virtual. So even they don't get called they must
// be defined so linker will be happy
//
CPacket *CKnownFile::CreateSrcInfoPacket(
	const CUpDownClient *, uint8 /*byRequestedVersion*/, uint16 /*nRequestedOptions*/)
{
	wxFAIL;
	return 0;
}

bool CKnownFile::LoadFromFile(const class CFileDataIO *)
{
	wxFAIL;
	return false;
}

void CKnownFile::UpdatePartsInfo()
{
	wxFAIL;
}

CPacket *CPartFile::CreateSrcInfoPacket(
	CUpDownClient const *, uint8 /*byRequestedVersion*/, uint16 /*nRequestedOptions*/)
{
	wxFAIL;
	return 0;
}

void CPartFile::UpdatePartsInfo()
{
	wxFAIL;
}

void CPartFile::UpdateFileRatingCommentAvail()
{
	bool prevComment = m_hasComment;
	int prevRating = m_iUserRating;

	m_hasComment = false;
	m_iUserRating = 0;
	int ratingCount = 0;

	FileRatingList::iterator it = m_FileRatingList.begin();
	for (; it != m_FileRatingList.end(); ++it) {
		SFileRating &cur_rat = *it;

		if (!cur_rat.Comment.IsEmpty()) {
			m_hasComment = true;
		}

		uint8 rating = cur_rat.Rating;
		if (rating) {
			wxASSERT(rating <= 5);

			ratingCount++;
			m_iUserRating += rating;
		}
	}

	if (ratingCount) {
		m_iUserRating /= ratingCount;
		wxASSERT(m_iUserRating > 0 && m_iUserRating <= 5);
	}

	if ((prevComment != m_hasComment) || (prevRating != m_iUserRating)) {
		UpdateDisplayedInfo();
	}
}

void CStatTreeRem::DoRequery()
{
	CECPacket request(EC_OP_GET_STATSTREE);
	if (thePrefs::GetMaxClientVersions() != 0) {
		request.AddTag(CECTag(EC_TAG_STATTREE_CAPPING, (uint8)thePrefs::GetMaxClientVersions()));
	}
	m_conn->SendRequest(this, &request);
}

void CStatTreeRem::HandlePacket(const CECPacket *p)
{
	const CECTag *treeRoot = p->GetTagByName(EC_TAG_STATTREE_NODE);
	if (treeRoot) {
		if (theApp->amuledlg) {
			theApp->amuledlg->m_statisticswnd->RebuildStatTreeRemote(treeRoot);
			theApp->amuledlg->m_statisticswnd->ShowStatistics();
		}
	}
}

namespace
{
// See the comment in DoRequery: the daemon's newest history range holds
// this many records at its finest spacing, so this is how much it can
// serve before the answers stop lining up with the scale we asked for.
// Tracks CStatistics::GetPointsPerRange() and has to be raised with it.
//
// Against an older daemon, whose ranges are shallower, a request this
// deep runs off the end of the resolution it was asked for and the far
// left of the first backfill is drawn time-compressed. It corrects
// itself as live points replace the backfilled ones.
const uint16 kMaxHistoryPoints = 1800;

} // namespace

void CStatGraphRem::DoRequery()
{
	CECPacket request(EC_OP_GET_STATSGRAPHS, EC_DETAIL_FULL);
	// Send back the most recent timestamp we've seen; daemon-side
	// CStatistics::GetHistoryForGui uses it as the lower bound so the
	// response only carries points the GUI hasn't drawn yet.
	// (GetHistoryForWeb is amuleweb's near-identical twin, not this path.)
	request.AddTag(CECTag(EC_TAG_STATSGRAPH_LAST, m_lastTimestamp));
	// Seconds between points. This has to be the same step the graphs are
	// going to plot at -- COScopeCtrl gets it from the "Update delay"
	// preference via CStatisticsDlg::SetUpdatePeriod, and asks
	// CStatistics::GetHistory for points that far apart. Requesting a
	// fixed 1 s here (which is what this used to do) meant amulegui
	// honoured the preference when drawing and ignored it when fetching,
	// so the graphs only ever had the daemon's 1-second range behind them
	// however far back the user had asked to see. Monolithic amule has no
	// equivalent step because it reads the same history directly.
	const uint16 nScale = std::max<uint16>(thePrefs::GetTrafficOMeterInterval(), 1);
	if ((double)nScale != m_sScale) {
		// The ring keeps one record per requested interval, so what is in
		// it is only meaningful at the scale it was fetched at: replotting
		// 8 s records on a 3 s axis would place them at the wrong times.
		// Drop it and let it refill at the new resolution. Deliberately
		// without resetting m_lastTimestamp -- asking for a fresh backfill
		// here would race the reply still in the air from the previous
		// scale, whose points would then be timestamped with this one.
		theApp->m_statistics->ClearHistory();
		m_sScale = (double)nScale;
	}
	request.AddTag(CECTag(EC_TAG_STATSGRAPH_SCALE, nScale));
	// Upper bound on points per reply. In the steady state the daemon
	// only sends what is newer than the timestamp above, i.e. one point
	// per poll, so this bites in exactly two cases: the first poll after
	// connecting, where m_lastTimestamp is still 0 and the daemon serves
	// its whole history, and catching up after a stall or a reconnect.
	// Both then arrive in a single round-trip and the graphs open with
	// history behind them instead of filling in over the next minutes.
	//
	// The value is the depth of the daemon's 1-second history range:
	// CStatistics allocates GetPointsPerRange() records per range and
	// keeps only the newest range at 1 s spacing (the ones behind it go
	// to 2 s, 4 s, ...). Asking for more would return records that are
	// not 1 s apart while HandlePacket reconstructs their timestamps
	// assuming they are, which stretches the left of the plot rather
	// than showing more of the past.
	request.AddTag(CECTag(EC_TAG_STATSGRAPH_WIDTH, std::min(kMaxHistoryPoints, m_nDaemonDepth)));
	m_conn->SendRequest(this, &request);
}

void CStatGraphRem::HandlePacket(const CECPacket *p)
{
	// EC_OP_FAILED with "No points for graph." -> daemon has nothing
	// newer than m_lastTimestamp; nothing to do this cycle.
	if (p->GetOpCode() != EC_OP_STATSGRAPHS) {
		return;
	}
	const CECTag *dataTag = p->GetTagByName(EC_TAG_STATSGRAPH_DATA);
	const CECTag *tsTag = p->GetTagByName(EC_TAG_STATSGRAPH_LAST);
	if (!dataTag || !tsTag) {
		return;
	}
	m_lastTimestamp = tsTag->GetDoubleData();

	// How many points this daemon can answer with before it starts
	// repeating a record. Absent from daemons predating the tag, which is
	// exactly the case the conservative default covers. Learning that it
	// can serve more than we asked for is worth a one-off refetch: the
	// ring only accepts points newer than what it holds, so the deeper
	// history would never arrive otherwise.
	const CECTag *depthTag = p->GetTagByName(EC_TAG_STATSGRAPH_DEPTH);
	if (depthTag) {
		const uint16 nDepth = std::max<uint16>((uint16)depthTag->GetInt(), 1);
		if (nDepth > m_nDaemonDepth) {
			m_nDaemonDepth = nDepth;
			theApp->m_statistics->ClearHistory();
			m_lastTimestamp = 0.0;
			return;
		}
		m_nDaemonDepth = nDepth;
	}

	// EC_TAG_STATSGRAPH_DATA carries N x (dl_Bps, ul_Bps, conn, kadCur)
	// uint32 4-tuples in network byte order. Points are m_sScale seconds
	// apart, that being the scale DoRequery asked this reply for.
	const uint8_t *raw = (const uint8_t *)dataTag->GetTagData();
	size_t dataLen = dataTag->GetTagDataLen();
	size_t numPoints = dataLen / (4 * sizeof(uint32));

	// EC_TAG_STATSGRAPH_DATA_CONN (new tag, optional) carries the matching
	// N x (cntUploads, cntDownloads) uint32 pairs so the Connections scope
	// can show monolithic amule's 3-line breakdown. Absent when talking to
	// a pre-extension daemon — fall back to flat 0 lines for those slots.
	const CECTag *connTag = p->GetTagByName(EC_TAG_STATSGRAPH_DATA_CONN);
	const uint8_t *connRaw = NULL;
	size_t connPoints = 0;
	if (connTag) {
		connRaw = (const uint8_t *)connTag->GetTagData();
		connPoints = connTag->GetTagDataLen() / (2 * sizeof(uint32));
	}

	// Session totals — let amulegui compute the same
	// kBytesReceived / sTimestamp session average monolithic plots.
	// Absent on old daemons; fall back to 0 (Session line stays flat).
	const CECTag *sesDlTag = p->GetTagByName(EC_TAG_STATSGRAPH_SESSION_DL);
	const CECTag *sesUlTag = p->GetTagByName(EC_TAG_STATSGRAPH_SESSION_UL);
	const CECTag *sesKadTag = p->GetTagByName(EC_TAG_STATSGRAPH_SESSION_KAD);
	const CECTag *sesTsTag = p->GetTagByName(EC_TAG_STATSGRAPH_SESSION_TIMESPAN);
	const double sessionTs = sesTsTag ? sesTsTag->GetDoubleData() : 0.0;
	const uint64 sessionDlB = sesDlTag ? sesDlTag->GetInt() : 0;
	const uint64 sessionUlB = sesUlTag ? sesUlTag->GetInt() : 0;
	const uint64 sessionKadN = sesKadTag ? sesKadTag->GetInt() : 0;
	const float sessionDl = sessionTs > 0.0 ? (float)(sessionDlB / sessionTs) : 0.0f;
	const float sessionUl = sessionTs > 0.0 ? (float)(sessionUlB / sessionTs) : 0.0f;
	const float sessionKad = sessionTs > 0.0 ? (float)(sessionKadN / sessionTs) : 0.0f;

	// Cumulative session counters for each point in the reply.
	//
	// ComputeAverages derives the session-average trend as
	// kBytesReceived / sTimestamp, so it needs the cumulative figure as
	// it stood at each point, while the daemon sends only the newest.
	// The rest are reconstructed from the per-point rates this same reply
	// carries -- a rectangle-rule integral, so it carries a few percent
	// of error against a spiky signal.
	//
	// Which direction to integrate matters, because that error is divided
	// by the point's timestamp. Going backwards from the newest puts it
	// at the oldest point, and when a reply reaches back near the start of
	// the daemon's session that divisor is a few seconds: a 3% error on
	// several GB then reads as tens of MB/s where the truth is nearly
	// zero, and past the session start the divisor goes negative and the
	// trend flips sign entirely.
	//
	// So pick the end that is known exactly. If the reply spans the whole
	// session the oldest point's total is zero by definition, and
	// integrating forward from there leaves the error at the newest
	// point, where it is divided by the full session length. Otherwise
	// every timestamp in the window is large and backwards from the
	// daemon's own newest figure is both exact where it starts and
	// harmless where it ends.
	std::vector<double> aKBytesDl(numPoints, 0.0);
	std::vector<double> aKBytesUl(numPoints, 0.0);
	std::vector<double> aKadTotal(numPoints, 0.0);
	// Within one sample of zero means the oldest point IS the session's
	// first sample -- the daemon answers with what it has, so a reply that
	// reaches the start stops there rather than running past it. Testing
	// for a timestamp at or below zero would almost never fire.
	const double oldestTs = m_lastTimestamp - (double)(numPoints - 1) * m_sScale;
	const bool spansSessionStart = oldestTs <= m_sScale;
	{
		auto rateAt = [&](size_t j, size_t offset) {
			uint32 v;
			memcpy(&v, raw + j * 16 + offset, 4);
			return (double)ENDIAN_NTOHL(v);
		};

		if (spansSessionStart) {
			double kDl = 0.0, kUl = 0.0, kKad = 0.0;
			for (size_t j = 0; j < numPoints; ++j) {
				kDl += (rateAt(j, 0) / 1024.0) * m_sScale;
				kUl += (rateAt(j, 4) / 1024.0) * m_sScale;
				kKad += rateAt(j, 12) * m_sScale;
				aKBytesDl[j] = kDl;
				aKBytesUl[j] = kUl;
				aKadTotal[j] = kKad;
			}
		} else {
			double kDl = (double)sessionDlB;
			double kUl = (double)sessionUlB;
			double kKad = (double)sessionKadN;
			for (size_t j = numPoints; j-- > 0;) {
				aKBytesDl[j] = std::max(kDl, 0.0);
				aKBytesUl[j] = std::max(kUl, 0.0);
				aKadTotal[j] = std::max(kKad, 0.0);

				kDl -= (rateAt(j, 0) / 1024.0) * m_sScale;
				kUl -= (rateAt(j, 4) / 1024.0) * m_sScale;
				kKad -= rateAt(j, 12) * m_sScale;
			}
		}
	}

	for (size_t i = 0; i < numPoints; i++) {
		uint32 dl, ul, conn, kad;
		memcpy(&dl, raw + i * 16 + 0, 4);
		memcpy(&ul, raw + i * 16 + 4, 4);
		memcpy(&conn, raw + i * 16 + 8, 4);
		memcpy(&kad, raw + i * 16 + 12, 4);
		dl = ENDIAN_NTOHL(dl);
		ul = ENDIAN_NTOHL(ul);
		conn = ENDIAN_NTOHL(conn);
		kad = ENDIAN_NTOHL(kad);

		uint32 cntUp = 0, cntDown = 0;
		if (connRaw && i < connPoints) {
			memcpy(&cntUp, connRaw + i * 8 + 0, 4);
			memcpy(&cntDown, connRaw + i * 8 + 4, 4);
			cntUp = ENDIAN_NTOHL(cntUp);
			cntDown = ENDIAN_NTOHL(cntDown);
		}

		const float dl_kbps = dl / 1024.0f;
		const float ul_kbps = ul / 1024.0f;
		const float kad_cnt = (float)kad;

		GraphUpdateInfo update;
		update.timestamp = m_lastTimestamp;
		// Slot layout matches CStatistics::GetPointsForUpdate:
		//   downloads/uploads/kadnodes — [0] session avg, [1] running avg, [2] current.
		//   connections — [0] cntUploads, [1] cntConnections, [2] cntDownloads.
		// Only timestamp and kadnodes[2] are read now that the graphs draw
		// from the history rather than from the point handed to them; the
		// rest is filled to keep the struct's contract with the monolithic
		// producer. In particular the running-average slots are left at
		// their per-point value: the trend the graphs actually plot is
		// rebuilt by CStatistics::ComputeAverages, which sizes its window
		// from the sample step and so stays right whatever scale we asked
		// the daemon for.
		update.downloads[0] = sessionDl;
		update.downloads[1] = dl_kbps;
		update.downloads[2] = dl_kbps;
		update.uploads[0] = sessionUl;
		update.uploads[1] = ul_kbps;
		update.uploads[2] = ul_kbps;
		update.kadnodes[0] = sessionKad;
		update.kadnodes[1] = kad_cnt;
		update.kadnodes[2] = kad_cnt;
		update.connections[0] = (float)cntUp;
		update.connections[1] = (float)conn;
		update.connections[2] = (float)cntDown;

		if (conn > m_peakConnections) {
			m_peakConnections = conn;
		}

		// Mirror the decoded point into the client-side history ring
		// so COScopeCtrl (which draws straight from the history, and is
		// shared with monolithic) can replay across tab switches and
		// auto-rescale wipes without another daemon round-trip. This has
		// to happen before the graphs are told about the sample, which is
		// also the order the monolithic build sees: there,
		// CStatistics::GetPointsForUpdate reads the record back out of
		// the history to build update. Field mapping mirrors
		// GetPointsForUpdate so the same GetHistory + ComputeAverages
		// code paths read it back correctly:
		//   kBytes{Received,Sent} / kadNodesTotal are stored as
		//   (session rate * timestamp) so ComputeAverages's
		//   "kValueRun / sTimestamp" recovers the session-avg trend.
		// Per-point timestamps are reconstructed from the batch by
		// stepping back from m_lastTimestamp at 1 s spacing (matches
		// the scale we request in DoRequery).
		const double pointTs = m_lastTimestamp - (double)(numPoints - 1 - i) * m_sScale;

		// A reply can reach back past the moment the daemon's session
		// began -- its history list is preallocated, so it answers with
		// as many points as were asked for even when fewer exist. Those
		// points reconstruct to a timestamp at or before zero, and
		// ComputeAverages divides by it: at zero the session average is
		// undefined and below it the whole trend changes sign. Drop them
		// rather than store a value that cannot be drawn honestly.
		if (pointTs <= 0.0) {
			continue;
		}

		HR hr = { /* kBytesSent     */ aKBytesUl[i],
			/* kBytesReceived */ aKBytesDl[i],
			/* kBpsUpCur      */ ul_kbps,
			/* kBpsDownCur    */ dl_kbps,
			/* sTimestamp     */ pointTs,
			/* cntDownloads   */ (uint16)cntDown,
			/* cntUploads     */ (uint16)cntUp,
			/* cntConnections */ (uint16)conn,
			/* kadNodesCur    */ (uint16)kad,
			/* kadNodesTotal  */ (uint64)aKadTotal[i] };
		theApp->m_statistics->AddHistoryRecord(hr, m_sScale);

		if (theApp->amuledlg) {
			theApp->amuledlg->m_statisticswnd->UpdateStatGraphs(m_peakConnections, update);
			theApp->amuledlg->m_kademliawnd->UpdateGraph(update);
		}
	}
}

CamuleRemoteGuiApp *theApp;

//
// since gui is not linked with amule.cpp - define events here
//
wxDEFINE_EVENT(wxEVT_CORE_FINISHED_HTTP_DOWNLOAD, wxEvent);
wxDEFINE_EVENT(wxEVT_CORE_SOURCE_DNS_DONE, wxEvent);
wxDEFINE_EVENT(wxEVT_CORE_UDP_DNS_DONE, wxEvent);
wxDEFINE_EVENT(wxEVT_CORE_SERVER_DNS_DONE, wxEvent); // File_checked_for_headers
