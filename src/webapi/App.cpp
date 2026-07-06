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
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
//

#include "App.h"

#include "Api.h"
#include "HttpServer.h"
#include "Jwt.h"
#include "Refresher.h"

#include "MD4Hash.h"
#include "config.h" // VERSION

#include <common/Format.h>
#include <common/MD5Sum.h>

#include <wx/cmdline.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <thread>

IMPLEMENT_APP(CamuleapiApp)

namespace
{

// Signal-safe shutdown gate. SIGINT/SIGTERM flip the flag; the wxApp
// main loop polls it every 250 ms. A signalfd-driven path would be
// cleaner, but the polling cost is one atomic-load per tick and the
// code path stays portable to launchd/Windows (which don't have
// signalfd).
std::atomic<bool> g_shutdownRequested{ false };

void RequestShutdown(int)
{
	g_shutdownRequested.store(true, std::memory_order_release);
}

} // namespace

CamuleapiApp::CamuleapiApp() = default;
CamuleapiApp::~CamuleapiApp() = default;

void CamuleapiApp::OnInitCmdLine(wxCmdLineParser &parser)
{
	// Pulls in --host / --port / --password / --config-dir / --quiet /
	// --verbose / --locale. The base appname becomes the executable
	// name in usage text.
	CaMuleExternalConnector::OnInitCmdLine(parser, "amuleapi");

	parser.AddOption("",
		"bind",
		_("HTTP server bind address. (default: 127.0.0.1, override of amuleapi.conf "
		  "[Server]/BindAddress)"),
		wxCMD_LINE_VAL_STRING,
		wxCMD_LINE_PARAM_OPTIONAL);
	parser.AddOption("",
		"http-port",
		_("HTTP server port. (default: 4713, override of amuleapi.conf [Server]/Port)"),
		wxCMD_LINE_VAL_NUMBER,
		wxCMD_LINE_PARAM_OPTIONAL);
	parser.AddOption("",
		"config-dir",
		_("Path to amuleapi config dir (default: per-platform aMule data dir)."),
		wxCMD_LINE_VAL_STRING,
		wxCMD_LINE_PARAM_OPTIONAL);
	parser.AddOption("",
		"set-admin-pass",
		_("Hash <plain> with MD5 and write it as the admin password into amuleapi-passwords (mode "
		  "0600), then exit."),
		wxCMD_LINE_VAL_STRING,
		wxCMD_LINE_PARAM_OPTIONAL);
	parser.AddOption("",
		"set-guest-pass",
		_("Hash <plain> with MD5 and write it as the guest password into amuleapi-passwords (mode "
		  "0600), then exit."),
		wxCMD_LINE_VAL_STRING,
		wxCMD_LINE_PARAM_OPTIONAL);
	parser.AddSwitch("", "foreground", _("Stay in the foreground; default."), wxCMD_LINE_PARAM_OPTIONAL);
	// --daemon was reserved but never read — landed as a no-op switch.
	// Dropped to avoid implying detach support that isn't here yet.
	// Operators wanting fork-into-background today wrap amuleapi in
	// systemd/launchd/`nohup` like any other long-running CLI.
}

bool CamuleapiApp::OnCmdLineParsed(wxCmdLineParser &parser)
{
	// Capture the amuleapi-specific options BEFORE delegating to the
	// base, because the base call may exit early on --help.
	if (parser.Found("bind", &m_cliBindAddress)) {
		m_cliHasBindAddress = true;
	}
	if (parser.Found("http-port", &m_cliHttpPort)) {
		m_cliHasHttpPort = true;
	}
	parser.Found("config-dir", &m_cliConfigDirOverride);
	if (parser.Found("set-admin-pass", &m_cliSetAdminPass)) {
		m_cliHasSetAdminPass = true;
	}
	if (parser.Found("set-guest-pass", &m_cliSetGuestPass)) {
		m_cliHasSetGuestPass = true;
	}
	// The base class reads --host / --port / --password into m_host /
	// m_port / m_password before we get here, but it offers no "did
	// the user actually pass this?" predicate of its own — m_host
	// defaults to "127.0.0.1" unconditionally and m_port to 4712. We
	// poll the parser directly here so LoadAmuleapiConfig() can tell
	// "operator explicitly passed --host=127.0.0.1" apart from
	// "operator passed nothing and the base class filled in the
	// default", and only apply the amuleapi.conf override in the
	// second case. Same shape as the bind / http-port branches above.
	m_cliHasEcHost = parser.Found("host");
	m_cliHasEcPort = parser.Found("port");
	m_cliHasEcPassword = parser.Found("password");

	return CaMuleExternalConnector::OnCmdLineParsed(parser);
}

bool CamuleapiApp::OnInit()
{
	if (!CaMuleExternalConnector::OnInit()) {
		return false;
	}

	// Resolve the config dir. Order: explicit --config-dir > whatever
	// the base class picked (m_configDir, populated from --config-file
	// or the platform default) > wxStandardPaths::GetUserDataDir().
	wxString config_dir = m_cliConfigDirOverride.IsEmpty() ? m_configDir : m_cliConfigDirOverride;
	if (config_dir.IsEmpty()) {
		config_dir = DefaultConfigDir();
	}

	if (!LoadAmuleapiConfig()) {
		// Error already printed via Show(...).
		return false;
	}

	// Construct the SSE bus once the operator-tunable capacity is
	// known. Below the floor is clamped by CEventBus itself.
	m_event_bus = std::unique_ptr<webapi::CEventBus>(
		new webapi::CEventBus(m_apiConfig.StreamingCfg().event_bus_ring_capacity));

	// CLI override hooks. set-*-pass exits immediately after writing.
	if (m_cliHasSetAdminPass || m_cliHasSetGuestPass) {
		// Both options run as one-shot CLI flows. Operators script
		// these like `amuleapi --set-admin-pass=... && systemctl
		// restart amuleapi` — they MUST see the non-zero exit code on
		// disk-full / mode-bit rejection / etc., otherwise the
		// chain runs against the half-written file. Returning false
		// from OnInit() would cancel wxApp::OnRun() but still exit 0,
		// so we std::exit() here with the real return code.
		const int rc = m_cliHasSetAdminPass ? RunSetAdminPass() : RunSetGuestPass();
		std::exit(rc);
	}

	return true;
}

bool CamuleapiApp::LoadAmuleapiConfig()
{
	wxString config_dir = m_cliConfigDirOverride.IsEmpty() ? m_configDir : m_cliConfigDirOverride;
	if (config_dir.IsEmpty()) {
		config_dir = DefaultConfigDir();
		// Persist the resolved dir back into m_configDir so the base
		// class's later --write-config (and any subclasses) see the
		// same path.
		m_configDir = config_dir;
	}

	if (!m_apiConfig.Load(config_dir)) {
		Show(CFormat("amuleapi: failed to load config: %s\n") %
			wxString::FromUTF8(m_apiConfig.LastError().c_str()));
		return false;
	}

	// Wire the EC connection params into the base-class fields that
	// ConnectAndRun reads. CLI --host/--port/--password (captured via
	// wxCmdLineParser::Found() in OnCmdLineParsed) win over
	// amuleapi.conf. The old "is the field still at the default
	// value?" heuristic conflated "operator didn't pass" with
	// "operator explicitly passed the default" — so a literal
	// `amuleapi --host=127.0.0.1` would be silently overwritten by
	// the config file. The has-flag predicates are unambiguous.
	if (!m_cliHasEcHost) {
		const auto &h = m_apiConfig.EcCfg().host;
		if (!h.empty())
			m_host = wxString::FromUTF8(h.c_str());
	}
	if (!m_cliHasEcPort) {
		m_port = static_cast<long>(m_apiConfig.EcCfg().port);
	}
	if (!m_cliHasEcPassword && !m_apiConfig.EcCfg().password.empty()) {
		// amuleapi.conf [EC]/Password is plaintext; the base class
		// expects an MD5-hashed CMD4Hash (because that's what amuled
		// stores). MD5-hash here so a one-line amuleapi.conf edit
		// gives the operator a working setup.
		const wxString plain = wxString::FromUTF8(m_apiConfig.EcCfg().password.c_str());
		m_password.Decode(MD5Sum(plain).GetHash());
	}

	return true;
}

int CamuleapiApp::RunSetAdminPass()
{
	const wxString hashed = MD5Sum(m_cliSetAdminPass).GetHash();
	if (!m_apiConfig.WritePasswordsFile(m_apiConfig.ConfigDir(),
		    std::string(hashed.utf8_str()),
		    m_apiConfig.GuestPasswordMd5())) {
		Show(CFormat("amuleapi: --set-admin-pass failed: %s\n") %
			wxString::FromUTF8(m_apiConfig.LastError().c_str()));
		return 1;
	}
	Show(_("amuleapi: admin password updated.\n"));
	return 0;
}

int CamuleapiApp::RunSetGuestPass()
{
	const wxString hashed = MD5Sum(m_cliSetGuestPass).GetHash();
	if (!m_apiConfig.WritePasswordsFile(m_apiConfig.ConfigDir(),
		    m_apiConfig.AdminPasswordMd5(),
		    std::string(hashed.utf8_str()))) {
		Show(CFormat("amuleapi: --set-guest-pass failed: %s\n") %
			wxString::FromUTF8(m_apiConfig.LastError().c_str()));
		return 1;
	}
	Show(_("amuleapi: guest password updated.\n"));
	return 0;
}

int CamuleapiApp::OnRun()
{
	// Install signal handlers before the HTTP server starts so a
	// signal during bring-up doesn't default-terminate the daemon.
	//
	// sigaction (not std::signal) — std::signal's "reset to SIG_DFL
	// after firing" is implementation-defined (musl/Alpine, older
	// BSDs trip it), so a second SIGINT would terminate mid-shutdown.
	// SA_RESTART so blocking syscalls on the EC socket don't return
	// EINTR. Windows lacks sigaction; fall back to std::signal there.
#ifndef _WIN32
	struct sigaction sa;
	std::memset(&sa, 0, sizeof(sa));
	sa.sa_handler = RequestShutdown;
	sigemptyset(&sa.sa_mask);
	sigaddset(&sa.sa_mask, SIGINT);
	sigaddset(&sa.sa_mask, SIGTERM);
#ifdef SIGHUP
	sigaddset(&sa.sa_mask, SIGHUP);
#endif
	sa.sa_flags = SA_RESTART;
	::sigaction(SIGINT, &sa, nullptr);
	::sigaction(SIGTERM, &sa, nullptr);
#ifdef SIGHUP
	// SIGHUP is the "config reload" signal in long-running daemons.
	// has no reload story (configs are read at startup only);
	// treat SIGHUP as a soft shutdown so a systemd reload doesn't
	// leave the daemon in a half-state.
	::sigaction(SIGHUP, &sa, nullptr);
#endif
#ifdef SIGPIPE
	// SSE peers that disappear mid-write make Linux raise SIGPIPE on
	// the next asio::write to the closed fd. We don't pass
	// MSG_NOSIGNAL on any of the SSE socket writes (HttpServer.cpp),
	// so without ignoring SIGPIPE here the default disposition kills
	// the daemon on every dropped EventSource. Ignore it process-
	// wide; the writes return EPIPE and the streaming-handler loop
	// bails on the next writer.Alive() poll.
	struct sigaction sa_ign;
	std::memset(&sa_ign, 0, sizeof(sa_ign));
	sa_ign.sa_handler = SIG_IGN;
	sigemptyset(&sa_ign.sa_mask);
	sa_ign.sa_flags = 0;
	::sigaction(SIGPIPE, &sa_ign, nullptr);
#endif
#else // _WIN32
	std::signal(SIGINT, RequestShutdown);
	std::signal(SIGTERM, RequestShutdown);
#endif

	// ConnectAndRun does the EC bring-up (CRemoteConnect, ConnectToCore)
	// and then calls TextShell — which we've overridden so the daemon's
	// main loop runs there. On EC failure ConnectAndRun returns without
	// ever entering TextShell, which is the correct outcome for a
	// daemon that has nothing to do without amuled.
	ConnectAndRun(wxT("amuleapi"), wxString::FromAscii(VERSION));
	return 0;
}

void CamuleapiApp::TextShell(const wxString & /*prompt*/)
{
	// Resolve bind addr + port. CLI takes precedence over amuleapi.conf.
	std::string bind = m_apiConfig.ServerCfg().bind_address;
	unsigned port = m_apiConfig.ServerCfg().port;
	if (m_cliHasBindAddress && !m_cliBindAddress.IsEmpty()) {
		bind = std::string(m_cliBindAddress.utf8_str());
	}
	if (m_cliHasHttpPort && m_cliHttpPort > 0 && m_cliHttpPort < 65536) {
		port = static_cast<unsigned>(m_cliHttpPort);
	}

	// Bind-time hard gate against the "listening publicly with no
	// password configured" footgun. A daemon bound to a routable
	// interface before the operator runs `amuleapi --set-admin-pass`
	// would still answer the unauth surface (/api/v0/version) — and
	// any future read-without-auth surface — even though logins
	// return 503. Refuse to start instead.
	//
	// Loopback bind + empty passwords is fine; first-run flow IS
	// "start on loopback, then run --set-admin-pass".
	const bool non_loopback = (bind != "127.0.0.1" && bind != "::1" && bind != "localhost");
	const bool no_pass = m_apiConfig.AdminPasswordMd5().empty() && m_apiConfig.GuestPasswordMd5().empty();
	if (non_loopback && no_pass) {
		Show(CFormat(_("amuleapi: refusing to start with BindAddress=%s and no "
			       "admin/guest password configured - this would expose the "
			       "REST surface to anyone reachable on that interface. "
			       "Either bind to 127.0.0.1, or run "
			       "`amuleapi --set-admin-pass=<plain>` first.\n")) %
			wxString::FromUTF8(bind.c_str()));
		return;
	}

	// Build the JWT machinery from the loaded secret + a dispatcher
	// that holds the rate-limiter + revocation set by value. The
	// dispatcher reaches the State cache through CamuleapiApp; the
	// lambda below pins the dispatcher's lifetime to App's.
	m_jwt = std::unique_ptr<CJwt>(new CJwt(m_apiConfig.JwtSecret()));
	m_dispatcher =
		std::unique_ptr<CApiDispatcher>(new CApiDispatcher(m_apiConfig, *m_jwt, m_state, *this));
	CApiDispatcher *const dispatcher = m_dispatcher.get();
	auto handler = [dispatcher](const CHttpServer::Request &req) { return dispatcher->Dispatch(req); };

	// streaming resolver + handler for /api/v0/events. The
	// resolver picks every GET that matches the path (auth is
	// enforced inside the streaming handler — same role gate as
	// regular handlers).
	auto streaming_resolver = [](const CHttpServer::Request &req) {
		if (req.method != "GET" && req.method != "HEAD")
			return false;
		// Tolerate optional ?query / trailing slashes.
		const std::string &t = req.target;
		const std::size_t q = t.find('?');
		std::string path = (q == std::string::npos) ? t : t.substr(0, q);
		return path == "/api/v0/events";
	};
	auto streaming_handler = [dispatcher](const CHttpServer::Request &req,
					 CHttpServer::Writer &writer,
					 unsigned &http_status,
					 std::string &content_type,
					 std::map<std::string, std::string> &response_headers) {
		dispatcher->DispatchEvents(req, writer, http_status, content_type, response_headers);
	};
	// Preflight runs synchronously before the SSE worker thread is
	// spawned: short-circuit unauth requests with the standard 401
	// body so they can't tie up a streaming slot for the read-
	// timeout window.
	auto streaming_preflight =
		[dispatcher](const CHttpServer::Request &req) -> boost::optional<CHttpServer::Response> {
		return dispatcher->PreflightEvents(req);
	};

	m_http = std::unique_ptr<CHttpServer>(new CHttpServer());
	if (!m_http->Start(bind, port, handler, streaming_resolver, streaming_handler, streaming_preflight)) {
		Show(CFormat("amuleapi: HTTP server failed to start: %s\n") %
			wxString::FromUTF8(m_http->LastError().c_str()));
		return;
	}

	Show(CFormat(_("amuleapi: listening on http://%s:%d/\n")) % wxString::FromUTF8(bind.c_str()) %
		static_cast<int>(port));
	Show(CFormat(_("amuleapi: config dir %s\n")) % m_apiConfig.ConfigDir());
	Show(CFormat(_("amuleapi: aMule version %s; api v0\n")) % wxString::FromAscii(VERSION));

	// Refresher loop. One tick per second; HTTP threads read State
	// concurrently. EC roundtrips run on this thread (CRemoteConnect's
	// owner) but go through `SendRecvSerialized` so HTTP-thread
	// mutations can also call it under m_ec_mtx.
	//
	// `was_failed` tracks the success/failure edge so we wipe list
	// caches on the rising edge (failed → succeeded). The server's
	// CValueMap was reset across the disconnect; clearing first stops
	// stale entries lingering forever in the INC-delta path.
	bool was_failed = false;
	// Target 1 s wall-clock between tick starts. Measure the tick,
	// sleep the remainder; warn if a tick overruns the 3 s budget
	// (typically signals an EC stall or runaway SendRecvSerialized).
	// Fixed `tick + 4 × 250 ms sleep` drifts under EC-mutex contention.
	constexpr auto kTargetCycle = std::chrono::seconds(1);
	constexpr auto kSliceMs = std::chrono::milliseconds(250);
	constexpr auto kOverrunWarn = std::chrono::seconds(3);
	// Fail-loud on a sustained EC blackout. RefresherTick returns
	// false on any null packet from SendRecvSerialized — typically
	// amuled crashed, was killed, or the EC socket dropped. Today
	// the loop just retries forever; the daemon stays up serving
	// 503 ec_unavailable from every endpoint. After ~30 s of
	// failed ticks, log a sharp WARN so the operator's journal
	// shows it; after ~5 min, exit cleanly so a process supervisor
	// (systemd, launchd, docker restart=always) can bring the
	// whole pair back up. Reset on the first success.
	constexpr unsigned kEcFailWarnAfter = 30;
	constexpr unsigned kEcFailExitAfter = 300;
	unsigned ec_consecutive_failures = 0;
	bool ec_warn_logged = false;
	while (!g_shutdownRequested.load(std::memory_order_acquire)) {
		const auto cycle_start = std::chrono::steady_clock::now();
		if (was_failed) {
			m_state.ResetLists();
		}
		const bool ok = webapi::RefresherTick(*this, m_state);
		if (ok) {
			m_state.MarkTickSuccess();
			was_failed = false;
			ec_consecutive_failures = 0;
			ec_warn_logged = false;
			// Sole writer of m_last_seen. SSE-event publication runs
			// here, NOT inside RefresherTick — so mutation handlers
			// calling RefresherTick inline from the HTTP thread
			// don't race with this loop's diff walk.
			webapi::EmitDiffsForEventBus(*this, m_state);
		} else {
			m_state.MarkTickFailure();
			was_failed = true;
			++ec_consecutive_failures;
			if (!ec_warn_logged && ec_consecutive_failures >= kEcFailWarnAfter) {
				std::cerr << "amuleapi: WARN EC has returned null "
					     "for "
					  << ec_consecutive_failures << " consecutive ticks (~"
					  << ec_consecutive_failures
					  << " s). amuled may be down or the EC "
					     "socket may have dropped. Will exit "
					     "after "
					  << kEcFailExitAfter
					  << " failed ticks so a process supervisor "
					     "can restart the pair.\n";
				ec_warn_logged = true;
			}
			if (ec_consecutive_failures >= kEcFailExitAfter) {
				std::cerr << "amuleapi: FATAL EC has been silent "
					     "for "
					  << ec_consecutive_failures << " consecutive ticks. Exiting.\n";
				g_shutdownRequested.store(true, std::memory_order_release);
			}
		}
		const auto tick_end = std::chrono::steady_clock::now();
		const auto tick_duration = tick_end - cycle_start;
		if (tick_duration >= kOverrunWarn) {
			const auto ms =
				std::chrono::duration_cast<std::chrono::milliseconds>(tick_duration).count();
			std::cerr
				<< "amuleapi: WARN refresher tick took " << ms << " ms (> "
				<< std::chrono::duration_cast<std::chrono::milliseconds>(kOverrunWarn).count()
				<< " ms budget) — likely EC-mutex contention or a "
				   "stalled SendRecvSerialized.\n";
		}
		// Sleep the REMAINDER of the target cycle in small slices so
		// shutdown latency stays bounded. A tick that already
		// consumed >= the budget skips the sleep entirely so the
		// next cycle starts immediately.
		auto deadline = cycle_start + kTargetCycle;
		while (true) {
			if (g_shutdownRequested.load(std::memory_order_acquire))
				break;
			const auto now = std::chrono::steady_clock::now();
			if (now >= deadline)
				break;
			const auto remaining = deadline - now;
			std::this_thread::sleep_for(remaining < kSliceMs ? remaining : kSliceMs);
		}
	}

	Show(_("amuleapi: shutdown signal received; stopping...\n"));
}

const CECPacket *CamuleapiApp::SendRecvSerialized(const CECPacket *request)
{
	std::lock_guard<std::mutex> lock(m_ec_mtx);
	return SendRecvMsg_v2(request);
}

bool CamuleapiApp::IsServerPartialUpdateActive()
{
	// Inherited from CaMuleExternalConnector. No mutex needed —
	// it's a const bool snapshot taken at login.
	return CaMuleExternalConnector::IsServerPartialUpdateActive();
}

int CamuleapiApp::OnExit()
{
	// Tear down in reverse construction order: HTTP server first
	// (no in-flight Dispatch can reach a dangling dispatcher), then
	// dispatcher (references m_jwt), then m_jwt.
	//
	// Wake SSE drainers BEFORE Stop() returns so workers blocked on
	// the 15 s heartbeat bail out and release their dispatcher refs.
	// Without this, `m_dispatcher.reset()` below would race a
	// drainer mid-write against a destroyed dispatcher → UAF in the
	// signal-driven shutdown. m_http->Stop()'s join is then bounded.
	if (m_event_bus)
		m_event_bus->Shutdown();
	if (m_http) {
		m_http->Stop();
		m_http.reset();
	}
	m_dispatcher.reset();
	m_jwt.reset();
	return CaMuleExternalConnector::OnExit();
}

// Stub functions needed by the linker because ExternalConnector.cpp
// transitively references MuleNotify (via the EC tag handlers); the
// daemon-side bodies live in the monolithic amule binary, and console
// builds (amulecmd, amuleapi) supply no-ops since they never raise
// GUI notifications.
namespace MuleNotify
{
class CMuleNotiferBase;
void HandleNotification(const CMuleNotiferBase &) {}
void HandleNotificationAlways(const CMuleNotiferBase &) {}
} // namespace MuleNotify
