//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Original author: Emilio Sandoz
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

#include "PrefsUnifiedDlg.h"

#include <common/Constants.h>
#include <common/Macros.h> // Needed for itemsof()

#include <wx/bmpbndl.h> // wxBitmapBundle for DPI-aware page icons
#include <wx/colordlg.h>
#include <wx/combobox.h> // network-interface drop-down (bind-to-interface)
#include <wx/listctrl.h> // shared-folders editor (remote GUI)
#include <set>           // set-compare of shared roots (session refresh)
#include <wx/progdlg.h>
#include <wx/stdpaths.h>
#include <wx/tooltip.h>
#include <wx/utils.h> // wxGetUserHome

// Network-interface enumeration for the "Bind to interface" drop-down.
#ifdef __WINDOWS__
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <vector>
#else
#include <sys/types.h>
#include <ifaddrs.h>
#include <net/if.h>
#endif

#include "amule.h" // Needed for theApp
#include "amuleDlg.h"
#include "AutostartManager.h"       // Autostart-on-login toggle backend
#include "ProtocolHandlerManager.h" // ed2k:// + magnet: scheme-handler toggle backend
#ifdef GEOIP_GUI
#include "IP2Country.h"  // CIP2Country::Update / GetDatabasePath
#include <wx/artprov.h>  // wxArtProvider::GetBitmap for the IP2Country tab icon
#include <wx/filename.h> // wxFileName for status-line size lookup
#endif
#include "MuleColour.h"
#include "EditServerListDlg.h"
#include "SharedFileList.h" // Needed for CSharedFileList
#include "StatisticsDlg.h"  // Needed for graph parameters, colors
#include "IPFilter.h"       // Needed for CIPFilter
#include "ClientList.h"
#include "DirectoryTreeCtrl.h" // Needed for CDirectoryTreeCtrl
#include "Preferences.h"
#include "SharedDirsApplyTask.h" // Recursive-share expansion worker
#include "muuli_wdr.h"
#include "Logger.h"
#include "MediaProbe.h"    // Needed for the FFProbePath Detect button handler
#include <common/Format.h> // Needed for CFormat
#include "TransferWnd.h"   // Needed for CTransferWnd::UpdateCatTabTitles()
#include "KadDlg.h"        // Needed for CKadDlg
#include "OScopeCtrl.h"    // Needed for OScopeCtrl
#include "ServerList.h"
#include "Statistics.h"
#include "UserEvents.h"
#include "PlatformSpecific.h" // Needed for PLATFORMSPECIFIC_CAN_PREVENT_SLEEP_MODE

namespace
{
// Enumerate this machine's usable network interfaces for the "Bind to
// interface" drop-down. The strings returned are exactly what gets stored in
// the preference and later resolved to an index in LibSocketAsio.cpp: POSIX
// interface names (en0, eth0, tun0) and, on Windows, adapter friendly names
// (Ethernet, Wi-Fi). Loopback is skipped. The control stays editable, so an
// interface that is down right now (a VPN tunnel) can still be typed in.
wxArrayString DetectNetworkInterfaces()
{
	wxArrayString result;
#ifdef __WINDOWS__
	ULONG size = 15000;
	std::vector<uint8_t> buf(size);
	const ULONG flags = GAA_FLAG_SKIP_UNICAST | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
			    GAA_FLAG_SKIP_DNS_SERVER;
	PIP_ADAPTER_ADDRESSES aa = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(&buf[0]);
	ULONG ret = ::GetAdaptersAddresses(AF_UNSPEC, flags, NULL, aa, &size);
	if (ret == ERROR_BUFFER_OVERFLOW) {
		buf.resize(size);
		aa = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(&buf[0]);
		ret = ::GetAdaptersAddresses(AF_UNSPEC, flags, NULL, aa, &size);
	}
	if (ret == NO_ERROR) {
		for (PIP_ADAPTER_ADDRESSES p = aa; p != NULL; p = p->Next) {
			if (p->IfType == IF_TYPE_SOFTWARE_LOOPBACK || p->FriendlyName == NULL) {
				continue;
			}
			wxString name(p->FriendlyName);
			if (result.Index(name) == wxNOT_FOUND) {
				result.Add(name);
			}
		}
	}
#else
	struct ifaddrs *ifaces = NULL;
	if (getifaddrs(&ifaces) == 0) {
		for (struct ifaddrs *p = ifaces; p != NULL; p = p->ifa_next) {
			if (p->ifa_name == NULL || (p->ifa_flags & IFF_LOOPBACK)) {
				continue;
			}
			// getifaddrs lists one node per address family, so names repeat.
			wxString name = wxString::FromUTF8(p->ifa_name);
			if (result.Index(name) == wxNOT_FOUND) {
				result.Add(name);
			}
		}
		freeifaddrs(ifaces);
	}
#endif
	return result;
}
} // namespace

#ifdef CLIENT_GUI
namespace
{
// The open Preferences dialog, or NULL. Lets a GET_SHARED_DIRS reply repaint
// the editor without holding a pointer that could outlive the dialog.
PrefsUnifiedDlg *s_openPrefsDlg = nullptr;
} // namespace
#endif

wxBEGIN_EVENT_TABLE(PrefsUnifiedDlg, wxDialog)
// Events
#define USEREVENTS_EVENT(ID, NAME, VARS) \
	EVT_CHECKBOX(USEREVENTS_FIRST_ID + CUserEvents::ID * USEREVENTS_IDS_PER_EVENT + 1, \
		PrefsUnifiedDlg::OnCheckBoxChange) \
	EVT_CHECKBOX(USEREVENTS_FIRST_ID + CUserEvents::ID * USEREVENTS_IDS_PER_EVENT + 3, \
		PrefsUnifiedDlg::OnCheckBoxChange)
	USEREVENTS_EVENTLIST()
#undef USEREVENTS_EVENT

	// Proxy
	EVT_CHECKBOX(ID_PROXY_ENABLE_PROXY, PrefsUnifiedDlg::OnCheckBoxChange)
	EVT_CHECKBOX(ID_PROXY_ENABLE_PASSWORD, PrefsUnifiedDlg::OnCheckBoxChange)

	// Connection
	EVT_SPINCTRL(IDC_PORT, PrefsUnifiedDlg::OnTCPClientPortChange)

	// The rest. Organize it!
	EVT_CHECKBOX(IDC_UDPENABLE, PrefsUnifiedDlg::OnCheckBoxChange)
	EVT_CHECKBOX(IDC_CHECKDISKSPACE, PrefsUnifiedDlg::OnCheckBoxChange)
	EVT_CHECKBOX(IDC_ONLINESIG, PrefsUnifiedDlg::OnCheckBoxChange)
	EVT_CHECKBOX(IDC_REMOVEDEAD, PrefsUnifiedDlg::OnCheckBoxChange)
	EVT_CHECKBOX(IDC_AUTOSERVER, PrefsUnifiedDlg::OnCheckBoxChange)
	EVT_CHECKBOX(IDC_AUTOIPFILTER, PrefsUnifiedDlg::OnCheckBoxChange)
	EVT_CHECKBOX(IDC_MSGFILTER, PrefsUnifiedDlg::OnCheckBoxChange)
	EVT_CHECKBOX(IDC_MSGFILTER_ALL, PrefsUnifiedDlg::OnCheckBoxChange)
	EVT_CHECKBOX(IDC_MSGFILTER_WORD, PrefsUnifiedDlg::OnCheckBoxChange)
	EVT_CHECKBOX(IDC_FILTERCOMMENTS, PrefsUnifiedDlg::OnCheckBoxChange)
	EVT_CHECKBOX(IDC_STARTNEXTFILE, PrefsUnifiedDlg::OnCheckBoxChange)
	EVT_CHECKBOX(IDC_ENDGAME, PrefsUnifiedDlg::OnCheckBoxChange)
	EVT_CHECKBOX(IDC_ENABLETRAYICON, PrefsUnifiedDlg::OnCheckBoxChange)
	EVT_CHECKBOX(IDC_MACHIDEONCLOSE, PrefsUnifiedDlg::OnCheckBoxChange)
	EVT_CHECKBOX(IDC_VERTTOOLBAR, PrefsUnifiedDlg::OnCheckBoxChange)
	EVT_CHECKBOX(IDC_SUPPORT_PO, PrefsUnifiedDlg::OnCheckBoxChange)
	EVT_CHECKBOX(IDC_ENABLE_PO_OUTGOING, PrefsUnifiedDlg::OnCheckBoxChange)
	EVT_CHECKBOX(IDC_ENFORCE_PO_INCOMING, PrefsUnifiedDlg::OnCheckBoxChange)
	EVT_CHECKBOX(IDC_SHOWRATEONTITLE, PrefsUnifiedDlg::OnCheckBoxChange)
	EVT_CHECKBOX(IDC_NETWORKED2K, PrefsUnifiedDlg::OnCheckBoxChange)
	EVT_CHECKBOX(IDC_NETWORKKAD, PrefsUnifiedDlg::OnCheckBoxChange)
	EVT_CHECKBOX(IDC_UPNP_ENABLED, PrefsUnifiedDlg::OnCheckBoxChange)
	EVT_CHECKBOX(IDC_UPNP_WEBSERVER_ENABLED, PrefsUnifiedDlg::OnCheckBoxChange)
	EVT_CHECKBOX(IDC_MEDIAMETA_ENABLED, PrefsUnifiedDlg::OnCheckBoxChange)
	EVT_CHECKBOX(IDC_EXT_CONN_ACCEPT, PrefsUnifiedDlg::OnCheckBoxChange)
	EVT_CHECKBOX(IDC_ENABLE_WEB, PrefsUnifiedDlg::OnCheckBoxChange)
	EVT_CHECKBOX(IDC_ENABLE_AMULEAPI, PrefsUnifiedDlg::OnCheckBoxChange)

	// Autostart-on-login: state lives in the OS (registry / plist /
	// .desktop), not aMule.conf, so it gets its own handler that
	// writes immediately on toggle rather than waiting for OnOk.
	EVT_CHECKBOX(IDC_AUTOSTART_LOGIN, PrefsUnifiedDlg::OnAutostartToggle)

	// ed2k:// and magnet: URL-scheme handler toggles: same live-OS-
	// state model as autostart above, but with an "another app is
	// currently registered, overwrite?" confirm gate before Enable.
	EVT_CHECKBOX(IDC_PROTOCOL_ED2K, PrefsUnifiedDlg::OnProtocolEd2kToggle)
	EVT_CHECKBOX(IDC_PROTOCOL_MAGNET, PrefsUnifiedDlg::OnProtocolMagnetToggle)

	EVT_BUTTON(ID_PREFS_OK_TOP, PrefsUnifiedDlg::OnOk)
	EVT_BUTTON(ID_PREFS_CANCEL_TOP, PrefsUnifiedDlg::OnCancel)

	// Browse buttons
	//	EVT_BUTTON(IDC_SELSKIN,		PrefsUnifiedDlg::OnButtonDir)
	EVT_BUTTON(IDC_BROWSEV, PrefsUnifiedDlg::OnButtonBrowseApplication)
	EVT_BUTTON(IDC_SELTEMPDIR, PrefsUnifiedDlg::OnButtonDir)
	EVT_BUTTON(IDC_SELINCDIR, PrefsUnifiedDlg::OnButtonDir)
	EVT_BUTTON(IDC_SELOSDIR, PrefsUnifiedDlg::OnButtonDir)
#ifdef CLIENT_GUI
	EVT_BUTTON(IDC_SHAREDDIR_ADD, PrefsUnifiedDlg::OnSharedDirAdd)
	EVT_BUTTON(IDC_SHAREDDIR_REMOVE, PrefsUnifiedDlg::OnSharedDirRemove)
#endif
	EVT_BUTTON(IDC_SELBROWSER, PrefsUnifiedDlg::OnButtonBrowseApplication)
	EVT_BUTTON(IDC_MEDIAMETA_FFPROBEBROWSE, PrefsUnifiedDlg::OnButtonBrowseApplication)
	EVT_BUTTON(IDC_MEDIAMETA_FFPROBEDETECT, PrefsUnifiedDlg::OnButtonMediaMetaDetect)
	EVT_BUTTON(IDC_TWEAKS_RESET, PrefsUnifiedDlg::OnButtonTweaksReset)
#ifndef CLIENT_GUI
	EVT_BUTTON(IDC_EXCLUDE_SHARE_PREVIEW, PrefsUnifiedDlg::OnButtonExcludePreview)
#endif

	EVT_SPINCTRL(IDC_TOOLTIPDELAY, PrefsUnifiedDlg::OnToolTipDelayChange)

	EVT_BUTTON(IDC_EDITADR, PrefsUnifiedDlg::OnButtonEditAddr)
	EVT_BUTTON(IDC_IPFRELOAD, PrefsUnifiedDlg::OnButtonIPFilterReload)
	EVT_BUTTON(IDC_COLOR_BUTTON, PrefsUnifiedDlg::OnButtonColorChange)
	EVT_BUTTON(IDC_IPFILTERUPDATE, PrefsUnifiedDlg::OnButtonIPFilterUpdate)
#ifdef GEOIP_GUI
	EVT_CHOICE(IDC_GEOIP_SOURCE, PrefsUnifiedDlg::OnGeoIPSourceChange)
	EVT_BUTTON(IDC_GEOIP_UPDATE_NOW, PrefsUnifiedDlg::OnGeoIPUpdateNow)
	EVT_CHECKBOX(IDC_SHOW_COUNTRY_FLAGS, PrefsUnifiedDlg::OnGeoIPMasterToggle)
#endif
	EVT_CHOICE(IDC_COLORSELECTOR, PrefsUnifiedDlg::OnColorCategorySelected)
	EVT_LIST_ITEM_SELECTED(ID_PREFSLISTCTRL, PrefsUnifiedDlg::OnPrefsPageChange)

	EVT_INIT_DIALOG(PrefsUnifiedDlg::OnInitDialog)

	EVT_COMMAND_SCROLL(IDC_SLIDER, PrefsUnifiedDlg::OnScrollBarChange)
	EVT_COMMAND_SCROLL(IDC_SLIDER3, PrefsUnifiedDlg::OnScrollBarChange)
	EVT_COMMAND_SCROLL(IDC_SLIDER4, PrefsUnifiedDlg::OnScrollBarChange)
	EVT_COMMAND_SCROLL(IDC_SLIDER2, PrefsUnifiedDlg::OnScrollBarChange)
	EVT_COMMAND_SCROLL(IDC_FILEBUFFERSIZE, PrefsUnifiedDlg::OnScrollBarChange)
	EVT_COMMAND_SCROLL(IDC_QUEUESIZE, PrefsUnifiedDlg::OnScrollBarChange)
	EVT_COMMAND_SCROLL(IDC_SERVERKEEPALIVE, PrefsUnifiedDlg::OnScrollBarChange)

	EVT_SPINCTRL(IDC_MAXUP, PrefsUnifiedDlg::OnRateLimitChanged)

	EVT_LIST_ITEM_SELECTED(IDC_EVENTLIST, PrefsUnifiedDlg::OnUserEventSelected)

	EVT_CHOICE(IDC_LANGUAGE, PrefsUnifiedDlg::OnLanguageChoice)

	EVT_CLOSE(PrefsUnifiedDlg::OnClose)

wxEND_EVENT_TABLE()

/**
 * Creates an command-event for the given checkbox.
 *
 * This can be used enforce logical constraints by passing by
 * sending a check-box event for each checkbox, when transferring
 * to the UI. However, it should also be used for checkboxes that
 * have no side-effects other than enabling/disabling other
 * widgets in the preferences dialogs.
 */
static void SendCheckBoxEvent(wxWindow *parent, int id)
{
	wxCheckBox *widget = CastByID(id, parent, wxCheckBox);
	wxCHECK_RET(widget, "Invalid widget in CreateEvent");

	wxCommandEvent evt(wxEVT_CHECKBOX, id);
	evt.SetInt(widget->IsChecked() ? 1 : 0);

	parent->GetEventHandler()->ProcessEvent(evt);
}

/**
 * This struct provides a general way to represent config-tabs.
 */
struct PrefsPage
{
	//! The title of the page, used on the listctrl.
	wxString m_title;
	//! Function pointer to the wxDesigner function creating the dialog.
	wxSizer *(*m_function)(wxWindow *, bool, bool);
	//! The index of the image used on the list.
	int m_imageidx;
};

PrefsPage pages[] = { { wxTRANSLATE("General"), PreferencesGeneralTab, 13 },
	{ wxTRANSLATE("Connection"), PreferencesConnectionTab, 14 },
	{ wxTRANSLATE("Directories"), PreferencesDirectoriesTab, 17 },
	{ wxTRANSLATE("Servers"), PreferencesServerTab, 15 },
	{ wxTRANSLATE("Files"), PreferencesFilesTab, 16 },
	{ wxTRANSLATE("Security"), PreferencesSecurityTab, 22 },
	{ wxTRANSLATE("Interface"), PreferencesGuiTweaksTab, 19 },
#ifdef GEOIP_GUI
	// Inserted between Interface and Statistics so the GeoIP / country-flag
	// settings sit next to the related display option (the master
	// IDC_SHOW_COUNTRY_FLAGS checkbox lives in this new tab too). Hidden
	// from the page list when ENABLE_IP2COUNTRY is off so users who built
	// without libmaxminddb don't see a panel that can't function.
	{ wxTRANSLATE("IP2Country"), PreferencesIP2CountryTab, 13 },
#endif
	{ wxTRANSLATE("Statistics"), PreferencesStatisticsTab, 10 },
	{ wxTRANSLATE("Proxy"), PreferencesProxyTab, 24 },
	{ wxTRANSLATE("Filters"), PreferencesFilteringTab, 23 },
	{ wxTRANSLATE("Remote Controls"), PreferencesRemoteControlsTab, 11 },
	{ wxTRANSLATE("Online Signature"), PreferencesOnlineSigTab, 21 },
	{ wxTRANSLATE("Advanced"), PreferencesaMuleTweaksTab, 12 },
	{ wxTRANSLATE("Events"), PreferencesEventsTab, 5 }
#ifdef __DEBUG__
	,
	{ wxTRANSLATE("Debugging"), PreferencesDebug, 25 }
#endif
};

PrefsUnifiedDlg::PrefsUnifiedDlg(wxWindow *parent)
: wxDialog(parent,
	  -1,
	  _("Preferences"),
	  wxDefaultPosition,
	  wxDefaultSize,
	  wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
#ifdef GEOIP_GUI
	s_activeInstance = this;
#endif
#ifdef CLIENT_GUI
	s_openPrefsDlg = this;
	m_sharedDirsDirty = false;
#endif
	preferencesDlgTop(this, false);

	m_PrefsIcons = CastChild(ID_PREFSLISTCTRL, wxListCtrl);
	const int kPrefsIconW = 16;
	const int kPrefsIconH = 16;

	// The page art only exists at one (16x16) size. Wrap each icon in a
	// wxBitmapBundle with a smooth 2x upscale so DPI-aware builds render
	// it at the correct logical size on hi-DPI screens instead of a tiny
	// 16-physical-pixel square (same treatment as the main toolbar). The
	// mask is turned into an alpha channel first because high-quality
	// scaling needs it.
	auto makeIcon = [&](const wxBitmap &src) -> wxBitmapBundle {
		wxImage img = src.ConvertToImage();
		if (!img.HasAlpha()) {
			img.InitAlpha();
		}
		wxImage img2x = img.Scale(img.GetWidth() * 2, img.GetHeight() * 2, wxIMAGE_QUALITY_HIGH);
		return wxBitmapBundle::FromBitmaps(wxBitmap(img), wxBitmap(img2x));
	};

	// Add the single column used
	m_PrefsIcons->InsertColumn(0, "", wxLIST_FORMAT_LEFT, m_PrefsIcons->GetSize().GetWidth() - 5);

	// Temp variables for finding the smallest height and width needed
	int width = 0;
	int height = 0;

	// Build the page icons, in page order
	wxVector<wxBitmapBundle> iconBundles;
	for (unsigned int i = 0; i < itemsof(pages); ++i) {
		// The IP2Country tab uses an embedded-PNG icon shipped via
		// CamuleArtProvider (registered in CamuleGuiApp::OnInit) rather
		// than the hardcoded amuleSpecial XPM data the other tabs use.
		// Existing tabs are kept on amuleSpecial to avoid a wholesale
		// migration; new tabs should prefer the PNG path.
#ifdef GEOIP_GUI
		if (pages[i].m_function == PreferencesIP2CountryTab) {
			iconBundles.push_back(makeIcon(wxArtProvider::GetBitmap(
				"amule:prefs_ip2country", wxART_OTHER, wxSize(kPrefsIconW, kPrefsIconH))));
		} else
#endif
		{
			iconBundles.push_back(makeIcon(amuleSpecial(pages[i].m_imageidx)));
		}
	}
	m_PrefsIcons->SetSmallImages(iconBundles);

	// Add each page to the page-list
	for (unsigned int i = 0; i < itemsof(pages); ++i) {
		m_PrefsIcons->InsertItem(i, wxGetTranslation(pages[i].m_title), i);
	}

	// Set list-width so that there aren't any scrollers
	m_PrefsIcons->SetColumnWidth(0, wxLIST_AUTOSIZE);
	m_PrefsIcons->SetMinSize(wxSize(m_PrefsIcons->GetColumnWidth(0) + 10, -1));
	m_PrefsIcons->SetMaxSize(wxSize(m_PrefsIcons->GetColumnWidth(0) + 10, -1));

	// Now add the pages and calculate the minimum size
	wxPanel *DefaultWidget = NULL;
	for (unsigned int i = 0; i < itemsof(pages); ++i) {
		// Create a container widget and the contents of the page
		wxPanel *Widget = new wxPanel(this, -1);
		// Widget is stored as user data in the list control
		m_PrefsIcons->SetItemPtrData(i, (wxUIntPtr)Widget);
		pages[i].m_function(Widget, true, true);
		if (i == 0) {
			DefaultWidget = Widget;
		}

		// Add it to the sizer
		prefs_sizer->Add(Widget, wxSizerFlags().Expand().Expand());

		if (pages[i].m_function == PreferencesGeneralTab) {
// This must be done now or pages won't Fit();
#if defined(CLIENT_GUI)
			// Remote GUI: this checkbox toggles the *daemon's* version-check
			// preference, so its visibility follows the connected daemon's
			// capability, NOT amulegui's own build. An amulegui compiled
			// without ENABLE_VERSION_CHECK still shows it against a capable
			// daemon — it is only a remote editor of the daemon's pref. The
			// capability arrives via the EC tag on prefs-apply: a 3.1+ daemon
			// built without ENABLE_VERSION_CHECK reports false (hide); a
			// pre-3.1 daemon omits the tag and is treated as capable (show),
			// since it still supports the preference.
			if (!thePrefs::GetVersionCheckAvailable()) {
				if (wxWindow *vc = FindWindow(IDC_NEWVERSION)) {
					vc->Hide();
				}
			}
#elif !defined(ENABLE_VERSION_CHECK)
			// Monolithic built without the in-app check (OS-package build):
			// hide the now-dead "Check for new version at startup" checkbox.
			if (wxWindow *vc = FindWindow(IDC_NEWVERSION)) {
				vc->Hide();
			}
#endif
#ifdef __WINDOWS__
			CastChild(IDC_BROWSERTABS, wxCheckBox)->Enable(false);
#endif /* __WINDOWS__ */
			CastChild(IDC_PREVIEW_NOTE, wxStaticText)
				->SetLabel(_("The following variables will be substituted:\n    %PARTFILE - "
					     "full path to the file\n    %PARTNAME - file name only"));
			// Tray-icon checkboxes (IDC_ENABLETRAYICON,
			// IDC_MINTRAY) are visible on every platform now,
			// including macOS. wxTaskBarIcon → NSStatusItem on
			// Mac, NOTIFYICONDATA on Windows, GtkStatusIcon /
			// libayatana SNI on Linux. macOS users who prefer
			// the menu-bar status-item pattern (Spotify / Slack
			// / Discord style) can opt in.
#if defined(__WXGTK__) && !defined(WITH_LIBAYATANA_APPINDICATOR)
			// On Linux without libayatana-appindicator3 the only
			// backend wxTaskBarIcon can fall back to is the legacy
			// GtkStatusIcon API, which GNOME Shell dropped in 3.26
			// and wlroots-based compositors never implemented — the
			// tray icon is silently invisible. Disable the option
			// so users don't enable a feature that does nothing.
			// (CamuleApp::OnInit force-clears UseTrayIcon at startup
			// for the same reason, so dependent options cascade off
			// even before the user opens this panel.)
			FindWindow(IDC_ENABLETRAYICON)->Enable(false);
			FindWindow(IDC_ENABLETRAYICON)
				->SetToolTip(_("Tray icon support requires libayatana-appindicator3 at "
					       "compile time."));
#endif

#ifdef __WXGTK__
			// xdg-shell intentionally doesn't deliver iconified-state
			// notifications to clients, so the system minimize button
			// on Wayland cannot trigger our hide-to-tray path. Same
			// gap is documented across qBittorrent / Telegram /
			// KeePassXC / Slack — none of them have a fix either.
			// Grey out the option with a tooltip so the user
			// understands why; the runtime sanity check in
			// CamuleApp::OnInit keeps DoMinToTray() returning false
			// regardless of the saved value.
			if (CamuleAppCommon::IsWaylandSession()) {
				FindWindow(IDC_MINTRAY)
					->SetToolTip(_("Not available on Wayland: the protocol does not "
						       "report when a window is minimized, so this option "
						       "cannot intercept the system minimize button. "
						       "Workaround: launch aMule with `GDK_BACKEND=x11` "
						       "to use XWayland instead."));
			}
#endif
		} else if (pages[i].m_function == PreferencesEventsTab) {

#define USEREVENTS_REPLACE_VAR(VAR, DESC, CODE) +wxString("\n  %" VAR " - ") + wxGetTranslation(DESC)
#define USEREVENTS_EVENT(ID, NAME, VARS) \
	case CUserEvents::ID: \
		CreateEventPanels(idx, "" VARS, Widget); \
		break;

			wxListCtrl *list = CastChild(IDC_EVENTLIST, wxListCtrl);
			list->InsertColumn(0, "");
			for (unsigned int idx = 0; idx < CUserEvents::GetCount(); ++idx) {
				long lidx = list->InsertItem(idx,
					wxGetTranslation(CUserEvents::GetDisplayName(
						static_cast<enum CUserEvents::EventType>(idx))));
				if (lidx != -1) {
					list->SetItemData(
						lidx, USEREVENTS_FIRST_ID + idx * USEREVENTS_IDS_PER_EVENT);
					switch (idx) {
						USEREVENTS_EVENTLIST()
						/* This macro expands to handle all user event types. Here is
						   an example: case CUserEvents::NewChatSession: {
						       CreateEventPanels(idx, wxString("\n %SENDER - ") +
						   wxTRANSLATE("Message sender."), Widget); break;
						   } */
					}
				}
			}
			list->SetColumnWidth(0, wxLIST_AUTOSIZE);
		} else if (pages[i].m_function == PreferencesServerTab) {
			m_IndexServerTab = i;
			m_ServerWidget = Widget;
#ifdef GEOIP_GUI
		} else if (pages[i].m_function == PreferencesIP2CountryTab) {
			m_IndexIP2CountryTab = static_cast<int>(i);
#endif
		} else if (pages[i].m_function == PreferencesaMuleTweaksTab) {
			m_aMuleTweaksWidget = Widget;
			wxStaticText *txt = CastChild(IDC_AMULE_TWEAKS_WARNING, wxStaticText);
			// Do not wrap this line, Windows _() can't handle wrapped strings
			txt->SetLabel(_("Do not change these setting unless you know\nwhat you are doing, "
					"otherwise you can easily\nmake things worse for yourself.\n\naMule "
					"will run fine without adjusting any of\nthese settings."));
#if defined CLIENT_GUI || !PLATFORMSPECIFIC_CAN_PREVENT_SLEEP_MODE
			CastChild(IDC_PREVENT_SLEEP, wxCheckBox)->Enable(false);
			thePrefs::SetPreventSleepWhileDownloading(false);
#endif
		}
#ifdef __DEBUG__
		else if (pages[i].m_function == PreferencesDebug) {
			int count = theLogger.GetDebugCategoryCount();
			wxCheckListBox *list = CastChild(ID_DEBUGCATS, wxCheckListBox);

			for (int j = 0; j < count; j++) {
				list->Append(theLogger.GetDebugCategory(j).GetName());
			}
		}
#endif

		// Align and resize the page
		Fit();
		Layout();

		// Find the greatest sizes
		wxSize size = prefs_sizer->GetSize();
		if (size.GetWidth() > width) {
			width = size.GetWidth();
		}

		if (size.GetHeight() > height) {
			height = size.GetHeight();
		}

		// Hide it for now
		prefs_sizer->Detach(Widget);
		Widget->Show(false);
	}

	// Default to the General tab
	m_CurrentPanel = DefaultWidget;
	prefs_sizer->Add(DefaultWidget, wxSizerFlags().Expand().Expand());
	m_CurrentPanel->Show(true);

	// The reset button only applies to the Advanced page; hide it until that
	// page is selected (OnPrefsPageChange toggles it thereafter).
	if (wxButton *resetBtn = CastChild(IDC_TWEAKS_RESET, wxButton)) {
		resetBtn->Show(false);
	}

#ifdef CLIENT_GUI
	// The shared-file exclusion preview counts against the core's in-memory
	// shared list, which the remote GUI has no local access to (its handler
	// is compiled out too). Remove the button and its info label here; the
	// pattern/regex fields still work and sync to amuled over EC.
	if (wxWindow *previewBtn = FindWindow(IDC_EXCLUDE_SHARE_PREVIEW)) {
		previewBtn->Show(false);
	}
	if (wxWindow *previewInfo = FindWindow(IDC_EXCLUDE_SHARE_PREVIEW_INFO)) {
		previewInfo->Show(false);
	}
#endif

	// Select the first item
	m_PrefsIcons->SetItemState(0, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);

	// We now have the needed minimum height and width
	prefs_sizer->SetMinSize(width, height);

#ifdef CLIENT_GUI
	// amulegui: drop the IP2Country page from the menu when the connected core
	// has no GeoIP support — a 3.1+ core built without ENABLE_IP2COUNTRY, or a
	// pre-3.1 core that doesn't know the capability at all (both leave
	// IsGeoIPSupported() false, see CPreferencesRem::LoadRemote). Mirrors how
	// monolithic amule omits the page at compile time. Must run before
	// EnableServerTab, which may delete the earlier server tab and shift this
	// index; the page widget stays built but becomes unreachable.
	if (!thePrefs::IsGeoIPSupported() && m_IndexIP2CountryTab >= 0) {
		m_PrefsIcons->DeleteItem(m_IndexIP2CountryTab);
		m_IndexIP2CountryTab = -1;
	}
#endif

	// Don't show server prefs if ED2K is disabled
	m_ServerTabVisible = true;
	EnableServerTab(thePrefs::GetNetworkED2K());

	// Store some often used pointers
	m_ShareSelector = CastChild(IDC_SHARESELECTOR, CDirectoryTreeCtrl);
	m_buttonColor = CastChild(IDC_COLOR_BUTTON, wxButton);
	m_choiceColor = CastChild(IDC_COLORSELECTOR, wxChoice);

	// Fill the "Bind to interface" drop-down with the detected interfaces.
	// Done before the Cfg->widget transfer below so the stored value (which
	// may be an interface that is currently down) is preserved as typed text.
	// In CLIENT_GUI the control is a plain wxTextCtrl (the daemon's interfaces
	// are not this machine's), so there is nothing to enumerate.
#ifndef CLIENT_GUI
	if (wxComboBox *ifaceBox = CastChild(IDC_INTERFACE, wxComboBox)) {
		ifaceBox->Append(DetectNetworkInterfaces());
	}
	// Same for the EC-listener's own interface selector (Remote Controls tab).
	if (wxComboBox *ecIfaceBox = CastChild(IDC_EC_INTERFACE, wxComboBox)) {
		ecIfaceBox->Append(DetectNetworkInterfaces());
	}
#endif

	// Connect the Cfgs with their widgets
	thePrefs::CFGMap::iterator it = thePrefs::s_CfgList.begin();
	for (; it != thePrefs::s_CfgList.end(); ++it) {
		// Checking for failures
		if (!it->second->ConnectToWidget(it->first, this)) {
			AddLogLineNS(CFormat(_("Failed to connect Cfg to widget with the ID %d and key %s")) %
				     it->first % it->second->GetKey());
		}
	}
	Fit();

	// It must not be resized to something smaller than what it currently is
	wxSize size = GetClientSize();
	SetSizeHints(size.GetWidth(), size.GetHeight());

	// Position the dialog.
	Center();
}

void PrefsUnifiedDlg::EnableServerTab(bool enable)
{
	if (enable && !m_ServerTabVisible) {
		// turn server widget on
		m_PrefsIcons->InsertItem(m_IndexServerTab,
			wxGetTranslation(pages[m_IndexServerTab].m_title),
			m_IndexServerTab);
		m_PrefsIcons->SetItemPtrData(m_IndexServerTab, (wxUIntPtr)m_ServerWidget);
		m_ServerTabVisible = true;
	} else if (!enable && m_ServerTabVisible) {
		// turn server widget off
		m_PrefsIcons->DeleteItem(m_IndexServerTab);
		m_ServerTabVisible = false;
	}
}

Cfg_Base *PrefsUnifiedDlg::GetCfg(int id)
{
	thePrefs::CFGMap::iterator it = thePrefs::s_CfgList.find(id);

	if (it != thePrefs::s_CfgList.end()) {
		return it->second;
	}

	return NULL;
}

bool PrefsUnifiedDlg::TransferToWindow()
{
	// Connect the Cfgs with their widgets
	thePrefs::CFGMap::iterator it = thePrefs::s_CfgList.begin();
	for (; it != thePrefs::s_CfgList.end(); ++it) {
		// Checking for failures
		if (!it->second->TransferToWindow()) {
			AddLogLineNS(CFormat(_("Failed to transfer data from Cfg to Widget with the ID %d "
					       "and key %s")) %
				     it->first % it->second->GetKey());
		}
	}

	// Load the user's intent (explicit non-recursive vs marked-recursive
	// roots) into the tree control's two maps. shareddir_list itself
	// is the runtime expansion -- not useful as UI state since it
	// includes auto-discovered subdirs that shouldn't render as
	// user-selected.
#ifdef CLIENT_GUI
	// Remote GUI: no tree to seed. Show whatever roots we already hold and ask
	// the core for a fresh copy — the reply repaints us via
	// RefreshSharedDirsIfOpen, so an edit is never made against a stale list.
	PopulateSharedDirsList();
	static_cast<CPreferencesRem *>(theApp->glob_prefs)->LoadSharedDirsRemote();
#else
	m_ShareSelector->SetSharedDirectories(&theApp->glob_prefs->shareddir_explicit_list);
	m_ShareSelector->SetRecursiveSharedDirectories(&theApp->glob_prefs->shareddir_recursive_list);
#endif

	// Autostart checkbox: state lives in the OS, never aMule.conf, so
	// read live each time the dialog opens. The thePrefs Cfg machinery
	// above doesn't know about it.
	wxCheckBox *autostartCb = static_cast<wxCheckBox *>(FindWindow(IDC_AUTOSTART_LOGIN));
	if (autostartCb) {
		autostartCb->SetValue(AutostartManager::IsEnabled());
	}

	// URL-scheme handler checkboxes: same live-OS-state model.
	// On macOS, LaunchServices has no "clear default handler" call
	// (see ProtocolHandlerManager.cpp:BackendRemove), so Disable is a
	// no-op — toggling an already-checked box off doesn't change
	// anything the user can observe. Rather than expose a dead
	// control, hide the checkbox once aMule is the current handler:
	// the user opted in, we're the handler, there's nothing further
	// for them to do here. If they later switch away via macOS'
	// native "Change All…" chooser, the checkbox reappears on next
	// Preferences open with the "off" state, and they can opt back in.
	wxCheckBox *ed2kCb = static_cast<wxCheckBox *>(FindWindow(IDC_PROTOCOL_ED2K));
	if (ed2kCb) {
		bool enabled = ProtocolHandlerManager::IsEnabled(UriScheme::Ed2k);
		ed2kCb->SetValue(enabled);
#ifdef __WXMAC__
		ed2kCb->Show(!enabled);
#endif
	}
	wxCheckBox *magnetCb = static_cast<wxCheckBox *>(FindWindow(IDC_PROTOCOL_MAGNET));
	if (magnetCb) {
		bool enabled = ProtocolHandlerManager::IsEnabled(UriScheme::Magnet);
		magnetCb->SetValue(enabled);
#ifdef __WXMAC__
		magnetCb->Show(!enabled);
#endif
	}

#ifdef GEOIP_GUI
	// Sync the GeoIP source dropdown to the persisted source; the
	// Cfg_ system above handles the credential / URL / auto-update
	// fields, but the dropdown is driven through a custom handler so
	// hide/show of the source sub-panels stays consistent. Also
	// refresh the status block from the live CIP2Country state.
	wxChoice *geoipSource = CastChild(IDC_GEOIP_SOURCE, wxChoice);
	if (geoipSource) {
		geoipSource->SetSelection(static_cast<int>(thePrefs::GetGeoIPSource()));
		UpdateGeoIPSourcePanel();
		UpdateGeoIPStatus();
		UpdateGeoIPControlsEnabled();
	}
	// Snapshot the source + credential values that aren't tracked
	// by the Cfg system, so OnOk can tell whether anything
	// download-affecting changed during the dialog session.
	m_GeoIPSourceAtOpen = static_cast<int>(thePrefs::GetGeoIPSource());
	m_GeoIPMaxMindLicenseAtOpen = thePrefs::GetGeoIPMaxMindLicense();
	m_GeoIPCustomUrlAtOpen = thePrefs::GetGeoIPCustomUrl();
#endif

	for (int i = 0; i < cntStatColors; i++) {
		thePrefs::s_colors[i] = CMuleColour(CStatisticsDlg::acrStat[i]).GetULong();
		thePrefs::s_colors_ref[i] = CMuleColour(CStatisticsDlg::acrStat[i]).GetULong();
	}

	// Connection tab
	wxSpinEvent e;
	OnTCPClientPortChange(e);

	// Proxy tab initialization
	FindWindow(ID_PROXY_TYPE)->SetToolTip(_("The type of proxy you are connecting to"));
	if (!CastChild(ID_PROXY_ENABLE_PROXY, wxCheckBox)->IsChecked()) {
		FindWindow(ID_PROXY_TYPE)->Enable(false);
		FindWindow(ID_PROXY_NAME)->Enable(false);
		FindWindow(ID_PROXY_PORT)->Enable(false);
	}
	if (!CastChild(ID_PROXY_ENABLE_PASSWORD, wxCheckBox)->IsChecked()) {
		FindWindow(ID_PROXY_USER)->Enable(false);
		FindWindow(ID_PROXY_PASSWORD)->Enable(false);
	}

	// Enable/Disable some controls
	FindWindow(IDC_MINDISKSPACE)->Enable(thePrefs::IsCheckDiskspaceEnabled());
	FindWindow(IDC_OSDIR)->Enable(thePrefs::IsOnlineSignatureEnabled());
	FindWindow(IDC_OSUPDATE)->Enable(thePrefs::IsOnlineSignatureEnabled());
	FindWindow(IDC_UDPENABLE)->Enable(!thePrefs::GetNetworkKademlia());
	FindWindow(IDC_UDPPORT)->Enable(thePrefs::s_UDPEnable);
	FindWindow(IDC_SERVERRETRIES)->Enable(thePrefs::DeadServer());
	FindWindow(IDC_STARTNEXTFILE_SAME)->Enable(thePrefs::StartNextFile());
	FindWindow(IDC_STARTNEXTFILE_ALPHA)->Enable(thePrefs::StartNextFile());

	// Gate the ffprobe path controls on the master Media metadata toggle
	// so a disabled feature doesn't show a live-looking Detect / Browse
	// UI that silently does nothing.
	{
		const bool mmOn = thePrefs::GetMediaMetadataEnabled();
		FindWindow(IDC_MEDIAMETA_FFPROBEPATHTEXT)->Enable(mmOn);
		FindWindow(IDC_MEDIAMETA_FFPROBEPATH)->Enable(mmOn);
		FindWindow(IDC_MEDIAMETA_FFPROBEBROWSE)->Enable(mmOn);
		FindWindow(IDC_MEDIAMETA_FFPROBEDETECT)->Enable(mmOn);
	}

	// The tray icon is the only recovery surface for a window hidden
	// via the close button: on Linux/Windows the option needs the tray
	// to bring the window back, and on macOS the matching code path
	// (NSApplicationActivationPolicyAccessory) drops the Dock icon
	// while hidden, so the tray is also the only way back there.
	FindWindow(IDC_MACHIDEONCLOSE)->Enable(thePrefs::UseTrayIcon());

#ifdef __WXGTK__
	const bool minTrayUsable = thePrefs::UseTrayIcon() && !CamuleAppCommon::IsWaylandSession();
#else
	const bool minTrayUsable = thePrefs::UseTrayIcon();
#endif
	FindWindow(IDC_MINTRAY)->Enable(minTrayUsable);

	if (!CastChild(IDC_MSGFILTER, wxCheckBox)->IsChecked()) {
		FindWindow(IDC_MSGFILTER_ALL)->Enable(false);
		FindWindow(IDC_MSGFILTER_NONSECURE)->Enable(false);
		FindWindow(IDC_MSGFILTER_NONFRIENDS)->Enable(false);
		FindWindow(IDC_MSGFILTER_WORD)->Enable(false);
		FindWindow(IDC_MSGWORD)->Enable(false);
	} else if (CastChild(IDC_MSGFILTER_ALL, wxCheckBox)->IsChecked()) {
		FindWindow(IDC_MSGFILTER_NONSECURE)->Enable(false);
		FindWindow(IDC_MSGFILTER_NONFRIENDS)->Enable(false);
		FindWindow(IDC_MSGFILTER_WORD)->Enable(false);
		FindWindow(IDC_MSGWORD)->Enable(false);
	}

	FindWindow(IDC_MSGWORD)->Enable(CastChild(IDC_MSGFILTER_WORD, wxCheckBox)->IsChecked());
	FindWindow(IDC_COMMENTWORD)->Enable(CastChild(IDC_FILTERCOMMENTS, wxCheckBox)->IsChecked());

#ifdef CLIENT_GUI
	// Disable dirpickers unless it's a localhost connection
	if (!theApp->m_connect->IsConnectedToLocalHost()) {
		FindWindow(IDC_SELINCDIR)->Enable(false);
		FindWindow(IDC_SELTEMPDIR)->Enable(false);
	}

	// Hide preferences that are persisted only to amulegui's local
	// remote.conf but never sent to amuled via EC_OP_SET_PREFERENCES
	// (i.e. not packed by CEC_Prefs_Packet at all). The widget would
	// otherwise show amulegui's stale local default -- not amuled's
	// real value -- and editing it would silently affect nothing on
	// the daemon side. Same gap holds whether amulegui is on a
	// loopback or remote connection: amuled never reads remote.conf,
	// so the control is dead in both cases. Hide unconditionally for
	// CLIENT_GUI. Proxy settings (ID_PROXY_*) are *not* in this list.
	// They are packed by CEC_Prefs_Packet (EC_TAG_PROXY_*), so the remote
	// GUI configures the daemon's proxy -- amuled routes its P2P and HTTP
	// (server list, nodes.dat, GeoIP, version check) through it. amulegui
	// also uses the value locally for its own version-check HTTP (the
	// shared curl session in CVersionCheck). GeoIP, by contrast, is
	// daemon-only -- amulegui has no local resolver, country codes arrive
	// over EC -- so this control is meaningful either way.
	const int amuledOnlyPrefs[] = {
		// Web-server UPnP (amuleweb is deprecated) and EC-port UPnP (the EC
		// port is not a P2P port) stay hidden; only the P2P-router UPnP is
		// wired over EC, and it is capability-gated in the UPNP block below.
		IDC_UPNP_WEBSERVER_ENABLED,
		IDC_WEBUPNPTCPPORT,
		IDC_WEBUPNPTCPPORTTEXT,
		IDC_UPNP_EC_ENABLED,
		// Online-signature "Browse" is a local file picker; the directory field
		// + update frequency beside it are EC-wired and stay visible.
		IDC_SELOSDIR,
		IDC_EXT_CONN_PARAMS_BOX,
		IDC_EXT_CONN_ACCEPT,
		IDC_EXT_CONN_IP,
		IDC_EXT_CONN_IPTEXT,
		IDC_EC_INTERFACE,
		IDC_EC_INTERFACETEXT,
		IDC_EXT_CONN_TCP_PORT,
		IDC_EXT_CONN_TCPPORTTEXT,
		IDC_EXT_CONN_PASSWD,
		IDC_EXT_CONN_PASSWDTEXT,
		// ffprobe "Browse" (local file picker) and "Detect" (auto-detects on the
		// GUI host, not the daemon) cannot target the daemon filesystem. The
		// enable toggle + path field are EC-wired and stay visible.
		IDC_MEDIAMETA_FFPROBEBROWSE,
		IDC_MEDIAMETA_FFPROBEDETECT,
	};
	for (int id : amuledOnlyPrefs) {
		if (wxWindow *w = FindWindow(id)) {
			w->Hide();
		}
	}
#endif

	// Protocol obfuscation
	::SendCheckBoxEvent(this, IDC_SUPPORT_PO);
	::SendCheckBoxEvent(this, IDC_ENABLE_PO_OUTGOING);
	::SendCheckBoxEvent(this, IDC_ENFORCE_PO_INCOMING);

#ifndef GEOIP_GUI
	// The country-flags checkbox + the rest of the IP2Country controls
	// only live in the dedicated PreferencesIP2CountryTab, which is
	// `#ifdef GEOIP_GUI`-gated in the pages[] table. With
	// libmaxminddb missing, neither the tab nor any of its widgets
	// exists, so there's nothing to disable here -- the *.NewCfgItem
	// bindings below are gated the same way, and SetGeoIPEnabled stays
	// at its default false.
#endif

#ifdef __GIT__
	// Version is always shown on the title in development versions
	CastChild(IDC_SHOWVERSIONONTITLE, wxCheckBox)->SetValue(true);
	CastChild(IDC_SHOWVERSIONONTITLE, wxCheckBox)->Enable(false);
#endif

	// Show rates on title
	FindWindow(IDC_RATESBEFORETITLE)->Enable(thePrefs::GetShowRatesOnTitle() != 0);
	FindWindow(IDC_RATESAFTERTITLE)->Enable(thePrefs::GetShowRatesOnTitle() != 0);
	CastChild(IDC_SHOWRATEONTITLE, wxCheckBox)->SetValue(thePrefs::GetShowRatesOnTitle() != 0);
	CastChild(IDC_RATESBEFORETITLE, wxRadioButton)->SetValue(thePrefs::GetShowRatesOnTitle() == 2);
	CastChild(IDC_RATESAFTERTITLE, wxRadioButton)->SetValue(thePrefs::GetShowRatesOnTitle() != 2);

	// UPNP
#ifdef CLIENT_GUI
	// Gate the P2P-router UPnP controls on the daemon's advertised capability
	// (EC_TAG_GENERAL_UPNP_AVAILABLE), not amulegui's own ENABLE_UPNP -- the
	// daemon is what forwards. Do NOT clobber the pref when unavailable (the
	// value belongs to the daemon). Web-server + EC-port UPnP are hidden.
	if (thePrefs::GetUPnPAvailable()) {
		FindWindow(IDC_UPNPTCPPORT)->Enable(thePrefs::GetUPnPEnabled());
		FindWindow(IDC_UPNPTCPPORTTEXT)->Enable(thePrefs::GetUPnPEnabled());
	} else {
		FindWindow(IDC_UPNP_ENABLED)->Enable(false);
		FindWindow(IDC_UPNPTCPPORT)->Enable(false);
		FindWindow(IDC_UPNPTCPPORTTEXT)->Enable(false);
	}
#else
#ifndef ENABLE_UPNP
	FindWindow(IDC_UPNP_ENABLED)->Enable(false);
	FindWindow(IDC_UPNPTCPPORT)->Enable(false);
	FindWindow(IDC_UPNPTCPPORTTEXT)->Enable(false);
	thePrefs::SetUPnPEnabled(false);
	FindWindow(IDC_UPNP_WEBSERVER_ENABLED)->Enable(false);
	FindWindow(IDC_WEBUPNPTCPPORT)->Enable(false);
	FindWindow(IDC_WEBUPNPTCPPORTTEXT)->Enable(false);
	thePrefs::SetUPnPWebServerEnabled(false);
	FindWindow(IDC_UPNP_EC_ENABLED)->Enable(false);
	thePrefs::SetUPnPECEnabled(false);
#else
	FindWindow(IDC_UPNPTCPPORT)->Enable(thePrefs::GetUPnPEnabled());
	FindWindow(IDC_UPNPTCPPORTTEXT)->Enable(thePrefs::GetUPnPEnabled());
	FindWindow(IDC_WEBUPNPTCPPORT)->Enable(thePrefs::GetUPnPWebServerEnabled());
	FindWindow(IDC_WEBUPNPTCPPORTTEXT)->Enable(thePrefs::GetUPnPWebServerEnabled());
#endif
#endif

#ifdef __DEBUG__
	// Set debugging toggles
	int count = theLogger.GetDebugCategoryCount();
	wxCheckListBox *list = CastChild(ID_DEBUGCATS, wxCheckListBox);

	for (int i = 0; i < count; i++) {
		list->Check(i, theLogger.GetDebugCategory(i).IsEnabled());
	}
#endif

	m_verticalToolbar = thePrefs::VerticalToolbar();
	m_toolbarOrientationChanged = false;

	return true;
}

bool PrefsUnifiedDlg::TransferFromWindow()
{
	// Connect the Cfgs with their widgets
	thePrefs::CFGMap::iterator it = thePrefs::s_CfgList.begin();
	for (; it != thePrefs::s_CfgList.end(); ++it) {
		// Checking for failures
		if (!it->second->TransferFromWindow()) {
			AddLogLineNS(CFormat(_("Failed to transfer data from Widget to Cfg with the ID %d "
					       "and key %s")) %
				     it->first % it->second->GetKey());
		}
	}

	// shareddir_list is committed separately from OnOk (see
	// CommitSharedDirsWithProgress) so that recursive-share expansion
	// can run on a worker thread with a progress dialog and a cancel
	// button. Doing it eagerly here would re-introduce the multi-
	// minute UI freeze that issue #592 hit on /home-sized roots.

	for (int i = 0; i < cntStatColors; i++) {
		if (thePrefs::s_colors[i] != thePrefs::s_colors_ref[i]) {
			CStatisticsDlg::acrStat[i] = thePrefs::s_colors[i];
			theApp->amuledlg->m_statisticswnd->ApplyStatsColor(i);
		}

		theApp->amuledlg->m_kademliawnd->SetGraphColors();
	}

#ifdef __DEBUG__
	// Get debugging toggles
	int count = theLogger.GetDebugCategoryCount();
	wxCheckListBox *list = CastChild(ID_DEBUGCATS, wxCheckListBox);

	for (int i = 0; i < count; i++) {
		theLogger.SetEnabled(theLogger.GetDebugCategory(i).GetType(), list->IsChecked(i));
	}
#endif

	thePrefs::SetShowRatesOnTitle(
		CastChild(IDC_SHOWRATEONTITLE, wxCheckBox)->GetValue()
			? (CastChild(IDC_RATESBEFORETITLE, wxRadioButton)->GetValue() ? 2 : 1)
			: 0);

#ifdef CLIENT_GUI
#ifdef GEOIP_GUI
	// Parity with monolithic's auto-download-on-OK: if the GeoIP source or the
	// active source's credential changed since the panel opened, ask the daemon
	// to re-download from the new source by piggy-backing a one-shot UPDATE_NOW
	// on the prefs packet. Commit the credential fields to the statics first —
	// SendToRemote serialises from there, not from the live widgets.
	thePrefs::SetGeoIPMaxMindLicense(CastChild(IDC_GEOIP_MAXMIND_LIC, wxTextCtrl)->GetValue());
	thePrefs::SetGeoIPCustomUrl(CastChild(IDC_GEOIP_CUSTOM_URL, wxTextCtrl)->GetValue());
	const bool geoipSourceChanged = static_cast<int>(thePrefs::GetGeoIPSource()) != m_GeoIPSourceAtOpen;
	bool geoipCredChanged = false;
	switch (thePrefs::GetGeoIPSource()) {
	case CPreferences::GeoIPSourceMaxMind:
		geoipCredChanged = thePrefs::GetGeoIPMaxMindLicense() != m_GeoIPMaxMindLicenseAtOpen;
		break;
	case CPreferences::GeoIPSourceCustom:
		geoipCredChanged = thePrefs::GetGeoIPCustomUrl() != m_GeoIPCustomUrlAtOpen;
		break;
	default:
		break;
	}
	thePrefs::SetGeoIPUpdateRequested(
		thePrefs::IsGeoIPEnabled() && (geoipSourceChanged || geoipCredChanged));
#endif
	// Send preferences to core.
	theApp->glob_prefs->SendToRemote();
#ifdef GEOIP_GUI
	thePrefs::SetGeoIPUpdateRequested(false);
#endif
#endif

	return true;
}

bool PrefsUnifiedDlg::CfgChanged(int ID)
{
	Cfg_Base *cfg = GetCfg(ID);

	if (cfg) {
		return cfg->HasChanged();
	}

	return false;
}

void PrefsUnifiedDlg::OnOk(wxCommandEvent &WXUNUSED(event))
{
	TransferFromWindow();

	// Commit the share list with the recursive-expand-on-worker-
	// thread path. Done after TransferFromWindow (so other prefs
	// are already populated in glob_prefs) but before Save() so a
	// successful commit ends up in shareddir.dat alongside the rest.
	// If the user cancels at the confirm or the progress dialog,
	// bail out of OnOk *before* anything is persisted — so the prefs
	// dialog stays open and the user can adjust their selection
	// without losing the rest of their pending pref changes.
	const SharedDirsCommitResult shareResult = CommitSharedDirsWithProgress();
	if (shareResult == SharedDirsCommitResult::CancelledByUser) {
		return;
	}
	const bool sharedDirsCommitted = (shareResult == SharedDirsCommitResult::Committed);

	bool restart_needed = false;
	wxString restart_needed_msg = _("aMule must be restarted to enable these changes:\n\n");

	// do sanity checking, special processing, and user notifications here
	thePrefs::CheckUlDlRatio();

	if (CfgChanged(IDC_PORT)) {
		restart_needed = true;
		restart_needed_msg += _("- TCP port changed.\n");
	}

	if (CfgChanged(IDC_UDPPORT)) {
		restart_needed = true;
		restart_needed_msg += _("- UDP port changed.\n");
	}

	if (CfgChanged(IDC_INTERFACE)) {
		restart_needed = true;
		restart_needed_msg += _("- Network interface binding changed.\n");
	}

	if (CfgChanged(IDC_EXT_CONN_TCP_PORT)) {
		restart_needed = true;
		restart_needed_msg += _("- External connect port changed.\n");
	}
	if (CfgChanged(IDC_EXT_CONN_ACCEPT)) {
		restart_needed = true;
		restart_needed_msg += _("- External connect acceptance changed.\n");
	}
	if (CfgChanged(IDC_EXT_CONN_IP) || CfgChanged(IDC_EC_INTERFACE)) {
		restart_needed = true;
		restart_needed_msg += _("- External connect interface changed.\n");
	}
	if (CfgChanged(IDC_SUPPORT_PO)) {
		restart_needed = true;
		restart_needed_msg += _("- Protocol obfuscation support changed.\n");
	}

	// amuleapi is launched once at startup with its bind/port/password, so
	// any change to those takes effect only after aMule relaunches it. Skip
	// the prompt when external connections won't be usable (off, or on but
	// without a password — the guard further down then disables amuleapi
	// anyway), so we don't tell the user to restart for a service that will
	// not run.
	const bool ecUsable = thePrefs::AcceptExternalConnections() && !thePrefs::ECPassword().IsEmpty();
	if (ecUsable && (CfgChanged(IDC_ENABLE_AMULEAPI) || CfgChanged(IDC_AMULEAPI_PORT) ||
				CfgChanged(IDC_AMULEAPI_BIND) || CfgChanged(IDC_AMULEAPI_PASSWD))) {
		restart_needed = true;
		restart_needed_msg += _("- amuleapi settings changed.\n");
	}

	// Force port checking
	thePrefs::SetPort(thePrefs::GetPort());

	if ((CPath::GetFileSize(thePrefs::GetConfigDir() + "addresses.dat") == 0) &&
		CastChild(IDC_AUTOSERVER, wxCheckBox)->IsChecked()) {
		thePrefs::UnsetAutoServerStart();
		wxMessageBox(_("Your Auto-update server list is empty.\n'Auto-update server list at startup' "
			       "will be disabled."),
			_("Message"),
			wxOK | wxICON_INFORMATION,
			this);
	}

	if (thePrefs::AcceptExternalConnections() && thePrefs::ECPassword().IsEmpty()) {
		thePrefs::EnableExternalConnections(false);

		wxMessageBox(_(
			"You have enabled external connections but have not specified a password.\nExternal "
			"connections cannot be enabled unless a valid password is specified."));
	}

#ifndef CLIENT_GUI
	// The web server and amuleapi are EC clients of the core; without external
	// connections (which the check above may itself have just turned off for a
	// missing password) they can never connect. OnCheckBoxChange already warns
	// live and reverts the toggles, so this is a silent backstop that keeps the
	// saved prefs consistent for a config loaded in a mismatched state.
	//
	// amulegui (CLIENT_GUI) is itself a connected EC client, so external
	// connections are necessarily enabled on the daemon; the local
	// AcceptExternalConnections pref only mirrors amulegui's own remote.conf and
	// never the daemon's real state, so this backstop must not run there.
	if ((thePrefs::GetWSIsEnabled() || thePrefs::GetAmuleApiIsEnabled()) &&
		!thePrefs::AcceptExternalConnections()) {
		thePrefs::SetWSIsEnabled(false);
		thePrefs::SetAmuleApiIsEnabled(false);
	}
#endif

	// save the preferences on ok
	theApp->glob_prefs->Save();

	if (CfgChanged(IDC_FED2KLH) && theApp->amuledlg->GetActiveDialog() != CamuleDlg::DT_SEARCH_WND) {
		theApp->amuledlg->ShowED2KLinksHandler(thePrefs::GetFED2KLH());
	}

	if (CfgChanged(IDC_LANGUAGE)) {
		restart_needed = true;
		restart_needed_msg += _("- Language changed.\n");
	}

	if (CfgChanged(IDC_TEMPFILES)) {
		restart_needed = true;
		restart_needed_msg += _("- Temp folder changed.\n");
	}

	if (CfgChanged(IDC_NETWORKED2K) && thePrefs::GetNetworkED2K()) {
		restart_needed = true;
		restart_needed_msg += _("- ED2K network enabled.\n");
	}

	// CommitSharedDirsWithProgress already ran Reload (with progress
	// + cancel) when shareddir_list itself changed. We only need to
	// trigger a fresh Reload here for the other paths IDC_INCFILES /
	// IDC_TEMPFILES affect.
	if (!sharedDirsCommitted && (CfgChanged(IDC_INCFILES) || CfgChanged(IDC_TEMPFILES))) {
		theApp->sharedfiles->Reload();
	}

	if (CfgChanged(IDC_AUTO_RESCAN_SHARED)) {
		// Start or stop the fs-watcher immediately so the user sees the
		// effect of toggling without needing to restart amuled.
		theApp->sharedfiles->EnableDirectoryWatcher(thePrefs::AutoRescanSharedDirs());
	}

	if (CfgChanged(IDC_FOLLOW_SYMLINKS_SHARED) && !sharedDirsCommitted) {
		// Re-scan so the new symlink policy takes effect on the existing
		// shared tree: turning the toggle off should drop symlinked
		// entries already in the shareset, turning it on should pick
		// them up.
		theApp->sharedfiles->Reload();
	}

	if (CfgChanged(IDC_EXCLUDE_SHARE_PATTERNS) || CfgChanged(IDC_EXCLUDE_SHARE_REGEX)) {
		// Reject-on-apply: in regex mode an invalid expression leaves the
		// filter disabled (fail-open, never exclude-all), so tell the user
		// rather than silently sharing everything.
		if (thePrefs::ExcludeSharePatternsUseRegex()) {
			CShareExcludeFilter probe;
			probe.Compile(thePrefs::GetExcludeSharePatterns(), true);
			if (!probe.IsValid()) {
				wxMessageBox(_("The shared-file exclusion pattern is not a valid "
					       "regular expression. The filter has been left disabled."),
					_("Invalid regular expression"),
					wxOK | wxICON_WARNING,
					this);
			}
		}
		thePrefs::RecompileShareExcludeFilter();
		// Re-scan so newly excluded files leave the shareset and files no
		// longer matching return.
		if (!sharedDirsCommitted) {
			theApp->sharedfiles->Reload();
		}
	}

	if (CfgChanged(IDC_OSDIR) || CfgChanged(IDC_ONLINESIG)) {
		wxTextCtrl *widget = CastChild(IDC_OSDIR, wxTextCtrl);

		// Build the filenames for the two OS files
		theApp->SetOSFiles(widget->GetValue());
	}

	if (CfgChanged(IDC_IPFCLIENTS) && thePrefs::IsFilteringClients()) {
		theApp->clientlist->FilterQueues();
	}

	if (CfgChanged(IDC_IPFSERVERS) && thePrefs::IsFilteringServers()) {
		theApp->serverlist->FilterServers();
	}

	if (CfgChanged(ID_IPFILTERLEVEL)) {
		theApp->ipfilter->Reload();
	}

	theApp->ResetTitle();

	if (thePrefs::GetShowRatesOnTitle()) {
		// This avoids a 5 seconds delay to show the title
		theApp->amuledlg->ShowTransferRate();
	} else {
		// This resets the title
		theApp->amuledlg->SetTitle(theApp->m_FrameTitle);
	}

	if (CfgChanged(IDC_EXTCATINFO)) {
		theApp->amuledlg->m_transferwnd->UpdateCatTabTitles();
	}

	// Changes related to the statistics-dlg
	if (CfgChanged(IDC_SLIDER)) {
		theApp->amuledlg->m_statisticswnd->SetUpdatePeriod(thePrefs::GetTrafficOMeterInterval());
		theApp->amuledlg->m_kademliawnd->SetUpdatePeriod(thePrefs::GetTrafficOMeterInterval());
	}

	if (CfgChanged(IDC_SLIDER3)) {
		theApp->amuledlg->m_statisticswnd->ResetAveragingTime();
	}

	if (CfgChanged(IDC_DOWNLOAD_CAP)) {
		theApp->amuledlg->m_statisticswnd->SetARange(true, thePrefs::GetMaxGraphDownloadRate());
	}

	if (CfgChanged(IDC_UPLOAD_CAP)) {
		theApp->amuledlg->m_statisticswnd->SetARange(false, thePrefs::GetMaxGraphUploadRate());
	}

	if (CfgChanged(IDC_SKIN)) {
		theApp->amuledlg->Create_Toolbar(thePrefs::VerticalToolbar());
	}

	if (!thePrefs::GetNetworkED2K() && theApp->IsConnectedED2K()) {
		theApp->DisconnectED2K();
	}

	if (!thePrefs::GetNetworkKademlia() && theApp->IsConnectedKad()) {
		theApp->StopKad();
	}

	if (!thePrefs::GetNetworkED2K() && !thePrefs::GetNetworkKademlia()) {
		wxMessageBox(_("Both eD2k and Kad network are disabled.\nYou won't be able to connect until "
			       "you enable at least one of them."));
	}

	if (thePrefs::GetNetworkKademlia() && thePrefs::IsUDPDisabled()) {
		wxMessageBox(_("Kad will not start if your UDP port is disabled.\nEnable UDP port or disable "
			       "Kad."),
			_("Message"),
			wxOK | wxICON_INFORMATION,
			this);
	}

	if (CfgChanged(IDC_NETWORKKAD) || CfgChanged(IDC_NETWORKED2K)) {
		theApp->amuledlg->DoNetworkRearrange();
	}

	if (CfgChanged(IDC_SHOW_COUNTRY_FLAGS)) {
		// Local enable/disable toggle — treat as startup so enabling refreshes.
		theApp->EnableIP2Country(true);
	}

#if defined(ENABLE_IP2COUNTRY) && !defined(CLIENT_GUI)
	// Auto-download on OK when the user changed anything that affects
	// *which* file should be on disk. Without this, switching source
	// DB-IP→MaxMind (or pasting a new license) leaves the old file
	// loaded until the user remembers to click Update now. We skip
	// the download if:
	//   * IP2Country is disabled (the file is irrelevant), or
	//   * the user just toggled the master enable on — EnableIP2Country
	//     above already handles missing-file → Update() in that case.
	// Triggered as a manual update so the user sees a popup if their
	// new credentials are bad, rather than a silent log line.
	// Monolithic only: amulegui has no local resolver — its OK send
	// (SendToRemote) carries the new settings and the daemon re-downloads.
	if (thePrefs::IsGeoIPEnabled() && !CfgChanged(IDC_SHOW_COUNTRY_FLAGS) && theApp->amuledlg &&
		theApp->GetIP2Country()) {
		const bool sourceChanged =
			static_cast<int>(thePrefs::GetGeoIPSource()) != m_GeoIPSourceAtOpen;
		const bool licenseChanged = thePrefs::GetGeoIPMaxMindLicense() != m_GeoIPMaxMindLicenseAtOpen;
		const bool urlChanged = thePrefs::GetGeoIPCustomUrl() != m_GeoIPCustomUrlAtOpen;
		// Only re-download if the change matters for the *currently*
		// selected source. Editing the MaxMind license while DB-IP
		// is selected shouldn't trigger an unrelated DB-IP fetch.
		bool credentialChangedForActive = false;
		switch (thePrefs::GetGeoIPSource()) {
		case CPreferences::GeoIPSourceMaxMind:
			credentialChangedForActive = licenseChanged;
			break;
		case CPreferences::GeoIPSourceCustom:
			credentialChangedForActive = urlChanged;
			break;
		default:
			break;
		}
		if (sourceChanged || credentialChangedForActive) {
			theApp->GetIP2Country()->Update(true);
		}
	}
#endif

	if (restart_needed) {
		wxMessageBox(restart_needed_msg + _("\nYou MUST restart aMule now.\nIf you do not restart "
						    "now, don't complain if anything bad happens.\n"),
			_("WARNING"),
			wxOK | wxICON_EXCLAMATION,
			this);
	}

	EndSharedDirsSession();
	Show(false);
}

void PrefsUnifiedDlg::OnClose(wxCloseEvent &event)
{
	Show(false);
	EndSharedDirsSession();

	// Try to keep the window alive when possible
	if (event.CanVeto()) {
		event.Veto();
	} else {
		if (theApp->amuledlg) {
			theApp->amuledlg->m_prefsDialog = NULL;
		}

		// Un-Connect the Cfgs
		thePrefs::CFGMap::iterator it = thePrefs::s_CfgList.begin();
		for (; it != thePrefs::s_CfgList.end(); ++it) {
			// Checking for failures
			it->second->ConnectToWidget(0);
		}

		Destroy();
	}
}

void PrefsUnifiedDlg::OnCancel(wxCommandEvent &WXUNUSED(event))
{
	Show(false);
	// The edits are being thrown away, so the next session may refresh freely.
	EndSharedDirsSession();
	// restore state of server tab if necessary
	EnableServerTab(thePrefs::GetNetworkED2K());

	if (m_toolbarOrientationChanged) {
		theApp->amuledlg->Create_Toolbar(m_verticalToolbar);
		// Update the first tool (conn button)
		theApp->amuledlg->ShowConnectionState();
		theApp->amuledlg->Layout();
	}
}

void PrefsUnifiedDlg::OnAutostartToggle(wxCommandEvent &event)
{
	// Apply immediately. The OS is the source of truth — we don't
	// persist intent in aMule.conf, so there's no Apply-on-OK step
	// for this widget. If the write fails (e.g. read-only LaunchAgent
	// dir on a sandboxed macOS install), roll the checkbox back so
	// the UI reflects reality.
	bool wanted = event.IsChecked();
	bool ok = wanted ? AutostartManager::Enable() : AutostartManager::Disable();
	if (!ok) {
		wxCheckBox *cb = static_cast<wxCheckBox *>(FindWindow(IDC_AUTOSTART_LOGIN));
		if (cb) {
			cb->SetValue(!wanted);
		}
		wxMessageBox(wanted ? _("Could not register aMule for autostart at login. The autostart "
					"store may be read-only.")
				    : _("Could not remove the autostart-at-login entry."),
			_("Autostart"),
			wxOK | wxICON_WARNING,
			this);
	}
}

void PrefsUnifiedDlg::HandleProtocolToggle(UriScheme scheme, int checkboxId, bool wanted)
{
	// Same live-OS-state semantics as the autostart toggle above,
	// with one extra gate: before Enable overwrites a pre-existing
	// third-party handler, confirm with the user. Disable never
	// touches a non-aMule handler (Manager contract).
	if (wanted) {
		wxString current = ProtocolHandlerManager::GetCurrentHandler(scheme);
		if (!current.empty()) {
			wxString msg;
			if (scheme == UriScheme::Magnet) {
				msg = wxString::Format(
					_("aMule only handles eD2k-compatible magnets. If you replace the "
					  "current magnet handler (%s), BitTorrent magnet links will stop "
					  "working - aMule cannot download BitTorrent content. Continue?"),
					current);
			} else {
				msg = wxString::Format(_("Another application is currently the default "
							 "handler for these links (%s). Replace it with "
							 "aMule?"),
					current);
			}
			int answer = wxMessageBox(
				msg, _("Register URL handler"), wxYES_NO | wxICON_QUESTION, this);
			if (answer != wxYES) {
				// Roll the checkbox back - user declined.
				wxCheckBox *cb = static_cast<wxCheckBox *>(FindWindow(checkboxId));
				if (cb) {
					cb->SetValue(false);
				}
				return;
			}
		}
	}

	bool ok = wanted ? ProtocolHandlerManager::Enable(scheme) : ProtocolHandlerManager::Disable(scheme);
	if (!ok) {
		wxCheckBox *cb = static_cast<wxCheckBox *>(FindWindow(checkboxId));
		if (cb) {
			cb->SetValue(!wanted);
		}
		wxMessageBox(wanted ? _("Could not register aMule as the URL handler. The registration "
					"store may be read-only.")
				    : _("Could not remove the URL handler registration."),
			_("Register URL handler"),
			wxOK | wxICON_WARNING,
			this);
	}
}

void PrefsUnifiedDlg::OnProtocolEd2kToggle(wxCommandEvent &event)
{
	HandleProtocolToggle(UriScheme::Ed2k, IDC_PROTOCOL_ED2K, event.IsChecked());
}

void PrefsUnifiedDlg::OnProtocolMagnetToggle(wxCommandEvent &event)
{
	HandleProtocolToggle(UriScheme::Magnet, IDC_PROTOCOL_MAGNET, event.IsChecked());
}

void PrefsUnifiedDlg::OnCheckBoxChange(wxCommandEvent &event)
{
	bool value = event.IsChecked();
	int id = event.GetId();

	// Check if this checkbox is one of the User Events checkboxes
	if (id >= USEREVENTS_FIRST_ID &&
		id < USEREVENTS_FIRST_ID + (int)CUserEvents::GetCount() * USEREVENTS_IDS_PER_EVENT) {
		// The corresponding text control always has
		// an ID one greater than the checkbox
		FindWindow(id + 1)->Enable(value);
		return;
	}

	switch (id) {
	case IDC_UDPENABLE:
		FindWindow(IDC_UDPPORT)->Enable(value);
		break;

	case IDC_UPNP_ENABLED:
		FindWindow(IDC_UPNPTCPPORT)->Enable(value);
		FindWindow(IDC_UPNPTCPPORTTEXT)->Enable(value);
		break;

	case IDC_MEDIAMETA_ENABLED:
		FindWindow(IDC_MEDIAMETA_FFPROBEPATHTEXT)->Enable(value);
		FindWindow(IDC_MEDIAMETA_FFPROBEPATH)->Enable(value);
		FindWindow(IDC_MEDIAMETA_FFPROBEBROWSE)->Enable(value);
		FindWindow(IDC_MEDIAMETA_FFPROBEDETECT)->Enable(value);
		break;

	case IDC_UPNP_WEBSERVER_ENABLED:
		FindWindow(IDC_WEBUPNPTCPPORT)->Enable(value);
		FindWindow(IDC_WEBUPNPTCPPORTTEXT)->Enable(value);
		break;

	// The web server and amuleapi are EC clients of the core: they can only
	// run when external connections are enabled. Warn live and refuse the
	// invalid combination instead of waiting for OK.
	//
	// amulegui (CLIENT_GUI) is itself a connected EC client, so external
	// connections are necessarily enabled on the daemon and the precondition is
	// always met. The IDC_EXT_CONN_ACCEPT checkbox is hidden there and only
	// mirrors amulegui's local remote.conf, so reading it would wrongly block
	// the toggle -- skip the EC guard and keep just the deprecation nudge.
	case IDC_ENABLE_WEB:
	case IDC_ENABLE_AMULEAPI:
#ifndef CLIENT_GUI
		if (value && !CastChild(IDC_EXT_CONN_ACCEPT, wxCheckBox)->IsChecked()) {
			wxMessageBox(_("The web server and aMule API require external connections "
				       "to be enabled."),
				_("Message"),
				wxOK | wxICON_INFORMATION);
			CastChild(id, wxCheckBox)->SetValue(false);
			break;
		}
#endif
		if (value && id == IDC_ENABLE_WEB) {
			// Enabled and allowed: nudge toward the actively-developed API.
			wxMessageBox(_("The aMule web server is deprecated. Consider running the aMule "
				       "API instead."),
				_("Message"),
				wxOK | wxICON_INFORMATION);
		}
		break;

#ifndef CLIENT_GUI
	case IDC_EXT_CONN_ACCEPT: {
		// Turning external connections off strands both EC-client services.
		// (amulegui hides this checkbox and always runs over EC, so the case is
		// core-only.)
		wxCheckBox *web = CastChild(IDC_ENABLE_WEB, wxCheckBox);
		wxCheckBox *api = CastChild(IDC_ENABLE_AMULEAPI, wxCheckBox);
		if (!value && (web->IsChecked() || api->IsChecked())) {
			web->SetValue(false);
			api->SetValue(false);
			wxMessageBox(_("The web server and aMule API require external connections "
				       "to be enabled."),
				_("Message"),
				wxOK | wxICON_INFORMATION);
		}
		break;
	}
#endif

	case IDC_NETWORKKAD: {
		wxCheckBox *udpPort = (wxCheckBox *)FindWindow(IDC_UDPENABLE);
		if (value) {
			// Kad enabled: disable check box, turn UDP on, enable port spin control
			udpPort->Enable(false);
			udpPort->SetValue(true);
			FindWindow(IDC_UDPPORT)->Enable(true);
		} else {
			// Kad disabled: enable check box
			udpPort->Enable(true);
		}
		break;
	}

	case IDC_CHECKDISKSPACE:
		FindWindow(IDC_MINDISKSPACE)->Enable(value);
		break;

	case IDC_ONLINESIG:
		FindWindow(IDC_OSDIR)->Enable(value);
		;
		FindWindow(IDC_OSUPDATE)->Enable(value);
		break;

	case IDC_REMOVEDEAD:
		FindWindow(IDC_SERVERRETRIES)->Enable(value);
		;
		break;

	case IDC_AUTOSERVER:
		if ((CPath::GetFileSize(thePrefs::GetConfigDir() + "addresses.dat") == 0) &&
			CastChild(event.GetId(), wxCheckBox)->IsChecked()) {
			wxMessageBox(_("Your Auto-update servers list is in blank.\nPlease fill in at least "
				       "one URL to point to a valid server.met file.\nClick on the button "
				       "\"List\" by this checkbox to enter an URL."),
				_("Message"),
				wxOK | wxICON_INFORMATION);
			CastChild(event.GetId(), wxCheckBox)->SetValue(false);
		}
		break;

	case IDC_MSGFILTER:
		// Toggle All filter options
		FindWindow(IDC_MSGFILTER_ALL)->Enable(value);
		FindWindow(IDC_MSGFILTER_NONSECURE)->Enable(value);
		FindWindow(IDC_MSGFILTER_NONFRIENDS)->Enable(value);
		FindWindow(IDC_MSGFILTER_WORD)->Enable(value);
		if (value) {
			FindWindow(IDC_MSGWORD)
				->Enable(CastChild(IDC_MSGFILTER_WORD, wxCheckBox)->IsChecked());
		} else {
			FindWindow(IDC_MSGWORD)->Enable(false);
		}
		break;

	case IDC_MSGFILTER_ALL:
		// Toggle filtering by data.
		FindWindow(IDC_MSGFILTER_NONSECURE)->Enable(!value);
		FindWindow(IDC_MSGFILTER_NONFRIENDS)->Enable(!value);
		FindWindow(IDC_MSGFILTER_WORD)->Enable(!value);
		if (!value) {
			FindWindow(IDC_MSGWORD)
				->Enable(CastChild(IDC_MSGFILTER_WORD, wxCheckBox)->IsChecked());
		} else {
			FindWindow(IDC_MSGWORD)->Enable(false);
		}
		break;

	case IDC_MSGFILTER_WORD:
		// Toggle filter word list.
		FindWindow(IDC_MSGWORD)->Enable(value);
		break;

	case IDC_FILTERCOMMENTS:
		FindWindow(IDC_COMMENTWORD)->Enable(value);
		break;

	case ID_PROXY_ENABLE_PROXY:
		FindWindow(ID_PROXY_TYPE)->Enable(value);
		FindWindow(ID_PROXY_NAME)->Enable(value);
		FindWindow(ID_PROXY_PORT)->Enable(value);
		break;

	case ID_PROXY_ENABLE_PASSWORD:
		FindWindow(ID_PROXY_USER)->Enable(value);
		FindWindow(ID_PROXY_PASSWORD)->Enable(value);
		break;

	case IDC_STARTNEXTFILE:
		FindWindow(IDC_STARTNEXTFILE_SAME)->Enable(value);
		FindWindow(IDC_STARTNEXTFILE_ALPHA)->Enable(value);
		break;

	case IDC_ENABLETRAYICON:
		FindWindow(IDC_MINTRAY)->Enable(value);
		// HideOnClose's recovery surface is the tray icon, so its
		// checkbox follows tray-icon state too. Live-update both
		// here so the user doesn't have to close + reopen prefs to
		// see dependent options gate correctly.
		FindWindow(IDC_MACHIDEONCLOSE)->Enable(value);
		if (value) {
			theApp->amuledlg->CreateSystray();
		} else {
			theApp->amuledlg->RemoveSystray();
		}
		thePrefs::SetUseTrayIcon(value);
		break;

	case IDC_NOTIF:
		FindWindow(IDC_NOTIF)->Enable(value);
		break;

	case IDC_VERTTOOLBAR:
		m_toolbarOrientationChanged = (m_verticalToolbar != value);
		theApp->amuledlg->Create_Toolbar(value);
		// Update the first tool (conn button)
		theApp->amuledlg->ShowConnectionState();
		theApp->amuledlg->Layout();
		break;

	case IDC_ENFORCE_PO_INCOMING:
		FindWindow(IDC_ENABLE_PO_OUTGOING)->Enable(!value);
		break;

	case IDC_ENABLE_PO_OUTGOING:
		FindWindow(IDC_SUPPORT_PO)->Enable(!value);
		FindWindow(IDC_ENFORCE_PO_INCOMING)->Enable(value);
		break;

	case IDC_SUPPORT_PO:
		FindWindow(IDC_ENABLE_PO_OUTGOING)->Enable(value);
		break;

	case IDC_SHOWRATEONTITLE:
		FindWindow(IDC_RATESBEFORETITLE)->Enable(value);
		FindWindow(IDC_RATESAFTERTITLE)->Enable(value);
		break;

	case IDC_NETWORKED2K: {
		EnableServerTab(value);
		wxSpinEvent e;
		OnTCPClientPortChange(e);
		break;
	}

	default:
		break;
	}
}

void PrefsUnifiedDlg::OnButtonColorChange(wxCommandEvent &WXUNUSED(event))
{
	int index = m_choiceColor->GetSelection();
	wxColour col = wxGetColourFromUser(this, CMuleColour(thePrefs::s_colors[index]));
	if (col.Ok()) {
		m_buttonColor->SetBackgroundColour(col);
		thePrefs::s_colors[index] = CMuleColour(col).GetULong();
	}
}

void PrefsUnifiedDlg::OnColorCategorySelected(wxCommandEvent &WXUNUSED(evt))
{
	m_buttonColor->SetBackgroundColour(CMuleColour(thePrefs::s_colors[m_choiceColor->GetSelection()]));
}

void PrefsUnifiedDlg::OnButtonDir(wxCommandEvent &event)
{
	wxString type;

	int id = 0;
	switch (event.GetId()) {
	case IDC_SELTEMPDIR:
		id = IDC_TEMPFILES;
		type = _("Temporary files");
		break;

	case IDC_SELINCDIR:
		id = IDC_INCFILES;
		type = _("Incoming files");
		break;

	case IDC_SELOSDIR:
		id = IDC_OSDIR;
		type = _("Online Signatures");
		break;

		//	case IDC_SELSKIN:
		//		id = IDC_SKIN;
		//		type = _("Skins directory");
		//		break;

	default:
		wxFAIL;
		return;
	}

	type = CFormat(_("Choose a folder for %s")) % type;
	wxTextCtrl *widget = CastChild(id, wxTextCtrl);
	wxString dir = widget->GetValue();
	wxString str = wxDirSelector(type, dir, wxDD_DEFAULT_STYLE, wxDefaultPosition, this);
	if (!str.IsEmpty()) {
		widget->SetValue(str);
	}
}

#ifndef CLIENT_GUI
void PrefsUnifiedDlg::OnButtonExcludePreview(wxCommandEvent &WXUNUSED(event))
{
	wxStaticText *info = CastChild(IDC_EXCLUDE_SHARE_PREVIEW_INFO, wxStaticText);

	const wxString patterns = CastChild(IDC_EXCLUDE_SHARE_PATTERNS, wxTextCtrl)->GetValue();
	const bool useRegex = CastChild(IDC_EXCLUDE_SHARE_REGEX, wxCheckBox)->GetValue();

	wxArrayString names;
	theApp->sharedfiles->GetSharedFileNames(names);

	const int excluded = thePrefs::PreviewExcludeCount(patterns, useRegex, names);
	if (excluded == wxNOT_FOUND) {
		info->SetLabel(_("Invalid regular expression"));
	} else {
		info->SetLabel(CFormat(wxPLURAL("Would exclude %u of %u shared file",
				       "Would exclude %u of %u shared files",
				       names.GetCount())) %
			       (unsigned)excluded % (unsigned)names.GetCount());
	}
	// The label width just changed; re-lay-out its row so it is not clipped.
	info->GetParent()->Layout();
}
#endif

void PrefsUnifiedDlg::OnButtonBrowseApplication(wxCommandEvent &event)
{
	wxString title;
	int id = 0;
	switch (event.GetId()) {
	case IDC_BROWSEV:
		id = IDC_VIDEOPLAYER;
		title = _("Browse for videoplayer");
		break;
	case IDC_SELBROWSER:
		id = IDC_BROWSERSELF;
		title = _("Select browser");
		break;
	case IDC_MEDIAMETA_FFPROBEBROWSE:
		id = IDC_MEDIAMETA_FFPROBEPATH;
		title = _("Select ffprobe binary");
		break;
	default:
		wxFAIL;
		return;
	}
	wxString wildcard = CFormat(_("Executable%s"))
#ifdef __WINDOWS__
			    % " (*.exe)|*.exe";
#else
			    % "|*";
#endif

	wxString str = wxFileSelector(title, "", "", "", wildcard, 0, this);

#ifdef __WXMAC__
	// wxCocoa quirk: the modal NSOpenPanel steals key-window status;
	// when it dismisses (Open OR Cancel), Cocoa returns focus + Z-order
	// to whichever aMule window was active before Preferences opened,
	// not to Preferences itself. IsShown() still returns true and no
	// wxEVT_CLOSE_WINDOW fires — the dialog is alive but ordered
	// behind the main window, which visibly reads as "Preferences
	// closed after Browse click". Raise() restores its Z-order.
	Raise();
#endif

	if (!str.IsEmpty()) {
		wxTextCtrl *widget = CastChild(id, wxTextCtrl);
		widget->SetValue(str);
	}
}

void PrefsUnifiedDlg::OnButtonMediaMetaDetect(wxCommandEvent &WXUNUSED(evt))
{
	// Kick MediaProbe's autodetect and populate the path field with
	// whatever it finds. Empty result -> tell the user politely; a
	// path in-hand is the more useful common case so we don't try
	// to also validate the binary here (Browse... covers that).
	const wxString path = MediaProbe::AutoDetectPath();
	if (path.IsEmpty()) {
		wxMessageBox(_("ffprobe not found on PATH or in the standard install locations. Install "
			       "ffmpeg (which ships ffprobe) or use Browse to pick a binary manually."),
			_("Media metadata extraction"),
			wxOK | wxICON_INFORMATION,
			this);
		return;
	}
	CastChild(IDC_MEDIAMETA_FFPROBEPATH, wxTextCtrl)->SetValue(path);
}

void PrefsUnifiedDlg::OnButtonTweaksReset(wxCommandEvent &WXUNUSED(evt))
{
	if (wxMessageBox(_("Reset the settings on this page to their default values?"),
		    _("Reset to defaults"),
		    wxYES_NO | wxICON_QUESTION,
		    this) != wxYES) {
		return;
	}

	// Walk the currently shown page's controls and reset each one that is bound
	// to a preference. Only the widgets are updated; the change is committed on
	// OK and discarded on Cancel. The button is shown only on the Advanced page
	// (see OnPrefsPageChange), so m_CurrentPanel is that page.
	if (!m_CurrentPanel) {
		return;
	}

	for (wxWindow *child : m_CurrentPanel->GetChildren()) {
		Cfg_Base *cfg = GetCfg(child->GetId());
		if (cfg) {
			cfg->ResetToDefault();
		}
	}
}

void PrefsUnifiedDlg::OnButtonEditAddr(wxCommandEvent &WXUNUSED(evt))
{
	wxString fullpath(thePrefs::GetConfigDir() + "addresses.dat");

	EditServerListDlg *test = new EditServerListDlg(this,
		_("Edit server list"),
		_("Add here URL's to download server.met files.\nOnly one url on each line."),
		fullpath);

	test->ShowModal();
	delete test;
}

void PrefsUnifiedDlg::OnButtonIPFilterReload(wxCommandEvent &WXUNUSED(event))
{
	theApp->ipfilter->Reload();
}

void PrefsUnifiedDlg::OnButtonIPFilterUpdate(wxCommandEvent &WXUNUSED(event))
{
	theApp->ipfilter->Update(CastChild(IDC_IPFILTERURL, wxTextCtrl)->GetValue());
}

#ifdef GEOIP_GUI
PrefsUnifiedDlg *PrefsUnifiedDlg::s_activeInstance = NULL;

PrefsUnifiedDlg::~PrefsUnifiedDlg()
{
	// Clear the active-instance pointer so the IP2Country download
	// callback can't poke a freed dialog if a download completes after
	// the user has closed Preferences.
	if (s_activeInstance == this) {
		s_activeInstance = NULL;
	}
#ifdef CLIENT_GUI
	// Same reasoning for the shared-dirs editor: a GET_SHARED_DIRS reply can
	// land after the dialog is gone, and must not repaint a freed dialog.
	if (s_openPrefsDlg == this) {
		s_openPrefsDlg = nullptr;
	}
#endif
}

void PrefsUnifiedDlg::RefreshIP2CountryStatusIfOpen()
{
	// IP2Country download-completion hook. CamuleDlg::IP2CountryDownloadFinished
	// calls this after the new MMDB has been opened, so an open prefs
	// dialog can refresh its status line without the user having to
	// flip the source dropdown to trigger a redraw.
	if (s_activeInstance) {
		s_activeInstance->UpdateGeoIPStatus();
	}
}

void PrefsUnifiedDlg::NotifyIP2CountryUpdateFailedIfOpen(const wxString &msg)
{
	// Manual "Update now" failure popup. Skipped if the prefs dialog
	// has been closed in the meantime: the user has already moved on,
	// and an unparented popup with no obvious trigger would be more
	// confusing than the log line they can find under Network → Log.
	if (s_activeInstance) {
		wxMessageBox(msg, _("IP2Country update failed"), wxICON_WARNING | wxOK, s_activeInstance);
	}
}

void PrefsUnifiedDlg::OnGeoIPSourceChange(wxCommandEvent &WXUNUSED(event))
{
	// Translate the dropdown index back into the canonical persisted
	// source string. The dropdown order is fixed: 0 = DB-IP, 1 = MaxMind,
	// 2 = Custom — matching CPreferences::GeoIPSource numeric values.
	const int sel = CastChild(IDC_GEOIP_SOURCE, wxChoice)->GetSelection();
	thePrefs::SetGeoIPSource(static_cast<thePrefs::GeoIPSource>(sel));
	UpdateGeoIPSourcePanel();
	UpdateGeoIPStatus();
}

void PrefsUnifiedDlg::OnGeoIPUpdateNow(wxCommandEvent &WXUNUSED(event))
{
	// Persist the credential / URL fields the user just edited *before*
	// kicking off the download — the URL helper reads them from the
	// static backing store, not the live widget values. The full prefs
	// commit happens on OK, but for "Update now" we need a partial save.
	thePrefs::SetGeoIPMaxMindLicense(CastChild(IDC_GEOIP_MAXMIND_LIC, wxTextCtrl)->GetValue());
	thePrefs::SetGeoIPCustomUrl(CastChild(IDC_GEOIP_CUSTOM_URL, wxTextCtrl)->GetValue());

#ifdef CLIENT_GUI
	// amulegui has no local resolver; the daemon owns the GeoIP DB (#440).
	// Ask it to refresh by sending the current prefs with a one-shot
	// UPDATE_NOW trigger piggy-backed on the normal prefs packet. SendToRemote
	// serializes from the statics we just wrote, so the license / custom URL
	// the user typed reach the daemon before it resolves the download URL.
	thePrefs::SetGeoIPUpdateRequested(true);
	theApp->glob_prefs->SendToRemote();
	thePrefs::SetGeoIPUpdateRequested(false);
#else
	// Monolithic amule: kick off the download locally. DownloadFinished swaps
	// the new file in and re-opens the database asynchronously; the status
	// line refreshes next time the panel is shown (the running download isn't
	// blocking, so polling would just show "...").
	if (theApp->GetIP2Country()) {
		theApp->GetIP2Country()->Update(true);
	}
#endif
}

void PrefsUnifiedDlg::UpdateGeoIPSourcePanel()
{
	// Show exactly one of the three sub-panels based on the selected
	// source. Each is a discrete wxPanel hosting its own labels and
	// fields, so toggling the panel collapses the slot cleanly without
	// leaving orphaned ID-less labels visible.
	//
	// Critical: use the *sizer's* Show(window, bool) — wxWindow::Show()
	// only hides the window, leaving the sizer item still occupying its
	// slot. Going through wxSizer::Show() releases the slot AND hides
	// the window, which is what actually re-flows the layout.
	const thePrefs::GeoIPSource src = thePrefs::GetGeoIPSource();
	wxWindow *dbip = FindWindow(IDC_GEOIP_INFO_DBIP);
	wxWindow *maxmind = FindWindow(IDC_GEOIP_INFO_MAXMIND);
	wxWindow *custom = FindWindow(IDC_GEOIP_INFO_CUSTOM);
	if (!dbip || !maxmind || !custom) {
		return;
	}

	wxSizer *containerSizer = dbip->GetContainingSizer();
	if (!containerSizer) {
		return;
	}
	containerSizer->Show(dbip, src == thePrefs::GeoIPSourceDBIP);
	containerSizer->Show(maxmind, src == thePrefs::GeoIPSourceMaxMind);
	containerSizer->Show(custom, src == thePrefs::GeoIPSourceCustom);

	// Re-layout the prefs page so the height delta from the now-hidden
	// panel propagates upward through the wxStaticBoxSizer chain. Each
	// sub-panel is a real wxPanel (leaf from the layout engine's view),
	// so the cascade-loop risk that motivated dropping Layout() earlier
	// does not apply here.
	if (m_CurrentPanel) {
		m_CurrentPanel->Layout();
	}
}

void PrefsUnifiedDlg::OnGeoIPMasterToggle(wxCommandEvent &event)
{
	// Forward to the generic CfgChanged tracker so the OK button noticed
	// the dirty state, then grey out everything below the master switch.
	OnCheckBoxChange(event);
	UpdateGeoIPControlsEnabled();
}

void PrefsUnifiedDlg::UpdateGeoIPControlsEnabled()
{
	// Master "Show country flags for clients" gates every downstream control:
	// source selector, all three sub-panel fields, the Update Now button,
	// auto-update checkbox, and the status line. Each control is looked up by
	// ID so missing widgets (e.g. earlier init failure) don't crash. When the
	// connected core has no GeoIP support the whole page is dropped from the
	// menu (see the constructor / CPreferencesRem::LoadRemote), so we never
	// reach here unsupported.
	wxCheckBox *master = CastChild(IDC_SHOW_COUNTRY_FLAGS, wxCheckBox);
	if (!master) {
		return;
	}
	const bool on = master->IsChecked();

	const int ids[] = {
		IDC_GEOIP_SOURCE,
		IDC_GEOIP_INFO_DBIP,
		IDC_GEOIP_INFO_MAXMIND,
		IDC_GEOIP_INFO_CUSTOM,
		IDC_GEOIP_MAXMIND_LIC,
		IDC_GEOIP_CUSTOM_URL,
		IDC_GEOIP_UPDATE_NOW,
		IDC_GEOIP_AUTOUPDATE,
		IDC_GEOIP_STATUS,
	};
	for (size_t i = 0; i < WXSIZEOF(ids); ++i) {
		wxWindow *w = FindWindow(ids[i]);
		if (w) {
			w->Enable(on);
		}
	}
}

void PrefsUnifiedDlg::UpdateGeoIPStatus()
{
	if (!theApp->amuledlg) {
		return;
	}
	wxStaticText *st = CastChild(IDC_GEOIP_STATUS, wxStaticText);
	if (!st) {
		return;
	}
	CIP2Country *ip2c = theApp->GetIP2Country();
	if (!ip2c) {
		// amulegui has no local resolver — render the status mirrored from the
		// daemon over EC (#440). The "unavailable" case is handled by
		// UpdateGeoIPControlsEnabled(); here we show the loaded-source
		// attribution and the last update result (reusing existing strings).
		if (!thePrefs::IsGeoIPSupported()) {
			return;
		}
		wxString line;
		const wxString &src = thePrefs::GetGeoIPStatusLoadedSource();
		if (thePrefs::IsGeoIPStatusLoaded()) {
			if (src == "maxmind") {
				line = _("Data by MaxMind GeoLite2");
			} else if (src == "custom") {
				line = _("Custom source");
			} else if (src == "dbip") {
				line = _("Data by DB-IP.com");
			}
		}
		const wxString &last = thePrefs::GetGeoIPStatusLastResult();
		if (!last.IsEmpty()) {
			if (!line.IsEmpty()) {
				line += " - ";
			}
			line += last;
		}
		st->SetLabel(line);
		return;
	}

#ifndef CLIENT_GUI
	// Local-resolver status (monolithic amule). amulegui has no CIP2Country
	// and already returned above via the !ip2c branch, so guarding this out
	// keeps it free of CIP2Country link symbols.
	//
	// Attribution for the *loaded* file (the source that actually wrote
	// it) — not the currently-selected dropdown source. If the file was
	// hand-installed (LoadedSource is empty), no attribution is shown:
	// we don't know who to credit and the per-source sub-panel below
	// already covers the legal-display obligation.
	wxString attribution;
	const wxString &loaded = thePrefs::GetGeoIPLoadedSource();
	if (loaded == "maxmind") {
		attribution = _("Data by MaxMind GeoLite2");
	} else if (loaded == "custom") {
		attribution = _("Custom source");
	} else if (loaded == "dbip") {
		attribution = _("Data by DB-IP.com");
	}

	if (ip2c->IsEnabled()) {
		// Loaded — single-line summary keeps the dialog height bounded
		// across sources. We deliberately omit the on-disk path: it's
		// already in the log line at load time, and wxStaticText
		// tooltips on wxOSX are unreliable enough that promising it in
		// the UI would mislead Mac users.
		const wxFileName fn(ip2c->GetDatabasePath());
		wxString sizeLabel;
		if (fn.FileExists()) {
			const wxULongLong bytes = fn.GetSize();
			if (bytes != wxInvalidSize) {
				sizeLabel =
					wxString::Format(" (%.1f MB)", bytes.ToDouble() / (1024.0 * 1024.0));
			}
		}
		if (attribution.IsEmpty()) {
			// Hand-installed / migrated file — no attribution to show.
			st->SetLabel(wxString::Format(_("Status: Loaded%s"), sizeLabel));
		} else {
			st->SetLabel(wxString::Format(_("Status: Loaded%s - %s"), sizeLabel, attribution));
		}
	} else if (wxFileName::FileExists(ip2c->GetDatabasePath())) {
		// File exists but database failed to open — corrupt / wrong format.
		st->SetLabel(_("Status: Failed to load - click 'Update now' to refresh."));
	} else {
		st->SetLabel(_("Status: Not found - click 'Update now' to download."));
	}
#endif // !CLIENT_GUI
}
#endif // GEOIP_GUI

void PrefsUnifiedDlg::OnPrefsPageChange(wxListEvent &event)
{
	prefs_sizer->Detach(m_CurrentPanel);
	m_CurrentPanel->Show(false);

	m_CurrentPanel = reinterpret_cast<wxPanel *>(m_PrefsIcons->GetItemData(event.GetIndex()));
	if (pages[event.GetIndex()].m_function == PreferencesDirectoriesTab) {
#ifdef CLIENT_GUI
		// Nothing to initialise: there is no tree here, and the roots are
		// refreshed per editing session (PrepareSharedDirsForSession) rather
		// than on every page change, which would discard pending edits when
		// the user navigates away and back.
#else
		CastChild(IDC_SHARESELECTOR, CDirectoryTreeCtrl)->Init();
#endif
	}

	// The reset-to-defaults button applies to the current page's controls; keep
	// it to the Advanced page, whose expert tuning knobs it is meant to undo.
	if (wxButton *resetBtn = CastChild(IDC_TWEAKS_RESET, wxButton)) {
		// Identify the page by widget, not list index: hiding the server /
		// IP2Country tab shifts the list index away from the pages[] index.
		resetBtn->Show(m_CurrentPanel == m_aMuleTweaksWidget);
	}

	prefs_sizer->Add(m_CurrentPanel, wxSizerFlags().Expand().Expand());
	m_CurrentPanel->Show(true);

	Layout();

	event.Skip();
}

void PrefsUnifiedDlg::OnToolTipDelayChange(wxSpinEvent &event)
{
	wxToolTip::SetDelay(event.GetPosition() * 1000);
}

void PrefsUnifiedDlg::OnInitDialog(wxInitDialogEvent &WXUNUSED(evt))
{
	// This function exists solely to avoid automatic transfer-to-widget calls
}

void PrefsUnifiedDlg::OnScrollBarChange(wxScrollEvent &event)
{
	int id = 0;
	wxString label;

	switch (event.GetId()) {
	case IDC_SLIDER:
		id = IDC_SLIDERINFO;
		label = CFormat(wxPLURAL(
				"Update delay: %d second", "Update delay: %d seconds", event.GetPosition())) %
			event.GetPosition();
		theApp->amuledlg->m_statisticswnd->SetUpdatePeriod(event.GetPosition());
		theApp->amuledlg->m_kademliawnd->SetUpdatePeriod(event.GetPosition());
		break;

	case IDC_SLIDER3:
		id = IDC_SLIDERINFO3;
		label = CFormat(wxPLURAL("Time for average graph: %d minute",
				"Time for average graph: %d minutes",
				event.GetPosition())) %
			event.GetPosition();
		theApp->m_statistics->SetAverageMinutes(event.GetPosition());
		break;

	case IDC_SLIDER4:
		id = IDC_SLIDERINFO4;
		label = CFormat(_("Connections Graph Scale: %d")) % event.GetPosition();
		theApp->amuledlg->m_statisticswnd->GetConnScope()->SetRanges(0, event.GetPosition());
		break;

	case IDC_SLIDER2:
		id = IDC_SLIDERINFO2;
		label = CFormat(wxPLURAL(
				"Update delay: %d second", "Update delay: %d seconds", event.GetPosition())) %
			event.GetPosition();
		break;

	case IDC_FILEBUFFERSIZE:
		id = IDC_FILEBUFFERSIZE_STATIC;
		// Yes, it seems odd to add the singular form here, but other languages might need to know the
		// number to select the appropriate translation
		label = CFormat(wxPLURAL("File Buffer Size: %d byte",
				"File Buffer Size: %d bytes",
				event.GetPosition() * 15000)) %
			(event.GetPosition() * 15000);
		break;

	case IDC_QUEUESIZE:
		id = IDC_QUEUESIZE_STATIC;
		// Yes, it seems odd to add the singular form here, but other languages might need to know the
		// number to select the appropriate translation
		label = CFormat(wxPLURAL("Upload Queue Size: %d client",
				"Upload Queue Size: %d clients",
				event.GetPosition() * 100)) %
			(event.GetPosition() * 100);
		break;

	case IDC_SERVERKEEPALIVE:
		id = IDC_SERVERKEEPALIVE_LABEL;

		if (event.GetPosition()) {
			label = CFormat(wxPLURAL("Server connection refresh interval: %d minute",
					"Server connection refresh interval: %d minutes",
					event.GetPosition())) %
				event.GetPosition();
		} else {
			label = _("Server connection refresh interval: Disabled");
		}
		break;

	default:
		return;
	}

	wxStaticText *widget = CastChild(id, wxStaticText);

	if (widget) {
		widget->SetLabel(label);
		widget->GetParent()->Layout();
	}
}

void PrefsUnifiedDlg::OnRateLimitChanged(wxSpinEvent &event)
{
	// Here we do immediate sanity checking of the up/down ratio,
	// so that the user can see if his choice is illegal

	// We only do checks if the rate is limited
	if (event.GetPosition() != (int)UNLIMITED) {
		wxSpinCtrl *dlrate = CastChild(IDC_MAXDOWN, wxSpinCtrl);

		if (event.GetPosition() < 4) {
			if ((event.GetPosition() * 3 < dlrate->GetValue()) ||
				(dlrate->GetValue() == (int)UNLIMITED)) {
				dlrate->SetValue(event.GetPosition() * 3);
			}
		} else if (event.GetPosition() < 10) {
			if ((event.GetPosition() * 4 < dlrate->GetValue()) ||
				(dlrate->GetValue() == (int)UNLIMITED)) {
				dlrate->SetValue(event.GetPosition() * 4);
			}
		}
	}
}

void PrefsUnifiedDlg::OnTCPClientPortChange(wxSpinEvent &WXUNUSED(event))
{
	CastChild(ID_TEXT_CLIENT_UDP_PORT, wxStaticText)
		->SetLabel(m_ServerTabVisible
				   ? (wxString() << (CastChild(IDC_PORT, wxSpinCtrl)->GetValue() + 3))
				   : wxString(_("disabled")));
}

void PrefsUnifiedDlg::OnUserEventSelected(wxListEvent &event)
{
	for (unsigned int i = 0; i < CUserEvents::GetCount(); ++i) {
		IDC_PREFS_EVENTS_PAGE->Hide(i + 1);
	}

	IDC_PREFS_EVENTS_PAGE->Show(
		(event.GetData() - USEREVENTS_FIRST_ID) / USEREVENTS_IDS_PER_EVENT + 1, true);

	IDC_PREFS_EVENTS_PAGE->Layout();

	event.Skip();
}

void PrefsUnifiedDlg::OnLanguageChoice(wxCommandEvent &evt)
{
	thePrefs::GetCfgLang()->UpdateChoice(evt.GetSelection());
}

void PrefsUnifiedDlg::CreateEventPanels(const int idx, const wxString &vars, wxWindow *parent)
{
	wxStaticBox *item8 = new wxStaticBox(parent,
		-1,
		CFormat(_("Execute command on '%s' event")) %
			wxGetTranslation(
				CUserEvents::GetDisplayName(static_cast<enum CUserEvents::EventType>(idx))));
	wxStaticBoxSizer *item7 = new wxStaticBoxSizer(item8, wxVERTICAL);

	wxCheckBox *item9 = new wxCheckBox(parent,
		USEREVENTS_FIRST_ID + idx * USEREVENTS_IDS_PER_EVENT + 1,
		_("Enable command execution on core"),
		wxDefaultPosition,
		wxDefaultSize,
		0);
	item7->Add(item9, wxSizerFlags().CenterVertical().Border(wxLEFT | wxRIGHT, 5));

	wxFlexGridSizer *item10 = new wxFlexGridSizer(3, 0, 0);
	item10->AddGrowableCol(2);

	item10->Add(20, 20, wxSizerFlags().Center());

	wxStaticText *item11 =
		new wxStaticText(parent, -1, _("Core command:"), wxDefaultPosition, wxDefaultSize, 0);
	item10->Add(item11, wxSizerFlags().Center().Border(wxALL, 5));

	wxTextCtrl *item12 = new wxTextCtrl(parent,
		USEREVENTS_FIRST_ID + idx * USEREVENTS_IDS_PER_EVENT + 2,
		"",
		wxDefaultPosition,
		wxDefaultSize,
		0);
	item12->Enable(CUserEvents::IsCoreCommandEnabled(static_cast<enum CUserEvents::EventType>(idx)));
	item10->Add(item12, wxSizerFlags().Expand().CenterVertical().Border(wxALL, 5));

	item7->Add(item10, wxSizerFlags().Expand().CenterVertical().Border(wxALL, 0));

	wxCheckBox *item14 = new wxCheckBox(parent,
		USEREVENTS_FIRST_ID + idx * USEREVENTS_IDS_PER_EVENT + 3,
		_("Enable command execution on GUI"),
		wxDefaultPosition,
		wxDefaultSize,
		0);
	item7->Add(item14, wxSizerFlags().CenterVertical().Border(wxLEFT | wxRIGHT, 5));

	wxFlexGridSizer *item15 = new wxFlexGridSizer(3, 0, 0);
	item15->AddGrowableCol(2);

	item15->Add(20, 20, wxSizerFlags().Center());

	wxStaticText *item16 =
		new wxStaticText(parent, -1, _("GUI command:"), wxDefaultPosition, wxDefaultSize, 0);
	item15->Add(item16, wxSizerFlags().Center().Border(wxALL, 5));

	wxTextCtrl *item17 = new wxTextCtrl(parent,
		USEREVENTS_FIRST_ID + idx * USEREVENTS_IDS_PER_EVENT + 4,
		"",
		wxDefaultPosition,
		wxDefaultSize,
		0);
	item17->Enable(CUserEvents::IsGUICommandEnabled(static_cast<enum CUserEvents::EventType>(idx)));
	item15->Add(item17, wxSizerFlags().Expand().CenterVertical().Border(wxALL, 5));

	item7->Add(item15, wxSizerFlags().Expand().CenterVertical().Border(wxALL, 0));

	wxStaticText *item13 = new wxStaticText(parent,
		-1,
		_("The following variables will be replaced:") + vars,
		wxDefaultPosition,
		wxDefaultSize,
		0);
	item7->Add(item13, wxSizerFlags().Expand().CenterVertical().Border(wxALL, 5));

	IDC_PREFS_EVENTS_PAGE->Add(item7, wxSizerFlags().Expand().CenterVertical().Border(wxALL, 5));

	IDC_PREFS_EVENTS_PAGE->Layout();
	IDC_PREFS_EVENTS_PAGE->Hide(idx + 1);
}

namespace
{

// Hard-coded list of paths that look like obvious "did you really mean
// to share this?" candidates. Matched by IsSensitiveSharePath as either
// the exact path or a strict descendant (separator-boundary aware),
// so e.g. ~/Documents/Tax2024 is flagged because ~/Documents is on
// the list. Empty list entries are skipped.
//
// This is not meant to be exhaustive — its job is to catch the most
// common "accidental right-click" cases (issue #592) by raising a
// confirmation dialog, not to be a privacy boundary. Users can always
// say "Yes I really do want this" and proceed.
wxArrayString BuildSensitivePathList()
{
	wxArrayString out;

	auto pushIfNotEmpty = [&out](const wxString &s) {
		if (!s.IsEmpty()) {
			out.Add(s);
		}
	};

	const wxString home = wxGetUserHome();
	pushIfNotEmpty(home);
	if (!home.IsEmpty()) {
		const wxChar sep = wxFileName::GetPathSeparator();
		pushIfNotEmpty(home + sep + "Documents");
		pushIfNotEmpty(home + sep + "Desktop");
		pushIfNotEmpty(home + sep + "Pictures");
		pushIfNotEmpty(home + sep + "Library"); // macOS
		pushIfNotEmpty(home + sep + "AppData"); // Windows
		pushIfNotEmpty(home + sep + ".aMule");
		pushIfNotEmpty(home + sep + ".config");
		pushIfNotEmpty(home + sep + ".ssh");
		pushIfNotEmpty(home + sep + ".gnupg");
	}

#ifdef __WINDOWS__
	pushIfNotEmpty("C:\\");
	pushIfNotEmpty("C:\\Windows");
	pushIfNotEmpty("C:\\Program Files");
	pushIfNotEmpty("C:\\Program Files (x86)");
	pushIfNotEmpty("C:\\ProgramData");
	pushIfNotEmpty("C:\\Users"); // parent of every user's home
#else
	pushIfNotEmpty("/");
	pushIfNotEmpty("/etc");
	pushIfNotEmpty("/var");
	pushIfNotEmpty("/tmp");
	pushIfNotEmpty("/boot");
	pushIfNotEmpty("/usr");
	pushIfNotEmpty("/home");         // parent of every user's home on Linux
	pushIfNotEmpty("/root");         // root user's home on Linux
	pushIfNotEmpty("/Applications"); // macOS
	pushIfNotEmpty("/System");       // macOS
	pushIfNotEmpty("/Users");        // parent of every user's home on macOS
#endif

	return out;
}

bool IsSensitiveSharePath(const CPath &path)
{
	static const wxArrayString sensitive = BuildSensitivePathList();
	const wxString raw = path.GetRaw();
	const wxChar sep = wxFileName::GetPathSeparator();
	for (size_t i = 0; i < sensitive.GetCount(); ++i) {
		const wxString &root = sensitive[i];
		if (raw == root) {
			return true;
		}
		// Prefix match with a separator boundary so /home doesn't
		// also flag /home2 or /homework. The length floor at 4 keeps
		// "filesystem-root" entries — `/` (1 char) and `C:\` (3 chars)
		// — exact-match-only: otherwise every path on the platform
		// would be a descendant of the root and every share would
		// trip the confirm dialog. Real subtrees like `/etc`, `/var`,
		// `C:\Windows` keep their prefix-match behaviour.
		if (root.length() >= 4 && raw.length() > root.length() && raw.StartsWith(root) &&
			(root.Last() == sep || raw[root.length()] == sep)) {
			return true;
		}
	}
	return false;
}

} // namespace

PrefsUnifiedDlg::SharedDirsCommitResult PrefsUnifiedDlg::CommitSharedDirsWithProgress()
{
#ifdef CLIENT_GUI
	// The core owns the shared-folder files; we just hand it the roots. No
	// local progress dialog or rescan here — the rescan happens daemon-side,
	// and any path it refuses comes back as a rejection we surface then.
	HarvestSharedDirsList();
	static_cast<CPreferencesRem *>(theApp->glob_prefs)->SendSharedDirsToRemote();
	m_sharedDirsDirty = false;
	return SharedDirsCommitResult::Committed;
#else
	if (!m_ShareSelector || !m_ShareSelector->HasChanged) {
		return SharedDirsCommitResult::NothingToCommit;
	}

	CDirectoryTreeCtrl::PathList explicitShares;
	CDirectoryTreeCtrl::PathList recursiveIntents;
	m_ShareSelector->GetSharedDirectories(&explicitShares);
	m_ShareSelector->GetRecursiveSharedDirectories(&recursiveIntents);

	// Strip entries that are descendants of a recursive root: the
	// UI's right-click handler populates m_lstShared with the already-
	// rendered subtree as a side-effect of MarkChildren (so the
	// in-tree visual stays consistent), but those subdirs aren't
	// "explicit" intent -- they're the recursive expansion. Without
	// this filter they'd land in shareddir-explicit.dat and stick
	// around as orphan pinned paths if the user later removed the
	// recursive marker externally (no DelSharesUnder cleanup runs
	// outside the UI). Filter at the commit boundary keeps the
	// canonical files semantically clean.
	if (!recursiveIntents.empty()) {
		const wxChar sep = wxFileName::GetPathSeparator();
		auto isInsideRecursive = [&recursiveIntents, sep](const CPath &p) {
			const wxString target = p.GetRaw();
			for (const CPath &root : recursiveIntents) {
				const wxString r = root.GetRaw();
				if (r.IsEmpty()) {
					continue;
				}
				if (target == r) {
					return true;
				}
				if (target.length() > r.length() && target.StartsWith(r) &&
					(r.Last() == sep || target[r.length()] == sep)) {
					return true;
				}
			}
			return false;
		};
		CDirectoryTreeCtrl::PathList filtered;
		filtered.reserve(explicitShares.size());
		for (const CPath &p : explicitShares) {
			if (!isInsideRecursive(p)) {
				filtered.push_back(p);
			}
		}
		explicitShares.swap(filtered);
	}

	// Confirm before expanding sensitive recursive roots.
	std::vector<CPath> sensitive;
	for (const CPath &p : recursiveIntents) {
		if (IsSensitiveSharePath(p)) {
			sensitive.push_back(p);
		}
	}
	if (!sensitive.empty()) {
		wxString msg = _("The following folders look like system or sensitive locations:\n\n");
		for (const CPath &p : sensitive) {
			msg += "  " + p.GetPrintable() + "\n";
		}
		msg += "\n";
		msg += _("Sharing them recursively will publish every file underneath on the eD2k/Kad "
			 "networks. Do you really want to do this?");
		const int rv = wxMessageBox(
			msg, _("Confirm shared folders"), wxICON_QUESTION | wxYES_NO | wxNO_DEFAULT, this);
		if (rv != wxYES) {
			return SharedDirsCommitResult::CancelledByUser;
		}
	}

	// Snapshot the current shared-dirs state so we can restore it
	// atomically if the user cancels at any phase (expansion or
	// reload). Cancel means "leave my saved state alone" — we do not
	// persist a half-committed list to disk. All three lists are
	// captured: the explicit/recursive intent + the runtime union,
	// since SaveSharedFolders persists all three together.
	const CDirectoryTreeCtrl::PathList originalShares = theApp->glob_prefs->shareddir_list;
	const CDirectoryTreeCtrl::PathList originalExplicit = theApp->glob_prefs->shareddir_explicit_list;
	const CDirectoryTreeCtrl::PathList originalRecursive = theApp->glob_prefs->shareddir_recursive_list;

	// One progress dialog covers both phases: the optional recursive
	// expansion, plus the always-present Reload phase. Even a single
	// double-click add of one folder still triggers a full Reload of
	// CSharedFileList, which on a large library (200k+ files) freezes
	// the UI for a couple of minutes — so the progress UI applies
	// regardless of whether expansion preceded it. The dialog
	// auto-hides on Update(100), so on small libraries where Reload
	// finishes in milliseconds it just flashes briefly.
	//
	// The initial body text reflects which phase will run first: if
	// there is a recursive intent we start in the expansion walk, if
	// not we go straight into the file-list Reload.
	const wxString initialBody = recursiveIntents.empty() ? _("Reloading shared files...")
							      : _("Scanning for subdirectories...");
	wxProgressDialog progress(_("Updating shared folders"),
		initialBody,
		/*maximum=*/100,
		this,
		wxPD_CAN_ABORT | wxPD_APP_MODAL | wxPD_AUTO_HIDE);

	CDirectoryTreeCtrl::PathList finalShares = explicitShares;

	// ----- Phase 1: optional recursive expansion ---------------------
	//
	// Pass `this` as the event owner so progress and done events flow
	// back into our event handlers below — keeping the GTK main loop
	// alive during the walk, which in turn keeps the Cancel button on
	// the progress dialog responsive. A pure polling loop with
	// wxMilliSleep+Yield works on Cocoa but starves GTK's event queue
	// and makes the whole UI feel frozen.
	if (!recursiveIntents.empty()) {
		CSharedDirsApplyTask task(explicitShares, recursiveIntents, this);
		if (task.Create() != wxTHREAD_NO_ERROR || task.Run() != wxTHREAD_NO_ERROR) {
			// Worker couldn't start. Fall back to the explicit list
			// (no recursion) so the user at least gets the non-
			// recursive part of their selection saved.
			finalShares = explicitShares;
		} else {
			bool done = false;
			bool userCancelled = false;
			auto onProgress = [&](wxThreadEvent &ev) {
				const wxString status = CFormat(_("Scanned %u directories...")) %
							static_cast<unsigned>(ev.GetInt());
				if (!progress.Pulse(status)) {
					task.Cancel(); // wxThread::Delete joins the worker
					userCancelled = true;
					done = true;
				}
			};
			auto onDone = [&](wxThreadEvent &ev) {
				userCancelled = userCancelled || (ev.GetInt() != 0);
				done = true;
			};
			Bind(wxEVT_SHARED_DIRS_APPLY_PROGRESS, onProgress);
			Bind(wxEVT_SHARED_DIRS_APPLY_DONE, onDone);

			while (!done) {
				wxTheApp->Yield(/*onlyIfNeeded=*/false);
			}

			Unbind(wxEVT_SHARED_DIRS_APPLY_PROGRESS, onProgress);
			Unbind(wxEVT_SHARED_DIRS_APPLY_DONE, onDone);
			task.Wait();

			if (userCancelled || task.WasCancelled()) {
				// shareddir_list was never touched yet — nothing to
				// roll back.
				progress.Update(100);
				return SharedDirsCommitResult::CancelledByUser;
			}
			finalShares = task.GetExpandedShares();
		}
	}

	// ----- Phase 2: persist + reload --------------------------------
	//
	// Update all three canonical/derived lists on the Preferences
	// instance and persist them to disk *before* invoking Reload():
	// FindSharedFiles starts by calling ReloadSharedFolders() which
	// re-reads from disk, so the in-memory state alone isn't enough.
	// shareddir-explicit.dat and shareddir-recursive.dat record the
	// user's intent (non-recursive vs recursive roots); shareddir.dat
	// is regenerated as the runtime union (= finalShares from the
	// apply walk) so older binaries still see the right paths.
	theApp->glob_prefs->shareddir_explicit_list = explicitShares;
	theApp->glob_prefs->shareddir_recursive_list = recursiveIntents;
	theApp->glob_prefs->shareddir_list = finalShares;
	theApp->glob_prefs->SaveSharedFolders();

	bool reloadAborted = false;
	auto reloadYield = [&progress, &reloadAborted](size_t filesScanned) -> bool {
		const wxString msg = CFormat(_("Reloading shared files: %u files scanned")) %
				     static_cast<unsigned>(filesScanned);
		if (!progress.Pulse(msg)) {
			reloadAborted = true;
			return false;
		}
		return true;
	};

	const bool reloadOk = theApp->sharedfiles->Reload(reloadYield);
	progress.Update(100);

	if (!reloadOk || reloadAborted) {
		// Roll back: both the in-memory state and the on-disk
		// files (shareddir.dat + shareddir-explicit.dat +
		// shareddir-recursive.dat) have to be reverted. The
		// in-memory shared-file map is partially populated against
		// the new list, so rebuild it from the restored list
		// (without a yield callback — the restored list is by
		// construction the one the user was already running with
		// before this OK click).
		theApp->glob_prefs->shareddir_list = originalShares;
		theApp->glob_prefs->shareddir_explicit_list = originalExplicit;
		theApp->glob_prefs->shareddir_recursive_list = originalRecursive;
		theApp->glob_prefs->SaveSharedFolders();
		theApp->sharedfiles->Reload(nullptr);
		return SharedDirsCommitResult::CancelledByUser;
	}

	return SharedDirsCommitResult::Committed;
#endif // CLIENT_GUI
}
#ifdef CLIENT_GUI

void PrefsUnifiedDlg::RefreshSharedDirsIfOpen()
{
	// Never repaint over uncommitted edits: the reply may land after the user
	// has already started adding rows.
	if (s_openPrefsDlg != nullptr && !s_openPrefsDlg->m_sharedDirsDirty) {
		s_openPrefsDlg->PopulateSharedDirsList();
	}
}

void PrefsUnifiedDlg::PopulateSharedDirsList()
{
	wxListCtrl *list = CastChild(IDC_SHAREDDIRS_LIST, wxListCtrl);
	if (list == nullptr) {
		return;
	}
	if (list->GetColumnCount() == 0) {
		list->InsertColumn(0, _("Shared folder"), wxLIST_FORMAT_LEFT, 320);
		list->InsertColumn(1, _("Recursive"), wxLIST_FORMAT_LEFT, 90);
	}
	list->DeleteAllItems();

	const bool supported =
		theApp->m_connect != nullptr && theApp->m_connect->ServerSupportsSharedDirsConfig();
	// An older core can neither report nor accept these, so leave the editor
	// inert rather than implying an edit here would reach it.
	FindWindow(IDC_SHAREDDIR_PATH)->Enable(supported);
	FindWindow(IDC_SHAREDDIR_RECURSIVE)->Enable(supported);
	FindWindow(IDC_SHAREDDIR_ADD)->Enable(supported);
	FindWindow(IDC_SHAREDDIR_REMOVE)->Enable(supported);
	if (!supported) {
		return;
	}

	long row = 0;
	for (const CPath &dir : theApp->glob_prefs->shareddir_explicit_list) {
		list->InsertItem(row, dir.GetPrintable());
		list->SetItem(row, 1, wxEmptyString);
		++row;
	}
	for (const CPath &dir : theApp->glob_prefs->shareddir_recursive_list) {
		list->InsertItem(row, dir.GetPrintable());
		list->SetItem(row, 1, _("Yes"));
		++row;
	}
}

void PrefsUnifiedDlg::HarvestSharedDirsList()
{
	wxListCtrl *list = CastChild(IDC_SHAREDDIRS_LIST, wxListCtrl);
	if (list == nullptr) {
		return;
	}
	CPreferences::PathList explicitDirs;
	CPreferences::PathList recursiveDirs;
	for (long row = 0; row < list->GetItemCount(); ++row) {
		const CPath path(list->GetItemText(row));
		wxListItem field;
		field.SetId(row);
		field.SetColumn(1);
		field.SetMask(wxLIST_MASK_TEXT);
		list->GetItem(field);
		if (field.GetText().IsEmpty()) {
			explicitDirs.push_back(path);
		} else {
			recursiveDirs.push_back(path);
		}
	}
	theApp->glob_prefs->shareddir_explicit_list = explicitDirs;
	theApp->glob_prefs->shareddir_recursive_list = recursiveDirs;
}

void PrefsUnifiedDlg::OnSharedDirAdd(wxCommandEvent &WXUNUSED(evt))
{
	wxTextCtrl *pathCtrl = CastChild(IDC_SHAREDDIR_PATH, wxTextCtrl);
	wxListCtrl *list = CastChild(IDC_SHAREDDIRS_LIST, wxListCtrl);
	if (pathCtrl == nullptr || list == nullptr) {
		return;
	}
	const wxString path = pathCtrl->GetValue().Strip(wxString::both);
	if (path.IsEmpty()) {
		return;
	}
	// The path names a folder on the *core's* machine, so it can't be checked
	// here; the core validates on apply and reports anything it refuses.
	for (long row = 0; row < list->GetItemCount(); ++row) {
		if (list->GetItemText(row) == path) {
			return; // already listed
		}
	}
	const bool recursive = CastChild(IDC_SHAREDDIR_RECURSIVE, wxCheckBox)->GetValue();
	const long row = list->InsertItem(list->GetItemCount(), path);
	list->SetItem(row, 1, recursive ? _("Yes") : wxString(wxEmptyString));
	pathCtrl->Clear();
	m_sharedDirsDirty = true;
}

void PrefsUnifiedDlg::OnSharedDirRemove(wxCommandEvent &WXUNUSED(evt))
{
	wxListCtrl *list = CastChild(IDC_SHAREDDIRS_LIST, wxListCtrl);
	if (list == nullptr) {
		return;
	}
	const long sel = list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
	if (sel != -1) {
		list->DeleteItem(sel);
		m_sharedDirsDirty = true;
	}
}

#endif // CLIENT_GUI

void PrefsUnifiedDlg::EndSharedDirsSession()
{
#ifdef CLIENT_GUI
	m_sharedDirsDirty = false;
#else
	if (m_ShareSelector != nullptr) {
		m_ShareSelector->HasChanged = false;
	}
#endif
}

void PrefsUnifiedDlg::PrepareSharedDirsForSession()
{
#ifndef CLIENT_GUI
	// TransferToWindow() has already refreshed the tree's backing maps from
	// glob_prefs by the time we run, so the model is never the stale part. The
	// view is: marks below the root's immediate children are applied when a node
	// is created, so anything already expanded keeps the marks it was built
	// with. Rebuild only when the roots have actually moved since the paint,
	// since rebuilding collapses the tree and re-scans the drives.
	if (m_ShareSelector == nullptr || m_ShareSelector->HasChanged) {
		// Edits pending in this session; leave them be.
		return;
	}
	if (m_ShareSelector->NeedsRepaintFor(theApp->glob_prefs->shareddir_explicit_list,
		    theApp->glob_prefs->shareddir_recursive_list)) {
		m_ShareSelector->Rebuild();
	}
#endif
}

// File_checked_for_headers
