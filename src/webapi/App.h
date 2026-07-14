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

#ifndef WEBAPI_APP_H
#define WEBAPI_APP_H

#include <memory>

#include "ExternalConnector.h"

#include "AmuleApiConfig.h"
#include "EcService.h"
#include "EventBus.h"
#include "EventDiff.h"
#include "State.h"

#include "Jwt.h"
#include "RLE.h" // PartFileEncoderData

#include <cstdint>
#include <map>
#include <mutex>

class CApiDispatcher;
class CECPacket;
class CHttpServer;

// amuleapi daemon entry. Inherits CaMuleExternalConnector to reuse
// the EC bring-up machinery (CRemoteConnect, --host / --port /
// --password, ZLIB negotiation, MD5 password handling). Adds the
// amuleapi-specific CLI options (--bind, --http-port, --config-dir,
// --set-{admin,guest}-pass), config-file loading via
// CAmuleApiConfig, and a Boost.Beast HTTP server thread.
//
// TextShell is overridden so `ConnectAndRun` becomes "connect EC,
// spawn the HTTP thread, run the refresher loop on this (wxApp)
// thread, tear down on shutdown signal".
//
// Concurrent REST handlers reach EC only through
// `SendRecvSerialized`, which holds a process-wide mutex around
// every EC roundtrip — CRemoteConnect stays single-threaded as
// required, and concurrent mutations + the refresher tick interleave
// correctly.
class CamuleapiApp : public CaMuleExternalConnector
{
public:
	CamuleapiApp();
	~CamuleapiApp();

	const wxString GetGreetingTitle() override { return _("aMule REST API"); }

	bool OnInit() override;
	int OnRun() override;
	int OnExit() override;

	// Serialized EC roundtrip. Takes the EC lock, calls
	// SendRecvMsg_v2, releases. Callable from any thread; the
	// refresher and mutation handlers funnel every EC call
	// through here so amule's wx-socket-driven CRemoteConnect stays
	// single-reader by construction.
	//
	// Returns the response packet on success or nullptr if EC has
	// disconnected. Caller owns the returned packet.
	const CECPacket *SendRecvSerialized(const CECPacket *request);

	// True when amuled advertised EC_TAG_CAN_PARTIAL_UPDATE during
	// login. Refresher uses this to decide between "trust the
	// EC_TAG_FILE_REMOVED markers" and the legacy "bulk-delete
	// entries we didn't see this tick" path.
	bool IsServerPartialUpdateActive();

	// Version string of the connected amuled, captured from the
	// EC_TAG_SERVER_VERSION tag of the AUTH_OK handshake. Empty string
	// when EC is not (yet) connected, or when talking to a daemon old
	// enough to omit the tag. Read under m_ec_mtx since m_ECClient is
	// torn down / rebuilt across reconnects.
	wxString GetDaemonVersion();

	// Refresher needs the cache. Single CState instance per process.
	webapi::CState &State() { return m_state; }

	// Per-partfile RLE decoder state, persisted across ticks. amule's
	// EC server sends GAP_STATUS / PART_STATUS as differentially-
	// encoded blobs (each frame is XOR-deltaed against the previous
	// decoded buffer) so the decoder MUST retain state across calls.
	// Keyed by partfile ECID; entry erased when the file is removed.
	//
	// **Concurrency:** mutated only under `CState::m_mu` held
	// EXCLUSIVE — typically inside a `MutateDownloads` writer lambda.
	// `m_ec_mtx` is incidentally held across the same call stack
	// (`SendRecvSerialized` takes it) but the State write lock is
	// the actual serializer. The method name encodes the
	// precondition so a code-review reader notices it.
	std::map<std::uint32_t, PartFileEncoderData> &PartfileRleStateRequireStateWriteLock()
	{
		return m_partfile_rle;
	}

	// SSE event bus. The refresher publishes events after
	// each successful tick; streaming-handler threads drain from
	// here. Exposed by raw reference — CEventBus is internally
	// thread-safe. Lazy-constructed in OnInit so the operator-
	// configured ring capacity from amuleapi.conf is honored.
	webapi::CEventBus &EventBus() { return *m_event_bus; }

	// prior-tick snapshot used to compute event deltas.
	// Owned by the App; mutated AFTER each successful refresher
	// tick by `EmitDiffsAndUpdate`. Exposed so RefresherTick can
	// reach it without re-routing through CState (which is read-
	// only from the refresher's perspective post-tick).
	webapi::LastSeenState &LastSeenForEvents() { return m_last_seen; }

private:
	void OnInitCmdLine(wxCmdLineParser &parser) override;
	bool OnCmdLineParsed(wxCmdLineParser &parser) override;

	// Loads amuleapi.conf + amuleapi-jwt-secret + amuleapi-passwords.
	// Returns false on any unrecoverable error (missing required file,
	// wrong mode bits on POSIX, malformed INI). The error has already
	// been printed via Show() at the point of return.
	bool LoadAmuleapiConfig();

	// CLI-only flows. Both write the requested file under
	// m_amuleapiConfigDir with mode 0600 and exit immediately —
	// they never start the HTTP server or connect to EC.
	int RunSetAdminPass();
	int RunSetGuestPass();

	// TextShell override drives the refresher loop and the HTTP
	// server. Called by CaMuleExternalConnector::ConnectAndRun after
	// the EC connection is established; we override it so the daemon
	// never enters the interactive readline path that amulecmd uses.
	void TextShell(const wxString &prompt) override;

	CAmuleApiConfig m_apiConfig;
	webapi::CState m_state;
	// All EC roundtrips run on this service's single worker thread (bounded
	// FIFO queue). Sole owner of the EC socket after login, so it also
	// serialises m_ECClient — the reason m_ec_mtx below is now only needed
	// for the immutable-after-login version accessor.
	webapi::CEcService m_ec_service;
	std::mutex m_ec_mtx; // serializes m_ECClient
	std::unique_ptr<CJwt> m_jwt;
	std::unique_ptr<CApiDispatcher> m_dispatcher;
	std::unique_ptr<CHttpServer> m_http;
	std::map<std::uint32_t, PartFileEncoderData> m_partfile_rle;
	std::unique_ptr<webapi::CEventBus> m_event_bus;
	webapi::LastSeenState m_last_seen;

	// CLI capture: --bind / --http-port override the matching keys in
	// amuleapi.conf when present. --set-*-pass and --foreground are
	// runtime-mode toggles. The `m_cliHas*` flags discriminate between
	// "operator passed nothing" and "operator passed the default
	// value verbatim" — the base class' m_host / m_port / m_password
	// fields have no such predicate of their own.
	wxString m_cliBindAddress;
	long m_cliHttpPort = 0;
	wxString m_cliConfigDirOverride;
	wxString m_cliSetAdminPass;
	wxString m_cliSetGuestPass;
	// --amule-config-file: read the EC connection (host/port/hashed password)
	// straight from an amule.conf, same as amuleweb. Lets amule auto-start
	// amuleapi without a plaintext EC password.
	wxString m_cliAmuleConfigFile;
	bool m_cliHasAmuleConfigFile = false;
	bool m_cliHasBindAddress = false;
	bool m_cliHasHttpPort = false;
	bool m_cliHasSetAdminPass = false;
	bool m_cliHasSetGuestPass = false;
	// Did the operator pass --host / --port / --password explicitly?
	bool m_cliHasEcHost = false;
	bool m_cliHasEcPort = false;
	bool m_cliHasEcPassword = false;
};

DECLARE_APP(CamuleapiApp)

#endif // WEBAPI_APP_H
