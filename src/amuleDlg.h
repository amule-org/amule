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

#ifndef AMULEDLG_H
#define AMULEDLG_H

#include "config.h" // for ENABLE_VERSION_CHECK (gates the startup version-check members)

#include <wx/archive.h>
#include <wx/bmpbndl.h>
#include <wx/filename.h>
#include <wx/frame.h> // Needed for wxFrame
#include <wx/imaglist.h>
#include <wx/timer.h>
#include <wx/wfstream.h>
#include <wx/zipstrm.h>

#include <vector>

#include "Types.h" // Needed for uint32
#include "StatisticsDlg.h"

class wxTimerEvent;
class wxTextCtrl;
class CVersionCheck;

class CIP2Country;
class CTransferWnd;
class CServerWnd;
class CSharedFilesWnd;
class CSearchDlg;
class CChatWnd;
class CKadDlg;
class PrefsUnifiedDlg;

class CMuleTrayIcon;

struct PageType
{
	wxWindow *page;
	wxString name;
};

#define MP_RESTORE 4001
#define MP_CONNECT 4002
#define MP_DISCONNECT 4003
#define MP_EXIT 4004

#define DEFAULT_SIZE_X 800
#define DEFAULT_SIZE_Y 600

enum ClientSkinEnum
{
	Client_Green_Smiley = 0,
	Client_Red_Smiley,
	Client_Yellow_Smiley,
	Client_Grey_Smiley,
	Client_White_Smiley,
	Client_ExtendedProtocol_Smiley,
	Client_SecIdent_Smiley,
	Client_BadGuy_Smiley,
	Client_CreditsGrey_Smiley,
	Client_CreditsYellow_Smiley,
	Client_Upload_Smiley,
	Client_Friend_Smiley,
	Client_eMule_Smiley,
	Client_mlDonkey_Smiley,
	Client_eDonkeyHybrid_Smiley,
	Client_aMule_Smiley,
	Client_lphant_Smiley,
	Client_Shareaza_Smiley,
	Client_xMule_Smiley,
	Client_Unknown,
	Client_InvalidRating_Smiley,
	Client_PoorRating_Smiley,
	Client_FairRating_Smiley,
	Client_GoodRating_Smiley,
	Client_ExcellentRating_Smiley,
	Client_CommentOnly_Smiley,
	Client_Encryption_Smiley,
	// Add items here.
	CLIENT_SKIN_SIZE
};

// Indexes into CamuleDlg::m_tblist. Must match the order of the
// Add_Skin_Icon("Toolbar_...") calls in Apply_Toolbar_Skin.
enum ToolbarSkinEnum
{
	Toolbar_Connect = 0,
	Toolbar_Disconnect,
	Toolbar_Connecting,
	Toolbar_Network,
	Toolbar_Transfers,
	Toolbar_Search,
	Toolbar_Shared,
	Toolbar_Messages,
	Toolbar_Stats,
	Toolbar_Prefs,
	Toolbar_Import,
	Toolbar_About,
	Toolbar_Blink,
	// Add items here.
	TOOLBAR_SKIN_SIZE
};

// CamuleDlg Dialogfeld
class CamuleDlg : public wxFrame
{
public:
	CamuleDlg(wxWindow *pParent = NULL,
		const wxString &title = "",
		wxPoint where = wxDefaultPosition,
		wxSize dlg_size = wxSize(DEFAULT_SIZE_X, DEFAULT_SIZE_Y));
	~CamuleDlg();

	void AddLogLine(const wxString &line);
	void AddServerMessageLine(wxString &message);
	void ResetLog(int id);

	// Bracket a burst of AddLogLine() calls so the log view is repainted
	// and scrolled once for the whole batch instead of per line (issue
	// #445 — a remote-GUI first-sync backlog is thousands of lines).
	void BeginLogBatch();
	void EndLogBatch();

	void ShowUserCount(const wxString &info = "");
	void ShowConnectionState(bool skinChanged = false);
	void ShowTransferRate();

	bool StatisticsWindowActive() { return (m_activewnd == static_cast<wxWindow *>(m_statisticswnd)); }

	/* Returns the active dialog. Needed to check what to redraw. */
	enum DialogType
	{
		DT_TRANSFER_WND,
		DT_NETWORKS_WND,
		DT_SEARCH_WND,
		DT_SHARED_WND,
		DT_CHAT_WND,
		DT_STATS_WND,
		DT_KAD_WND // this one is still unused
	};
	DialogType GetActiveDialog() { return m_nActiveDialog; }
	void SetActiveDialog(DialogType type, wxWindow *dlg);

	/**
	 * Helper function for deciding if a certain dlg is visible.
	 *
	 * @return True if the dialog is visible to the user, false otherwise.
	 */
	bool IsDialogVisible(DialogType dlg)
	{
		return m_nActiveDialog == dlg && m_is_safe_state /* && !IsIconized() */;
	}

	void ShowED2KLinksHandler(bool show);

	void DlgShutDown();
	void OnClose(wxCloseEvent &evt);
	void OnBnConnect(wxCommandEvent &evt);

	bool SafeState() { return m_is_safe_state; }

	void LaunchUrl(const wxString &url);

	void CreateSystray();
	void RemoveSystray();

	void StartGuiTimer() { gui_timer->Start(100); }
	void StopGuiTimer() { gui_timer->Stop(); }

	/**
	 * This function ensures that _all_ list widgets are properly sorted.
	 */
	void InitSort();

	void SetMessageBlink(bool state) { m_BlinkMessages = state; }
	void Create_Toolbar(bool orientation);

	void DoNetworkRearrange();

	CIP2Country *m_IP2Country;
	void IP2CountryDownloadFinished(uint32 result);
	void EnableIP2Country();

	wxWindow *m_activewnd;
	CTransferWnd *m_transferwnd;
	CServerWnd *m_serverwnd;
	CSharedFilesWnd *m_sharedfileswnd;
	CSearchDlg *m_searchwnd;
	CChatWnd *m_chatwnd;
	CStatisticsDlg *m_statisticswnd;
	CKadDlg *m_kademliawnd;
	//! Pointer to the current preference dialog, if any.
	PrefsUnifiedDlg *m_prefsDialog;

	int m_srv_split_pos;

	// Last frame geometry seen while NOT iconized. SaveGUIPrefs uses
	// it as the fallback when the user exits from a minimized state
	// (otherwise the iconized GetPosition() returns sentinel values
	// like -32000,-32000 on Windows and the saved pos is unusable).
	wxPoint m_lastShownPos;
	wxSize m_lastShownSize;
	bool m_lastShownMaximized;
	bool m_lastShownValid;

	wxImageList m_imagelist;
	// Toolbar icons as resolution-aware bundles (the 32x32 art plus a
	// smooth 2x upscale). The executable is per-monitor-DPI aware, so a
	// plain 32px wxBitmap would be drawn at 32 *physical* pixels — tiny
	// and blurry on hi-DPI screens.
	std::vector<wxBitmapBundle> m_tblist;

protected:
	void OnToolBarButton(wxCommandEvent &ev);
	void OnAboutButton(wxCommandEvent &ev);
	void OnPrefButton(wxCommandEvent &ev);
	void OnImportButton(wxCommandEvent &ev);
	void OnMinimize(wxIconizeEvent &evt);
	void OnShow(wxShowEvent &evt);
	void OnBnClickedFast(wxCommandEvent &evt);
	void OnGUITimer(wxTimerEvent &evt);
	void OnMainGUISizeChange(wxSizeEvent &evt);
	void OnMainGUIMove(wxMoveEvent &evt);
	// Stash the current (non-iconized) pos / size / maximized state so
	// SaveGUIPrefs can fall back to the last good geometry if the user
	// exits from a minimized window.
	void CacheLastShownGeometry();
	void OnExit(wxCommandEvent &evt);

private:
	//! Specifies if the prefs-dialog was shown before minimizing.
	bool m_prefsVisible;
	wxToolBar *m_wndToolbar;
	wxTimer *gui_timer;
	CMuleTrayIcon *m_wndTaskbarNotifier;
	DialogType m_nActiveDialog;
	bool m_is_safe_state;
	bool m_BlinkMessages;
	//! True while a BeginLogBatch()/EndLogBatch() bracket is open, so
	//! AddLogLine() defers the per-line scroll to the end of the batch.
	bool m_logBatching = false;
	//! Last log-line weight applied to the view's default style (1 = bold,
	//! 0 = normal, -1 = not yet set) so SetDefaultStyle() is only touched
	//! when the weight actually changes.
	int m_logLastCritical = -1;
	int m_CurrentBlinkBitmap;
	uint32 m_last_iconizing;
	// The "new version available" popup is shown at most once per session;
	// the daily periodic re-check must not re-pop within the same run.
	bool m_versionPopupShown = false;

#if defined(ENABLE_VERSION_CHECK) && defined(CLIENT_GUI)
	// amulegui-only: it is not a CamuleApp and so has no core version-check
	// engine, so the remote GUI runs its own CVersionCheck. The monolithic
	// app instead drives the popup from the shared core engine via
	// Notify_VersionCheckResult -> ShowVersionAvailable(). Owned; created
	// lazily by StartupVersionCheck() when the preference is on. A periodic
	// re-check is fired from OnGUITimer.
	CVersionCheck *m_startupVersionCheck = nullptr;
	time_t m_lastGuiVersionCheck = 0;
	void StartupVersionCheck();
	void OnStartupVersionCheckDone(wxCommandEvent &evt);
#endif // ENABLE_VERSION_CHECK && CLIENT_GUI

public:
	// Show the "a new version is available" popup: at most once per session
	// (m_versionPopupShown), and never for a version the user muted via the
	// dialog's "Don't ask again" checkbox (recorded per-version in
	// last_version_notified, so a newer release still asks). Called from the
	// core engine (Notify_VersionCheckResult) in the monolithic app and from
	// OnStartupVersionCheckDone in amulegui.
	void ShowVersionAvailable(const wxString &latest);

	// Track iconize state from wxIconizeEvent::IsIconized(), which is
	// reliable across platforms — unlike wxFrame::IsIconized() which
	// can return false on wxGTK after a minimize-button click while
	// the OS still has the window iconized. Tray menu and DoShowHide
	// consult this to decide whether the window is "visible to the
	// user" so the "Show aMule"/"Hide aMule" label and the click
	// action stay in sync with reality.
	bool IsTrayLogicallyIconized() const { return m_iconized_logical; }

private:
	bool m_iconized_logical = false;
	wxFileName m_skinFileName;
	std::vector<wxString> m_clientSkinNames;
	bool m_GeoIPavailable;

	WX_DECLARE_STRING_HASH_MAP(wxZipEntry *, ZipCatalog);
	ZipCatalog cat;

	PageType m_logpages[4];
	PageType m_networkpages[2];

	bool LoadGUIPrefs(bool override_pos, bool override_size);
	bool SaveGUIPrefs();

	void UpdateTrayIcon(int percent);

	void Apply_Clients_Skin();
	void Apply_Toolbar_Skin(wxToolBar *wndToolbar);
	bool Check_and_Init_Skin();
	void Add_Skin_Icon(const wxString &iconName, const wxBitmap &stdIcon, bool useSkins);
	void ToogleED2KLinksHandler();
	void SetMessagesTool();
	void OnKeyPressed(wxKeyEvent &evt);

	wxDECLARE_EVENT_TABLE();
};

#endif

// File_checked_for_headers
