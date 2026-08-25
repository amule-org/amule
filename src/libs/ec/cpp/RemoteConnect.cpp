//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2004-2011 Angel Vidal ( kry@amule.org )
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

#include "RemoteConnect.h"

#include "ECCrypt.h"
#include "ECLog.h"
#include "../../../Logger.h"

#include <common/SmartPtr.h> // Needed for CSmartPtr
#include <common/MD5Sum.h>
#include <common/Format.h>
#include "../../../amuleIPV4Address.h"
#include "../../../NetworkFunctions.h" // IsLoopbackIP / IsLanIP / IsLinkLocalIP / StringIPtoUint32

#include <wx/intl.h>
#include <common/StringFunctions.h> // unicode2char for stderr message
#include <algorithm>                // std::fill, wiping the ephemeral private key
#ifdef __WINDOWS__
#include <process.h> // _exit
#else
#include <unistd.h> // _exit
#endif

wxDEFINE_EVENT(wxEVT_EC_CONNECTION, wxEvent);

namespace
{
// Optional graceful-shutdown handler for OnLost(); see SetEcConnectionLostHandler.
void (*s_connectionLostHandler)() = nullptr;
} // namespace

void SetEcConnectionLostHandler(void (*handler)())
{
	s_connectionLostHandler = handler;
}

CECLoginPacket::CECLoginPacket(const wxString &client,
	const wxString &version,
	bool canZLIB,
	bool canUTF8numbers,
	bool canNotify,
	bool preferNoZlib,
	bool canMultiSearch,
	bool canChat,
	bool canChatSessions,
	bool canAEAD,
	const std::vector<uint8_t> &clientNonce,
	const std::vector<uint8_t> &clientPubKey)
: CECPacket(EC_OP_AUTH_REQ)
{
	AddTag(CECTag(EC_TAG_CLIENT_NAME, client));
	AddTag(CECTag(EC_TAG_CLIENT_VERSION, version));
	AddTag(CECTag(EC_TAG_PROTOCOL_VERSION, (uint64)EC_CURRENT_PROTOCOL_VERSION));

#ifdef EC_VERSION_ID
	CMD4Hash versionhash;
	wxCHECK2(versionhash.Decode(EC_VERSION_ID), /* Do nothing. */);
	AddTag(CECTag(EC_TAG_VERSION_ID, versionhash));
#endif

	// Send capabilities:
	// support ZLIB compression
	if (canZLIB)
		AddTag(CECEmptyTag(EC_TAG_CAN_ZLIB));
	// support encoding of integers as UTF-8
	if (canUTF8numbers)
		AddTag(CECEmptyTag(EC_TAG_CAN_UTF8_NUMBERS));
	// client accepts push messages
	if (canNotify)
		AddTag(CECEmptyTag(EC_TAG_CAN_NOTIFY));
	// client can decode the sentinel-extended children-count wire format
	// from CECTag::WriteChildren (#199). Always advertised by new
	// clients; old servers ignore the unknown tag.
	AddTag(CECEmptyTag(EC_TAG_CAN_LARGE_TAG_COUNT));
	// Client implements the partial-update INC_UPDATE protocol — server
	// may skip unchanged files and signal deletions explicitly via
	// `EC_TAG_FILE_REMOVED` instead of relying on absence-implies-
	// deletion. Always advertised by new clients; old servers ignore
	// the unknown tag and the server falls back to emitting alive-
	// marker tags for unchanged files (still backward-compatible).
	AddTag(CECEmptyTag(EC_TAG_CAN_PARTIAL_UPDATE));
	// Client applies the same skip-unchanged / explicit-removal rule to the
	// multi-search results union. Advertised separately from
	// EC_TAG_CAN_PARTIAL_UPDATE because only a client that implements it on
	// the *search* container may be sent skipped results; older clients omit
	// this tag and keep getting every result on every poll.
	AddTag(CECEmptyTag(EC_TAG_CAN_PARTIAL_SEARCH));
	// Client can read and write the daemon's shared-directory configuration
	// over EC (EC_OP_GET/SET_SHARED_DIRS). Always advertised; old daemons
	// ignore the unknown tag and simply never echo it back.
	AddTag(CECEmptyTag(EC_TAG_CAN_SHAREDDIRS_CONFIG));
	// Client can enumerate the daemon's searches with EC_OP_SEARCH_LIST, so it
	// sees searches it did not start itself (restored from disk, or begun by
	// another EC client). Always advertised; old daemons ignore the unknown tag
	// and never echo it, which is what keeps the client from sending an opcode
	// they would reject -- a pre-#680 daemon logs "invalid opcode received:
	// 0x60" and trips an assert on it.
	AddTag(CECEmptyTag(EC_TAG_CAN_SEARCH_LIST));
	// Client tells the server "we believe transit between us is fast
	// (loopback / LAN), so skip per-packet ZLIB up to the receiver
	// gate". The server honours this hint at WritePacket time; see
	// ECSocket.cpp `m_isLocalPeer`. The decision lives on the client
	// because only the client knows the IP it dialed — server-side
	// peer-IP inspection would misclassify e.g. WireGuard tunnel
	// endpoints as "local" when the underlying transit is anything but.
	// Old servers ignore the unknown tag and fall back to always-zlib.
	if (preferNoZlib)
		AddTag(CECEmptyTag(EC_TAG_PREFER_NO_ZLIB));
	// Client implements the multi-search protocol — addresses EC searches
	// by a daemon-allocated `EC_TAG_SEARCH_ID` so several run at once. Only
	// advertised by clients that read the ID back; old servers ignore the
	// unknown tag and the client stays single-search.
	if (canMultiSearch)
		AddTag(CECEmptyTag(EC_TAG_CAN_MULTI_SEARCH));
	// Client polls every open search's progress with ONE id-less
	// EC_OP_SEARCH_PROGRESS instead of one request per search. Strictly an
	// extension of multi-search, so it is only advertised alongside it: an
	// id-less progress request from a single-search client keeps its legacy
	// "the current search" meaning, which is what amulecmd's `search progress`
	// with no argument relies on. Old daemons ignore the unknown tag and never
	// echo it, and the client then falls back to polling per id.
	if (canMultiSearch)
		AddTag(CECEmptyTag(EC_TAG_CAN_SEARCH_PROGRESS_UNION));
	// Client wants incoming peer chat messages relayed over EC (amulegui
	// surfaces them read-only). Only advertised by clients with a chat
	// window; old servers ignore the unknown tag and never relay.
	if (canChat)
		AddTag(CECEmptyTag(EC_TAG_CAN_CHAT));
	// Separate from EC_TAG_CAN_CHAT above: a daemon that echoes that one may
	// still know none of the session opcodes, so the client needs its own
	// confirmation before sending any of them.
	if (canChatSessions)
		AddTag(CECEmptyTag(EC_TAG_CAN_CHAT_SESSIONS));
	// Transport encryption: the ciphers we can do, in preference order, our
	// half of the derivation salt, and our ephemeral public key. A daemon that
	// does not know these tags ignores them and the session stays in clear.
	//
	// The public key is not optional and has no capability tag of its own:
	// encryption has never shipped, so anything that offers EC_TAG_CAN_AEAD is
	// new enough to do X25519, and offering the cipher list without a key is
	// treated by the daemon as malformed rather than as an older client.
	if (canAEAD && !clientNonce.empty() && !clientPubKey.empty()) {
		const std::vector<uint8_t> ciphers = ECCrypt::SupportedCiphers();
		AddTag(CECTag(EC_TAG_CAN_AEAD, ciphers.size(), ciphers.data()));
		AddTag(CECTag(EC_TAG_AEAD_CLIENT_NONCE, clientNonce.size(), clientNonce.data()));
		AddTag(CECTag(EC_TAG_AEAD_CLIENT_PUBKEY, clientPubKey.size(), clientPubKey.data()));
	}
}

CECAuthPacket::CECAuthPacket(const wxString &pass, const std::vector<uint8_t> &clientConfirm)
: CECPacket(EC_OP_AUTH_PASSWD)
{
	CMD4Hash passhash;
	wxCHECK2(passhash.Decode(pass), /* Do nothing. */);
	AddTag(CECTag(EC_TAG_PASSWD_HASH, passhash));
	// Proof that we hold the credential over this exact handshake. The salted
	// challenge above proves the same thing, but says nothing about the key
	// exchange it travelled beside; this binds the two together.
	if (!clientConfirm.empty()) {
		AddTag(CECTag(EC_TAG_AEAD_CLIENT_CONFIRM, clientConfirm.size(), clientConfirm.data()));
	}
}

/*!
 * Connection to remote core
 *
 */

CRemoteConnect::CRemoteConnect(wxEvtHandler *evt_handler)
: CECMuleSocket(evt_handler != 0)
, m_ec_state(EC_INIT)
, m_req_fifo()
,
// Give application some indication about how fast requests are served
// When request fifo contain more that certain number of entries, it may
// indicate that either core or network is slowing us down
m_req_count(0)
,
// This is not mean to be absolute limit, because we can't drop requests
// out of calling context; it is just signal to application to slow down
m_req_fifo_thr(20)
, m_notifier(evt_handler)
, m_canZLIB(false)
, m_canUTF8numbers(false)
, m_canNotify(false)
, m_canAEAD(true)
, m_aeadNegotiated(false)
, m_preferNoZlib(false)
, m_forceZlib(false)
, m_serverSessionId(0)
, m_serverPartialUpdate(false)
, m_serverClientHistory(false)
, m_serverPartialSearch(false)
, m_canMultiSearch(false)
, m_serverMultiSearch(false)
, m_canChat(false)
, m_serverChat(false)
, m_canChatSessions(false)
, m_serverChatSessions(false)
, m_serverSharedDirsConfig(false)
, m_serverSearchList(false)
, m_serverSearchProgressUnion(false)
, m_lastReplyAt(std::chrono::steady_clock::now())
{
}

void CRemoteConnect::SetCapabilities(bool canZLIB, bool canUTF8numbers, bool canNotify)
{
	m_canZLIB = canZLIB;
	if (canZLIB) {
		m_my_flags |= EC_FLAG_ZLIB;
	}
	m_canUTF8numbers = canUTF8numbers;
	if (canUTF8numbers) {
		m_my_flags |= EC_FLAG_UTF8_NUMBERS;
	}
	m_canNotify = canNotify;
}

bool CRemoteConnect::ConnectToCore(const wxString &host,
	int port,
	const wxString &WXUNUSED(login),
	const wxString &pass,
	const wxString &client,
	const wxString &version)
{
	m_connectionPassword = pass;
	// Both ends hold md5(password) and neither sends it; the salted challenge
	// below replaces m_connectionPassword with a value that DOES go on the
	// wire, so capture the usable key material first.
	m_aeadSecret = pass.Lower();

	m_client = client;
	m_version = version;

	// don't even try to connect without a valid password
	if (m_connectionPassword.IsEmpty() || m_connectionPassword == "d41d8cd98f00b204e9800998ecf8427e") {
		m_server_reply = _("You must specify a non-empty password.");
		return false;
	} else {
		CMD4Hash hash;
		if (!hash.Decode(m_connectionPassword)) {
			m_server_reply = _("Invalid password, not a MD5 hash!");
			return false;
		} else if (hash.IsEmpty()) {
			m_server_reply = _("You must specify a non-empty password.");
			return false;
		}
	}

	amuleIPV4Address addr;

	// Report a resolution failure as itself. Discarding this return left
	// the address at 0.0.0.0 and surfaced as the generic "unable to
	// connect" further up, which says nothing about the actual cause —
	// a typo in the host, or a name that only has AAAA records (aMule
	// speaks IPv4 only, so there is nothing to dial there).
	if (!addr.Hostname(host)) {
		m_server_reply = CFormat(_("Could not resolve %s to an IPv4 address.")) % host;
		return false;
	}
	addr.Service(port);

	// Compute the prefer-no-ZLIB hint after host resolution: if we
	// dialed a loopback / RFC1918 LAN / RFC3927 link-local IP, the
	// transit is fast and per-packet ZLIB is pure overhead. Skip the
	// check entirely if the user opted out of ZLIB via /EC/ZLIB=0
	// (capability isn't advertised so the hint is moot) or set
	// `/EC/ForceZLIB=1` / `--force-zlib` to handle e.g. a WireGuard
	// tunnel endpoint that resolves to an RFC1918 IP but whose
	// underlying transit is slow Internet.
	// One fresh nonce per connection attempt: reusing it across attempts would
	// reuse a key, and a retry after a dropped socket is a new session.
	m_aeadClientNonce.clear();
	m_aeadOffered.clear();
	m_aeadEphPriv.clear();
	m_aeadEphPub.clear();
	m_aeadClientConfirm.clear();
	m_aeadExpectedServerConfirm.clear();
	if (m_canAEAD) {
		m_aeadClientNonce = ECCrypt::RandomBytes(ECCrypt::NONCE_TAG_LEN);
		m_aeadOffered = ECCrypt::SupportedCiphers();
		// One fresh key pair per attempt for the same reason as the nonce, and
		// more sharply: reusing it across attempts is what forward secrecy is
		// about, since a key that outlives the session is one that can be
		// stolen after it.
		if (!ECCrypt::GenerateX25519KeyPair(m_aeadEphPriv, m_aeadEphPub)) {
			m_aeadEphPriv.clear();
			m_aeadEphPub.clear();
		}
		if (m_aeadClientNonce.empty() || m_aeadEphPub.empty()) {
			// No usable randomness: offer nothing rather than derive a key
			// from a predictable salt or a predictable private key.
			m_aeadOffered.clear();
			m_aeadClientNonce.clear();
			m_aeadEphPriv.clear();
			m_aeadEphPub.clear();
		}
	}

	m_preferNoZlib = false;
	if (m_canZLIB && !m_forceZlib) {
		uint32 resolved_ip = 0;
		if (StringIPtoUint32(addr.IPAddress(), resolved_ip)) {
			m_preferNoZlib = IsLoopbackIP(resolved_ip) || IsLanIP(resolved_ip) ||
					 IsLinkLocalIP(resolved_ip);
		}
	}

	if (ConnectSocket(addr)) {
		// We get here only in case of synchronous connect.
		// Otherwise we continue in OnConnect.
		CECLoginPacket login_req(m_client,
			m_version,
			m_canZLIB,
			m_canUTF8numbers,
			m_canNotify,
			m_preferNoZlib,
			m_canMultiSearch,
			m_canChat,
			m_canChatSessions,
			m_canAEAD,
			m_aeadClientNonce,
			m_aeadEphPub);

		CSmartPtr<const CECPacket> getSalt(SendRecvPacket(&login_req));
		m_ec_state = EC_REQ_SENT;

		// Honour the AUTH_SALT verdict before sending the credential. The
		// require-encryption refusal (we offered AEAD but the core did not
		// negotiate it) must stop here rather than send AUTH_PASSWD in clear;
		// ProcessAuthPacket has already set m_server_reply and closed the
		// socket. The async path does the same check in OnPacketReceived.
		if (!ProcessAuthPacket(getSalt.get())) {
			return false;
		}

		CECAuthPacket passwdPacket(m_connectionPassword, m_aeadClientConfirm);

		CSmartPtr<const CECPacket> reply(SendRecvPacket(&passwdPacket));
		m_ec_state = EC_PASSWD_SENT;

		return ProcessAuthPacket(reply.get());
	} else if (m_notifier) {
		m_ec_state = EC_CONNECT_SENT;
	} else {
		return false;
	}

	return true;
}

bool CRemoteConnect::IsConnectedToLocalHost()
{
	amuleIPV4Address addr;
	return addr.Hostname(GetPeer()) ? addr.IsLocalHost() : false;
}

void CRemoteConnect::WriteDoneAndQueueEmpty() {}

void CRemoteConnect::OnConnect()
{
	// Start the watchdog clock here, not at construction: a wrapper reused
	// across a reconnect would otherwise carry the previous connection's
	// staleness into the new one and trip the timeout immediately.
	m_lastReplyAt = std::chrono::steady_clock::now();
	// Apply the EC-tuned socket options now that the underlying asio
	// socket is fully connected on the async path (sync clients got
	// them inside CECMuleSocket::InternalConnect already; this is the
	// amulegui / amuleweb / amuleapi side where InternalConnect returns
	// before the connect actually completes).
	ApplyEcSocketOptions();

	if (m_notifier) {
		wxASSERT(m_ec_state == EC_CONNECT_SENT);
		CECLoginPacket login_req(m_client,
			m_version,
			m_canZLIB,
			m_canUTF8numbers,
			m_canNotify,
			m_preferNoZlib,
			m_canMultiSearch,
			m_canChat,
			m_canChatSessions,
			m_canAEAD,
			m_aeadClientNonce,
			m_aeadEphPub);
		CECSocket::SendPacket(&login_req);

		m_ec_state = EC_REQ_SENT;
	} else {
		// do nothing, calling code will take from here
	}
}

uint64 CRemoteConnect::MillisecondsSinceLastReply() const
{
	return static_cast<uint64>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - m_lastReplyAt)
					   .count());
}

void CRemoteConnect::OnLost()
{
	if (m_notifier) {
		// A dial is still pending: the async connect hasn't completed, so
		// EC_CONNECT_SENT hasn't advanced to EC_REQ_SENT yet. A LibSocketLost
		// arriving now is not the death of this dial — it's a stale event that
		// was already queued for the *previous* connection before the socket
		// impl was swapped (CLibSocket::ResetForReconnect). It still targets
		// this reused wrapper, but IsDestroying() now sees the fresh impl, so
		// it slips through. Swallowing it stops that stale lost from aborting
		// an in-flight reconnect; a genuinely failed dial is caught instead by
		// amulegui's connect-timeout watchdog (CamuleRemoteGuiApp::OnConnectTimeout).
		if (m_ec_state == EC_CONNECT_SENT) {
			return;
		}
		// Notify app of failure — amulegui's wxEvent handler flips the
		// UI to "disconnected" and stops trying to update.
		wxECSocketEvent event(wxEVT_EC_CONNECTION, false, _("Connection failure"));
		m_notifier->AddPendingEvent(event);
		return;
	}
	// Headless EC clients (amulecmd, amuleweb) construct CRemoteConnect
	// with NULL m_notifier. Continuing to run would mean serving stale
	// data in amuleweb (HTTP requests still return the template shell
	// without live amuled data, no error visible to the user) or
	// sitting at the amulecmd prompt with a dead socket. Failing fast
	// is the right semantic — supervisor (systemd unit / docker /
	// shell loop) is the recovery layer and decides whether to restart.
	fprintf(stderr, "%s\n", (const char *)unicode2char(_("External Connection lost - exiting.")));
	fflush(stderr);
	if (s_connectionLostHandler) {
		// A client (amuleapi) opted into a graceful shutdown: hand off to it
		// and return, so the orderly teardown runs on the client's own thread
		// instead of racing static destructors from this asio callback. The
		// client is responsible for actually stopping (it requests its normal
		// shutdown path, e.g. flipping the main-loop's shutdown flag).
		s_connectionLostHandler();
		return;
	}
	// _exit instead of exit() because the asio worker thread that
	// just fired this is mid-callback; racing static destructors
	// against it risks the kind of use-after-free #748 was about.
	// The OS reaps file descriptors / sockets on exit anyway.
	_exit(1);
}

void CRemoteConnect::SetupAEADFromSalt(const CECPacket *reply)
{
	m_aeadNegotiated = false;
	m_aeadClientConfirm.clear();
	m_aeadExpectedServerConfirm.clear();
	if (!m_canAEAD || m_aeadClientNonce.empty() || m_aeadEphPriv.empty()) {
		return;
	}
	const CECTag *cipherTag = reply->GetTagByName(EC_TAG_AEAD_CIPHER);
	const CECTag *serverNonceTag = reply->GetTagByName(EC_TAG_AEAD_SERVER_NONCE);
	const CECTag *serverPubTag = reply->GetTagByName(EC_TAG_AEAD_SERVER_PUBKEY);
	if (cipherTag == nullptr || serverNonceTag == nullptr) {
		// An old daemon, or one with encryption switched off. Nothing to do:
		// the session stays in clear.
		return;
	}
	if (serverPubTag == nullptr) {
		// A daemon that named a cipher but sent no key. Nothing here can be
		// derived without forward secrecy, and quietly falling back to the
		// password-keyed derivation is precisely the downgrade this exists to
		// prevent, so refuse.
		AddDebugLogLineN(logEC, "AEAD: daemon sent no public key, staying in clear");
		return;
	}

	const uint8_t cipher = (uint8_t)cipherTag->GetInt();
	if (!ECCrypt::IsCipherSupported(cipher)) {
		AddDebugLogLineN(logEC, "AEAD: daemon chose a cipher we cannot do");
		return;
	}

	// Validate the received lengths here, at the point of receipt, the way the
	// daemon validates the client's. X25519Agree and Session::Init would reject
	// a wrong length downstream too, but checking locally keeps the "every field
	// the transcript concatenates is fixed-length" invariant obvious rather than
	// resting on a distant guard.
	if (serverNonceTag->GetTagDataLen() != ECCrypt::NONCE_TAG_LEN ||
		serverPubTag->GetTagDataLen() != ECCrypt::X25519_KEY_LEN) {
		AddDebugLogLineN(logEC, "AEAD: daemon nonce or public key has a bad length");
		return;
	}

	const uint8_t *serverNonceData = (const uint8_t *)serverNonceTag->GetTagData();
	const std::vector<uint8_t> serverNonce(
		serverNonceData, serverNonceData + serverNonceTag->GetTagDataLen());
	const uint8_t *serverPubData = (const uint8_t *)serverPubTag->GetTagData();
	const std::vector<uint8_t> serverPub(serverPubData, serverPubData + serverPubTag->GetTagDataLen());

	// The channel key comes from here and from nothing else. Deriving it from
	// the password instead is what cost forward secrecy: the password outlives
	// the session, this does not.
	std::vector<uint8_t> shared;
	const bool agreed = ECCrypt::X25519Agree(m_aeadEphPriv, serverPub, shared);
	// Wipe the private half the moment it has done its one job, whether or not
	// that job succeeded. Holding it any longer is the only thing that could
	// reopen a recorded session later.
	ECCrypt::SecureWipe(m_aeadEphPriv);
	if (!agreed) {
		AddDebugLogLineN(logEC, "AEAD: bad daemon public key, staying in clear");
		return;
	}

	// Transcript: everything both sides exchanged, so a byte edited anywhere in
	// the handshake yields a different key on each end and the first sealed
	// packet fails, rather than the session silently using a weaker cipher.
	const std::vector<uint8_t> transcript = ECCrypt::BuildTranscript(
		m_aeadOffered, cipher, m_aeadClientNonce, serverNonce, m_aeadEphPub, serverPub);

	if (!SetupAEAD(cipher, shared, serverNonce, m_aeadClientNonce, transcript, false)) {
		AddDebugLogLineN(logEC, "AEAD: key derivation failed, staying in clear");
		return;
	}

	// The password no longer keys the channel, so it can no longer be what
	// stops a man in the middle -- an attacker can run an exchange with each of
	// us and relay. These are what stop it instead: the transcripts on the two
	// legs differ, so the tags do too.
	const wxCharBuffer secret = m_aeadSecret.utf8_str();
	std::vector<uint8_t> credential(
		(const uint8_t *)secret.data(), (const uint8_t *)secret.data() + strlen(secret.data()));
	m_aeadClientConfirm = ECCrypt::ConfirmTag(credential, transcript, "ec-confirm-client");
	m_aeadExpectedServerConfirm = ECCrypt::ConfirmTag(credential, transcript, "ec-confirm-server");
	ECCrypt::SecureWipe(credential);

	m_aeadNegotiated = true; // EC_OP_AUTH_PASSWD is the last packet that must go out in clear.
	EnableAEADAfterNextWrite();
	AddDebugLogLineN(logEC, CFormat("AEAD: negotiated %s with X25519") % ECCrypt::CipherName(cipher));
}

bool CRemoteConnect::VerifyServerConfirm(const CECPacket *reply) const
{
	if (m_aeadExpectedServerConfirm.empty()) {
		return false;
	}
	const CECTag *tag = reply->GetTagByName(EC_TAG_AEAD_SERVER_CONFIRM);
	if (tag == nullptr) {
		AddDebugLogLineN(logEC, "AEAD: daemon sent no key confirmation");
		return false;
	}
	if (!tag->IsCustom()) {
		// GetTagData() asserts on a non-custom tag; a peer must not be able to
		// trip that in a debug build by mistyping the confirm tag.
		AddDebugLogLineN(logEC, "AEAD: daemon key confirmation has the wrong tag type");
		return false;
	}
	const uint8_t *data = (const uint8_t *)tag->GetTagData();
	const std::vector<uint8_t> got(data, data + tag->GetTagDataLen());
	if (!ECCrypt::ConstantTimeEquals(got, m_aeadExpectedServerConfirm)) {
		AddDebugLogLineN(logEC, "AEAD: daemon key confirmation does not match");
		return false;
	}
	return true;
}

const CECPacket *CRemoteConnect::OnPacketReceived(const CECPacket *packet, uint32 trueSize)
{
	CECPacket *next_packet = 0;
	// Liveness stamp for the reply watchdog. Taken for EVERY inbound packet,
	// including the handshake ones, so a connection that has only just come up
	// is never mistaken for one that has gone quiet.
	m_lastReplyAt = std::chrono::steady_clock::now();
	m_req_count--;
	packet->DebugPrint(true, trueSize);
	switch (m_ec_state) {
	case EC_REQ_SENT:
		if (ProcessAuthPacket(packet)) {
			CECAuthPacket passwdPacket(m_connectionPassword, m_aeadClientConfirm);
			CECSocket::SendPacket(&passwdPacket);
			m_ec_state = EC_PASSWD_SENT;
		}
		break;
	case EC_PASSWD_SENT:
		ProcessAuthPacket(packet);
		break;
	case EC_OK:
		if (IsCryptReady() && !WasLastPacketEncrypted()) {
			// Established encrypted session: every response must arrive
			// sealed. A cleartext packet here is an injection attempt; drop
			// the connection rather than hand a forged reply to a handler.
			AddDebugLogLineN(logEC, "EC: cleartext packet on an encrypted session, dropping");
			CloseSocket();
			break;
		}
		if (!m_req_fifo.empty()) {
			CECPacketHandlerBase *handler = m_req_fifo.front();
			m_req_fifo.pop_front();
			if (handler) {
				handler->HandlePacket(packet);
			}
		} else {
			printf("EC error - packet received, but request fifo is empty\n");
		}
		break;
	default:
		break;
	}

	// no reply by default
	return next_packet;
}

/*
 * Our requests are served by core in FCFS order. And core always replies. So, even
 * if we're not interested in reply, we preserve place in request fifo.
 */
void CRemoteConnect::SendRequest(CECPacketHandlerBase *handler, const CECPacket *request)
{
	m_req_count++;
	m_req_fifo.push_back(handler);
	CECSocket::SendPacket(request);
}

void CRemoteConnect::SendPacket(const CECPacket *request)
{
	SendRequest(0, request);
}

void CRemoteConnect::DiscardRequestQueue()
{
	// The core never replies to requests that were on the air when the
	// socket died, so their handlers would linger and every reply on the
	// reconnected session would pop the wrong (stale) handler off the FIFO.
	// Rewind each orphaned handler's request state so it re-requests, then
	// clear the queue and the in-flight counter (see header for the #444
	// wiped-list symptom this prevents).
	for (CECPacketHandlerBase *handler : m_req_fifo) {
		if (handler) {
			handler->AbortPendingRequest();
		}
	}
	m_req_fifo.clear();
	m_req_count = 0;
}

void CRemoteConnect::NotifyConnectionResult(bool connected)
{
	if (m_notifier) {
		wxECSocketEvent event(wxEVT_EC_CONNECTION, connected, m_server_reply);
		m_notifier->AddPendingEvent(event);
	}
}

bool CRemoteConnect::ProcessAuthPacket(const CECPacket *reply)
{
	bool result = false;

	if (!reply) {
		m_server_reply = _("EC connection failed. Empty reply.");
		CloseSocket();
	} else {
		if ((m_ec_state == EC_REQ_SENT) && (reply->GetOpCode() == EC_OP_AUTH_SALT)) {
			const CECTag *passwordSalt = reply->GetTagByName(EC_TAG_PASSWD_SALT);
			if (NULL != passwordSalt) {
				// Set up transport encryption before the challenge below
				// overwrites m_connectionPassword.
				SetupAEADFromSalt(reply);
				if (m_canAEAD && !m_aeadNegotiated) {
					// Encryption is required unless the user opted out.
					// m_canAEAD is that opt-out: with it off we never offer,
					// so this branch cannot be reached and a clear session is
					// only ever the user's explicit choice. The core did not
					// negotiate encryption, and we cannot tell an older aMule
					// without encryption support from an on-path attacker who
					// stripped the offer, so we refuse rather than send the
					// credential and every later command in clear.
					m_server_reply =
						m_aeadOffered.empty()
							? _("Could not set up connection encryption. "
							    "Connection closed.")
							: _("The core did not negotiate an encrypted "
							    "connection. It may be an older aMule without "
							    "encryption support, or the connection may have "
							    "been tampered with. To connect without "
							    "encryption, turn it off in the connection "
							    "settings.");
					m_ec_state = EC_FAIL;
					CloseSocket();
					NotifyConnectionResult(false);
					return false;
				}
				wxString saltHash = MD5Sum(CFormat("%lX") % passwordSalt->GetInt()).GetHash();
				m_connectionPassword =
					MD5Sum(m_connectionPassword.Lower() + saltHash).GetHash();
				m_ec_state = EC_SALT_RECEIVED;
				return true;
			} else {
				m_server_reply = _("External Connection: Bad reply, handshake failed. "
						   "Connection closed.");
				m_ec_state = EC_FAIL;
				CloseSocket();
			}
		} else if ((m_ec_state == EC_PASSWD_SENT) && (reply->GetOpCode() == EC_OP_AUTH_OK)) {
			// The daemon must prove it holds the credential too, over this
			// exact handshake. Until this passes we know only that we are
			// talking to something that completed a key exchange -- which a
			// relay can also do, twice. Checked before anything in the reply is
			// believed, and fatal rather than a downgrade: a session that
			// reaches here without a valid tag is one being relayed.
			if (m_aeadNegotiated && !VerifyServerConfirm(reply)) {
				m_server_reply = _("External Connection: the daemon failed to prove it "
						   "knows the password. Connection closed.");
				m_ec_state = EC_FAIL;
				CloseSocket();
				NotifyConnectionResult(false);
				return false;
			}
			m_ec_state = EC_OK;
			result = true;
			if (reply->GetTagByName(EC_TAG_SERVER_VERSION)) {
				m_serverVersion = reply->GetTagByName(EC_TAG_SERVER_VERSION)->GetStringData();
				m_server_reply =
					_("Succeeded! Connection established to aMule ") + m_serverVersion;
			} else {
				m_server_reply = _("Succeeded! Connection established.");
			}
			// Mirror server's negotiated capabilities into m_my_flags so
			// outgoing per-packet flags include them (auto-stripped by
			// `flags &= m_my_flags` in CECSocket::WritePacket otherwise).
			// EC_TAG_CAN_LARGE_TAG_COUNT only appears in AUTH_OK from
			// new daemons (#199); old daemons just don't echo the tag,
			// and the sentinel wire format stays disabled in both
			// directions for the duration of this connection.
			if (reply->GetTagByName(EC_TAG_CAN_LARGE_TAG_COUNT)) {
				m_my_flags |= EC_FLAG_LARGE_TAG_COUNT;
			}
			// Server confirms it speaks the partial-update protocol:
			// `Get_EC_Response_GetUpdate` may now omit unchanged files
			// and emit explicit `EC_TAG_FILE_REMOVED` markers, and our
			// INC_UPDATE handler must skip the bulk
			// "missing-from-response == deleted" loop. Old daemons
			// (#727 pre-fix or earlier) don't echo this tag and the
			// client stays on the legacy bulk-deletion path, which
			// the new server keeps compatible by emitting alive-marker
			// tags for unchanged files (#713).
			if (reply->GetTagByName(EC_TAG_CAN_PARTIAL_UPDATE)) {
				m_serverPartialUpdate = true;
			}
			// Which daemon process we're talking to. Stays 0 against a
			// daemon that doesn't send it, which the reconnect path
			// reads as "can't tell" and handles the safe way.
			if (const CECTag *sessionTag = reply->GetTagByName(EC_TAG_SESSION_ID)) {
				m_serverSessionId = sessionTag->GetInt();
			}
			// Server confirms it knows EC_OP_GET_CLIENT_HISTORY. No echo
			// means the Known-clients tab stays empty rather than the
			// request being tried and failing: on a debug daemon the
			// unknown opcode asserts rather than answering EC_OP_FAILED.
			if (reply->GetTagByName(EC_TAG_CAN_CLIENT_HISTORY)) {
				m_serverClientHistory = true;
			}
			if (reply->GetTagByName(EC_TAG_CAN_PARTIAL_SEARCH)) {
				m_serverPartialSearch = true;
			}
			// Server confirmed multi-search: it will allocate a distinct
			// `EC_TAG_SEARCH_ID` per search. Old daemons omit the echo and
			// the client stays on the single-search sentinel path.
			if (reply->GetTagByName(EC_TAG_CAN_MULTI_SEARCH)) {
				m_serverMultiSearch = true;
			}
			// Server confirmed it answers an id-less EC_OP_SEARCH_PROGRESS
			// with every open search's progress as children, so one request
			// covers every tab. Old daemons omit the echo and the client
			// keeps polling one request per search id.
			if (reply->GetTagByName(EC_TAG_CAN_SEARCH_PROGRESS_UNION)) {
				m_serverSearchProgressUnion = true;
			}
			// Server confirmed chat relay: it will buffer incoming peer
			// messages and hand them out via EC_OP_GET_CHAT_MESSAGES. Old
			// daemons omit the echo and the client never polls for chat.
			if (reply->GetTagByName(EC_TAG_CAN_CHAT)) {
				m_serverChat = true;
			}
			// Server confirmed the chat session ops. Its own tag, because
			// EC_TAG_CAN_CHAT above is echoed by daemons that predate them:
			// gating on that one would send EC_OP_GET_CHAT_SESSIONS to a core
			// whose dispatcher asserts on it.
			if (reply->GetTagByName(EC_TAG_CAN_CHAT_SESSIONS)) {
				m_serverChatSessions = true;
			}
			// Server confirmed it serves EC_OP_SEARCH_LIST. Old daemons omit
			// the echo and the client must not send that opcode: they have no
			// case for it, so it lands in ProcessRequest2's unknown-opcode
			// branch, which logs and asserts.
			if (reply->GetTagByName(EC_TAG_CAN_SEARCH_LIST)) {
				m_serverSearchList = true;
			}
			// Server confirmed it serves the shared-directory configuration
			// ops. Old daemons omit the echo and the GUI keeps the shared
			// folders panel read-only rather than silently discarding edits.
			if (reply->GetTagByName(EC_TAG_CAN_SHAREDDIRS_CONFIG)) {
				m_serverSharedDirsConfig = true;
			}
		} else {
			m_ec_state = EC_FAIL;
			const CECTag *reason = reply->GetTagByName(EC_TAG_STRING);
			if (reason != NULL) {
				m_server_reply = wxString(_("External Connection: Access denied because: ")) +
						 wxGetTranslation(reason->GetStringData());
			} else {
				m_server_reply = _("External Connection: Handshake failed.");
			}
			CloseSocket();
		}
	}
	NotifyConnectionResult(result);
	return result;
}

/******************** EC API ***********************/

void CRemoteConnect::StartKad()
{
	CECPacket req(EC_OP_KAD_START);
	SendPacket(&req);
}

void CRemoteConnect::StopKad()
{
	CECPacket req(EC_OP_KAD_STOP);
	SendPacket(&req);
}

void CRemoteConnect::ConnectED2K(uint32 ip, uint16 port)
{
	CECPacket req(EC_OP_SERVER_CONNECT);
	if (ip && port) {
		req.AddTag(CECTag(EC_TAG_SERVER, EC_IPv4_t(ip, port)));
	}
	SendPacket(&req);
}

void CRemoteConnect::DisconnectED2K()
{
	CECPacket req(EC_OP_SERVER_DISCONNECT);
	SendPacket(&req);
}

void CRemoteConnect::RemoveServer(uint32 ip, uint16 port)
{
	CECPacket req(EC_OP_SERVER_REMOVE);
	if (ip && port) {
		req.AddTag(CECTag(EC_TAG_SERVER, EC_IPv4_t(ip, port)));
	}
	SendPacket(&req);
}
// File_checked_for_headers
