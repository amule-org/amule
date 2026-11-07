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
#include <sys/wait.h>
#include <wx/init.h>
#include <wx/app.h>
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

void PumpLogs()
{
	if (wxTheApp != nullptr) {
		wxTheApp->ProcessPendingEvents();
	}
}

template <typename Predicate> void WaitFor(Predicate predicate, const char *message, unsigned seconds = 12)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
	while (std::chrono::steady_clock::now() < deadline) {
		PumpLogs();
		if (predicate()) {
			return;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	Check(false, message);
}

constexpr unsigned retryIntervalSeconds = 1;
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
	std::string reason;
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
			g_dbus_connection_set_exit_on_close(bus, false);
			GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(xml, &error);
			Check(info != nullptr, "invalid service XML");
			static const GDBusInterfaceVTable vtable = { Call, nullptr, nullptr, { nullptr } };
			const char *paths[] = {
				"/org/gnome/SessionManager", "/org/freedesktop/portal/desktop", requestPath
			};
			for (unsigned i = 0; i < 3; ++i) {
				const guint registration = g_dbus_connection_register_object(
					bus, paths[i], info->interfaces[i], &vtable, this, nullptr, &error);
				Check(registration != 0, "register object failed");
				if (i == 2) {
					requestRegistration = registration;
				}
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
		WaitFor(
			[this] {
				const State snapshot = Read();
				return snapshot.direct.empty() && snapshot.portal.empty();
			},
			"server inhibitor leaked after release/disconnection");
		PumpLogs();
	}

	void DropPortalRequest()
	{
		std::lock_guard<std::mutex> lock(mutex);
		Check(g_dbus_connection_unregister_object(bus, requestRegistration),
			"request unregister failed");
		requestRegistration = 0;
		state.portal.clear();
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
				Check(std::string(reason) == "Transferring files" ||
						std::string(reason) == "aMule is running",
					"wrong portal reason");
				s.reason = reason;
				g_variant_unref(options);
			}
			Check(flags == 4, "must inhibit suspend, not screen blanking/idle");
			if (direct) {
				s.reason = reason;
				Check(std::string(reason) == "Transferring files" ||
						std::string(reason) == "aMule is running",
					"wrong direct reason");
			}
			if (direct ? s.denyDirect : s.denyPortal) {
				g_dbus_method_invocation_return_dbus_error(invocation,
					"org.freedesktop.DBus.Error.AccessDenied",
					"test refusal");
				return;
			}
			if (!direct && self.requestRegistration == 0) {
				GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(xml, nullptr);
				static const GDBusInterfaceVTable vtable = {
					Call, nullptr, nullptr, { nullptr }
				};
				self.requestRegistration = g_dbus_connection_register_object(self.bus,
					requestPath,
					info->interfaces[2],
					&vtable,
					&self,
					nullptr,
					nullptr);
				g_dbus_node_info_unref(info);
				Check(self.requestRegistration != 0, "request re-export failed");
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
	guint requestRegistration = 0;
	State state;
	GDBusConnection *bus = nullptr;
	GMainLoop *loop = nullptr;
	std::thread worker;
	std::vector<GDBusMethodInvocation *> pending;
};

void WaitForRecovery(Service &service, bool portal)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
	while (std::chrono::steady_clock::now() < deadline) {
		PlatformSpecific::PreventSleepMode();
		PumpLogs();
		const State state = service.Read();
		if ((portal ? state.portal : state.direct).size() == 1) {
			return;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	Check(false, "sleep inhibition did not recover without a policy toggle");
}

void RestartService(Service &service, const char *address, bool portal)
{
	service.Configure(portal);
	PlatformSpecific::PreventSleepMode();
	WaitFor([&] { return (portal ? service.Read().portal : service.Read().direct).size() == 1; },
		"missing inhibitor before service restart");
	service.SetName("org.gnome.SessionManager", false);
	service.SetName("org.freedesktop.portal.Desktop", false);
	{
		Service replacement(address);
		if (portal) {
			replacement.SetName("org.gnome.SessionManager", false);
		}
		WaitForRecovery(replacement, portal);
		service.WaitReleased();
		// Replacement services reuse the same cookie/request path. Cleanup must
		// address the old owner, leaving the replacement's inhibitor intact.
		Check(replacement.Read().releases == 0, "released against the replacement owner");
		PlatformSpecific::AllowSleepMode();
		replacement.WaitReleased();
	}
	service.SetName("org.gnome.SessionManager", true);
	service.SetName("org.freedesktop.portal.Desktop", true);
	service.Configure();
}

int DaemonModes(Service &service, char **argv)
{
	struct Case
	{
		const char *mode;
		const char *legacy;
		const char *expected;
		const char *scenario = "normal";
	};
	const Case cases[] = { { "missing", "missing", "0" },
		{ "missing", "0", "0" },
		{ "missing", "1", "1" },
		{ "0", "1", "0" },
		{ "1", "0", "1" },
		{ "2", "0", "2" },
		{ "-1", "1", "0" },
		{ "4294967297", "1", "0" },
		{ "invalid", "1", "0" },
		{ "2", "0", "2", "held" } };
	for (const auto &test : cases) {
		const bool held = std::string(test.scenario) == "held";
		service.Configure(false, false, false, held, held);
		const State before = service.Read();
		gchar *args[] = { argv[2],
			argv[3],
			argv[4],
			const_cast<char *>(test.mode),
			const_cast<char *>(test.legacy),
			const_cast<char *>(test.expected),
			const_cast<char *>(test.scenario),
			nullptr };
		GError *error = nullptr;
		gint status = 0;
		Check(g_spawn_sync(nullptr,
			      args,
			      nullptr,
			      G_SPAWN_DEFAULT,
			      nullptr,
			      nullptr,
			      nullptr,
			      nullptr,
			      &status,
			      &error),
			error ? error->message : "could not start daemon fixture");
		if (WIFEXITED(status) && WEXITSTATUS(status) == 77) {
			return 77;
		}
		Check(status == 0, "daemon sleep-mode fixture failed");
		service.WaitReleased();
		const State after = service.Read();
		const unsigned expected = std::string(test.expected) == "2" ? 2 : 0;
		// Shutdown cancels I/O and releases leases by disconnecting the private bus.
		Check(after.directCalls == before.directCalls + expected &&
				after.releases == before.releases && after.portalCalls == before.portalCalls,
			"empty daemon did not follow its configured sleep mode or release on shutdown");
		if (expected != 0) {
			Check(after.reason == "aMule is running",
				"daemon used the wrong running-mode reason");
		}
	}
	return 0;
}

void Cycle(Service &service, bool portal, bool whileRunning = false)
{
	const State before = service.Read();
	PlatformSpecific::PreventSleepMode(whileRunning);
	WaitFor([&] { return (portal ? service.Read().portal : service.Read().direct).size() == 1; },
		"no inhibitor retained");
	State acquired = service.Read();
	Check((portal ? acquired.portal : acquired.direct).size() == 1, "no inhibitor retained");
	Check(acquired.reason == (whileRunning ? "aMule is running" : "Transferring files"),
		"wrong reason for sleep mode");
	Check(acquired.portalCalls == before.portalCalls + (portal ? 1 : 0), "wrong fallback order");
	PlatformSpecific::PreventSleepMode(whileRunning);
	State repeated = service.Read();
	Check(repeated.directCalls == acquired.directCalls && repeated.portalCalls == acquired.portalCalls,
		"repeated prevent reacquired inhibitor");
	PlatformSpecific::PreventSleepMode(!whileRunning);
	WaitFor(
		[&] {
			const auto state = service.Read();
			const auto &leases = portal ? state.portal : state.direct;
			const auto &previous = portal ? acquired.portal : acquired.direct;
			return state.reason == (!whileRunning ? "aMule is running" : "Transferring files") &&
			       leases.size() == 1 && leases.begin()->first != previous.begin()->first;
		},
		"retained inhibitor reason did not follow mode change");
	PlatformSpecific::AllowSleepMode();
	service.WaitReleased();
	// The service can allocate a lease before the worker receives its reply.
	// Off may then cancel acquisition and clean up by disconnecting instead of
	// sending Uninhibit/Close. Both paths must release every server resource.
	const auto releaseCalls = service.Read().releases;
	Check(releaseCalls <= before.releases + 2, "released more leases than acquired");
	PlatformSpecific::AllowSleepMode();
	Check(service.Read().releases == releaseCalls, "repeated allow released twice");
}
} // namespace

int main(int argc, char **argv)
{
	PlatformSpecific::SetSleepInhibitionRetryIntervalForTesting(retryIntervalSeconds);
	wxInitializer init;
	Check(init.IsOk(), "wx initialization failed");
	theLogger.SetVerbose(true);
	theLogger.SetEnabled(logGeneral, true);
	// GTestDBus forks a watcher; do this before starting the inhibitor worker.
	gchar *daemon = g_find_program_in_path("dbus-daemon");
	if (daemon == nullptr) {
		std::puts("SKIP: dbus-daemon is unavailable");
		return 77;
	}
	g_free(daemon);
	GTestDBus *testBus = g_test_dbus_new(G_TEST_DBUS_NONE);
	g_test_dbus_up(testBus);
	if (argc == 2 && std::string(argv[1]) == "--no-bus") {
		g_setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/amule-sleep-test", true);
		PlatformSpecific::PreventSleepMode();
		PlatformSpecific::PreventSleepMode();
		WaitFor([] { return theLogger.GetUnwrittenLog().Contains("Cannot inhibit idle sleep:"); },
			"missing session-bus failure log");
		const wxString &log = theLogger.GetUnwrittenLog();
		Check(log.Contains("Cannot inhibit idle sleep:"), "missing session-bus failure log");
		Check(!log.AfterFirst('\n').Contains("Cannot inhibit idle sleep:"),
			"failure logged every tick");
		Check(log.Contains("[log status=0 replay=1]"), "failure was not noncritical debug logging");
		g_setenv("DBUS_SESSION_BUS_ADDRESS", g_test_dbus_get_bus_address(testBus), true);
	}

	if (argc == 5 && std::string(argv[1]) == "--daemon") {
		int status = 0;
		{
			Service service(g_test_dbus_get_bus_address(testBus));
			status = DaemonModes(service, argv);
		}
		g_test_dbus_down(testBus);
		g_object_unref(testBus);
		return status;
	}
	if (argc == 2 && std::string(argv[1]) == "--restart-bus") {
		{
			Service service(g_test_dbus_get_bus_address(testBus));
			PlatformSpecific::PreventSleepMode();
			WaitFor([&] { return service.Read().direct.size() == 1; },
				"initial bus inhibition failed");
		}
		g_test_dbus_stop(testBus);
		GTestDBus *replacementBus = g_test_dbus_new(G_TEST_DBUS_NONE);
		g_test_dbus_up(replacementBus);
		{
			Service service(g_test_dbus_get_bus_address(replacementBus));
			WaitForRecovery(service, false);
			PlatformSpecific::AllowSleepMode();
			service.WaitReleased();
		}
		PlatformSpecific::ShutdownSleepInhibition();
		PumpLogs();
		g_test_dbus_down(replacementBus);
		g_object_unref(replacementBus);
		g_test_dbus_down(testBus);
		g_object_unref(testBus);
		std::puts("Sleep inhibition recovered after session bus restart");
		return 0;
	}
	{
		Service service(g_test_dbus_get_bus_address(testBus));
		if (argc == 2 && std::string(argv[1]) == "--no-bus") {
			WaitForRecovery(service, false);
			PlatformSpecific::AllowSleepMode();
			service.WaitReleased();
		} else {
			// Hold the shared connection throughout: production must use a private one.
			GDBusConnection *shared = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
			Check(shared != nullptr, "shared connection failed");
			Cycle(service, false);
			Cycle(service, false, true); // running reason
			Cycle(service, false);       // release/re-enable
			service.Configure(false, false, true);
			Cycle(service, false); // Uninhibit failure cleans up on disconnect
			service.Configure(true);
			Cycle(service, true, true); // running reason through portal
			Cycle(service, true);       // permission refusal falls back
			service.Configure(true, false, true);
			Cycle(service, true); // Close failure cleans up despite shared caller
			service.Configure();
			service.SetName("org.gnome.SessionManager", false);
			Cycle(service, true); // absent service (Plasma/Flatpak path)
			service.SetName("org.freedesktop.portal.Desktop", false);
			const auto logOffset = theLogger.GetUnwrittenLog().length();
			PlatformSpecific::PreventSleepMode(); // neither service
			PlatformSpecific::PreventSleepMode();
			WaitFor(
				[&] {
					return theLogger.GetUnwrittenLog().Mid(logOffset).Contains(
						"Cannot inhibit idle sleep:");
				},
				"absent services were not exercised");
			service.SetName("org.gnome.SessionManager", true);
			service.SetName("org.freedesktop.portal.Desktop", true);
			WaitForRecovery(service, false);
			PlatformSpecific::AllowSleepMode();
			service.WaitReleased();
			service.Configure(true, true);
			const State before = service.Read();
			PlatformSpecific::PreventSleepMode();
			PlatformSpecific::PreventSleepMode();
			WaitFor([&] { return service.Read().portalCalls == before.portalCalls + 1; },
				"failed acquisition did not reach fallback");
			Check(service.Read().directCalls == before.directCalls + 1 &&
					service.Read().portalCalls == before.portalCalls + 1,
				"failed acquisition retried every tick");
			service.Configure();
			WaitForRecovery(service, false);
			Check(service.Read().directCalls == before.directCalls + 2 &&
					service.Read().portalCalls == before.portalCalls + 1,
				"recovery retried more than necessary");
			PlatformSpecific::AllowSleepMode();
			service.WaitReleased();
			const wxString &log = theLogger.GetUnwrittenLog();
			Check(log.Contains("trying portal:") && log.Contains("test refusal"),
				"lost fallback failure details");
			Check(log.Contains("Cannot release idle sleep inhibition:") &&
					log.Contains("test release failure"),
				"lost release failure details");
			Check(!log.Contains("[log status=1"), "sleep failures became critical logs");
			service.Configure(true, false, false, false, true);
			const auto portalCalls = service.Read().portalCalls;
			PlatformSpecific::PreventSleepMode(); // portal allocates before timeout
			WaitFor([&] { return service.Read().portalCalls > portalCalls; },
				"portal timeout was not exercised");
			service.WaitReleased();
			PlatformSpecific::AllowSleepMode();
			service.Configure(false, false, false, true);
			Cycle(service, true); // direct timeout falls back; disconnect releases both
			service.Configure();
			Cycle(service, false); // recovery after failures
			PlatformSpecific::PreventSleepMode();
			WaitFor([&] { return service.Read().direct.size() == 1; },
				"healthy acquisition failed");
			const State healthy = service.Read();
			std::this_thread::sleep_for(std::chrono::seconds(retryIntervalSeconds + 1));
			PlatformSpecific::PreventSleepMode();
			Check(service.Read().directCalls == healthy.directCalls &&
					service.Read().releases == healthy.releases,
				"healthy owner was unnecessarily released or reacquired");
			PlatformSpecific::AllowSleepMode();
			service.WaitReleased();
			RestartService(service, g_test_dbus_get_bus_address(testBus), false);
			RestartService(service, g_test_dbus_get_bus_address(testBus), true);
			service.Configure(true);
			PlatformSpecific::PreventSleepMode();
			WaitFor([&] { return service.Read().portal.size() == 1; },
				"portal acquisition failed");
			service.DropPortalRequest();
			WaitForRecovery(service, true);
			PlatformSpecific::AllowSleepMode();
			service.WaitReleased();

			service.Configure(false, false, false, true, true);
			const auto calls = service.Read().directCalls;
			auto started = std::chrono::steady_clock::now();
			PlatformSpecific::PreventSleepMode(true);
			Check(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(500),
				"sleep acquisition blocked the core thread");
			WaitFor([&] { return service.Read().directCalls > calls; },
				"held request was not sent");
			const auto modeChangeCalls = service.Read().directCalls;
			started = std::chrono::steady_clock::now();
			PlatformSpecific::PreventSleepMode(false);
			Check(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(500),
				"mode change blocked the core thread");
			WaitFor(
				[&] {
					const auto state = service.Read();
					return state.directCalls > modeChangeCalls &&
					       state.direct.size() == 1 &&
					       state.reason == "Transferring files";
				},
				"mode change did not cancel and replace a pending acquisition");
			started = std::chrono::steady_clock::now();
			PlatformSpecific::AllowSleepMode();
			Check(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(500),
				"turning sleep inhibition off blocked the core thread");
			service.WaitReleased();
			service.Configure();
			// Rapid changes must converge to the final desired state.
			for (unsigned i = 0; i < 50; ++i) {
				PlatformSpecific::PreventSleepMode(i % 2 == 0);
				PlatformSpecific::AllowSleepMode();
			}
			PlatformSpecific::PreventSleepMode(true);
			WaitFor(
				[&] {
					const auto state = service.Read();
					return state.direct.size() == 1 && state.reason == "aMule is running";
				},
				"rapid changes did not converge to the final mode and reason");
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			Check(service.Read().direct.size() == 1,
				"rapid changes left the final inhibitor cancelled");
			PlatformSpecific::AllowSleepMode();
			service.WaitReleased();
			service.Configure(false, false, false, true);
			const auto shutdownCalls = service.Read().directCalls;
			PlatformSpecific::PreventSleepMode();
			WaitFor([&] { return service.Read().directCalls > shutdownCalls; },
				"shutdown request was not sent");
			started = std::chrono::steady_clock::now();
			PlatformSpecific::ShutdownSleepInhibition();
			Check(std::chrono::steady_clock::now() - started < std::chrono::seconds(2),
				"shutdown did not cancel pending inhibition");
			service.WaitReleased();
			Check(!g_dbus_connection_is_closed(shared),
				"production closed the shared connection");
			g_object_unref(shared);
		}
	}
	PlatformSpecific::ShutdownSleepInhibition();
	PumpLogs();
	g_test_dbus_down(testBus);
	g_object_unref(testBus);
	std::puts("Sleep inhibition D-Bus checks passed");
	return 0;
}
