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
#include "Logger.h"

#include <gio/gio.h>
#include <wx/init.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
void Check(bool passed, const char *message)
{
	if (!passed) {
		std::fprintf(stderr, "%s\n", message);
		std::exit(1);
	}
}

constexpr const char *requestPath = "/org/freedesktop/portal/desktop/request/test/inhibitor";
constexpr const char *xml = R"(<node>
<interface name="org.gnome.SessionManager">
 <method name="Inhibit"><arg type="s" direction="in"/><arg type="u" direction="in"/>
  <arg type="s" direction="in"/><arg type="u" direction="in"/><arg type="u" direction="out"/></method>
 <method name="Uninhibit"><arg type="u" direction="in"/></method>
</interface>
<interface name="org.freedesktop.portal.Inhibit">
 <method name="Inhibit"><arg type="s" direction="in"/><arg type="u" direction="in"/>
  <arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
</interface>
<interface name="org.freedesktop.portal.Request"><method name="Close"/></interface>
</node>)";

struct State
{
	bool denyDirect = false;
	bool denyPortal = false;
	bool failRelease = false;
	bool timeoutDirect = false;
	bool timeoutPortal = false;
	unsigned directCalls = 0;
	unsigned portalCalls = 0;
	unsigned releases = 0;
	guint cookie = 0;
	// Server resources are owned by the caller's unique connection name.
	std::map<std::string, bool> direct;
	std::map<std::string, bool> portal;
};

class Service
{
public:
	explicit Service(const char *address)
	{
		std::promise<void> ready;
		auto future = ready.get_future();
		worker = std::thread([this, address, &ready] {
			GMainContext *context = g_main_context_new();
			g_main_context_push_thread_default(context);
			loop = g_main_loop_new(context, false);
			GError *error = nullptr;
			// NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
			const auto flags = static_cast<GDBusConnectionFlags>(
				G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
				G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION);
			bus = g_dbus_connection_new_for_address_sync(
				address, flags, nullptr, nullptr, &error);
			Check(bus != nullptr, error ? error->message : "service connection failed");
			GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(xml, &error);
			Check(info != nullptr, "invalid service XML");
			static const GDBusInterfaceVTable vtable = { Call, nullptr, nullptr, { nullptr } };
			const char *paths[] = {
				"/org/gnome/SessionManager", "/org/freedesktop/portal/desktop", requestPath
			};
			for (unsigned i = 0; i < 3; ++i) {
				Check(g_dbus_connection_register_object(bus,
					      paths[i],
					      info->interfaces[i],
					      &vtable,
					      this,
					      nullptr,
					      &error) != 0,
					"register object failed");
			}
			g_dbus_node_info_unref(info);
			g_dbus_connection_signal_subscribe(bus,
				"org.freedesktop.DBus",
				"org.freedesktop.DBus",
				"NameOwnerChanged",
				"/org/freedesktop/DBus",
				nullptr,
				G_DBUS_SIGNAL_FLAGS_NONE,
				OwnerChanged,
				this,
				nullptr);
			SetName("org.gnome.SessionManager", true);
			SetName("org.freedesktop.portal.Desktop", true);
			ready.set_value();
			g_main_loop_run(loop);
			for (auto *invocation : pending) {
				g_dbus_method_invocation_return_dbus_error(
					invocation, "org.test.Stopped", "service stopped");
				g_object_unref(invocation);
			}
			g_dbus_connection_close_sync(bus, nullptr, nullptr);
			g_object_unref(bus);
			g_main_context_pop_thread_default(context);
			g_main_context_unref(context);
		});
		future.get();
	}

	~Service()
	{
		g_main_context_invoke(
			g_main_loop_get_context(loop),
			[](gpointer data) -> gboolean {
				g_main_loop_quit(static_cast<GMainLoop *>(data));
				return G_SOURCE_REMOVE;
			},
			loop);
		worker.join();
		g_main_loop_unref(loop);
	}

	void SetName(const char *name, bool own)
	{
		GError *error = nullptr;
		GVariant *reply = g_dbus_connection_call_sync(bus,
			"org.freedesktop.DBus",
			"/org/freedesktop/DBus",
			"org.freedesktop.DBus",
			own ? "RequestName" : "ReleaseName",
			own ? g_variant_new("(su)", name, 0u) : g_variant_new("(s)", name),
			G_VARIANT_TYPE("(u)"),
			G_DBUS_CALL_FLAGS_NONE,
			5000,
			nullptr,
			&error);
		Check(reply != nullptr, error ? error->message : "name operation failed");
		g_variant_unref(reply);
	}

	State Read()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return state;
	}

	void Configure(bool denyDirect = false,
		bool denyPortal = false,
		bool failRelease = false,
		bool timeoutDirect = false,
		bool timeoutPortal = false)
	{
		std::lock_guard<std::mutex> lock(mutex);
		state.denyDirect = denyDirect;
		state.denyPortal = denyPortal;
		state.failRelease = failRelease;
		state.timeoutDirect = timeoutDirect;
		state.timeoutPortal = timeoutPortal;
		state.cookie = 42;
	}

	void WaitReleased()
	{
		for (unsigned i = 0; i < 200; ++i) {
			State snapshot = Read();
			if (snapshot.direct.empty() && snapshot.portal.empty()) {
				return;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		Check(false, "server inhibitor leaked after release/disconnection");
	}

private:
	static void OwnerChanged(GDBusConnection *,
		const gchar *,
		const gchar *,
		const gchar *,
		const gchar *,
		GVariant *parameters,
		gpointer data)
	{
		auto &self = *static_cast<Service *>(data);
		const char *name = nullptr, *oldOwner = nullptr, *newOwner = nullptr;
		g_variant_get(parameters, "(&s&s&s)", &name, &oldOwner, &newOwner);
		if (*newOwner == '\0') {
			std::lock_guard<std::mutex> lock(self.mutex);
			self.state.direct.erase(name);
			self.state.portal.erase(name);
		}
	}

	static void Call(GDBusConnection *,
		const gchar *sender,
		const gchar *path,
		const gchar *interface,
		const gchar *method,
		GVariant *parameters,
		GDBusMethodInvocation *invocation,
		gpointer data)
	{
		auto &self = *static_cast<Service *>(data);
		std::lock_guard<std::mutex> lock(self.mutex);
		State &s = self.state;
		const bool direct = std::string(interface) == "org.gnome.SessionManager";
		if (std::string(method) == "Inhibit") {
			guint flags = 0;
			const char *reason = nullptr;
			if (direct) {
				++s.directCalls;
				const char *app = nullptr;
				guint window = 1;
				g_variant_get(parameters, "(&su&su)", &app, &window, &reason, &flags);
				Check(std::string(app) == "org.amule.aMule" && window == 0,
					"wrong application ID/window");
			} else {
				++s.portalCalls;
				const char *window = nullptr;
				GVariant *options = nullptr;
				g_variant_get(parameters, "(&su@a{sv})", &window, &flags, &options);
				Check(*window == '\0', "wrong portal window");
				Check(g_variant_n_children(options) == 1 &&
						g_variant_lookup(options, "reason", "&s", &reason),
					"wrong portal options");
				Check(std::string(reason) == "Transferring files", "wrong portal reason");
				g_variant_unref(options);
			}
			Check(flags == 4, "must inhibit suspend, not screen blanking/idle");
			if (direct) {
				Check(std::string(reason) == "Transferring files", "wrong direct reason");
			}
			if (direct ? s.denyDirect : s.denyPortal) {
				g_dbus_method_invocation_return_dbus_error(invocation,
					"org.freedesktop.DBus.Error.AccessDenied",
					"test refusal");
				return;
			}
			(direct ? s.direct : s.portal)[sender] = true;
			if (direct ? s.timeoutDirect : s.timeoutPortal) {
				// Allocate before withholding the reply: disconnection must clean it up.
				self.pending.push_back(
					static_cast<GDBusMethodInvocation *>(g_object_ref(invocation)));
				return;
			}
			// Return the configured cookie, including zero, to check retention without a
			// sentinel.
			g_dbus_method_invocation_return_value(invocation,
				direct ? g_variant_new("(u)", s.cookie) : g_variant_new("(o)", requestPath));
		} else {
			++s.releases;
			Check((direct ? s.direct : s.portal).count(sender) == 1,
				"release used another connection");
			if (direct) {
				guint cookie = 1;
				g_variant_get(parameters, "(u)", &cookie);
				Check(cookie == s.cookie && std::string(method) == "Uninhibit",
					"wrong cookie/release method");
			} else {
				Check(std::string(path) == requestPath && std::string(method) == "Close",
					"wrong portal handle");
			}
			if (s.failRelease) {
				// Keep the resource until NameOwnerChanged, matching a failed real release.
				g_dbus_method_invocation_return_dbus_error(
					invocation, "org.test.ReleaseFailed", "test release failure");
				return;
			}
			(direct ? s.direct : s.portal).erase(sender);
			g_dbus_method_invocation_return_value(invocation, g_variant_new("()"));
		}
	}

	std::mutex mutex;
	State state;
	GDBusConnection *bus = nullptr;
	GMainLoop *loop = nullptr;
	std::thread worker;
	std::vector<GDBusMethodInvocation *> pending;
};

void Cycle(Service &service, bool portal)
{
	const State before = service.Read();
	PlatformSpecific::PreventSleepMode();
	State acquired = service.Read();
	Check((portal ? acquired.portal : acquired.direct).size() == 1, "no inhibitor retained");
	Check(acquired.portalCalls == before.portalCalls + (portal ? 1 : 0), "wrong fallback order");
	PlatformSpecific::PreventSleepMode();
	State repeated = service.Read();
	Check(repeated.directCalls == acquired.directCalls && repeated.portalCalls == acquired.portalCalls,
		"repeated prevent reacquired inhibitor");
	PlatformSpecific::AllowSleepMode();
	service.WaitReleased();
	Check(service.Read().releases == before.releases + 1, "missing release call");
	PlatformSpecific::AllowSleepMode();
	Check(service.Read().releases == before.releases + 1, "repeated allow released twice");
}
} // namespace

int main(int argc, char **argv)
{
	wxInitializer init;
	Check(init.IsOk(), "wx initialization failed");
	theLogger.SetVerbose(true);
	theLogger.SetEnabled(logGeneral, true);
	if (argc == 2 && std::string(argv[1]) == "--no-bus") {
		g_setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/amule-sleep-test", true);
		PlatformSpecific::PreventSleepMode();
		PlatformSpecific::PreventSleepMode();
		PlatformSpecific::AllowSleepMode();
		const wxString &log = theLogger.GetUnwrittenLog();
		Check(log.Contains("Cannot inhibit idle sleep:"), "missing session-bus failure log");
		Check(!log.AfterFirst('\n').Contains("Cannot inhibit idle sleep:"),
			"failure logged every tick");
		Check(log.Contains("[log status=0 replay=1]"), "failure was not noncritical debug logging");
		return 0;
	}
	gchar *daemon = g_find_program_in_path("dbus-daemon");
	if (daemon == nullptr) {
		std::puts("SKIP: dbus-daemon is unavailable");
		return 77;
	}
	g_free(daemon);
	GTestDBus *testBus = g_test_dbus_new(G_TEST_DBUS_NONE);
	g_test_dbus_up(testBus);
	{
		Service service(g_test_dbus_get_bus_address(testBus));
		// Hold the shared connection throughout: production must use a private one.
		GDBusConnection *shared = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
		Check(shared != nullptr, "shared connection failed");
		Cycle(service, false);
		Cycle(service, false); // release/re-enable
		service.Configure(false, false, true);
		Cycle(service, false); // Uninhibit failure cleans up on disconnect
		service.Configure(true);
		Cycle(service, true); // permission refusal falls back
		service.Configure(true, false, true);
		Cycle(service, true); // Close failure cleans up despite shared caller
		service.Configure();
		service.SetName("org.gnome.SessionManager", false);
		Cycle(service, true); // absent service (Plasma/Flatpak path)
		service.SetName("org.freedesktop.portal.Desktop", false);
		PlatformSpecific::PreventSleepMode(); // neither service
		PlatformSpecific::PreventSleepMode();
		PlatformSpecific::AllowSleepMode();
		service.SetName("org.gnome.SessionManager", true);
		service.SetName("org.freedesktop.portal.Desktop", true);
		service.Configure(true, true);
		const State before = service.Read();
		PlatformSpecific::PreventSleepMode();
		PlatformSpecific::PreventSleepMode();
		Check(service.Read().directCalls == before.directCalls + 1 &&
				service.Read().portalCalls == before.portalCalls + 1,
			"failed acquisition retried every tick");
		PlatformSpecific::AllowSleepMode();
		const wxString &log = theLogger.GetUnwrittenLog();
		Check(log.Contains("trying portal:") && log.Contains("test refusal"),
			"lost fallback failure details");
		Check(log.Contains("Cannot release idle sleep inhibition:") &&
				log.Contains("test release failure"),
			"lost release failure details");
		Check(!log.Contains("[log status=1"), "sleep failures became critical logs");
		service.Configure(true, false, false, false, true);
		PlatformSpecific::PreventSleepMode(); // portal allocates before timeout
		service.WaitReleased();
		PlatformSpecific::AllowSleepMode();
		service.Configure(false, false, false, true);
		Cycle(service, true); // direct timeout falls back; disconnect releases both
		service.Configure();
		Cycle(service, false); // recovery after failures
		Check(!g_dbus_connection_is_closed(shared), "production closed the shared connection");
		g_object_unref(shared);
	}
	g_test_dbus_down(testBus);
	g_object_unref(testBus);
	std::puts("Sleep inhibition D-Bus checks passed");
	return 0;
}
