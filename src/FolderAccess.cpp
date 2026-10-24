// This file is part of aMule. Licensed under GPL-2.0-or-later.
#include "FolderAccess.h"

#include <wx/fileconf.h>
#include <wx/file.h>
#include <wx/mstream.h>
#include <wx/log.h>
#include "CamuleFileConfig.h"
#include <wx/filename.h>
#ifdef __linux__
#include <sys/xattr.h>
#endif
#include <wx/utils.h>
#include "Preferences.h"
#ifndef CLIENT_GUI
#include "amule.h"
#endif
#include "FolderWriteSandbox.h"

#ifndef AMULE_DAEMON
#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/dirdlg.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/timer.h>
#ifdef HAVE_GIO
#include <gio/gio.h>
#endif
#endif

namespace FolderAccess
{
bool IsSandboxed()
{
	return wxFileExists("/.flatpak-info") || wxGetEnv("FLATPAK_ID", nullptr);
}

namespace
{
template <typename Writer> bool AtomicProfileWrite(const wxString &path, const Writer &write)
{
	wxFile file;
	// mkstemp creates the file owner-only; wxTempFile would then try to chmod it.
	const wxString temporary = wxFileName::CreateTempFileName(path + ".tmp.", &file);
	if (temporary.empty()) {
		return false;
	}
	bool ok = write(file) && file.Flush();
	ok = file.Close() && ok;
	if (ok) {
		ok = wxRenameFile(temporary, path, true);
	}
	if (!ok) {
		wxRemoveFile(temporary);
		wxLogError("Could not save profile file: %s", path);
	}
	return ok;
}
} // namespace

bool SaveConfig(wxFileConfig &config, const wxString &path, const wxMBConv &conversion)
{
	wxMemoryOutputStream contents;
	if (!config.Save(contents, conversion)) {
		return false;
	}
	const size_t length = contents.GetSize();
	std::vector<char> bytes(length);
	if (length != 0) {
		contents.CopyTo(bytes.data(), length);
	}
	return AtomicProfileWrite(path,
		[&](wxFile &file) { return length == 0 || file.Write(bytes.data(), length) == length; });
}

bool BackupConfig(const wxString &path)
{
	return CPath::BackupFile(CPath(path), ".bak");
}

wxString HostPath(const wxString &path)
{
#ifdef __linux__
	const auto filename = path.fn_str();
	const char *attribute = "user.document-portal.host-path";
	const ssize_t length = getxattr(filename, attribute, nullptr, 0);
	if (length > 0 && length < 1024 * 1024) {
		std::vector<char> value(static_cast<size_t>(length) + 1, 0);
		if (getxattr(filename, attribute, value.data(), static_cast<size_t>(length)) == length) {
			return wxString::FromUTF8(value.data());
		}
	}
#endif
	wxFileConfig config("", "", thePrefs::GetConfigDir() + "folder-access.ini");
	long count = 0;
	config.Read("Count", &count, 0);
	for (long i = 0; i < count; ++i) {
		wxString folder, host;
		if (config.Read(wxString::Format("Folder%ld", i), &folder) &&
			config.Read(wxString::Format("HostFolder%ld", i), &host) &&
			CPath(path).StartsWith(CPath(folder))) {
			return host + path.Mid(folder.length());
		}
	}
	wxFileName normalized = wxFileName::DirName(path);
	normalized.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_TILDE | wxPATH_NORM_ABSOLUTE);
	return normalized.GetPath();
}

std::vector<CPath> ApprovedFolders()
{
	wxFileConfig config("", "", thePrefs::GetConfigDir() + "folder-access.ini");
	std::vector<CPath> folders;
	long count = 0;
	config.Read("Count", &count, 0);
	for (long i = 0; i < count; ++i) {
		wxString path;
		if (config.Read(wxString::Format("Folder%ld", i), &path) && !path.empty()) {
			folders.emplace_back(path);
		}
	}
	return folders;
}

namespace
{
bool writesRestricted = false;
std::vector<CPath> writableFolders;

bool IsWritable(const CPath &path)
{
	for (const auto &folder : writableFolders) {
		if (path.StartsWith(folder)) {
			return true;
		}
	}
	return false;
}
} // namespace

bool RestrictWrites(wxString &error)
{
	if (!IsSandboxed() || writesRestricted) {
		return true;
	}
	writableFolders.clear();
	writableFolders.emplace_back(thePrefs::GetConfigDir());
	writableFolders.emplace_back(thePrefs::GetTempDir());
	writableFolders.emplace_back(thePrefs::GetIncomingDir());
#ifndef CLIENT_GUI
	for (unsigned i = 1; i < theApp->glob_prefs->GetCatCount(); ++i) {
		writableFolders.push_back(theApp->glob_prefs->GetCatPath(i));
	}
#endif
	wxFileConfig config("", "", thePrefs::GetConfigDir() + "folder-access.ini");
	long count = 0;
	config.Read("Count", &count, 0);
	for (long i = 0; i < count; ++i) {
		// Keep permissions paired with their original registry entry, even when
		// another entry has an empty or missing path.
		wxString path;
		if (config.ReadBool(wxString::Format("Writable%ld", i), false) &&
			config.Read(wxString::Format("Folder%ld", i), &path) && !path.empty()) {
			writableFolders.emplace_back(path);
		}
	}
	// Flatpak provides private app storage and a private /tmp. These are required
	// for wxGTK caches, temporary files, helper processes and normal device I/O.
	for (const char *name : { "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME" }) {
		wxString path;
		if (wxGetEnv(name, &path) && !path.empty()) {
			wxFileName::Mkdir(path, 0700, wxPATH_MKDIR_FULL);
			writableFolders.emplace_back(path);
		}
	}
	writableFolders.emplace_back("/tmp");
	writableFolders.emplace_back("/dev");
	wxString runtime;
	if (wxGetEnv("XDG_RUNTIME_DIR", &runtime)) {
		wxString app;
		wxGetEnv("FLATPAK_ID", &app);
		writableFolders.emplace_back(runtime + "/app/" + app);
	}
	std::vector<std::string> roots;
	for (const auto &folder : writableFolders) {
		roots.emplace_back(folder.GetRaw().utf8_str().data());
	}
	std::string reason;
	if (!FolderWriteSandbox::Restrict(roots, reason)) {
		error = wxString::FromUTF8(reason);
		return false;
	}
	writesRestricted = true;
	return true;
}

#ifndef AMULE_DAEMON
namespace
{
void Remember(const wxString &path, bool writable)
{
	CamuleFileConfig config("", "", thePrefs::GetConfigDir() + "folder-access.ini");
	config.SetUmask(0077);
	long count = 0;
	config.Read("Count", &count, 0);
	for (long i = 0; i < count; ++i) {
		wxString folder;
		if (config.Read(wxString::Format("Folder%ld", i), &folder) && CPath(folder) == CPath(path)) {
			if (writable) {
				config.Write(wxString::Format("Writable%ld", i), true);
				config.Flush();
			}
			return;
		}
	}
	config.Write(wxString::Format("Folder%ld", count), path);
	config.Write(wxString::Format("HostFolder%ld", count), HostPath(path));
	config.Write(wxString::Format("Writable%ld", count), writable);
	config.Write("Count", count + 1);
	config.Flush();
}

#ifdef HAVE_GIO
// The subscription is established before OpenFile to handle portals which respond
// before the method reply. Use a private context so callbacks cannot outlive state.
struct Selection
{
	wxString request;
	wxString path;
	bool done = false;
};

void Response(GDBusConnection *,
	const gchar *,
	const gchar *objectPath,
	const gchar *,
	const gchar *,
	GVariant *parameters,
	gpointer data)
{
	auto &selection = *static_cast<Selection *>(data);
	if (selection.request != wxString::FromUTF8(objectPath)) {
		return;
	}
	guint response;
	GVariant *results = nullptr;
	g_variant_get(parameters, "(u@a{sv})", &response, &results);
	if (response == 0) {
		gchar **uris = nullptr;
		if (g_variant_lookup(results, "uris", "^as", &uris)) {
			if (uris[0]) {
				gchar *filename = g_filename_from_uri(uris[0], nullptr, nullptr);
				if (filename) {
					selection.path = wxString::FromUTF8(filename);
					g_free(filename);
				}
			}
			g_strfreev(uris);
		}
	}
	g_variant_unref(results);
	selection.done = true;
}

wxString PortalChoose(wxWindow *parent, const wxString &title, const wxString &hint, bool writable)
{
	GError *error = nullptr;
	GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
	if (!connection) {
		g_clear_error(&error);
		return wxEmptyString;
	}
	GMainContext *context = g_main_context_new();
	g_main_context_push_thread_default(context);
	Selection selection;
	gchar *uuid = g_uuid_string_random();
	wxString token = "amule_" + wxString::FromUTF8(uuid);
	g_free(uuid);
	token.Replace("-", "_");
	wxString sender = wxString::FromUTF8(g_dbus_connection_get_unique_name(connection)).Mid(1);
	sender.Replace(".", "_");
	selection.request = "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
	const guint subscription = g_dbus_connection_signal_subscribe(connection,
		"org.freedesktop.portal.Desktop",
		"org.freedesktop.portal.Request",
		"Response",
		nullptr,
		nullptr,
		G_DBUS_SIGNAL_FLAGS_NONE,
		Response,
		&selection,
		nullptr);
	GVariantBuilder options;
	g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token.utf8_str()));
	g_variant_builder_add(&options, "{sv}", "directory", g_variant_new_boolean(true));
	g_variant_builder_add(&options, "{sv}", "multiple", g_variant_new_boolean(false));
	if (!hint.empty()) {
		g_variant_builder_add(
			&options, "{sv}", "current_folder", g_variant_new_bytestring(hint.utf8_str()));
	}
	GVariant *reply = g_dbus_connection_call_sync(connection,
		"org.freedesktop.portal.Desktop",
		"/org/freedesktop/portal/desktop",
		"org.freedesktop.portal.FileChooser",
		"OpenFile",
		g_variant_new("(ssa{sv})", "", title.utf8_str().data(), &options),
		G_VARIANT_TYPE("(o)"),
		G_DBUS_CALL_FLAGS_NONE,
		5000,
		nullptr,
		&error);
	if (reply) {
		const gchar *request = nullptr;
		g_variant_get(reply, "(&o)", &request);
		selection.request = wxString::FromUTF8(request);
		g_variant_unref(reply);
		wxDialog waiting(parent, wxID_ANY, title);
		auto *sizer = new wxBoxSizer(wxVERTICAL);
		sizer->Add(new wxStaticText(
				   &waiting, wxID_ANY, _("Approve a folder in the system folder chooser.")),
			0,
			wxALL,
			16);
		sizer->Add(waiting.CreateButtonSizer(wxCANCEL), wxSizerFlags().Right().Border(wxALL, 8));
		waiting.SetSizerAndFit(sizer);
		wxTimer timer(&waiting);
		waiting.Bind(wxEVT_TIMER, [&](wxTimerEvent &) {
			while (g_main_context_pending(context)) {
				g_main_context_iteration(context, false);
			}
			if (selection.done) {
				waiting.EndModal(wxID_OK);
			}
		});
		timer.Start(20);
		waiting.ShowModal();
		timer.Stop();
		if (!selection.done) {
			GVariant *closed = g_dbus_connection_call_sync(connection,
				"org.freedesktop.portal.Desktop",
				selection.request.utf8_str(),
				"org.freedesktop.portal.Request",
				"Close",
				nullptr,
				nullptr,
				G_DBUS_CALL_FLAGS_NONE,
				5000,
				nullptr,
				nullptr);
			if (closed) {
				g_variant_unref(closed);
			}
			selection.path.clear();
		}
		if (!selection.path.empty() && !writable && writesRestricted &&
			IsWritable(CPath(selection.path))) {
			wxMessageBox(
				_("This folder already has write access for downloads or application data. "
				  "Sharing it does not remove that existing write access."),
				_("Existing write access"),
				wxOK | wxICON_INFORMATION,
				parent);
		}

		if (!selection.path.empty() && writable &&
			!CPath(selection.path).IsDir(CPath::readwritable)) {
			wxMessageBox(_("This destination needs read and write access. Approve writing in the "
				       "system chooser."),
				_("Download folder"),
				wxOK | wxICON_ERROR,
				parent);
			selection.path.clear();
		}

	} else {
		wxMessageBox(_("The system folder chooser is unavailable. Check that your desktop "
			       "portal is installed and running. Folder permissions have not been changed."),
			_("Folder access"),
			wxOK | wxICON_ERROR,
			parent);
	}
	g_clear_error(&error);
	g_dbus_connection_signal_unsubscribe(connection, subscription);
	g_main_context_pop_thread_default(context);
	g_main_context_unref(context);
	g_object_unref(connection);
	return selection.path;
}
#endif
} // namespace

wxString Choose(wxWindow *parent, const wxString &title, const wxString &hint, bool writable)
{
	wxString path;
	if (IsSandboxed()) {
#ifdef HAVE_GIO
		path = PortalChoose(parent, title, hint, writable);
#else
		wxMessageBox(_("This build does not support the system folder portal."),
			_("Folder access"),
			wxOK | wxICON_ERROR,
			parent);
#endif
	} else {
		path = wxDirSelector(title, hint, wxDD_DEFAULT_STYLE, wxDefaultPosition, parent);
	}
	if (!path.empty()) {
		Remember(path, writable);
		if (writable && writesRestricted && !IsWritable(CPath(path))) {
			wxMessageBox(
				_("Write access has been approved. Restart aMule, then select this "
				  "download destination. Existing download paths and files are unchanged."),
				_("Restart required"),
				wxOK | wxICON_INFORMATION,
				parent);
			return wxEmptyString;
		}
	}
	return path;
}
#endif
} // namespace FolderAccess
