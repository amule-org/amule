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
	// Client can decode the sentinel-extended children-count wire format from
	// CECTag::WriteChildren (#199).
	AddTag(CECEmptyTag(EC_TAG_CAN_LARGE_TAG_COUNT));
	// Client implements the partial-update INC_UPDATE protocol: the server may skip
	// unchanged files and mark deletions with EC_TAG_FILE_REMOVED instead of
	// absence.
	AddTag(CECEmptyTag(EC_TAG_CAN_PARTIAL_UPDATE));
	// Same skip-unchanged / explicit-removal rule on the multi-search results union.
	// Separate from EC_TAG_CAN_PARTIAL_UPDATE because only a client that implements
	// it on the *search* container may be sent skipped results.
	AddTag(CECEmptyTag(EC_TAG_CAN_PARTIAL_SEARCH));
	// Client can read and write the daemon's shared directories over EC
	// (EC_OP_GET/SET_SHARED_DIRS).
	AddTag(CECEmptyTag(EC_TAG_CAN_SHAREDDIRS_CONFIG));
	// Client can enumerate the daemon's searches with EC_OP_SEARCH_LIST, so it sees
	// searches it did not start. A pre-#680 daemon never echoes it and asserts on the
	// opcode, so the echo is what keeps the client from sending it.
	AddTag(CECEmptyTag(EC_TAG_CAN_SEARCH_LIST));
	// Client believes transit between us is fast (loopback / LAN), so the server may
	// skip per-packet ZLIB up to the receiver gate. The decision lives here because
	// only the client knows the IP it dialed: server-side peer-IP inspection would
	// misclassify a WireGuard tunnel endpoint as local.
	if (preferNoZlib)
		AddTag(CECEmptyTag(EC_TAG_PREFER_NO_ZLIB));
	// Client implements multi-search: EC searches are addressed by a daemon-allocated
	// `EC_TAG_SEARCH_ID`, so several run at once.
	if (canMultiSearch)
		AddTag(CECEmptyTag(EC_TAG_CAN_MULTI_SEARCH));
	// Client polls every open search with ONE id-less EC_OP_SEARCH_PROGRESS. Only
	// advertised alongside multi-search: for a single-search client an id-less
	// request keeps its legacy "the current search" meaning, which amulecmd's
	// argument-less `search progress` relies on.
	if (canMultiSearch)
		AddTag(CECEmptyTag(EC_TAG_CAN_SEARCH_PROGRESS_UNION));
	// Client wants incoming peer chat messages relayed over EC (amulegui shows them
	// read-only). Only advertised by clients with a chat window.
	if (canChat)
		AddTag(CECEmptyTag(EC_TAG_CAN_CHAT));
	// Separate from EC_TAG_CAN_CHAT: a daemon that echoes that one may still know
	// none of the session opcodes.
	if (canChatSessions)
		AddTag(CECEmptyTag(EC_TAG_CAN_CHAT_SESSIONS));
	// Transport encryption: our ciphers in preference order, our half of the
	// derivation salt, and our ephemeral public key. A daemon that does not know
	// these tags ignores them and the session stays in clear. The public key needs no
	// capability tag of its own: encryption has never shipped, so anything offering
	// EC_TAG_CAN_AEAD can do X25519, and a cipher list without a key is malformed
	// rather than old.
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
	// Binds the credential to this exact handshake. The salted challenge alone says
	// nothing about the key exchange it travelled beside.
	if (!clientConfirm.empty()) {
		AddTag(CECTag(EC_TAG_AEAD_CLIENT_CONFIRM, clientConfirm.size(), clientConfirm.data()));
	}
}

/*!
 * Connection to remote core
 */

CRemoteConnect::CRemoteConnect(wxEvtHandler *evt_handler)
: CECMuleSocket(evt_handler != 0)
, m_ec_state(EC_INIT)
, m_req_fifo()
,
// Depth of the request fifo indicates how fast requests are served: a long queue
// means the core or the network is slowing us down.
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

bool CRemoteConnect::ConnectToCore(
	const wxString &host, int port, const wxString &pass, const wxString &client, const wxString &version)
{
	m_connectionPassword = pass;
	// The salted challenge below overwrites m_connectionPassword with a value that
	// goes on the wire, so capture the usable key material first.
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

	// Report a resolution failure as itself. Discarding this return left the address
	// at 0.0.0.0 and surfaced as the generic "unable to connect", which says nothing
	// about a typo in the host or a name with only AAAA records (aMule speaks IPv4
	// only).
	if (!addr.Hostname(host)) {
		m_server_reply = CFormat(_("Could not resolve %s to an IPv4 address.")) % host;
		return false;
	}
	addr.Service(port);

	// Compute the prefer-no-ZLIB hint after host resolution: a loopback, RFC1918 or
	// RFC3927 IP means fast transit, where per-packet ZLIB is pure overhead. Skipped
	// if the user disabled ZLIB (the capability is not advertised) or set ForceZLIB,
	// for a WireGuard endpoint that resolves to an RFC1918 IP over slow transit.
	//
	// One fresh nonce per attempt: reusing it reuses a key, and a retry after a
	// dropped socket is a new session.
	m_aeadClientNonce.clear();
	m_aeadOffered.clear();
	m_aeadEphPriv.clear();
	m_aeadEphPub.clear();
	m_aeadClientConfirm.clear();
	m_aeadExpectedServerConfirm.clear();
	if (m_canAEAD) {
		m_aeadClientNonce = ECCrypt::RandomBytes(ECCrypt::NONCE_TAG_LEN);
		m_aeadOffered = ECCrypt::SupportedCiphers();
		// One fresh key pair per attempt, for the same reason as the nonce and more
		// sharply: a key that outlives the session is one that can be stolen after it.
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

		// Honour the AUTH_SALT verdict before sending the credential: a
		// require-encryption refusal must stop here rather than send AUTH_PASSWD in
		// clear. ProcessAuthPacket has already set m_server_reply and closed the socket.
		// OnPacketReceived does the same check on the async path.
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
	// Start the watchdog clock here, not at construction: a wrapper reused across a
	// reconnect would carry the previous connection's staleness into the new one.
	m_lastReplyAt = std::chrono::steady_clock::now();
	// Apply the EC-tuned socket options now that the asio socket is fully connected.
	// Sync clients got them in CECMuleSocket::InternalConnect; this is the amulegui /
	// amuleweb / amuleapi side, where InternalConnect returns before the connect
	// completes.
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
		// A dial is still pending: the async connect has not completed, so
		// EC_CONNECT_SENT has not advanced to EC_REQ_SENT. A LibSocketLost here is a
		// stale event from the PREVIOUS connection that still targets this reused
		// wrapper, and IsDestroying() sees the fresh impl, so it slips through.
		// Swallowing it stops a stale lost aborting an in-flight reconnect; a truly
		// failed dial is caught by amulegui's connect-timeout watchdog.
		if (m_ec_state == EC_CONNECT_SENT) {
			return;
		}
		// Notify app of failure -- amulegui's wxEvent handler flips the
		// UI to "disconnected" and stops trying to update.
		wxECSocketEvent event(wxEVT_EC_CONNECTION, false, _("Connection failure"));
		m_notifier->AddPendingEvent(event);
		return;
	}
	// Headless EC clients (amulecmd, amuleweb) pass a NULL m_notifier. Continuing
	// would mean amuleweb serving an empty shell with no error, or amulecmd at a
	// prompt with a dead socket. Failing fast is right; the supervisor recovers.
	fprintf(stderr, "%s\n", (const char *)unicode2char(_("External Connection lost - exiting.")));
	fflush(stderr);
	if (s_connectionLostHandler) {
		// amuleapi opted into a graceful shutdown: hand off so teardown runs on its own
		// thread instead of racing static destructors from this asio callback.
		s_connectionLostHandler();
		return;
	}
	// _exit, not exit(): the asio worker thread that fired this is mid-callback, and
	// racing static destructors against it risks the use-after-free of #748. The OS
	// reaps descriptors anyway.
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
		// A daemon that named a cipher but sent no key. Nothing can be derived without
		// forward secrecy, and falling back to the password-keyed derivation is precisely
		// the downgrade this exists to prevent.
		AddDebugLogLineN(logEC, "AEAD: daemon sent no public key, staying in clear");
		return;
	}

	const uint8_t cipher = (uint8_t)cipherTag->GetInt();
	if (!ECCrypt::IsCipherSupported(cipher)) {
		AddDebugLogLineN(logEC, "AEAD: daemon chose a cipher we cannot do");
		return;
	}

	// Validate the received lengths here, as the daemon does for the client's.
	// Downstream would reject them too, but checking locally keeps the "every field
	// the transcript concatenates is fixed-length" invariant obvious.
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

	// The channel key comes from here and from nothing else. Deriving it from the
	// password is what cost forward secrecy: the password outlives the session.
	std::vector<uint8_t> shared;
	const bool agreed = ECCrypt::X25519Agree(m_aeadEphPriv, serverPub, shared);
	// Wipe the private half as soon as it has done its one job, success or not.
	// Holding it longer is the only thing that could reopen a recorded session.
	ECCrypt::SecureWipe(m_aeadEphPriv);
	if (!agreed) {
		AddDebugLogLineN(logEC, "AEAD: bad daemon public key, staying in clear");
		return;
	}

	// Transcript: everything both sides exchanged, so an edited handshake byte yields
	// a different key on each end and the first sealed packet fails.
	const std::vector<uint8_t> transcript = ECCrypt::BuildTranscript(
		m_aeadOffered, cipher, m_aeadClientNonce, serverNonce, m_aeadEphPub, serverPub);

	if (!SetupAEAD(cipher, shared, serverNonce, m_aeadClientNonce, transcript, false)) {
		AddDebugLogLineN(logEC, "AEAD: key derivation failed, staying in clear");
		return;
	}

	// The password no longer keys the channel, so it can no longer stop a man in the
	// middle who runs an exchange with each of us and relays. These do instead: the
	// transcripts on the two legs differ, so the tags do too.
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
	// handshake included, so a fresh connection is never mistaken for a quiet one.
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
			// Established encrypted session: every response must arrive sealed. A
			// cleartext packet here is an injection attempt.
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

// Core serves requests FCFS and always replies, so we keep a place in the fifo
// even for replies we do not want.
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
	// The core never replies to requests that were on the air when the socket died,
	// so their handlers would linger and every reply on the reconnected session would
	// pop the wrong one. Rewind each orphaned handler so it re-requests.
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
					// Encryption is required unless the user opted out, and
					// m_canAEAD is that opt-out -- with it off we never offer,
					// so a clear session is only ever an explicit choice. The
					// core did not negotiate encryption, and an older aMule
					// cannot be told from an attacker who stripped the offer.
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
			// The daemon must prove it holds the credential over this exact handshake:
			// until this passes we know only that we are talking to something that
			// completed a key exchange, which a relay can also do, twice. Fatal rather
			// than a downgrade.
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
			// Mirror the server's negotiated capabilities into m_my_flags, or
			// `flags &= m_my_flags` in CECSocket::WritePacket drops them from outgoing
			// packets. Old daemons do not echo EC_TAG_CAN_LARGE_TAG_COUNT (#199) and the
			// sentinel wire format stays disabled both ways.
			if (reply->GetTagByName(EC_TAG_CAN_LARGE_TAG_COUNT)) {
				m_my_flags |= EC_FLAG_LARGE_TAG_COUNT;
			}
			// Server speaks the partial-update protocol: Get_EC_Response_GetUpdate may
			// omit unchanged files and emit explicit EC_TAG_FILE_REMOVED markers, and our
			// INC_UPDATE handler must skip the bulk "missing == deleted" loop. Old
			// daemons do not echo this and stay on the legacy alive-marker path (#713).
			if (reply->GetTagByName(EC_TAG_CAN_PARTIAL_UPDATE)) {
				m_serverPartialUpdate = true;
			}
			// Which daemon process we're talking to. Stays 0 against a daemon that does
			// not send it, which the reconnect path reads as "can't tell".
			if (const CECTag *sessionTag = reply->GetTagByName(EC_TAG_SESSION_ID)) {
				m_serverSessionId = sessionTag->GetInt();
			}
			// Server knows EC_OP_GET_CLIENT_HISTORY. No echo leaves the Known-clients tab
			// empty rather than sending it: a debug daemon asserts on the opcode.
			if (reply->GetTagByName(EC_TAG_CAN_CLIENT_HISTORY)) {
				m_serverClientHistory = true;
			}
			if (reply->GetTagByName(EC_TAG_CAN_PARTIAL_SEARCH)) {
				m_serverPartialSearch = true;
			}
			// Server confirmed multi-search: it allocates a distinct EC_TAG_SEARCH_ID per
			// search. Old daemons omit the echo and we stay on the sentinel path.
			if (reply->GetTagByName(EC_TAG_CAN_MULTI_SEARCH)) {
				m_serverMultiSearch = true;
			}
			// Server answers an id-less EC_OP_SEARCH_PROGRESS with every open search's
			// progress as children, so one request covers every tab. Old daemons omit the
			// echo and we keep polling one request per search id.
			if (reply->GetTagByName(EC_TAG_CAN_SEARCH_PROGRESS_UNION)) {
				m_serverSearchProgressUnion = true;
			}
			// Server buffers incoming peer messages for EC_OP_GET_CHAT_MESSAGES. Old
			// daemons omit the echo and the client never polls for chat.
			if (reply->GetTagByName(EC_TAG_CAN_CHAT)) {
				m_serverChat = true;
			}
			// Its own tag, because EC_TAG_CAN_CHAT above is echoed by daemons that predate
			// the session ops: gating on that one would send EC_OP_GET_CHAT_SESSIONS to a
			// core whose dispatcher asserts on it.
			if (reply->GetTagByName(EC_TAG_CAN_CHAT_SESSIONS)) {
				m_serverChatSessions = true;
			}
			// Server serves EC_OP_SEARCH_LIST. Old daemons omit the echo and the client
			// must not send the opcode: it lands in ProcessRequest2's unknown-opcode
			// branch, which logs and asserts.
			if (reply->GetTagByName(EC_TAG_CAN_SEARCH_LIST)) {
				m_serverSearchList = true;
			}
			// Server serves the shared-directory config ops. Without the echo the GUI keeps
			// the folders panel read-only rather than discarding edits silently.
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
