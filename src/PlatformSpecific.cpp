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

#include "PlatformSpecific.h"
#include "config.h"

#if defined(__linux__) && defined(HAVE_GIO)
#include "Logger.h"
#include "common/Format.h"
#include <gio/gio.h>
#endif

// NTFS Sparse Files (only for MSW)
#ifdef __WINDOWS__
#include "common/Format.h"
#include "Logger.h"
#include <winbase.h>
#include <winioctl.h>
#ifndef FSCTL_SET_SPARSE
#define FSCTL_SET_SPARSE CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 49, METHOD_BUFFERED, FILE_SPECIAL_ACCESS)
#endif
#ifndef FSCTL_SET_ZERO_DATA
#define FSCTL_SET_ZERO_DATA CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 50, METHOD_BUFFERED, FILE_WRITE_DATA)
#endif

// Create a message from a Windows error code
static wxString SystemError()
{
	WCHAR *lpMsgBuf = NULL;

	FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
		NULL,
		GetLastError(),
		0, // Default language
		(LPWSTR)&lpMsgBuf,
		0,
		NULL);

	wxString ret(lpMsgBuf);
	LocalFree(lpMsgBuf);
	return ret;
}

// Create a file in sparse mode
bool PlatformSpecific::CreateSparseFile(const CPath &name, uint64_t size)
{
	DWORD dwReturnedBytes = 0;

	HANDLE hd = CreateFileW(name.GetRaw().c_str(),
		GENERIC_READ | GENERIC_WRITE,
		0,    // share - not shareable
		NULL, // security - not inheritable
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_ARCHIVE,
		NULL);
	if (hd == INVALID_HANDLE_VALUE) {
		AddDebugLogLineC(logPartFile,
			CFormat("converting %s to sparse failed (OPEN): %s ") % name % SystemError());
		return false;
	}

	if (!DeviceIoControl(hd, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &dwReturnedBytes, NULL)) {
		AddDebugLogLineC(logPartFile,
			CFormat("converting %s to sparse failed (SET_SPARSE): %s ") % name % SystemError());
	} else {
		// FILE_ZERO_DATA_INFORMATION is not defined here
		struct
		{
			uint64 FileOffset;
			uint64 BeyondFinalZero;
		} fzdi;
		fzdi.FileOffset = 0;
		fzdi.BeyondFinalZero = size;
		LARGE_INTEGER largo;
		largo.QuadPart = size;

		// zero the data
		if (!DeviceIoControl(hd,
			    FSCTL_SET_ZERO_DATA,
			    (LPVOID)&fzdi,
			    sizeof(fzdi),
			    NULL,
			    0,
			    &dwReturnedBytes,
			    NULL)) {
			AddDebugLogLineC(logPartFile,
				CFormat("converting %s to sparse failed (ZERO): %s") % name % SystemError());
		} else if (!SetFilePointerEx(hd, largo, NULL, FILE_BEGIN) || !SetEndOfFile(hd)) {
			AddDebugLogLineC(logPartFile,
				CFormat("converting %s to sparse failed (SEEK): %s") % name % SystemError());
		}
	}
	CloseHandle(hd);
	return true;
}

#else // non Windows systems don't need all this
#include "CFile.h"

bool PlatformSpecific::CreateSparseFile(const CPath &name, uint64_t WXUNUSED(size))
{
	CFile f;
	return f.Create(name.GetRaw(), true) && f.Close();
}

#endif

#ifdef __WINDOWS__
#include <wx/msw/registry.h>
#include <wx/utils.h>

// Get the max number of connections that the OS supports, or -1 for default
int PlatformSpecific::GetMaxConnections()
{
	int maxconn = -1;
	// Try to get the max connection value in the registry
	wxRegKey key("HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\VxD\\MSTCP\\MaxConnections");
	wxString value;
	if (key.Exists()) {
		value = key.QueryDefaultValue();
	}
	if (!value.IsEmpty() && value.IsNumber()) {
		long mc;
		value.ToLong(&mc);
		maxconn = (int)mc;
	} else {
		switch (wxGetOsVersion()) {
		case wxOS_WINDOWS_9X:
			// This includes all Win9x versions
			maxconn = 50;
			break;
		case wxOS_WINDOWS_NT:
			// This includes NT based windows
			maxconn = 500;
			break;
		default:
			// Anything else. Let aMule decide...
			break;
		}
	}

	return maxconn;
}
#endif

#ifdef __WINDOWS__
#include <winbase.h>
#include <shlwapi.h>

static PlatformSpecific::EFSType doGetFilesystemType(const CPath &path)
{
	wxWritableWCharBuffer pathRaw(path.GetRaw().wchar_str());
	LPWSTR volume = pathRaw;
	if (!PathStripToRootW(volume)) {
		return PlatformSpecific::fsOther;
	}
	PathAddBackslashW(volume);

	DWORD maximumComponentLength = 0;
	DWORD filesystemFlags = 0;
	WCHAR filesystemNameBuffer[128];
	if (!GetVolumeInformationW(volume,
		    NULL,
		    0,
		    NULL,
		    &maximumComponentLength,
		    &filesystemFlags,
		    filesystemNameBuffer,
		    128)) {
		return PlatformSpecific::fsOther;
	}
	if (wxStrnicmp(filesystemNameBuffer, "FAT", 3) == 0) {
		return PlatformSpecific::fsFAT;
	} else if (wxStrcmp(filesystemNameBuffer, "NTFS") == 0) {
		return PlatformSpecific::fsNTFS;
	}
	return PlatformSpecific::fsOther;
};

#elif defined(HAVE_GETMNTENT) && defined(HAVE_MNTENT_H)
#include <stdio.h>
#include <string.h>
#include <mntent.h>
#ifndef _PATH_MOUNTED
#define _PATH_MOUNTED "/etc/mtab"
#endif
#include <common/StringFunctions.h>

static PlatformSpecific::EFSType doGetFilesystemType(const CPath &path)
{
	struct mntent *entry = NULL;
	PlatformSpecific::EFSType retval = PlatformSpecific::fsOther;
	FILE *mnttab = fopen(_PATH_MOUNTED, "r");
	unsigned bestPrefixLen = 0;

	if (mnttab == NULL) {
		return PlatformSpecific::fsOther;
	}

	while ((entry = getmntent(mnttab)) != NULL) {
		if (entry->mnt_dir) {
			wxString dir = char2unicode(entry->mnt_dir);
			if (dir == path.GetRaw().Mid(0, dir.Length())) {
				if (dir.Length() >= bestPrefixLen) {
					if (entry->mnt_type == NULL) {
						break;
					} else if (!strcmp(entry->mnt_type, "ntfs")) {
						retval = PlatformSpecific::fsNTFS;
					} else if (!strcmp(entry->mnt_type, "msdos") ||
						   !strcmp(entry->mnt_type, "umsdos") ||
						   !strcmp(entry->mnt_type, "vfat") ||
						   !strncmp(entry->mnt_type, "fat", 3)) {
						retval = PlatformSpecific::fsFAT;
					} else if (!strcmp(entry->mnt_type, "hfs")) {
						retval = PlatformSpecific::fsHFS;
					} else if (!strcmp(entry->mnt_type, "hpfs")) {
						retval = PlatformSpecific::fsHPFS;
					} else if (!strcmp(entry->mnt_type, "minix")) {
						retval = PlatformSpecific::fsMINIX;
					} /* Add more filesystem types here */
					else if (dir.Length() > bestPrefixLen) {
						retval = PlatformSpecific::fsOther;
					}
					bestPrefixLen = dir.Length();
				}
			}
		}
	}
	fclose(mnttab);
	return retval;
}

#elif defined(HAVE_GETMNTENT) && defined(HAVE_SYS_MNTENT_H) && defined(HAVE_SYS_MNTTAB_H)
#include <stdio.h>
#include <string.h>
#include <sys/mntent.h>
#include <sys/mnttab.h>
#ifndef MNTTAB
#define MNTTAB "/etc/mnttab"
#endif
#include <common/StringFunctions.h>

static PlatformSpecific::EFSType doGetFilesystemType(const CPath &path)
{
	struct mnttab entryStatic;
	struct mnttab *entry = &entryStatic;
	PlatformSpecific::EFSType retval = PlatformSpecific::fsOther;
	FILE *fmnttab = fopen(MNTTAB, "r");
	unsigned bestPrefixLen = 0;

	if (fmnttab == NULL) {
		return PlatformSpecific::fsOther;
	}

	while (getmntent(fmnttab, entry) == 0) {
		if (entry->mnt_mountp) {
			wxString dir = char2unicode(entry->mnt_mountp);
			if (dir == path.GetRaw().Mid(0, dir.Length())) {
				if (dir.Length() >= bestPrefixLen) {
					if (entry->mnt_fstype == NULL) {
						break;
					} else if (!strcmp(entry->mnt_fstype, MNTTYPE_PCFS)) {
						retval = PlatformSpecific::fsFAT;
					} else if (hasmntopt(entry, MNTOPT_NOLARGEFILES)) {
						// MINIX is a file system that can handle special chars but
						// has no large files.
						retval = PlatformSpecific::fsMINIX;
					} else if (dir.Length() > bestPrefixLen) {
						retval = PlatformSpecific::fsOther;
					}
					bestPrefixLen = dir.Length();
				}
			}
		}
	}
	fclose(fmnttab);
	return retval;
}

#else

// No way to determine filesystem type, no restrictions apply.
static inline PlatformSpecific::EFSType doGetFilesystemType(const CPath &WXUNUSED(path))
{
	return PlatformSpecific::fsOther;
}

#endif

#include <map>
#include <wx/thread.h>

PlatformSpecific::EFSType PlatformSpecific::GetFilesystemType(const CPath &path)
{
	typedef std::map<wxString, EFSType> FSMap;
	// Caching previous results, to speed up further checks.
	static FSMap s_fscache;
	// Lock used to ensure the integrity of the cache.
	static wxMutex s_lock;

	wxCHECK_MSG(path.IsOk(), fsOther, "Invalid path in GetFilesystemType()");

	wxMutexLocker locker(s_lock);

	FSMap::iterator it = s_fscache.find(path.GetRaw());
	if (it != s_fscache.end()) {
		return it->second;
	}

	return s_fscache[path.GetRaw()] = doGetFilesystemType(path);
}

#if defined(__linux__) && defined(HAVE_GIO)
static guint sleepInhibitorCookie = 0;
static GDBusConnection *sleepInhibitorBus = nullptr;
static gchar *sleepInhibitorRequest = nullptr;
static gchar *sleepInhibitorOwner = nullptr;
static gint64 sleepInhibitorNextCheck = 0;
constexpr gint64 sleepInhibitorCheckInterval = 30 * G_TIME_SPAN_SECOND;

// Pin requests to the service's unique owner: after a restart an old cookie must
// never be released against a replacement service that could have reused it.
static gchar *GetSleepServiceOwner(GDBusConnection *bus, const char *service, GError **error)
{
	GVariant *reply = g_dbus_connection_call_sync(bus,
		"org.freedesktop.DBus",
		"/org/freedesktop/DBus",
		"org.freedesktop.DBus",
		"GetNameOwner",
		g_variant_new("(s)", service),
		G_VARIANT_TYPE("(s)"),
		G_DBUS_CALL_FLAGS_NONE,
		5000,
		nullptr,
		error);
	gchar *owner = nullptr;
	if (reply != nullptr) {
		g_variant_get(reply, "(s)", &owner);
		g_variant_unref(reply);
	}
	return owner;
}

static GVariant *InhibitSleepService(GDBusConnection *bus,
	const char *service,
	const char *path,
	const char *interface,
	GVariant *parameters,
	const GVariantType *replyType,
	gchar **owner,
	GError **error)
{
	// Send to the well-known name to retain normal D-Bus auto-activation. Pin
	// cleanup to the actual reply sender, without a race between owner lookup
	// and acquisition or a dependency on permission to StartServiceByName.
	GDBusMessage *message = g_dbus_message_new_method_call(service, path, interface, "Inhibit");
	g_dbus_message_set_body(message, parameters);
	GDBusMessage *reply = g_dbus_connection_send_message_with_reply_sync(
		bus, message, G_DBUS_SEND_MESSAGE_FLAGS_NONE, 5000, nullptr, nullptr, error);
	g_object_unref(message);
	if (reply == nullptr) {
		return nullptr;
	}
	GVariant *result = nullptr;
	if (!g_dbus_message_to_gerror(reply, error)) {
		GVariant *body = g_dbus_message_get_body(reply);
		const gchar *sender = g_dbus_message_get_sender(reply);
		if (body != nullptr && g_variant_is_of_type(body, replyType) && sender != nullptr) {
			*owner = g_strdup(sender);
			result = g_variant_ref(body);
		} else {
			g_set_error_literal(error,
				G_DBUS_ERROR,
				G_DBUS_ERROR_INVALID_ARGS,
				"Unexpected sleep inhibition reply");
		}
	}
	g_object_unref(reply);
	return result;
}

static bool SleepServiceStillPresent()
{
	if (sleepInhibitorBus == nullptr || g_dbus_connection_is_closed(sleepInhibitorBus)) {
		return false;
	}
	GError *error = nullptr;
	gchar *owner = GetSleepServiceOwner(sleepInhibitorBus,
		sleepInhibitorRequest != nullptr ? "org.freedesktop.portal.Desktop"
						 : "org.gnome.SessionManager",
		&error);
	const bool unchanged = owner != nullptr && g_strcmp0(owner, sleepInhibitorOwner) == 0;
	g_free(owner);
	g_clear_error(&error);
	return unchanged;
}

#endif

// Power event vetoing

static bool m_preventingSleepMode = false;

#if defined(__WXMAC__) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 1050 // 10.5 only
#include <IOKit/pwr_mgt/IOPMLib.h>
static IOPMAssertionID assertionID;
#endif

void PlatformSpecific::PreventSleepMode()
{
#if defined(__linux__) && defined(HAVE_GIO)
	const gint64 now = g_get_monotonic_time();
	if (now < sleepInhibitorNextCheck) {
		return;
	}
	if (m_preventingSleepMode && !SleepServiceStillPresent()) {
		// Drops stale resources even when the bus has disconnected or the owner
		// has disappeared. This also permits acquisition through the fallback.
		AllowSleepMode();
	}
	// Poll healthy owners and retry failures at a bounded rate, never every tick.
	sleepInhibitorNextCheck = now + sleepInhibitorCheckInterval;
#endif

	if (!m_preventingSleepMode) {
#ifdef __WINDOWS__
		SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
		m_preventingSleepMode = true;

// IOPMAssertionCreate arrived in Leopard (10.5) but is deprecated from Snow Leopard (10.6). See
// http://developer.apple.com/library/mac/#qa/qa1340/_index.html and
// http://www.cimgf.com/2009/10/14/the-journey-to-disabling-sleep-with-iokit/
#elif defined(__WXMAC__) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 1060 // 10.6 only
		CFStringRef reasonForActivity = CFSTR("aMule is transferring files");
		IOReturn success = IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleSystemSleep,
			kIOPMAssertionLevelOn,
			reasonForActivity,
			&assertionID);
		if (success == kIOReturnSuccess) {
			// Correctly vetoed, flag so we don't do it again.
			m_preventingSleepMode = true;
		} else {
			// May be should be better to trace in log?
		}

#elif defined(__WXMAC__) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 1050 // 10.5 only
		IOReturn success = IOPMAssertionCreate(
			kIOPMAssertionTypeNoIdleSleep, kIOPMAssertionLevelOn, &assertionID);
		if (success == kIOReturnSuccess) {
			// Correctly vetoed, flag so we don't do it again.
			m_preventingSleepMode = true;
		} else {
			// ??
		}
#elif defined(__linux__) && defined(HAVE_GIO)
		GError *error = nullptr;
		// A private connection lets us release even a failed/timed-out request by
		// disconnecting, without closing another caller's shared session bus.
		gchar *address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, &error);
		GDBusConnection *bus = nullptr;
		if (address != nullptr) {
			// GIO requires this combination of bit flags, not a single enum value.
			// NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
			const auto flags = static_cast<GDBusConnectionFlags>(
				G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
				G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION);
			bus = g_dbus_connection_new_for_address_sync(
				address, flags, nullptr, nullptr, &error);
			g_free(address);
		}
		if (bus != nullptr) {
			// Cinnamon's portal may return a handle before failing because a native
			// process has no app ID. Supply it directly to the session manager first.
			g_dbus_connection_set_exit_on_close(bus, false);
			gchar *owner = nullptr;
			GVariant *reply = InhibitSleepService(bus,
				"org.gnome.SessionManager",
				"/org/gnome/SessionManager",
				"org.gnome.SessionManager",
				g_variant_new("(susu)", "org.amule.aMule", 0u, "Transferring files", 4u),
				G_VARIANT_TYPE("(u)"),
				&owner,
				&error);
			if (reply != nullptr) {
				g_variant_get(reply, "(u)", &sleepInhibitorCookie);
				sleepInhibitorBus = bus;
				sleepInhibitorOwner = owner;
				m_preventingSleepMode = true;
				g_variant_unref(reply);
				return;
			}
			AddDebugLogLineN(logGeneral,
				CFormat("Cannot inhibit idle sleep through session manager; trying portal: "
					"%s") %
					wxString::FromUTF8(error->message));
			g_clear_error(&error);
			g_free(owner);
			owner = nullptr;

			GVariantBuilder options;
			g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
			g_variant_builder_add(
				&options, "{sv}", "reason", g_variant_new_string("Transferring files"));
			// Suspend only; leave screen blanking and idle detection alone.
			reply = InhibitSleepService(bus,
				"org.freedesktop.portal.Desktop",
				"/org/freedesktop/portal/desktop",
				"org.freedesktop.portal.Inhibit",
				g_variant_new("(sua{sv})", "", 4u, &options),
				G_VARIANT_TYPE("(o)"),
				&owner,
				&error);
			if (reply != nullptr) {
				g_variant_get(reply, "(o)", &sleepInhibitorRequest);
				// The portal also releases the inhibition if this connection closes.
				sleepInhibitorBus = bus;
				sleepInhibitorOwner = owner;
				m_preventingSleepMode = true;
				g_variant_unref(reply);
			} else {
				g_free(owner);
				g_dbus_connection_close_sync(bus, nullptr, nullptr);
				g_object_unref(bus);
			}
		}
		if (error != nullptr) {
			AddDebugLogLineN(logGeneral,
				CFormat("Cannot inhibit idle sleep: %s") %
					wxString::FromUTF8(error->message));
			g_error_free(error);
		}
		// Start the backoff after slow failures too.
		sleepInhibitorNextCheck = g_get_monotonic_time() + sleepInhibitorCheckInterval;
#else
		// #warning Power event vetoing not implemented.
		//  Not implemented
#endif
	}
}

void PlatformSpecific::AllowSleepMode()
{
#if defined(__linux__) && defined(HAVE_GIO)
	// Off must cancel a pending retry even if acquisition never succeeded.
	sleepInhibitorNextCheck = 0;
#endif

	if (m_preventingSleepMode) {
#ifdef __WINDOWS__
		SetThreadExecutionState(ES_CONTINUOUS); // Clear the system request flag.
		m_preventingSleepMode = false;
#elif defined(__WXMAC__) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 1050 // 10.5 only
		IOReturn success = IOPMAssertionRelease(assertionID);
		if (success == kIOReturnSuccess) {
			// Correctly restored, flag so we don't do it again.
			m_preventingSleepMode = false;
		} else {
			// ??
		}
#elif defined(__linux__) && defined(HAVE_GIO)
		if (sleepInhibitorBus != nullptr) {
			GError *error = nullptr;
			const bool portal = sleepInhibitorRequest != nullptr;
			GVariant *reply = nullptr;
			if (!g_dbus_connection_is_closed(sleepInhibitorBus)) {
				reply = g_dbus_connection_call_sync(sleepInhibitorBus,
					sleepInhibitorOwner,
					portal ? sleepInhibitorRequest : "/org/gnome/SessionManager",
					portal ? "org.freedesktop.portal.Request"
					       : "org.gnome.SessionManager",
					portal ? "Close" : "Uninhibit",
					portal ? nullptr : g_variant_new("(u)", sleepInhibitorCookie),
					G_VARIANT_TYPE("()"),
					G_DBUS_CALL_FLAGS_NONE,
					5000,
					nullptr,
					&error);
			}
			if (reply != nullptr) {
				g_variant_unref(reply);
			}
			if (error != nullptr) {
				AddDebugLogLineN(logGeneral,
					CFormat("Cannot release idle sleep inhibition: %s") %
						wxString::FromUTF8(error->message));
				g_error_free(error);
			}
			g_free(sleepInhibitorRequest);
			sleepInhibitorRequest = nullptr;
			g_free(sleepInhibitorOwner);
			sleepInhibitorOwner = nullptr;
			sleepInhibitorCookie = 0;
			g_dbus_connection_close_sync(sleepInhibitorBus, nullptr, nullptr);
			g_object_unref(sleepInhibitorBus);
			sleepInhibitorBus = nullptr;
		}
		m_preventingSleepMode = false;
#else
		// Not implemented
#endif
	}
}
