//
// This file is part of the aMule Project.
//
// Copyright (c) 2004-2011 Angel Vidal ( kry@amule.org )
// Copyright (c) 2003-2011 Patrizio Bassi ( hetfield@amule.org )
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

#include <wx/app.h>

#include "MuleTrayIcon.h"

#include <common/ClientVersion.h>
#include <common/Constants.h>

#include "amule.h"            // Needed for theApp
#include "amuleDlg.h"         // Needed for IsShown
#include "Preferences.h"      // Needed for thePrefs
#include "ServerConnect.h"    // Needed for CServerConnect
#include "Server.h"           // Needed for CServer
#include "Statistics.h"       // Needed for theStats
#include "NetworkFunctions.h" // Needed for Uint32toStringIP
#include "OtherFunctions.h"   // Needed for CastItoXBytes / CastSecondsToHM
#include <common/Format.h>    // Needed for CFormat
#include <common/MenuIDs.h>   // Needed to access menu item constants

#ifdef WITH_LIBAYATANA_APPINDICATOR
// gtk_window_present is the xdg-activation-aware way to request
// focus on Wayland — wxFrame::Raise() alone doesn't reach the
// compositor's activation path there. Needed early so DoShow()
// below can call it.
#include <gtk/gtk.h>
#endif

// =====================================================================
// Common action handlers — invoked from either backend.
// =====================================================================

void CMuleTrayIcon::DoConnectDisconnect()
{
	wxCommandEvent evt;
	theApp->amuledlg->OnBnConnect(evt);
}

void CMuleTrayIcon::DoShowHide()
{
	// Treat an iconized window as not-visible: minimized-to-Dock on
	// Mac, taskbar-iconized on Windows, and Iconize() on Linux all
	// keep IsShown()==true even though the user can't actually see
	// the frame. Acting on plain IsShown() would make the tray menu
	// offer "Hide aMule" in those states, and clicking would make
	// the window vanish entirely (no Dock thumbnail / no taskbar
	// entry) — destructive UX. Treat iconized as "not visible" so
	// the menu offers "Show aMule" and clicking restores the frame.
	const bool visible = theApp->amuledlg->IsVisibleToUser();
	if (visible) {
		theApp->amuledlg->HideToTray();
	} else {
		theApp->amuledlg->RestoreMainWindow();
	}
#ifdef WITH_LIBAYATANA_APPINDICATOR
	// Refresh so the menu label flips to "Hide aMule" / "Show aMule"
	// to match the new window state. SNI menu is static between
	// state changes — without this poke the label would lag a click.
	RebuildMenu();
#endif
}

void CMuleTrayIcon::DoShow()
{
	theApp->amuledlg->RestoreMainWindow();
#ifdef WITH_LIBAYATANA_APPINDICATOR
	// Ask the compositor to bring our window forward. The timestamp
	// matters on Wayland: GNOME Shell's focus-stealing-prevention
	// treats GDK_CURRENT_TIME (0) as suspicious and shows an
	// "app is ready" notification instead of granting focus. Pull
	// the timestamp of the actual menu-click event via
	// gtk_get_current_event_time() so the compositor sees this as
	// a fresh user gesture and honours the request directly. If
	// no event is currently being processed (returns
	// GDK_CURRENT_TIME) we still pass it through — the worst case
	// is the focus-denial notification, which is itself clickable.
	if (GtkWidget *gtkw = static_cast<GtkWidget *>(theApp->amuledlg->GetHandle())) {
		gtk_window_present_with_time(GTK_WINDOW(gtkw), gtk_get_current_event_time());
	}
	RebuildMenu();
#endif
}

void CMuleTrayIcon::DoHide()
{
	theApp->amuledlg->HideToTray();
#ifdef WITH_LIBAYATANA_APPINDICATOR
	RebuildMenu();
#endif
}

void CMuleTrayIcon::DoExit()
{
	if (theApp->amuledlg->IsEnabled()) {
		// Mark as quitting so OnClose skips HideOnClose, but still
		// pass force=false (Close()) so the confirm-exit prompt can
		// run if enabled. If the user answers No there, the prompt
		// vetoes and clears the IsQuitting flag.
		theApp->SetQuitting();
		theApp->amuledlg->Close();
	}
}

void CMuleTrayIcon::DoSetUploadLimit(long kBytesPerSec)
{
	// uint32, not uint16: the preference, its setter and its getter are all
	// uint32, and a 16-bit cast here silently wrapped anything above 65535
	// KiB/s. A preset of 125000 applied as 59464 -- under half the figure on
	// the menu item the user clicked, with nothing logged either side.
	thePrefs::SetMaxUpload(kBytesPerSec < 0 ? UNLIMITED : (uint32)kBytesPerSec);
#ifdef CLIENT_GUI
	theApp->glob_prefs->SendToRemote();
#endif
}

void CMuleTrayIcon::DoSetDownloadLimit(long kBytesPerSec)
{
	// See the note in DoSetUploadLimit.
	thePrefs::SetMaxDownload(kBytesPerSec < 0 ? UNLIMITED : (uint32)kBytesPerSec);
#ifdef CLIENT_GUI
	theApp->glob_prefs->SendToRemote();
#endif
}

// The limit presets both tray backends offer, and their labels.
//
// One ladder, one set of strings. The appindicator backend builds GtkWidgets
// and the wx backend builds a wxMenu, but what goes in them is identical, and
// it used to be written out twice -- which is how the GTK side ended up
// showing "Unlimited" and "KiB/s" untranslated while the wx side translated
// both.
//
// The presets are fractions of the configured line capacity rather than of the
// current limit. Scaling from the limit would mean the menu could only ever
// lower it: with a 50 KiB/s cap in force, every entry would be at or below 50
// and there would be no way back up. Capacity is what the line can do, so
// fifths of it span throttled to full speed in both directions.
namespace
{
// Divisors applied to the line capacity, descending. Not fifths: an even
// ladder can only ever cover one order of magnitude, so on a fast line every
// entry lands high and there is no way to throttle hard from the tray, while
// on a slow one they bunch together near the top. Spacing them out covers
// full speed down to a heavy throttle from the same capacity, which is what
// makes one setting work for a 4 Mbit line and a 200 Mbit one alike.
const unsigned int TRAY_SPEED_DIVISORS[] = { 1, 2, 4, 10, 50 };
const int TRAY_SPEED_PRESETS = (int)(sizeof(TRAY_SPEED_DIVISORS) / sizeof(TRAY_SPEED_DIVISORS[0]));

// What to assume when the user has set their capacity to "unlimited": there is
// nothing to take fractions of otherwise.
const uint32 TRAY_FALLBACK_CAPACITY = 100;

/// Fills @a speeds with the presets in descending order, highest first.
void GetTraySpeedPresets(uint32 capacity, unsigned int (&speeds)[TRAY_SPEED_PRESETS])
{
	if (capacity == UNLIMITED) {
		capacity = TRAY_FALLBACK_CAPACITY;
	}
	// Keep the smallest entry at 1 KiB/s or more: a preset of 0 would read as
	// a limit and act as "unlimited", which is the opposite of what picking
	// the bottom of the list means.
	const uint32 smallest = TRAY_SPEED_DIVISORS[TRAY_SPEED_PRESETS - 1];
	if (capacity < smallest) {
		capacity = smallest;
	}
	for (int i = 0; i < TRAY_SPEED_PRESETS; i++) {
		speeds[i] = (unsigned int)(capacity / TRAY_SPEED_DIVISORS[i]);
	}
}

/// The menu label for one preset, e.g. "2500 KiB/s".
wxString TraySpeedLabel(unsigned int kBytesPerSec)
{
	return CFormat("%u %s") % kBytesPerSec % _("KiB/s");
}
} // namespace

// =====================================================================
// Backend selection — see MuleTrayIcon.h for rationale.
// =====================================================================

#ifdef WITH_LIBAYATANA_APPINDICATOR

// ---------------------------------------------------------------------
//  StatusNotifierItem (SNI) backend via libayatana-appindicator3.
//
//  This is what GNOME Shell with the AppIndicators extension (Ubuntu's
//  default), KDE Plasma, Sway/Hyprland with waybar, and most other
//  modern Linux desktops actually render. The legacy GtkStatusIcon API
//  that wxTaskBarIcon talks was dropped in GNOME 3.26 (2017) and never
//  implemented in wlroots-based compositors, so without this backend
//  the tray icon is silently invisible on most current distros.
// ---------------------------------------------------------------------

#include <libayatana-appindicator/app-indicator.h>
#include <gtk/gtk.h>

namespace
{

// All menu items reach the C++ side through this single callback. The
// item carries two int "action" + "arg" fields via g_object_set_data,
// so we don't need a separate static function per menu entry.
enum TrayAction
{
	TRAY_ACTION_CONNECT_DISCONNECT = 1,
	TRAY_ACTION_SHOW_HIDE,
	TRAY_ACTION_SHOW,
	TRAY_ACTION_HIDE,
	TRAY_ACTION_EXIT,
	TRAY_ACTION_SET_UPLOAD_LIMIT,
	TRAY_ACTION_SET_DOWNLOAD_LIMIT,
};

// Left-click on the indicator. SNI hosts (KDE Plasma, and GNOME via the
// AppIndicator extension) call org.kde.StatusNotifierItem.Activate for the
// primary button; libayatana-appindicator forwards that as this signal.
//
// Signature comes from AppIndicatorClass::activate_event: the x/y of the
// click, which we do not need -- the window goes wherever it already was.
void on_indicator_activate(AppIndicator *, gint, gint, gpointer user_data)
{
	CMuleTrayIcon *tray = static_cast<CMuleTrayIcon *>(user_data);
	// Toggle rather than always-show, matching the Windows tray, where a
	// single left click has flipped show/hide since 3.0.1.
	tray->DoShowHide();
}

void on_menu_item_activated(GtkMenuItem *item, gpointer user_data)
{
	CMuleTrayIcon *tray = static_cast<CMuleTrayIcon *>(user_data);
	intptr_t action = reinterpret_cast<intptr_t>(g_object_get_data(G_OBJECT(item), "action"));
	intptr_t arg = reinterpret_cast<intptr_t>(g_object_get_data(G_OBJECT(item), "arg"));

	switch (action) {
	case TRAY_ACTION_CONNECT_DISCONNECT:
		tray->DoConnectDisconnect();
		break;
	case TRAY_ACTION_SHOW_HIDE:
		tray->DoShowHide();
		break;
	case TRAY_ACTION_SHOW:
		tray->DoShow();
		break;
	case TRAY_ACTION_HIDE:
		tray->DoHide();
		break;
	case TRAY_ACTION_EXIT:
		tray->DoExit();
		break;
	case TRAY_ACTION_SET_UPLOAD_LIMIT:
		tray->DoSetUploadLimit((long)arg);
		break;
	case TRAY_ACTION_SET_DOWNLOAD_LIMIT:
		tray->DoSetDownloadLimit((long)arg);
		break;
	}
}

GtkWidget *make_action_item(const char *label, TrayAction action, long arg, gpointer user_data)
{
	GtkWidget *item = gtk_menu_item_new_with_label(label);
	g_object_set_data(
		G_OBJECT(item), "action", reinterpret_cast<gpointer>(static_cast<intptr_t>(action)));
	g_object_set_data(G_OBJECT(item), "arg", reinterpret_cast<gpointer>(static_cast<intptr_t>(arg)));
	g_signal_connect(item, "activate", G_CALLBACK(on_menu_item_activated), user_data);
	return item;
}

GtkWidget *make_speed_submenu(uint32 max_speed, TrayAction action, gpointer user_data)
{
	GtkWidget *submenu = gtk_menu_new();
	gtk_menu_shell_append(GTK_MENU_SHELL(submenu),
		make_action_item(wxString(_("Unlimited")).utf8_str(), action, -1, user_data));
	unsigned int speeds[TRAY_SPEED_PRESETS];
	GetTraySpeedPresets(max_speed, speeds);
	for (int i = 0; i < TRAY_SPEED_PRESETS; i++) {
		gtk_menu_shell_append(GTK_MENU_SHELL(submenu),
			make_action_item(
				TraySpeedLabel(speeds[i]).utf8_str(), action, (long)speeds[i], user_data));
	}
	gtk_widget_show_all(submenu);
	return submenu;
}

// Append a non-clickable "info" label to the menu. SNI menus support
// disabled items, but rendering varies between desktops — KDE shows
// them grey, GNOME-with-AppIndicators shows them in the menu's normal
// style. Either way they're not interactive.
void append_info(GtkWidget *menu, const wxString &text)
{
	GtkWidget *item = gtk_menu_item_new_with_label((const char *)text.utf8_str());
	gtk_widget_set_sensitive(item, FALSE);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

} // anonymous namespace

CMuleTrayIcon::CMuleTrayIcon()
: m_indicator(nullptr)
, m_menu(nullptr)
, m_lastIconState(-1)
{
	// `org.amule.aMule` is both the AppStream/.desktop id and the icon
	// name installed under share/icons/hicolor/*/apps/. AppIndicator3
	// looks the icon up via the standard XDG icon-theme path.
	//
	// AyatanaIndicators upstream split the library into
	// libayatana-appindicator-glib (GLib-only, GMenu-based, no GTK
	// dep) and started emitting a deprecation warning when the
	// GTK-based libayatana-appindicator3-0.1 is loaded. Migrating
	// to the new library is tracked as future work — -glib isn't
	// yet packaged on Ubuntu / Fedora / openSUSE / Debian, so
	// switching today would lock out every major distro. The
	// warning is harmless console noise; silence it locally so a
	// project-wide -Werror=deprecated-declarations stays useful.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	m_indicator = app_indicator_new(
		"org.amule.aMule", "org.amule.aMule", APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
#pragma GCC diagnostic pop

	// ACTIVE = visible. The user already opted in by enabling the tray
	// icon in Preferences, so showing it immediately is the expected
	// behaviour; SetTrayIcon below only updates the menu's
	// Connect/Disconnect label and never re-hides the indicator.
	// (We deliberately don't use APP_INDICATOR_STATUS_ATTENTION for
	// the disconnected state — that requires a separately-set
	// attention icon via app_indicator_set_attention_icon_full(), and
	// without it some SNI hosts render the indicator as invisible.)
	app_indicator_set_status(m_indicator, APP_INDICATOR_STATUS_ACTIVE);
	app_indicator_set_title(m_indicator, "aMule");

	// Left-click opens the main window instead of the menu, where the
	// installed library can tell us about it. libayatana-appindicator only
	// grew an Activate handler in 0.6.0; before that the primary click was
	// not exposed at all, which is why the tray has behaved this way on Linux
	// since the SNI backend landed (discussion #1115).
	//
	// Looked up at RUNTIME rather than behind a build-time version check,
	// because the two genuinely differ: a distro may ship a newer library
	// than we built against, and our AppImage and Flatpak bundle their own.
	// On an older library the lookup returns 0, nothing is connected, and the
	// panel keeps opening the menu exactly as it does today. The library
	// handles that direction too, answering the Activate D-Bus call with an
	// error when no handler is connected so the host falls back to the menu.
	// Nothing is logged in that case: a debug line compiles out of release
	// builds, which is precisely where this would matter.
	//
	// APP_INDICATOR_TYPE, not G_OBJECT_TYPE(m_indicator): the latter is a raw
	// dereference and app_indicator_new can return NULL. The GObject setters
	// above only warn on NULL, so this must not be the line that crashes.
	g_type_class_ref(APP_INDICATOR_TYPE);
	if (m_indicator && g_signal_lookup("activate", APP_INDICATOR_TYPE)) {
		g_signal_connect(m_indicator, "activate", G_CALLBACK(on_indicator_activate), this);
	}

	RebuildMenu();
}

CMuleTrayIcon::~CMuleTrayIcon()
{
	if (m_indicator) {
		g_object_unref(m_indicator);
		m_indicator = nullptr;
	}
	// m_menu is owned by the indicator (set_menu took the floating ref)
	// so we don't unref it explicitly.
}

void CMuleTrayIcon::SetTrayIcon(int Icon, uint32 /*percent*/)
{
	// SNI doesn't support per-frame icon overlays — the percent bar
	// from the legacy backend is dropped on this path. We reflect the
	// connection state purely through the menu (the Connect /
	// Disconnect item label flips), not via the indicator's status,
	// because flipping ACTIVE↔ATTENTION can hide the indicator on
	// some hosts when no attention icon is configured.
	if (Icon != m_lastIconState) {
		m_lastIconState = Icon;
		// Rebuild so Connect/Disconnect label reflects current state.
		RebuildMenu();
	}
}

void CMuleTrayIcon::SetTrayToolTip(const wxString &Tip)
{
	// SNI doesn't surface tooltips on hover (compositors disagree on
	// whether to render them). Use it as the accessible title — screen
	// readers and KDE's hover popup pick it up.
	app_indicator_set_title(m_indicator, (const char *)Tip.utf8_str());
}

void CMuleTrayIcon::RebuildMenu()
{
	// Static layout, rebuilt only on connection-state changes
	// (driven by SetTrayIcon below). app_indicator_set_menu posts a
	// dbusmenu LayoutUpdated D-Bus signal which some SNI hosts react
	// to with a brief icon redraw — so refreshing on a 2 s timer
	// would visibly flicker. Keeping the menu lean (action items
	// only, no live stats) means we rebuild only when state actually
	// changes, eliminating the flicker. Live values like download /
	// upload speed are visible in the main aMule window.
	GtkWidget *menu = gtk_menu_new();

	// ---- Version banner ------------------------------------------
	append_info(menu, MOD_VERSION_LONG);

	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

	// Show / Hide. On a Wayland session we can't reliably detect that
	// the window has been iconized by the OS minimize button (xdg-shell
	// doesn't deliver the event), so a single toggle entry would mis-
	// label itself in that state. Show two deterministic entries
	// instead — click Show to bring the window back, click Hide to
	// hide it. On X11 / macOS / Windows the toggle is reliable, so
	// the single label-aware entry stays.
	if (CamuleAppCommon::IsWaylandSession()) {
		gtk_menu_shell_append(GTK_MENU_SHELL(menu),
			make_action_item((const char *)wxString(_("Show aMule")).utf8_str(),
				TRAY_ACTION_SHOW,
				0,
				this));
		gtk_menu_shell_append(GTK_MENU_SHELL(menu),
			make_action_item((const char *)wxString(_("Hide aMule")).utf8_str(),
				TRAY_ACTION_HIDE,
				0,
				this));
	} else {
		// Treat iconized as not visible — see DoShowHide for rationale.
		const bool visible = theApp->amuledlg && theApp->amuledlg->IsShown() &&
				     !theApp->amuledlg->IsTrayLogicallyIconized();
		const wxString label = visible ? wxString(_("Hide aMule")) : wxString(_("Show aMule"));
		gtk_menu_shell_append(GTK_MENU_SHELL(menu),
			make_action_item((const char *)label.utf8_str(), TRAY_ACTION_SHOW_HIDE, 0, this));
	}

	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

	// ---- Client information submenu ------------------------------
	// Snapshot at the moment of the last connection-state change.
	// Skips truly-live fields (uptime, totals, queued clients) so the
	// menu can stay static between state changes — putting those in
	// would force a periodic rebuild and bring back the flicker.
	{
		GtkWidget *sub = gtk_menu_new();

		// ED2k status
		{
			wxString s = _("eD2k: ");
			if (theApp->IsConnectedED2K()) {
				s += theApp->IsFirewalled() ? wxString(_("Connected (LowID)"))
							    : wxString(_("Connected (HighID)"));
			} else {
				s += _("Disconnected");
			}
			append_info(sub, s);
		}

		// Kad status
		{
			wxString s = _("Kad: ");
			if (theApp->IsConnectedKad()) {
				s += theApp->IsFirewalledKad() ? wxString(_("Connected (firewalled)"))
							       : wxString(_("Connected"));
			} else {
				s += _("Disconnected");
			}
			append_info(sub, s);
		}

		// Server identity (only meaningful while connected)
		{
			wxString name = _("Server: ");
			wxString ip = _("Server IP: ");
			if (theApp->serverconnect->GetCurrentServer()) {
				name += theApp->serverconnect->GetCurrentServer()->GetListName();
				ip += theApp->serverconnect->GetCurrentServer()->GetFullIP();
			} else {
				name += _("Not connected");
				ip += _("Not connected");
			}
			append_info(sub, name);
			append_info(sub, ip);
		}

		// Public IP — populated post-connect
		append_info(sub,
			CFormat(_("IP: %s")) % (theApp->GetPublicIP()
							       ? Uint32toStringIP(theApp->GetPublicIP())
							       : wxString(_("Unknown"))));

		// Listen ports — change only on prefs save
		append_info(sub,
			thePrefs::GetPort() ? wxString(CFormat(_("TCP port: %d")) % thePrefs::GetPort())
					    : wxString(_("TCP port: Not ready")));

		append_info(sub,
			thePrefs::GetEffectiveUDPPort()
				? wxString(CFormat(_("UDP port: %d")) % thePrefs::GetEffectiveUDPPort())
				: wxString(_("UDP port: Not ready")));

		gtk_widget_show_all(sub);
		GtkWidget *item = gtk_menu_item_new_with_label(
			(const char *)wxString(_("Client Information")).utf8_str());
		gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), sub);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	}

	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

	// ---- Action items --------------------------------------------

	// Upload limit submenu
	{
		uint32 max_ul = thePrefs::GetMaxGraphUploadRate();
		GtkWidget *sub = make_speed_submenu(max_ul, TRAY_ACTION_SET_UPLOAD_LIMIT, this);
		GtkWidget *item = gtk_menu_item_new_with_label(_("Upload limit").utf8_str());
		gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), sub);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	}

	// Download limit submenu
	{
		uint32 max_dl = thePrefs::GetMaxGraphDownloadRate();
		GtkWidget *sub = make_speed_submenu(max_dl, TRAY_ACTION_SET_DOWNLOAD_LIMIT, this);
		GtkWidget *item = gtk_menu_item_new_with_label(_("Download limit").utf8_str());
		gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), sub);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	}

	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

	// Connect / Disconnect — label depends on current connection state.
	{
		const wxString label =
			theApp->IsConnected() ? wxString(_("Disconnect")) : wxString(_("Connect"));
		gtk_menu_shell_append(GTK_MENU_SHELL(menu),
			make_action_item(
				(const char *)label.utf8_str(), TRAY_ACTION_CONNECT_DISCONNECT, 0, this));
	}

	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

	gtk_menu_shell_append(GTK_MENU_SHELL(menu),
		make_action_item((const char *)wxString(_("Exit")).utf8_str(), TRAY_ACTION_EXIT, 0, this));

	gtk_widget_show_all(menu);

	// app_indicator_set_menu sinks the floating ref and unrefs any
	// previously-set menu, so we can replace it on every rebuild
	// without leaking.
	app_indicator_set_menu(m_indicator, GTK_MENU(menu));
	m_menu = menu;
}

#else // !WITH_LIBAYATANA_APPINDICATOR

// ---------------------------------------------------------------------
//  Legacy wxTaskBarIcon backend. Works correctly on Windows
//  (NOTIFYICONDATA), macOS (NSStatusItem), and on X11 desktops that
//  still consume GtkStatusIcon. On modern Wayland desktops the icon
//  goes nowhere — build with libayatana-appindicator3 to fix that.
// ---------------------------------------------------------------------

#include <algorithm> // Needed for std::max / std::min

#include <wx/artprov.h> // Needed for wxArtProvider::GetBitmap

#include <wx/menu.h>

#include "StatisticsDlg.h" // Needed for CStatisticsDlg::getColors()

/****************************************************/
/******************* Event Table ********************/
/****************************************************/

wxBEGIN_EVENT_TABLE(CMuleTrayIcon, wxTaskBarIcon)
#ifdef __WINDOWS__
	// Windows convention: single-left click on the tray icon toggles
	// the primary action (show/hide the main window). NSStatusItem
	// on macOS opens the menu on single-click via its own default,
	// so leave that path alone.
	EVT_TASKBAR_LEFT_UP(CMuleTrayIcon::SwitchShow)
#endif
	EVT_TASKBAR_LEFT_DCLICK(CMuleTrayIcon::SwitchShow)
	EVT_MENU(TRAY_MENU_EXIT, CMuleTrayIcon::Close)
	EVT_MENU(TRAY_MENU_CONNECT, CMuleTrayIcon::ServerConnection)
	EVT_MENU(TRAY_MENU_DISCONNECT, CMuleTrayIcon::ServerConnection)
	EVT_MENU(TRAY_MENU_HIDE, CMuleTrayIcon::ShowHide)
	EVT_MENU(TRAY_MENU_SHOW, CMuleTrayIcon::ShowHide)
	EVT_MENU(UPLOAD_ITEM1, CMuleTrayIcon::SetUploadSpeed)
	EVT_MENU(UPLOAD_ITEM2, CMuleTrayIcon::SetUploadSpeed)
	EVT_MENU(UPLOAD_ITEM3, CMuleTrayIcon::SetUploadSpeed)
	EVT_MENU(UPLOAD_ITEM4, CMuleTrayIcon::SetUploadSpeed)
	EVT_MENU(UPLOAD_ITEM5, CMuleTrayIcon::SetUploadSpeed)
	EVT_MENU(UPLOAD_ITEM6, CMuleTrayIcon::SetUploadSpeed)
	EVT_MENU(DOWNLOAD_ITEM1, CMuleTrayIcon::SetDownloadSpeed)
	EVT_MENU(DOWNLOAD_ITEM2, CMuleTrayIcon::SetDownloadSpeed)
	EVT_MENU(DOWNLOAD_ITEM3, CMuleTrayIcon::SetDownloadSpeed)
	EVT_MENU(DOWNLOAD_ITEM4, CMuleTrayIcon::SetDownloadSpeed)
	EVT_MENU(DOWNLOAD_ITEM5, CMuleTrayIcon::SetDownloadSpeed)
	EVT_MENU(DOWNLOAD_ITEM6, CMuleTrayIcon::SetDownloadSpeed)
wxEND_EVENT_TABLE()

/****************************************************/
/************ Constructor / Destructor **************/
/****************************************************/

static long GetSpeedFromString(wxString label)
{
	long temp;
	label.Replace(_("KiB/s"), "", TRUE);
	label.Trim(FALSE);
	label.Trim(TRUE);
	label.ToLong(&temp);
	return temp;
}

void CMuleTrayIcon::SetUploadSpeed(wxCommandEvent &event)
{

	wxObject *obj = event.GetEventObject();
	if (obj != NULL) {
		wxMenu *menu = dynamic_cast<wxMenu *>(obj);
		if (menu) {
			wxMenuItem *item = menu->FindItem(event.GetId());
			if (item != NULL) {
				if (item->GetItemLabelText() == (_("Unlimited"))) {
					DoSetUploadLimit(-1);
				} else {
					DoSetUploadLimit(GetSpeedFromString(item->GetItemLabelText()));
				}
			}
		}
	}
}

void CMuleTrayIcon::SetDownloadSpeed(wxCommandEvent &event)
{

	wxObject *obj = event.GetEventObject();
	if (obj != NULL) {
		wxMenu *menu = dynamic_cast<wxMenu *>(obj);
		if (menu) {
			wxMenuItem *item = menu->FindItem(event.GetId());
			if (item != NULL) {
				if (item->GetItemLabelText() == (_("Unlimited"))) {
					DoSetDownloadLimit(-1);
				} else {
					DoSetDownloadLimit(GetSpeedFromString(item->GetItemLabelText()));
				}
			}
		}
	}
}

void CMuleTrayIcon::ServerConnection(wxCommandEvent &WXUNUSED(event))
{
	DoConnectDisconnect();
}

void CMuleTrayIcon::ShowHide(wxCommandEvent &WXUNUSED(event))
{
	DoShowHide();
}

void CMuleTrayIcon::Close(wxCommandEvent &WXUNUSED(event))
{
	DoExit();
}

CMuleTrayIcon::CMuleTrayIcon()
{
	Old_Icon = -1;
	Old_SpeedSize = 0xFFFF; // must be > any possible one.
}

CMuleTrayIcon::~CMuleTrayIcon() {}

/****************************************************/
/***************** Public Functions *****************/
/****************************************************/

void CMuleTrayIcon::SetTrayIcon(int Icon, uint32 percent)
{
	wxASSERT(Icon >= 0 && Icon < (int)WXSIZEOF(BaseImage));

	if (!BaseImage[Icon].IsOk()) {
		const char *artId = "amule:tray_disconnected";
		switch (Icon) {
		case TRAY_ICON_HIGHID:
			artId = "amule:tray_high";
			break;
		case TRAY_ICON_LOWID:
			artId = "amule:tray_low";
			break;
		default:
			break;
		}
		wxImage image = wxArtProvider::GetBitmap(artId, wxART_OTHER).ConvertToImage();
		if (!image.IsOk()) {
			return;
		}
		// Artwork that predates alpha marks its transparency with pure red.
		// Turn that into a real alpha channel once, here, so everything below
		// this point works on one representation.
		if (!image.HasAlpha()) {
			image.SetMaskColour(255, 0, 0);
			image.InitAlpha();
		}
		BaseImage[Icon] = image;
	}

	const wxImage &base = BaseImage[Icon];
	const int width = base.GetWidth();
	const int height = base.GetHeight();
	const int barHeight = (height * (int)percent) / 100;

	if ((Old_Icon == Icon) && (Old_SpeedSize == barHeight)) {
		return;
	}
	Old_Icon = Icon;
	Old_SpeedSize = barHeight;

	// Always compose onto a copy of the untouched artwork. Composing onto the
	// icon we produced last time loses the transparency a little more each
	// round: converting an icon back to a bitmap returns its transparent
	// pixels as black, so a rising transfer rate -- which never takes the
	// rebuild-from-scratch path -- ends up with a solid black background.
	//
	// The bar is written into the image rather than drawn with a wxDC because
	// wxMSW's DC writes the colour channels and leaves alpha alone: a bar
	// drawn over transparent pixels keeps alpha 0 and never appears, which is
	// what happens to any artwork that carries its own alpha channel.
	wxImage composed = base.Copy();
	if (barHeight > 0) {
		const wxColour barColour = CStatisticsDlg::getColors(11);
		// Two pixels wide, in the last two columns, growing from the bottom.
		// It used to sit two columns further in, which the artwork of the day
		// left clear. Newer artwork fills more of its canvas, so the bar landed
		// on the drawing instead of beside it; the outermost columns are the
		// ones an icon is least likely to use.
		const int barLeft = std::max(0, width - 2);
		const int barRight = std::min(width, barLeft + 2);
		for (int y = height - barHeight; y < height; ++y) {
			for (int x = barLeft; x < barRight; ++x) {
				composed.SetRGB(x, y, barColour.Red(), barColour.Green(), barColour.Blue());
				composed.SetAlpha(x, y, wxALPHA_OPAQUE);
			}
		}
	}

	CurrentIcon.CopyFromBitmap(wxBitmap(composed));
	UpdateTray();
}

void CMuleTrayIcon::SetTrayToolTip(const wxString &Tip)
{
	CurrentTip = Tip;
	UpdateTray();
}

/****************************************************/
/**************** Private Functions *****************/
/****************************************************/

void CMuleTrayIcon::UpdateTray()
{
	// Icon update and Tip update
#ifndef __WXCOCOA__
	if (IsOk())
#endif
	{
		SetIcon(CurrentIcon, CurrentTip);
	}
}

wxMenu *CMuleTrayIcon::CreatePopupMenu()
{
	float kBpsUp = theStats::GetUploadRate() / 1024.0;
	float kBpsDown = theStats::GetDownloadRate() / 1024.0;
	float MBpsUp = kBpsUp / 1024.0;
	float MBpsDown = kBpsDown / 1024.0;
	bool showMBpsUp = (MBpsUp >= 1);
	bool showMBpsDown = (MBpsDown >= 1);

	// Dynamically creates the menu to show the user.
	wxMenu *traymenu = new wxMenu();
	traymenu->SetTitle(_("aMule Tray Menu"));

	// Build the Top string name
	wxString label = MOD_VERSION_LONG;
	traymenu->Append(TRAY_MENU_INFO, label);
	traymenu->AppendSeparator();

	// Treat iconized as not visible — see DoShowHide for rationale.
	if (theApp->amuledlg->IsShown() && !theApp->amuledlg->IsTrayLogicallyIconized()) {
		traymenu->Append(TRAY_MENU_HIDE, _("Hide aMule"));
	} else {
		traymenu->Append(TRAY_MENU_SHOW, _("Show aMule"));
	}

	// Separator
	traymenu->AppendSeparator();

	label = wxString(_("Speed limits:")) + " ";

	// Check for upload limits
	unsigned int max_upload = thePrefs::GetMaxUpload();
	if (max_upload == UNLIMITED) {
		label += _("UL: None");
	} else {
		label += CFormat(_("UL: %u")) % max_upload;
	}
	label += ", ";

	// Check for download limits
	unsigned int max_download = thePrefs::GetMaxDownload();
	if (max_download == UNLIMITED) {
		label += _("DL: None");
	} else {
		label += CFormat(_("DL: %u")) % max_download;
	}

	traymenu->Append(TRAY_MENU_INFO, label);
	label = CFormat(_("Download speed: %.1f%s")) % (showMBpsDown ? MBpsDown : kBpsDown) %
		(showMBpsDown ? _(" MiB/s") : ((kBpsDown > 0) ? _(" KiB/s") : ""));
	traymenu->Append(TRAY_MENU_INFO, label);
	label = CFormat(_("Upload speed: %.1f%s")) % (showMBpsUp ? MBpsUp : kBpsUp) %
		(showMBpsUp ? _(" MiB/s") : ((kBpsUp > 0) ? _(" KiB/s") : ""));
	traymenu->Append(TRAY_MENU_INFO, label);
	traymenu->AppendSeparator();

	// Client Info
	wxMenu *ClientInfoMenu = new wxMenu();
	ClientInfoMenu->SetTitle(_("Client Information"));

	// User nick-name
	{
		wxString temp = CFormat(_("Nickname: %s")) %
				(thePrefs::GetUserNick().IsEmpty() ? wxString(_("No Nickname Selected!"))
								   : thePrefs::GetUserNick());

		ClientInfoMenu->Append(TRAY_MENU_CLIENTINFO_ITEM, temp);
	}

	// Client ID
	{
		wxString temp = _("ClientID: ");

		if (theApp->IsConnectedED2K()) {
			temp += CFormat("%u") % theApp->GetED2KID();
		} else {
			temp += _("Not connected");
		}
		ClientInfoMenu->Append(TRAY_MENU_CLIENTINFO_ITEM, temp);
	}

	// Current Server and Server IP
	{
		wxString temp_name = _("ServerName: ");
		wxString temp_ip = _("ServerIP: ");

		if (theApp->serverconnect->GetCurrentServer()) {
			temp_name += theApp->serverconnect->GetCurrentServer()->GetListName();
			temp_ip += theApp->serverconnect->GetCurrentServer()->GetFullIP();
		} else {
			temp_name += _("Not connected");
			temp_ip += _("Not Connected");
		}
		ClientInfoMenu->Append(TRAY_MENU_CLIENTINFO_ITEM, temp_name);
		ClientInfoMenu->Append(TRAY_MENU_CLIENTINFO_ITEM, temp_ip);
	}

	// IP Address
	{
		wxString temp = CFormat(_("IP: %s")) %
				((theApp->GetPublicIP()) ? Uint32toStringIP(theApp->GetPublicIP())
							 : wxString(_("Unknown")));

		ClientInfoMenu->Append(TRAY_MENU_CLIENTINFO_ITEM, temp);
	}

	// TCP PORT
	{
		wxString temp;
		if (thePrefs::GetPort()) {
			temp = CFormat(_("TCP port: %d")) % thePrefs::GetPort();
		} else {
			temp = _("TCP port: Not ready");
		}
		ClientInfoMenu->Append(TRAY_MENU_CLIENTINFO_ITEM, temp);
	}

	// UDP PORT
	{
		wxString temp;
		if (thePrefs::GetEffectiveUDPPort()) {
			temp = CFormat(_("UDP port: %d")) % thePrefs::GetEffectiveUDPPort();
		} else {
			temp = _("UDP port: Not ready");
		}
		ClientInfoMenu->Append(TRAY_MENU_CLIENTINFO_ITEM, temp);
	}

	// Online Signature
	{
		wxString temp;
		if (thePrefs::IsOnlineSignatureEnabled()) {
			temp = _("Online Signature: Enabled");
		} else {
			temp = _("Online Signature: Disabled");
		}
		ClientInfoMenu->Append(TRAY_MENU_CLIENTINFO_ITEM, temp);
	}

	// Uptime
	{
		wxString temp = CFormat(_("Uptime: %s")) % CastSecondsToHM(theStats::GetUptimeSeconds());
		ClientInfoMenu->Append(TRAY_MENU_CLIENTINFO_ITEM, temp);
	}

	// Number of shared files
	{
		wxString temp = CFormat(_("Shared files: %d")) % theStats::GetSharedFileCount();
		ClientInfoMenu->Append(TRAY_MENU_CLIENTINFO_ITEM, temp);
	}

	// Number of queued clients
	{
		wxString temp = CFormat(_("Queued clients: %d")) % theStats::GetWaitingUserCount();
		ClientInfoMenu->Append(TRAY_MENU_CLIENTINFO_ITEM, temp);
	}

	// Total Downloaded
	{
		wxString temp = CastItoXBytes(theStats::GetTotalReceivedBytes());
		temp = CFormat(_("Total DL: %s")) % temp;
		ClientInfoMenu->Append(TRAY_MENU_CLIENTINFO_ITEM, temp);
	}

	// Total Uploaded
	{
		wxString temp = CastItoXBytes(theStats::GetTotalSentBytes());
		temp = CFormat(_("Total UL: %s")) % temp;
		ClientInfoMenu->Append(TRAY_MENU_CLIENTINFO_ITEM, temp);
	}

	traymenu->Append(TRAY_MENU_CLIENTINFO, ClientInfoMenu->GetTitle(), ClientInfoMenu);

	// Separator
	traymenu->AppendSeparator();

	// Upload Speed sub-menu
	wxMenu *UploadSpeedMenu = new wxMenu();
	UploadSpeedMenu->SetTitle(_("Upload limit"));

	// Download Speed sub-menu
	wxMenu *DownloadSpeedMenu = new wxMenu();
	DownloadSpeedMenu->SetTitle(_("Download limit"));

	// Upload Speed sub-menu
	{
		UploadSpeedMenu->Append(UPLOAD_ITEM1, _("Unlimited"));

		unsigned int speeds[TRAY_SPEED_PRESETS];
		GetTraySpeedPresets(thePrefs::GetMaxGraphUploadRate(), speeds);
		for (int i = 0; i < TRAY_SPEED_PRESETS; i++) {
			UploadSpeedMenu->Append((int)UPLOAD_ITEM1 + i + 1, TraySpeedLabel(speeds[i]));
		}
	}
	traymenu->Append(0, UploadSpeedMenu->GetTitle(), UploadSpeedMenu);

	// Download Speed sub-menu
	{
		DownloadSpeedMenu->Append(DOWNLOAD_ITEM1, _("Unlimited"));

		unsigned int speeds[TRAY_SPEED_PRESETS];
		GetTraySpeedPresets(thePrefs::GetMaxGraphDownloadRate(), speeds);
		for (int i = 0; i < TRAY_SPEED_PRESETS; i++) {
			DownloadSpeedMenu->Append((int)DOWNLOAD_ITEM1 + i + 1, TraySpeedLabel(speeds[i]));
		}
	}

	traymenu->Append(0, DownloadSpeedMenu->GetTitle(), DownloadSpeedMenu);
	// Separator
	traymenu->AppendSeparator();

	if (theApp->IsConnected()) {
		// Disconnection Speed item
		traymenu->Append(TRAY_MENU_DISCONNECT, _("Disconnect"));
	} else {
		// Connect item
		traymenu->Append(TRAY_MENU_CONNECT, _("Connect"));
	}

	// Separator
	traymenu->AppendSeparator();

	// Exit item
	traymenu->Append(TRAY_MENU_EXIT, _("Exit"));

	return traymenu;
}

void CMuleTrayIcon::SwitchShow(wxTaskBarIconEvent &)
{
	DoShowHide();
}

#endif // !WITH_LIBAYATANA_APPINDICATOR

// File_checked_for_headers
