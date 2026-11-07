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
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <system_error>
#include <thread>
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
namespace
{
// All D-Bus state belongs to this worker. Queue ticks only change the desired
// state: a slow desktop service must never stall transfers or the GUI.
class LinuxSleepInhibitor
{
public:
	static LinuxSleepInhibitor &Get()
	{
		static LinuxSleepInhibitor inhibitor;
		return inhibitor;
	}

	~LinuxSleepInhibitor() { Stop(); }

	void SetDesired(bool desired, bool whileRunning = false)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_stopping) {
			return;
		}
		if (m_desired != desired || m_whileRunning != whileRunning) {
			m_desired = desired;
			m_whileRunning = whileRunning;
			++m_generation;
			if (m_cancellable != nullptr) {
				g_cancellable_cancel(m_cancellable);
			}
			m_changed.notify_all();
		}
		if (desired && !m_worker.joinable() && std::chrono::steady_clock::now() >= m_startRetry) {
			try {
				m_worker = std::thread(&LinuxSleepInhibitor::Run, this);
			} catch (const std::system_error &error) {
				m_startRetry = std::chrono::steady_clock::now() + m_retryInterval;
				AddDebugLogLineN(logGeneral,
					CFormat("Cannot start sleep inhibition worker: %s") %
						wxString::FromUTF8(error.what()));
			}
		}
	}

#if defined(AMULE_SLEEP_INHIBITION_TEST)
	void SetRetryIntervalForTesting(unsigned seconds)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_retryInterval = std::chrono::seconds(seconds);
	}
#endif

	void Stop()
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		if (m_stopping) {
			return;
		}
		m_stopping = true;
		++m_generation;
		if (m_cancellable != nullptr) {
			g_cancellable_cancel(m_cancellable);
		}
		m_changed.notify_all();
		std::thread worker = std::move(m_worker);
		lock.unlock();
		if (worker.joinable()) {
			worker.join();
		}
	}

private:
	LinuxSleepInhibitor() = default;

	void Run()
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		for (;;) {
			if (!m_stopping && !m_desired && m_bus == nullptr) {
				m_changed.wait(lock, [this] { return m_stopping || m_desired; });
			}
			const bool stopping = m_stopping;
			const bool prevent = m_desired && !stopping;
			const auto generation = m_generation;
			const bool whileRunning = m_whileRunning;
			m_cancellable = g_cancellable_new();
			if (stopping) {
				g_cancellable_cancel(m_cancellable);
			}
			lock.unlock();
			if (prevent) {
				Refresh(whileRunning, m_cancellable);
			} else {
				Release(m_cancellable);
			}
			lock.lock();
			g_object_unref(m_cancellable);
			m_cancellable = nullptr;
			if (stopping) {
				break;
			}
			if (generation == m_generation && m_desired) {
				m_changed.wait_for(lock, m_retryInterval, [this, generation] {
					return m_stopping || generation != m_generation;
				});
			}
		}
	}

	static void LogError(const char *operation, GError *error)
	{
		if (error != nullptr && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			AddDebugLogLineN(logGeneral,
				CFormat("%s: %s") % wxString::FromUTF8(operation) %
					wxString::FromUTF8(error->message));
		}
	}

	GVariant *Call(const char *destination,
		const char *path,
		const char *interface,
		const char *method,
		GVariant *parameters,
		const GVariantType *type,
		GCancellable *cancellable,
		GError **error)
	{
		return g_dbus_connection_call_sync(m_bus,
			destination,
			path,
			interface,
			method,
			parameters,
			type,
			G_DBUS_CALL_FLAGS_NONE,
			5000,
			cancellable,
			error);
	}

	bool IsRetained(GCancellable *cancellable)
	{
		if (m_bus == nullptr || g_dbus_connection_is_closed(m_bus)) {
			return false;
		}
		GError *error = nullptr;
		GVariant *reply = Call("org.freedesktop.DBus",
			"/org/freedesktop/DBus",
			"org.freedesktop.DBus",
			"GetNameOwner",
			g_variant_new("(s)",
				m_request != nullptr ? "org.freedesktop.portal.Desktop"
						     : "org.gnome.SessionManager"),
			G_VARIANT_TYPE("(s)"),
			cancellable,
			&error);
		const gchar *owner = nullptr;
		if (reply != nullptr) {
			g_variant_get(reply, "(&s)", &owner);
		}
		bool retained = owner != nullptr && g_strcmp0(owner, m_owner) == 0;
		if (reply != nullptr) {
			g_variant_unref(reply);
		}
		reply = nullptr;
		g_clear_error(&error);
		if (retained && m_request != nullptr) {
			// A portal may remove a failed request without changing its bus owner.
			// Check the Request interface too, rather than retaining a dead handle.
			reply = Call(m_owner,
				m_request,
				"org.freedesktop.DBus.Introspectable",
				"Introspect",
				nullptr,
				G_VARIANT_TYPE("(s)"),
				cancellable,
				&error);
			const gchar *xml = nullptr;
			if (reply != nullptr) {
				g_variant_get(reply, "(&s)", &xml);
			}
			GDBusNodeInfo *info =
				xml != nullptr ? g_dbus_node_info_new_for_xml(xml, nullptr) : nullptr;
			retained = info != nullptr && g_dbus_node_info_lookup_interface(info,
							      "org.freedesktop.portal.Request") != nullptr;
			if (info != nullptr) {
				g_dbus_node_info_unref(info);
			}
			if (reply != nullptr) {
				g_variant_unref(reply);
			}
			g_clear_error(&error);
		}
		return retained;
	}

	static GVariant *Inhibit(GDBusConnection *bus,
		const char *service,
		const char *path,
		const char *interface,
		GVariant *parameters,
		const GVariantType *type,
		gchar **owner,
		GCancellable *cancellable,
		GError **error)
	{
		// Keep normal service auto-activation, and pin cleanup to the actual reply
		// sender so a replacement service cannot inherit an old cookie or handle.
		GDBusMessage *message = g_dbus_message_new_method_call(service, path, interface, "Inhibit");
		g_dbus_message_set_body(message, parameters);
		GDBusMessage *reply = g_dbus_connection_send_message_with_reply_sync(
			bus, message, G_DBUS_SEND_MESSAGE_FLAGS_NONE, 5000, nullptr, cancellable, error);
		g_object_unref(message);
		if (reply == nullptr) {
			return nullptr;
		}
		GVariant *result = nullptr;
		if (!g_dbus_message_to_gerror(reply, error)) {
			GVariant *body = g_dbus_message_get_body(reply);
			const gchar *sender = g_dbus_message_get_sender(reply);
			if (body != nullptr && g_variant_is_of_type(body, type) && sender != nullptr) {
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

	void Refresh(bool whileRunning, GCancellable *cancellable)
	{
		if (IsRetained(cancellable) && m_inhibitedWhileRunning == whileRunning) {
			return;
		}
		Release(cancellable);
		if (g_cancellable_is_cancelled(cancellable)) {
			return;
		}
		const char *reason = whileRunning ? "aMule is running" : "Transferring files";
		GError *error = nullptr;
		gchar *address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, cancellable, &error);
		GDBusConnection *bus = nullptr;
		if (address != nullptr) {
			// GIO requires this combination of bit flags, not a single enum value.
			// NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
			const auto flags = static_cast<GDBusConnectionFlags>(
				G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
				G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION);
			bus = g_dbus_connection_new_for_address_sync(
				address, flags, nullptr, cancellable, &error);
			g_free(address);
		}
		if (bus != nullptr) {
			g_dbus_connection_set_exit_on_close(bus, false);
			gchar *owner = nullptr;
			GVariant *reply = Inhibit(bus,
				"org.gnome.SessionManager",
				"/org/gnome/SessionManager",
				"org.gnome.SessionManager",
				g_variant_new("(susu)", "org.amule.aMule", 0u, reason, 4u),
				G_VARIANT_TYPE("(u)"),
				&owner,
				cancellable,
				&error);
			if (reply != nullptr) {
				g_variant_get(reply, "(u)", &m_cookie);
				m_bus = bus;
				m_owner = owner;
				m_inhibitedWhileRunning = whileRunning;
				g_variant_unref(reply);
				return;
			}
			LogError("Cannot inhibit idle sleep through session manager; trying portal", error);
			g_clear_error(&error);
			g_free(owner);
			owner = nullptr;
			if (!g_cancellable_is_cancelled(cancellable)) {
				GVariantBuilder options;
				g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
				g_variant_builder_add(
					&options, "{sv}", "reason", g_variant_new_string(reason));
				// Suspend only; leave screen blanking and idle detection alone.
				reply = Inhibit(bus,
					"org.freedesktop.portal.Desktop",
					"/org/freedesktop/portal/desktop",
					"org.freedesktop.portal.Inhibit",
					g_variant_new("(sua{sv})", "", 4u, &options),
					G_VARIANT_TYPE("(o)"),
					&owner,
					cancellable,
					&error);
				if (reply != nullptr) {
					g_variant_get(reply, "(o)", &m_request);
					m_bus = bus;
					m_owner = owner;
					m_inhibitedWhileRunning = whileRunning;
					g_variant_unref(reply);
					return;
				}
			}
			g_free(owner);
			// Disconnect even after cancellation/timeout: the service may have
			// allocated an inhibitor before its reply was lost.
			g_dbus_connection_close_sync(bus, nullptr, nullptr);
			g_object_unref(bus);
		}
		LogError("Cannot inhibit idle sleep", error);
		g_clear_error(&error);
	}

	void Release(GCancellable *cancellable)
	{
		if (m_bus == nullptr) {
			return;
		}
		GError *error = nullptr;
		GVariant *reply = nullptr;
		if (!g_dbus_connection_is_closed(m_bus)) {
			reply = Call(m_owner,
				m_request != nullptr ? m_request : "/org/gnome/SessionManager",
				m_request != nullptr ? "org.freedesktop.portal.Request"
						     : "org.gnome.SessionManager",
				m_request != nullptr ? "Close" : "Uninhibit",
				m_request != nullptr ? nullptr : g_variant_new("(u)", m_cookie),
				G_VARIANT_TYPE("()"),
				cancellable,
				&error);
		}
		if (reply != nullptr) {
			g_variant_unref(reply);
		}
		LogError("Cannot release idle sleep inhibition", error);
		g_clear_error(&error);
		g_free(m_request);
		m_request = nullptr;
		g_free(m_owner);
		m_owner = nullptr;
		m_cookie = 0;
		g_dbus_connection_close_sync(m_bus, nullptr, nullptr);
		g_object_unref(m_bus);
		m_bus = nullptr;
	}

	std::mutex m_mutex;
	std::condition_variable m_changed;
	std::thread m_worker;
	std::chrono::steady_clock::time_point m_startRetry{};
	std::chrono::seconds m_retryInterval{ 30 };
	std::uint64_t m_generation = 0;
	bool m_desired = false;
	bool m_whileRunning = false;
	bool m_stopping = false;
	GCancellable *m_cancellable = nullptr; // protected by m_mutex
	// The fields below are accessed only by the worker.
	bool m_inhibitedWhileRunning = false;
	GDBusConnection *m_bus = nullptr;
	gchar *m_owner = nullptr;
	gchar *m_request = nullptr;
	guint m_cookie = 0;
};
} // namespace
#endif

// Power event vetoing

#if !defined(__linux__) || !defined(HAVE_GIO)
static bool m_preventingSleepMode = false;
static bool m_preventingWhileRunning = false;
#endif

#if defined(__WXMAC__) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 1050 // 10.5 only
#include <IOKit/pwr_mgt/IOPMLib.h>
static IOPMAssertionID assertionID;
#endif

void PlatformSpecific::PreventSleepMode(bool whileRunning)
{
#if defined(__linux__) && defined(HAVE_GIO)
	LinuxSleepInhibitor::Get().SetDesired(true, whileRunning);
#else

	if (m_preventingSleepMode && m_preventingWhileRunning != whileRunning) {
		AllowSleepMode();
		if (m_preventingSleepMode) {
			return; // Retry the mode change if releasing the previous assertion failed.
		}
	}
	if (!m_preventingSleepMode) {
#ifdef __WINDOWS__
		m_preventingSleepMode = SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED) != 0;

// IOPMAssertionCreate arrived in Leopard (10.5) but is deprecated from Snow Leopard (10.6). See
// http://developer.apple.com/library/mac/#qa/qa1340/_index.html and
// http://www.cimgf.com/2009/10/14/the-journey-to-disabling-sleep-with-iokit/
#elif defined(__WXMAC__) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 1060 // 10.6 only
		CFStringRef reasonForActivity =
			whileRunning ? CFSTR("aMule is running") : CFSTR("aMule is transferring files");
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

#else
		// #warning Power event vetoing not implemented.
		//  Not implemented
#endif
	}
	if (m_preventingSleepMode) {
		m_preventingWhileRunning = whileRunning;
	}
#endif
}

void PlatformSpecific::AllowSleepMode()
{
#if defined(__linux__) && defined(HAVE_GIO)
	LinuxSleepInhibitor::Get().SetDesired(false);
#else

	if (m_preventingSleepMode) {
#ifdef __WINDOWS__
		// Keep the flag set if clearing fails, so a later Off tick can retry.
		if (SetThreadExecutionState(ES_CONTINUOUS) != 0) {
			m_preventingSleepMode = false;
		}
#elif defined(__WXMAC__) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 1050 // 10.5 only
		IOReturn success = IOPMAssertionRelease(assertionID);
		if (success == kIOReturnSuccess) {
			// Correctly restored, flag so we don't do it again.
			m_preventingSleepMode = false;
		} else {
			// ??
		}

#else
		// Not implemented
#endif
	}
#endif
}

void PlatformSpecific::ShutdownSleepInhibition()
{
#if defined(__linux__) && defined(HAVE_GIO)
	LinuxSleepInhibitor::Get().Stop();
#else
	AllowSleepMode();
#endif
}

#if defined(AMULE_SLEEP_INHIBITION_TEST)
void PlatformSpecific::SetSleepInhibitionRetryIntervalForTesting(unsigned seconds)
{
	LinuxSleepInhibitor::Get().SetRetryIntervalForTesting(seconds);
}
#endif
