//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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

#include "ClientUDPSocket.h" // Interface declarations

#include <algorithm>
#include <array>

#include <protocol/Protocols.h>
#include <protocol/ed2k/Client2Client/TCP.h> // Sometimes we reply with TCP packets.
#include <protocol/ed2k/Client2Client/UDP.h>
#include <protocol/kad2/Client2Client/UDP.h>
#include <common/EventIDs.h>
#include <common/Format.h> // Needed for CFormat

#include "Preferences.h"          // Needed for CPreferences
#include "PartFile.h"             // Needed for CPartFile
#include "updownclient.h"         // Needed for CUpDownClient
#include "UploadQueue.h"          // Needed for CUploadQueue
#include "Packet.h"               // Needed for CPacket
#include "SharedFileList.h"       // Needed for CSharedFileList
#include "DownloadQueue.h"        // Needed for CDownloadQueue
#include "Statistics.h"           // Needed for theStats
#include "amule.h"                // Needed for theApp
#include "ClientList.h"           // Needed for clientlist (buddy support)
#include "KadServedBuddyPolicy.h" // Needed for MaxReaskRelaySize
#include "ClientTCPSocket.h"      // Needed for CClientTCPSocket
#include "MemFile.h"              // Needed for CMemFile
#include "Logger.h"
#include "kademlia/kademlia/Kademlia.h"
#include "kademlia/kademlia/Prefs.h"
#include "kademlia/kademlia/UDPFirewallTester.h"
#include "kademlia/utils/KadUDPKey.h"
#include <zlib.h>
#include "EncryptedDatagramSocket.h"
#ifdef AMULE_UTP_TRANSPORT
#include "UtpLibraryAdapter.h"
#include "UtpStreamAcceptor.h"
#endif
#include "NatRendezvousProtocol.h" // Needed for ParseRendezvousEnvelope, BuildForwardedRendezvous
#ifdef AMULE_QUIC_TRANSPORT
#include "QuicTls.h"
#include "QuicNgtcp2Adapter.h"
#include "QuicSocketTransport.h"
#include "QuicNattProtocol.h"
#include "NatRendezvousPolicy.h"
#include "QuicCapsRequestLedger.h"
#include "QuicCapabilityCache.h"
#endif
#if defined(AMULE_QUIC_TRANSPORT) || defined(ENABLE_NATT_SERVER_COORDINATION)
#include "IPFilter.h" // Needed for IsFiltered
#endif
#ifdef ENABLE_NATT_SERVER_COORDINATION
#include "LugdunumNatTraversal.h"   // Needed for IsServerWayDatagramFromOurServer, ParseForwardedCallback
#include "LugdunumNatCallbackLog.h" // Needed for LogForwardedCallback
#include "ServerConnect.h"          // Needed for CServerConnect
#include "Server.h"                 // Needed for CServer, and transitively OP_NAT_CALLBACKREQUESTED_UDP
#include "ServerList.h"             // Needed for theApp->serverlist->GetServerByAddress
#include "NatServerHolePunch.h"     // Needed for expected punch endpoints
#include "NetworkFunctions.h"       // Needed for IsGoodIP
#include "ClientRef.h"              // Needed for a safe temporary dial client
#include "ListenSocket.h"           // Needed for TooManySockets
#include "UdpObfuscationPolicy.h"   // Needed for ServerUdpReceiveKeys
#endif

//
// CClientUDPSocket -- Extended eMule UDP socket
//

namespace
{
#ifdef AMULE_UTP_TRANSPORT
constexpr bool kUtpBuilt = true;
#else
constexpr bool kUtpBuilt = false;
#endif
#ifdef AMULE_QUIC_TRANSPORT
constexpr bool kQuicBuilt = true;
#else
constexpr bool kQuicBuilt = false;
#endif
} // namespace

#if defined(AMULE_UTP_TRANSPORT) || defined(AMULE_QUIC_TRANSPORT)
bool CClientUDPSocket::NatTObfuscationAvailable(uint32_t ip)
{
	return thePrefs::IsClientCryptLayerSupported() && ClientKeyIP(ip) != 0;
}

bool CClientUDPSocket::SendNatKeyFrame(uint32_t ip, uint16_t port, const NatKeyFrame::Hash &key)
{
	CPacket *packet = new CPacket(
		OP_NATT_FRAME_KEY, static_cast<uint32_t>(NatKeyFrame::kHashBytes), OP_UDPRESERVEDPROT2);
	NatKeyFrame::Hash userHash{};
	std::copy_n(thePrefs::GetUserHash().GetHash(), userHash.size(), userHash.begin());
	const auto payload = NatKeyFrame::BuildPayload(userHash);
	packet->CopyToDataBuffer(0, payload.data(), static_cast<unsigned int>(payload.size()));
	return TrySendObfuscatedPacket(packet, ip, port, key.data());
}

namespace
{
//! Our login address, or 0 between server logins, where GetPublicIP() falls back to Kad's view
//! or the LAN address and would drop every key IP.
uint32_t LoginIP()
{
	return theApp->IsConnectedED2K() ? theApp->GetPublicIP(true) : 0;
}
} // namespace

void CClientUDPSocket::ExpectNatTPeer(
	uint32_t ip, uint16_t udpPort, const std::array<uint8_t, 16> *vouchedHash)
{
	std::optional<NatKeyFrame::Hash> hash;
	if (vouchedHash != nullptr) {
		hash = *vouchedHash;
	}
	const std::uint64_t now = ::GetTickCount64();
#ifdef AMULE_UTP_TRANSPORT
	m_utpExpectedPeers.Expect(ip, udpPort, now, hash);
#endif
#ifdef AMULE_QUIC_TRANSPORT
	m_keyFrames.ExpectPeer(ip, udpPort, now, hash);
#endif
	m_punchedPeers.Punched(ip, udpPort, hash, now);
	if (m_serverSeenUdpIP != 0) {
		std::lock_guard<std::mutex> lock(m_natKeyIPsMutex);
		m_natKeyIPs.Set(ip, m_serverSeenUdpIP, LoginIP(), NatSourceEcho::CKeyIps::From::Server, now);
	}
}

uint32_t CClientUDPSocket::NatKeyIPFor(uint32_t peerIp)
{
	std::lock_guard<std::mutex> lock(m_natKeyIPsMutex);
	return m_natKeyIPs.Lookup(peerIp, LoginIP(), ::GetTickCount64()).value_or(0);
}

uint32 CClientUDPSocket::StreamKeyIP(uint32 ip)
{
	std::lock_guard<std::mutex> lock(m_natKeyIPsMutex);
	return m_streamKeyIPs.Lookup(ip);
}

bool CClientUDPSocket::PinStreamKeyIP(uint32_t peerIp, uint32_t keyIP)
{
	std::lock_guard<std::mutex> lock(m_natKeyIPsMutex);
	return m_streamKeyIPs.Pin(peerIp, keyIP);
}

void CClientUDPSocket::UnpinStreamKeyIP(uint32_t peerIp)
{
	std::lock_guard<std::mutex> lock(m_natKeyIPsMutex);
	m_streamKeyIPs.Unpin(peerIp);
}

#ifdef AMULE_QUIC_TRANSPORT
bool CClientUDPSocket::HasQuicPeerKey(uint32_t ip, uint16_t port)
{
	return m_keyFrames.HasPeerKey(ip, port, ::GetTickCount64());
}
#endif

uint32_t CClientUDPSocket::IdentUdpIP() const
{
	// The server's view belongs to its login: between logins only Kad's is current.
	const uint32_t login = LoginIP();
	return NatSourceEcho::IdentUdpIP(login != 0 ? m_serverSeenUdpIP : 0,
		wxUINT32_SWAP_ALWAYS(Kademlia::CKademlia::GetIPAddress()),
		login);
}

#ifdef AMULE_UTP_TRANSPORT
namespace
{
uint16_t VerifiedKadUdpPort()
{
	if (!Kademlia::CKademlia::IsConnected() || !Kademlia::CKademlia::GetPrefs()->GetUseExternKadPort() ||
		!Kademlia::CUDPFirewallTester::IsVerified()) {
		return 0;
	}
	return Kademlia::CKademlia::GetPrefs()->GetExternalKadPort();
}

bool KadUdpPortsDisagree()
{
	return Kademlia::CKademlia::IsConnected() && Kademlia::CKademlia::GetPrefs()->GetUseExternKadPort() &&
	       Kademlia::CKademlia::GetPrefs()->ExternKadPortsDisagree();
}
} // namespace

void CClientUDPSocket::NoteServerSeenUdpPort(uint16_t port, uint64_t now)
{
	m_serverSeenUdpPort.Note(port, now);
	if (port == 0) {
		return;
	}
	const uint16_t kadPort = VerifiedKadUdpPort();
	const bool kadPortsDisagree = KadUdpPortsDisagree();
	switch (CompareUdpPortObservers(kadPort, port, kadPortsDisagree)) {
	case EUdpMappingEvidence::Unknown:
		AddDebugLogLineN(logServer,
			CFormat("Our UDP port: the server saw %u; Kad has no verified port") % port);
		break;
	case EUdpMappingEvidence::OnePort:
		AddDebugLogLineN(logServer, CFormat("Our UDP port: the server and Kad saw %u") % port);
		break;
	case EUdpMappingEvidence::PortPerDestination:
		if (kadPortsDisagree) {
			AddDebugLogLineN(logServer,
				CFormat("Our UDP port: the server saw %u, Kad's nodes saw different ports; "
					"our NAT maps a port per destination") %
					port);
		} else {
			AddDebugLogLineN(logServer,
				CFormat("Our UDP port: the server saw %u, Kad %u; "
					"our NAT maps a port per destination") %
					port % kadPort);
		}
		break;
	}
}

uint16_t CClientUDPSocket::ExternalUdpPortForPeers(uint64_t now) const
{
	const uint16_t kadPort = VerifiedKadUdpPort();
	const uint16_t serverSeenPort = m_serverSeenUdpPort.Current(now);
	return ::ExternalUdpPortForPeers(kadPort,
		serverSeenPort,
		CompareUdpPortObservers(kadPort, serverSeenPort, KadUdpPortsDisagree()));
}
#endif

void CClientUDPSocket::MaybeSendSourceEcho(
	uint32_t ip, uint16_t port, uint8_t firstByte, size_t length, bool decrypted)
{
	if (!NatTObfuscationAvailable(ip)) {
		return;
	}
	// Sent obfuscated, so it has no fixed plain signature. The split side can decode it: only its
	// own key is wrong.
	const std::uint64_t now = ::GetTickCount64();
	const std::optional<NatKeyFrame::Hash> hash =
		m_punchedPeers.TakeEcho(ip, port, firstByte, length, decrypted, now);
	if (!hash) {
		return;
	}
	const auto payload = NatSourceEcho::BuildPayload({ ip, port });
	CPacket *packet = new CPacket(OP_NATT_SOURCE_ECHO, static_cast<uint32>(payload.size()), OP_EMULEPROT);
	packet->CopyToDataBuffer(0, payload.data(), static_cast<unsigned int>(payload.size()));
	const uint32 packetSize = packet->GetPacketSize();
	if (TrySendObfuscatedPacket(packet, ip, port, hash->data())) {
		theStats::AddUpOverheadOther(packetSize);
		AddDebugLogLineN(logClientUDP,
			CFormat("NAT-T source echo to %s: its datagram failed every key") %
				Uint32_16toStringIP_Port(ip, port));
	} else {
		m_punchedPeers.EchoNotSent(ip, port, now);
	}
}

void CClientUDPSocket::HandleSourceEcho(
	const uint8_t *payload, size_t length, uint32_t ip, uint16_t port, bool obfuscated)
{
	const std::uint64_t now = ::GetTickCount64();
	const std::optional<NatSourceEcho::Source> seen = NatSourceEcho::ParsePayload(payload, length);
	if (!seen || !NatSourceEcho::AcceptsEcho(
			     m_punchedPeers, ip, port, IsGoodIP(seen->ip, true), obfuscated, now)) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(m_natKeyIPsMutex);
		m_natKeyIPs.Set(ip, seen->ip, LoginIP(), NatSourceEcho::CKeyIps::From::Peer, now);
	}
	AddDebugLogLineN(logClientUDP,
		CFormat("NAT-T source echo from %s: it sees our UDP at %s") %
			Uint32_16toStringIP_Port(ip, port) % Uint32_16toStringIP_Port(seen->ip, seen->port));
}
#endif

#ifdef AMULE_QUIC_TRANSPORT
namespace
{
std::array<uint8_t, 16> QuicLocalIdentityFromUserHash();

bool QuicNatTEnabled()
{
	// QUIC NAT-T has its own amule.conf opt-in and must not inherit the legacy
	// server-coordinated NAT settings. A QUIC-only build only accepts: every QUIC
	// dial needs ENABLE_NATT_SERVER_COORDINATION and ENABLE_UTP.
	return thePrefs::IsQuicNatTEnabled();
}

uint8_t QuicLocalCapabilityOptions()
{
	return static_cast<uint8_t>(QuicNatt::EAQC_CAP_QUIC |
				    ((Kademlia::CPrefs::GetMyConnectOptions(false, true) &
					     QuicNatt::EAQC_CAP_DIRECT_CALLBACK) != 0
						    ? QuicNatt::EAQC_CAP_DIRECT_CALLBACK
						    : 0) |
#ifdef AMULE_UTP_TRANSPORT
				    QuicNatt::EAQC_CAP_UTP
#else
				    0
#endif
	);
}
} // namespace

bool CClientUDPSocket::SendQuicCapsRequest(const std::array<uint8_t, 16> &peer,
	uint32_t ipv4,
	uint16_t udpPort,
	const std::array<uint8_t, 16> &fileHash)
{
	const auto zero = std::array<uint8_t, 16>{};
	if (!QuicNatTEnabled() || peer == zero || ipv4 == 0 || udpPort == 0 || !Ok())
		return false;
	const auto frame = QuicNatt::EncodeEaqcFrame(
		QuicLocalCapabilityOptions(), QuicLocalIdentityFromUserHash(), peer, fileHash);
	auto packet = std::make_unique<CPacket>(
		OP_NATT_FRAME_CAPS, static_cast<uint32>(frame.size()), OP_UDPRESERVEDPROT2);
	packet->CopyToDataBuffer(0, frame.data(), static_cast<unsigned int>(frame.size()));
	const QuicCapsSendPlan sendPlan = MakeQuicCapsSendPlan(
		thePrefs::IsClientCryptLayerSupported(), theApp->GetPublicIP(true), peer);
	SendPacket(packet.release(),
		ipv4,
		udpPort,
		sendPlan.obfuscate,
		sendPlan.obfuscate ? sendPlan.targetHash.data() : nullptr,
		false,
		0);
	m_capsRequestLedger.Add(peer, ipv4, udpPort, fileHash, ::GetTickCount64());
	return true;
}

bool CClientUDPSocket::SendQuicFrame(const uint8_t *payload, size_t length, uint32_t ip, uint16_t port)
{
	return m_keyFrames.SendDatagram(payload, length, ip, port, ::GetTickCount64());
}

NatKeyFrame::Callbacks CClientUDPSocket::KeyFrameCallbacks()
{
	NatKeyFrame::Callbacks callbacks;
	callbacks.enabled = [] { return QuicNatTEnabled(); };
	callbacks.obfuscationOn = [this](uint32_t ip) { return NatTObfuscationAvailable(ip); };
	callbacks.sendKeyFrame = [this](uint32_t ip, uint16_t port, const NatKeyFrame::Hash &key) -> bool {
		return SendNatKeyFrame(ip, port, key);
	};
	callbacks.sendDatagram = [this](const uint8_t *payload,
					 size_t length,
					 uint32_t ip,
					 uint16_t port,
					 const NatKeyFrame::Hash *key) -> bool {
		CPacket *packet =
			new CPacket(OP_NATT_FRAME_QUIC, static_cast<uint32_t>(length), OP_UDPRESERVEDPROT2);
		if (length != 0) {
			packet->CopyToDataBuffer(0, payload, static_cast<unsigned int>(length));
		}
		return TrySendTransportPacket(
			packet, ip, port, key != nullptr, key != nullptr ? key->data() : nullptr);
	};
	return callbacks;
}

void CClientUDPSocket::HandleNatKeyFrame(const uint8_t *payload, size_t length, uint32_t ip, uint16_t port)
{
	switch (m_keyFrames.OnKeyFrame(payload, length, ip, port, ::GetTickCount64())) {
	case NatKeyFrame::KeyFrameResult::Learned:
		AddDebugLogLineN(logClientUDP,
			CFormat("NAT-T key frame from %s:%u: obfuscating replies with its hash") %
				Uint32toStringIP(ip) % port);
		break;
	case NatKeyFrame::KeyFrameResult::Malformed:
		if (m_truncatedFrameLog.ShouldLog(::GetTickCount64())) {
			AddDebugLogLineN(logClientUDP,
				CFormat("Dropping malformed NAT-T key frame from %s:%u (%u further "
					"occurrences suppressed)") %
					Uint32toStringIP(ip) % port %
					m_truncatedFrameLog.TakeSuppressedCount());
		}
		break;
	case NatKeyFrame::KeyFrameResult::Disabled:
	case NatKeyFrame::KeyFrameResult::Unexpected:
	case NatKeyFrame::KeyFrameResult::Held:
		break;
	}
}

bool CClientUDPSocket::TryQuicDial(const std::array<uint8_t, 16> &peer,
	uint32_t ipv4,
	uint16_t udpPort,
	const std::array<uint8_t, 16> &fileHash,
	CUpDownClient *owner)
{
	if (!QuicNatTEnabled())
		return false;
	const uint64_t now = ::GetTickCount64();
	const auto result = m_capabilityCache.Lookup(peer, now);
	if (result.state == QuicCapabilityState::Unknown) {
		SendQuicCapsRequest(peer, ipv4, udpPort, fileHash);
#if defined(ENABLE_NATT_SERVER_COORDINATION) && defined(AMULE_UTP_TRANSPORT)
		// Only server-vouched LowID dials pass an owner, and only they have a uTP fallback to hold.
		if (owner != nullptr) {
			m_quicCapsHoldoffs.Add({ peer, ipv4, udpPort, fileHash },
				now,
				CCLIENTREF(owner, "CClientUDPSocket::quicCapsHoldoff"));
			return true;
		}
#endif
		return false;
	}
	if (result.state != QuicCapabilityState::Compatible || ipv4 == 0 || udpPort == 0 || !Ok())
		return false;
	m_keyFrames.NoteDialled(ipv4, udpPort, peer, now);
	if (m_quic.Dial(CNetworkAddress::FromIPv4NetworkOrder(ipv4), udpPort, peer, now, owner))
		return true;
	m_capabilityCache.RememberFailure(peer, now, 30000);
	return false;
}

#if defined(ENABLE_NATT_SERVER_COORDINATION) && defined(AMULE_UTP_TRANSPORT)
CQuicCapsHoldoffs<CClientRef>::SCallbacks CClientUDPSocket::QuicCapsHoldoffCallbacks()
{
	using Target = CQuicCapsHoldoffs<CClientRef>::STarget;
	return { [](CClientRef &owner) { return owner.GetClientChecked() != nullptr; },
		[this](const std::array<uint8_t, 16> &peer, uint64_t nowMs) {
			return m_capabilityCache.Lookup(peer, nowMs).state;
		},
		[this](const Target &target) {
			SendQuicCapsRequest(target.peer, target.ipv4, target.udpPort, target.fileHash);
		},
		[this](const Target &target, CClientRef &owner, uint64_t nowMs) {
			return target.ipv4 != 0 && target.udpPort != 0 && Ok() &&
			       m_quic.Dial(CNetworkAddress::FromIPv4NetworkOrder(target.ipv4),
				       target.udpPort,
				       target.peer,
				       nowMs,
				       owner.GetClientChecked());
		},
		[this](const Target &target, CClientRef &owner, uint64_t nowMs) {
			m_capabilityCache.RememberFailure(target.peer, nowMs, 30000);
			owner.GetClientChecked()->QuicDialFailed();
		},
		[](CClientRef &owner) { owner.GetClientChecked()->QuicDialFailed(); } };
}
#endif

namespace
{
//! What CQuicNgtcp2Factory asserts as "who we are" in the EAQN1 proof exchange
//! (QuicNattProtocol.h) every inbound QUIC stream goes through before being handed off.
std::array<uint8_t, 16> QuicLocalIdentityFromUserHash()
{
	std::array<uint8_t, 16> identity{};
	std::copy_n(thePrefs::GetUserHash().GetHash(), identity.size(), identity.begin());
	return identity;
}

//! Hands a QUIC datagram to CClientUDPSocket::SendQuicFrame(), which wraps it as an
//! OP_NATT_FRAME_QUIC reserved-protocol frame. The datagram's TLS 1.3 protection does not depend on
//! the ed2k UDP obfuscation layer; the layer only hides the Initial and its ALPN from the path.
class CQuicUdpSink final : public IQuicDatagramSink
{
public:
	explicit CQuicUdpSink(CClientUDPSocket &socket)
	: m_socket(socket)
	{
	}

	bool SendDatagram(
		const uint8_t *payload, size_t length, const CNetworkAddress &address, uint16_t port) override
	{
		uint32_t ip = 0;
		// Maximum IPv4 UDP payload, less the aMule envelope -- the same bound
		// QueueUtpDatagram() enforces for the same reason.
		if (!address.ToIPv4NetworkOrder(ip) || length > 65507 - 2 ||
			(length != 0 && payload == nullptr)) {
			return false;
		}
		return m_socket.SendQuicFrame(payload, length, ip, port);
	}

private:
	CClientUDPSocket &m_socket;
};
} // namespace
#endif

CClientUDPSocket::CClientUDPSocket(const amuleIPV4Address &address, const CProxyData *ProxyData)
: CMuleUDPSocket("Client UDP-Socket", ID_CLIENTUDPSOCKET_EVENT, address, ProxyData)
#ifdef AMULE_UTP_TRANSPORT
, m_utp(CreateUtpLibrary(), *this)
#endif
#ifdef AMULE_QUIC_TRANSPORT
, m_quicCredentials(CreateProductionQuicCredentials())
, m_quicSink(std::make_shared<CQuicUdpSink>(*this))
, m_quicEngine(CreateProductionQuicNgtcp2Engine())
, m_quicFactory(std::make_unique<CQuicNgtcp2Factory>(
	  CQuicTlsPolicy{ m_quicCredentials.get() },
	  m_quicSink,
	  m_quicEngine,
	  QuicLocalIdentityFromUserHash(),
	  nullptr,
	  [](CUpDownClient *owner, std::unique_ptr<IStreamTransport> transport) -> CQuicSocketTransport * {
		  if (owner == nullptr)
			  return nullptr;
		  auto *raw = dynamic_cast<CQuicSocketTransport *>(transport.get());
		  if (raw == nullptr)
			  return nullptr;
		  // The QUIC handoff must not let the socket constructor silently orphan a
		  // disconnected TCP/uTP socket. Remove that owner link first; the constructor
		  // then installs the new socket through the normal SetClient path.
		  if (!owner->ReplaceSocket(nullptr))
			  return nullptr;
		  auto *socket = new CClientTCPSocket(owner, thePrefs::GetProxyData());
		  socket->AttachTransport(std::move(transport));
		  return raw;
	  },
	  [this](CUpDownClient *owner, const std::array<uint8_t, 16> &peer) {
		  m_capabilityCache.RememberFailure(peer, ::GetTickCount64(), 30000);
		  if (owner != nullptr)
			  owner->QuicDialFailed();
	  }))
, m_quic(m_quicFactory.get())
#if defined(ENABLE_NATT_SERVER_COORDINATION) && defined(AMULE_UTP_TRANSPORT)
, m_quicCapsHoldoffs(QuicCapsHoldoffCallbacks())
#endif
, m_quicFineTick(CORE_TIMER_PERIOD)
, m_quicFineTimer(theApp, ID_QUIC_TIMER_EVENT)
#endif
{
	if (!thePrefs::IsUDPDisabled()) {
		Open();
	}
#ifdef AMULE_UTP_TRANSPORT
	// Arm the service before the first outbound dial, not only after ingress.
	m_utp.Configure();
	m_utp.SetAcceptor(&m_utpAcceptor);
#endif
#ifdef AMULE_QUIC_TRANSPORT
	m_quicFactory->SetAcceptor(&m_quicAcceptor);
#endif
}

bool CClientUDPSocket::Rebind(const amuleIPV4Address &address)
{
	// UDP can be disabled in preferences while the client socket object remains
	// alive. Keep its future bind address up to date without opening a socket.
	if (thePrefs::IsUDPDisabled()) {
		Close();
		return SetBindAddressIfClosed(address);
	}
	return CMuleUDPSocket::Rebind(address);
}

#ifdef AMULE_UTP_TRANSPORT
void CClientUDPSocket::Close()
{
	wxASSERT(wxIsMainThread());
	// The uTP context is not destroyed here. Close() and Open() are how a lost
	// Kad connection is recovered, and libutp does not own this socket: it only
	// asks us to send. Destroying the context would end every live uTP stream
	// for a reconnect that leaves TCP peers untouched.
	CMuleUDPSocket::Close();
}

CClientUDPSocket::~CClientUDPSocket()
{
	m_utp.Destroy();
}

void CClientUDPSocket::TickUtp()
{
	wxASSERT(wxIsMainThread());
#ifdef ENABLE_NATT_SERVER_COORDINATION
	TickLugdunumCallbackRendezvous();
	TickKadRendezvous(::GetTickCount64());
#endif
	m_utp.Tick();
}

void CClientUDPSocket::SendUtpDatagram(const uint8_t *payload,
	size_t length,
	uint32_t ip,
	uint16_t port,
	bool encrypt,
	const uint8_t *userHash)
{
	wxASSERT(wxIsMainThread());
	// The crypt parameters arrive with the datagram, carried down from the
	// socket that produced it.
	//
	// They are not derived here, and deriving them here is what the removed
	// FindClientByIP(ip, port) did wrong: it matched GetUserPort(), the ed2k
	// TCP port, against the peer's UDP port, so with the aMule defaults of 4662
	// and 4672 it never matched and the answer was "no encryption" by accident.
	// Worse, it could match a different peer at the same address that happened
	// to listen on a TCP port equal to the UDP port being dialled, keying the
	// datagram on that client's hash so the real recipient could not decrypt.
	//
	// Not counted as overhead: the client socket counts the stream bytes, as for TCP.
	QueueUtpDatagram<CPacket>(*this, payload, length, ip, port, encrypt, userHash);
}

void CClientUDPSocket::SendUtpKeyFrame(uint32_t ip, uint16_t port, const uint8_t *peerHash)
{
	wxASSERT(wxIsMainThread());
	const std::uint64_t now = ::GetTickCount64();
	if (!NatTObfuscationAvailable(ip) || !m_utpKeyFrameGate.TryAcquire(ip, port, now)) {
		return;
	}
	NatKeyFrame::Hash key{};
	std::copy_n(peerHash, key.size(), key.begin());
	if (!SendNatKeyFrame(ip, port, key)) {
		m_utpKeyFrameGate.Forget(ip, port);
	}
}

bool CClientUDPSocket::IsExpectedUtpPeer(uint32_t ip, uint16_t port) const
{
	return m_utpExpectedPeers.IsExpectedAllowingMovedPort(ip, port, ::GetTickCount64());
}

bool CClientUDPSocket::IsLugdunumRendezvousPeer([[maybe_unused]] uint32_t ip) const
{
#ifdef ENABLE_NATT_SERVER_COORDINATION
	return m_rendezvousOutcomes.LatestPath(ip, ::GetTickCount64()) ==
	       NatServerHolePunch::RendezvousPath::LugdunumCallback;
#else
	return false;
#endif
}

void CClientUDPSocket::OnUtpStreamMoved()
{
	theStats::AddUtpStreamMove();
}

void CClientUDPSocket::HandleUtpKeyFrame(const uint8_t *payload, size_t length, uint32_t ip, uint16_t port)
{
	const std::optional<NatKeyFrame::Hash> announced = NatKeyFrame::ParsePayload(payload, length);
	if (!announced) {
		return;
	}
	const std::optional<NatKeyFrame::Hash> key = NatKeyFrame::AcceptUtpKeyFrame(
		m_utpExpectedPeers, m_utpHeldKeys, ip, port, *announced, ::GetTickCount64());
	if (key && NatTObfuscationAvailable(ip) && m_utp.ApplyAnnouncedKey(ip, port, key->data())) {
		AddDebugLogLineN(logClientUDP,
			CFormat("NAT-T key frame from %s:%u: obfuscating our uTP dial with its hash") %
				Uint32toStringIP(ip) % port);
	}
}

NatKeyFrame::UtpSynPlan CClientUDPSocket::PrepareUtpSynReply(uint32_t ip, uint16_t port, bool synObfuscated)
{
	const std::uint64_t now = ::GetTickCount64();
	NatKeyFrame::UtpSynPlan plan = NatKeyFrame::PlanUtpSyn(m_utpExpectedPeers,
		m_utpHeldKeys,
		m_utpKeyFrameGate,
		ip,
		port,
		synObfuscated,
		NatTObfuscationAvailable(ip),
		now);
	if (plan.sendKeyFrame && !SendNatKeyFrame(ip, port, *plan.key)) {
		m_utpKeyFrameGate.Forget(ip, port);
	}
	return plan;
}
#endif

#ifdef AMULE_QUIC_TRANSPORT
void CClientUDPSocket::TickQuic()
{
	wxASSERT(wxIsMainThread());
	const uint64_t nowMs = ::GetTickCount64();
	m_quic.Tick(nowMs);
#if defined(ENABLE_NATT_SERVER_COORDINATION) && defined(AMULE_UTP_TRANSPORT)
	m_quicCapsHoldoffs.Service(nowMs);
#endif
	ScheduleQuicFineTick(nowMs);
}

void CClientUDPSocket::TickQuicFine()
{
	wxASSERT(wxIsMainThread());
	const uint64_t nowMs = ::GetTickCount64();
	m_quic.Tick(nowMs);
	ScheduleQuicFineTick(nowMs);
}

void CClientUDPSocket::ScheduleQuicFineTick(uint64_t nowMs)
{
	switch (m_quicFineTick.Update(m_quic.NextExpiryMs(), nowMs)) {
	case CQuicFineTick::EAction::Start:
		if (!m_quicFineTimer.Start(static_cast<int>(CQuicFineTick::kPeriodMs))) {
			m_quicFineTick.StartFailed(nowMs);
		}
		break;
	case CQuicFineTick::EAction::Stop:
		m_quicFineTimer.Stop();
		break;
	case CQuicFineTick::EAction::None:
		break;
	}
}
#endif

void CClientUDPSocket::OnReceive(int errorCode)
{
	CMuleUDPSocket::OnReceive(errorCode);
#ifdef AMULE_UTP_TRANSPORT
	if (m_utpAckBatch.AfterDatagram(HasQueuedDatagram())) {
		m_utp.IssueDeferredAcks();
	}
#endif

	// TODO: A better solution is needed.
	if (thePrefs::IsUDPDisabled()) {
		Close();
	}
}

void CClientUDPSocket::OnPacketReceived(
	const CNetworkAddress &address, uint16 port, uint8_t *buffer, size_t length)
{
	wxCHECK_RET(length >= 2, "Invalid packet.");

	// Legacy encrypted/Kad packet handlers remain IPv4-only; keep the family boundary explicit.
	uint32 ip;
	if (!GetIPv4PacketAddress(address, ip, logClientUDP, "client")) {
		return;
	}

#ifdef ENABLE_NATT_SERVER_COORDINATION
	if (TryProcessServerWayNatCallback(ip, buffer, length)) {
		return;
	}
#ifdef AMULE_UTP_TRANSPORT
	if (TryProcessForeignNatCallback(ip, buffer, length)) {
		return;
	}
#endif
#endif

	uint8_t *decryptedBuffer;
	uint32_t receiverVerifyKey;
	uint32_t senderVerifyKey;
	int packetLen = CEncryptedDatagramSocket::DecryptReceivedClient(
		buffer, length, &decryptedBuffer, ip, &receiverVerifyKey, &senderVerifyKey);

	uint8_t protocol = decryptedBuffer[0];
	uint8_t opcode = decryptedBuffer[1];

	if (packetLen >= 1) {
		try {
			switch (protocol) {
			case OP_EMULEPROT:
				// Needs protocol + opcode before any payload; a 1-byte packet would
				// make packetLen - 2 underflow to a huge length.
				if (packetLen >= 2) {
					ProcessPacket(decryptedBuffer + 2,
						packetLen - 2,
						opcode,
						ip,
						port,
						decryptedBuffer != buffer);
				} else {
					throw wxString("eMule UDP packet too short");
				}
				break;

			case OP_KADEMLIAHEADER:
				theStats::AddDownOverheadKad(length);
				if (packetLen >= 2) {
					Kademlia::CKademlia::ProcessPacket(decryptedBuffer,
						packetLen,
						wxUINT32_SWAP_ALWAYS(ip),
						port,
						(Kademlia::CPrefs::GetUDPVerifyKey(ip) == receiverVerifyKey),
						Kademlia::CKadUDPKey(
							senderVerifyKey, theApp->GetPublicIP(false)));
				} else {
					throw wxString("Kad packet too short");
				}
				break;

			case OP_KADEMLIAPACKEDPROT:
				theStats::AddDownOverheadKad(length);
				if (packetLen >= 2) {
					uint32_t newSize = packetLen * 10 + 300; // Should be enough...
					std::vector<uint8_t> unpack(newSize);
					uLongf unpackedsize = newSize - 2;
					uint16_t result = uncompress(&(unpack[2]),
						&unpackedsize,
						decryptedBuffer + 2,
						packetLen - 2);
					if (result == Z_OK) {
						AddDebugLogLineN(logClientKadUDP,
							"Correctly uncompressed Kademlia packet");
						unpack[0] = OP_KADEMLIAHEADER;
						unpack[1] = opcode;
						Kademlia::CKademlia::ProcessPacket(&(unpack[0]),
							unpackedsize + 2,
							wxUINT32_SWAP_ALWAYS(ip),
							port,
							(Kademlia::CPrefs::GetUDPVerifyKey(ip) ==
								receiverVerifyKey),
							Kademlia::CKadUDPKey(
								senderVerifyKey, theApp->GetPublicIP(false)));
					} else {
						AddDebugLogLineN(logClientKadUDP,
							"Failed to uncompress Kademlia packet");
					}
				} else {
					throw wxString("Kad packet (compressed) too short");
				}
				break;

			case OP_UDPRESERVEDPROT2:
				// eMuleAI NAT traversal. Not an eD2k opcode: the byte after the
				// protocol byte is a frame type, so it is dispatched here rather
				// than through ProcessPacket(), whose second argument is an opcode.
				//
				// Counted as overhead like every sibling branch: these bytes cross
				// the wire whether or not we can serve the frame, and leaving them
				// out makes aMule's own figures disagree with what the link shows.
				// The exception is a frame of a transport this build runs: the
				// client socket counts its stream bytes, as for TCP.
				//
				// This branch still reaches no *packet* accounting, which is what
				// keeps a dropped frame from feeding a ban: CPacketTracking is only
				// entered from the Kad listener, and an eMuleAI peer's NAT-T
				// traffic would otherwise read as malformed. Statistics and bans are
				// separate subsystems; only the second one must stay out of reach.
				if (!IsBuiltTransportFrame(
					    ClassifyReservedProt2Frame(decryptedBuffer + 1, packetLen - 1),
					    kUtpBuilt,
					    kQuicBuilt)) {
					theStats::AddDownOverheadOther(length);
				}
				ProcessReservedProt2Frame(decryptedBuffer + 1,
					packetLen - 1,
					ip,
					port,
					decryptedBuffer != buffer);
				break;

			default:
#if defined(AMULE_UTP_TRANSPORT) || defined(AMULE_QUIC_TRANSPORT)
				MaybeSendSourceEcho(ip, port, protocol, length, decryptedBuffer != buffer);
#endif
				AddDebugLogLineN(logClientUDP,
					CFormat("Unknown opcode on received packet: 0x%x") % protocol);
			}
		} catch (const wxString &e) {
			AddDebugLogLineN(logClientUDP, "Error while parsing UDP packet: " + e);
		} catch (const CInvalidPacket &e) {
			AddDebugLogLineN(logClientUDP, "Invalid UDP packet encountered: " + e.what());
		} catch (const CEOFException &e) {
			AddDebugLogLineN(logClientUDP,
				"Malformed packet encountered while parsing UDP packet: " + e.what());
		}
	}
}

#ifdef ENABLE_NATT_SERVER_COORDINATION
bool CClientUDPSocket::TryProcessServerWayNatCallback(uint32 ip, uint8_t *buffer, size_t length)
{
	if (theApp->serverconnect == nullptr) {
		return false;
	}
	CServer *server = theApp->serverconnect->GetCurrentServer();
	if (server == nullptr) {
		return false;
	}
	// Before the server-list lookup, which scans the list: this runs for every client datagram.
	const LugdunumNatTraversal::ServerWayDatagramFacts facts{
		ip, server->GetIP(), server->IsLugdunumNatCallbackActive()
	};
	if (!LugdunumNatTraversal::IsServerWayDatagramFromOurServer(facts)) {
		return false;
	}
	// The server UDP fields are updated on the server-list entry, not on cur_server's snapshot.
	CServer *serverListEntry =
		theApp->serverlist->GetServerByAddress(server->GetAddress(), server->GetPort());
	if (serverListEntry == nullptr) {
		return false;
	}

	const ServerUdpKeyCandidates keys = ServerUdpReceiveKeys(serverListEntry->GetCryptPingReplyPending(),
		serverListEntry->GetChallenge(),
		serverListEntry->GetServerKeyUDP());
	uint8_t *decrypted = buffer;
	const int decryptedLen =
		DecryptWithServerUdpKeys(keys, static_cast<int>(length), [&](std::uint32_t key) {
			return CEncryptedDatagramSocket::DecryptReceivedServer(
				buffer, static_cast<int>(length), &decrypted, key, ip);
		});
	// Every non-decrypting path returns the input length unchanged; a decrypt strips the header.
	if (decryptedLen >= static_cast<int>(length) || decrypted == nullptr || decryptedLen < 2) {
		return false;
	}
	// Decrypted in place and authenticated as our server's: never hand it to the client path.
	if (decrypted[0] != OP_EDONKEYPROT || decrypted[1] != OP_NAT_CALLBACKREQUESTED_UDP) {
		AddDebugLogLineN(logClientUDP,
			CFormat("Unexpected server UDP packet 0x%02x/0x%02x on the client socket") %
				decrypted[0] % decrypted[1]);
		return true;
	}

	const LugdunumNatTraversal::ForwardedCallback callback =
		LugdunumNatTraversal::ParseForwardedCallback(decrypted + 2, decryptedLen - 2);
	LugdunumNatTraversal::DispatchForwardedCallback(
		callback, [this](const LugdunumNatTraversal::ForwardedCallback &forwarded) {
			LugdunumNatTraversal::LogForwardedCallback(
				logClientUDP, "OP_NAT_CALLBACKREQUESTED_UDP", forwarded);
#ifdef AMULE_UTP_TRANSPORT
			// Requester ID 0 means a cross-server wake on a wake-keepalive server, sent by UDP
			// when we asked for a callback ourselves just before; anywhere else it is a reverse
			// callback. Every other 0xA7 answers our own 0xA6 and is dialled at once.
			if (forwarded.requesterId == 0 && theApp->serverconnect != nullptr &&
				theApp->serverconnect->IsWakeKeepaliveActive()) {
				AnswerWakeWithKeepalive(forwarded);
			} else {
				DialLugdunumNatCallbackPeer(forwarded);
			}
#endif
		});
	return true;
}
#endif

void CClientUDPSocket::ProcessReservedProt2Frame(
	const uint8_t *frame, size_t frameLength, uint32 ip, uint16 port, [[maybe_unused]] bool obfuscated)
{
	const SReservedProt2Frame classified = ClassifyReservedProt2Frame(frame, frameLength);

	switch (classified.disposition) {
	case RP2_TRUNCATED:
		// Nothing but the protocol byte arrived, so there is no type byte to read. Dropped
		// without reading the window -- the guard is the point, this being the shortest
		// datagram that can reach here.
		if (m_truncatedFrameLog.ShouldLog(::GetTickCount64())) {
			AddDebugLogLineN(logClientUDP,
				CFormat("Dropping truncated NAT-T datagram from %s:%u (%u further "
					"occurrences suppressed)") %
					Uint32toStringIP(ip) % port %
					m_truncatedFrameLog.TakeSuppressedCount());
		}
		return;

	case RP2_UNKNOWN_TYPE:
		// A frame type this protocol does not define. Dropped, and deliberately not counted
		// anywhere: see the OP_UDPRESERVEDPROT2 comment in OnPacketReceived().
		if (m_unknownFrameLog.ShouldLog(::GetTickCount64())) {
			AddDebugLogLineN(logClientUDP,
				CFormat("Dropping NAT-T frame of unknown type 0x%02X from %s:%u (%u further "
					"occurrences suppressed)") %
					classified.type % Uint32toStringIP(ip) % port %
					m_unknownFrameLog.TakeSuppressedCount());
		}
		return;

	case RP2_KNOWN_TYPE:
		break;
	}

	// Keep each registered type's handling in its own case: transports and capability
	// negotiation are independently compile-gated, and unsupported types remain inert.
	switch (classified.type) {
	case OP_NATT_FRAME_UTP: {
#ifdef AMULE_UTP_TRANSPORT
		wxASSERT(wxIsMainThread());
		// Classified before libutp sees it, because libutp answers a non-SYN frame
		// that matches no connection with an unsolicited RST to whatever source
		// address the datagram claimed (utp_internal.cpp, the flags != ST_SYN
		// branch). That makes this host a reflector for a forged source, and tells
		// a stranger who never connected that a uTP peer lives here. A SYN is the
		// one kind that may arrive from someone we do not know; everything else has
		// to come from an endpoint that already holds a socket.
		const EUtpFrameKind kind = ClassifyUtpFrame(classified.payload, classified.payloadLength);
		const std::uint64_t nowMs = ::GetTickCount64();
		const bool fromKnownPeer = m_utp.HasRegisteredPeer(ip, port);
		const bool fromExpectedHandshakePeer =
			kind == EUtpFrameKind::Existing && m_utp.HasExpectedPortRemapPeer(ip, port, nowMs);
		// A connected stream whose NAT moved the source port keeps talking from a port nothing is
		// registered at yet, so the adapter may still own the frame: it is admitted only when exactly
		// one connected stream on that address is within the remap window. Non-SYN only, because a
		// SYN asks for a new connection and must never be handed a live stream's tuple. The answer is
		// a predicate, so it grants no attempt and spends none of the adapter's move budget.
		const bool fromRepointedPeer = kind == EUtpFrameKind::Existing && !fromKnownPeer &&
					       m_utp.HasRepointCandidate(ip, port);
		if (kind != EUtpFrameKind::Malformed &&
			(kind == EUtpFrameKind::Syn || fromKnownPeer || fromExpectedHandshakePeer ||
				fromRepointedPeer)) {
			NatKeyFrame::UtpSynPlan synReply;
			if (kind == EUtpFrameKind::Syn) {
				synReply = PrepareUtpSynReply(ip, port, obfuscated);
			}
			if (ProcessUtpFrame(m_utp,
				    classified,
				    ip,
				    port,
				    nowMs,
				    obfuscated,
				    synReply.key ? synReply.key->data() : nullptr)) {
				return;
			}
		} else if (kind == EUtpFrameKind::Existing) {
			// The genuinely unmatched case, which used to leave as an RST with no
			// line anywhere because libutp had already reported it handled.
			if (m_utpUnmatchedFrameLog.ShouldLog(::GetTickCount64())) {
				AddDebugLogLineN(logClientUDP,
					CFormat("Dropping uTP frame from %s:%u: no uTP socket for that "
						"peer (%u further occurrences suppressed)") %
						Uint32toStringIP(ip) % port %
						m_utpUnmatchedFrameLog.TakeSuppressedCount());
			}
			break;
		}
		// Reached only for a frame libutp would not even look at: shorter than a uTP
		// header, or a version it does not implement.
		if (m_utpMalformedFrameLog.ShouldLog(::GetTickCount64())) {
			AddDebugLogLineN(logClientUDP,
				CFormat("Dropping malformed uTP frame from %s:%u: too short or an "
					"unsupported version (%u further occurrences suppressed)") %
					Uint32toStringIP(ip) % port %
					m_utpMalformedFrameLog.TakeSuppressedCount());
		}
#else
		if (m_unservedFrameLog.ShouldLog(::GetTickCount64())) {
			AddDebugLogLineN(logClientUDP,
				CFormat("Ignoring uTP NAT-T frame from %s:%u: no uTP transport in this "
					"build (%u further occurrences suppressed)") %
					Uint32toStringIP(ip) % port %
					m_unservedFrameLog.TakeSuppressedCount());
		}
#endif
		break;
	}

	case OP_NATT_FRAME_QUIC:
#ifdef AMULE_QUIC_TRANSPORT
		if (!QuicNatTEnabled())
			break;
		// Here, not at the stream: a filtered host must not get a TLS handshake first.
		if (theApp->ipfilter->IsFiltered(ip))
			break;
		if (m_quic.ProcessDatagram(classified.payload,
			    classified.payloadLength,
			    CNetworkAddress::FromIPv4NetworkOrderOrAbsent(ip),
			    port,
			    ::GetTickCount64())) {
			// While it runs, the fine tick checks every deadline itself.
			if (!m_quicFineTick.IsRunning()) {
				ScheduleQuicFineTick(::GetTickCount64());
			}
			return;
		}
		if (m_unservedFrameLog.ShouldLog(::GetTickCount64())) {
			AddDebugLogLineN(logClientUDP,
				CFormat("Ignoring QUIC NAT-T frame from %s:%u: QUIC context declined "
					"datagram (%u further occurrences suppressed)") %
					Uint32toStringIP(ip) % port %
					m_unservedFrameLog.TakeSuppressedCount());
		}
#else
		if (m_unservedFrameLog.ShouldLog(::GetTickCount64())) {
			AddDebugLogLineN(logClientUDP,
				CFormat("Ignoring QUIC NAT-T frame from %s:%u: no QUIC transport in this "
					"build (%u further occurrences suppressed)") %
					Uint32toStringIP(ip) % port %
					m_unservedFrameLog.TakeSuppressedCount());
		}
#endif
		break;

	case OP_NATT_FRAME_CAPS:
#ifdef AMULE_QUIC_TRANSPORT
	{
		if (!QuicNatTEnabled())
			break;
		QuicNatt::EaqcFrame request{};
		const auto localHash = QuicLocalIdentityFromUserHash();
		if (!QuicNatt::DecodeEaqcFrame(
			    classified.payload, classified.payloadLength, localHash, request)) {
			break;
		}
		const auto address = CNetworkAddress::FromIPv4NetworkOrderOrAbsent(ip);
		const auto now = ::GetTickCount64();
		// The ACK carries our user hash: only a peer a rendezvous announced may learn it.
		if (!m_keyFrames.IsExpectedPeer(ip, port, now) || theApp->ipfilter->IsFiltered(ip)) {
			break;
		}
		if (!m_capsAckLimiter.Admit(address, now)) {
			break;
		}
		const auto ack = QuicNatt::EncodeEaqcFrame(
			QuicLocalCapabilityOptions(), localHash, request.senderUserHash, request.fileHash);
		auto response = std::make_unique<CPacket>(
			OP_NATT_FRAME_CAPS_ACK, static_cast<uint32>(ack.size()), OP_UDPRESERVEDPROT2);
		response->CopyToDataBuffer(0, ack.data(), static_cast<unsigned int>(ack.size()));
		const QuicCapsSendPlan sendPlan =
			MakeQuicCapsSendPlan(thePrefs::IsClientCryptLayerSupported(),
				theApp->GetPublicIP(true),
				request.senderUserHash);
		SendPacket(response.release(),
			ip,
			port,
			sendPlan.obfuscate,
			sendPlan.obfuscate ? sendPlan.targetHash.data() : nullptr,
			false,
			0);
	}
#else
		if (m_unservedFrameLog.ShouldLog(::GetTickCount64())) {
			AddDebugLogLineN(logClientUDP,
				CFormat("Ignoring NAT-T CAPS from %s:%u: no QUIC transport in this "
					"build (%u further occurrences suppressed)") %
					Uint32toStringIP(ip) % port %
					m_unservedFrameLog.TakeSuppressedCount());
		}
#endif
	break;

	case OP_NATT_FRAME_CAPS_ACK:
#ifdef AMULE_QUIC_TRANSPORT
	{
		if (!QuicNatTEnabled())
			break;
		QuicNatt::EaqcFrame ack{};
		const auto now = ::GetTickCount64();
		if (!QuicNatt::DecodeEaqcFrame(classified.payload,
			    classified.payloadLength,
			    QuicLocalIdentityFromUserHash(),
			    ack))
			break;
		const auto zero = std::array<uint8_t, 16>{};
		if (ack.senderUserHash == zero || ack.options == 0 ||
			!m_capsRequestLedger.Consume(ack.senderUserHash, ip, port, ack.fileHash, now))
			break;
		m_capabilityCache.Remember(ack.senderUserHash, ack.options, now, 29 * 60 * 1000);
	}
#else
		if (m_unservedFrameLog.ShouldLog(::GetTickCount64())) {
			AddDebugLogLineN(logClientUDP,
				CFormat("Ignoring NAT-T CAPS_ACK from %s:%u: no QUIC transport in this "
					"build (%u further occurrences suppressed)") %
					Uint32toStringIP(ip) % port %
					m_unservedFrameLog.TakeSuppressedCount());
		}
#endif
	break;

	case OP_NATT_FRAME_KEY:
#if defined(AMULE_UTP_TRANSPORT) || defined(AMULE_QUIC_TRANSPORT)
#ifdef AMULE_UTP_TRANSPORT
		HandleUtpKeyFrame(classified.payload, classified.payloadLength, ip, port);
#endif
#ifdef AMULE_QUIC_TRANSPORT
		HandleNatKeyFrame(classified.payload, classified.payloadLength, ip, port);
#endif
#else
		if (m_unservedFrameLog.ShouldLog(::GetTickCount64())) {
			AddDebugLogLineN(logClientUDP,
				CFormat("Ignoring NAT-T key frame from %s:%u: no NAT traversal in this "
					"build (%u further occurrences suppressed)") %
					Uint32toStringIP(ip) % port %
					m_unservedFrameLog.TakeSuppressedCount());
		}
#endif
		break;

	default:
		// Unreachable: ClassifyReservedProt2Frame only reports RP2_KNOWN_TYPE for the five
		// cases above. Kept so that adding a type there without a case here fails loudly
		// rather than silently taking the drop path.
		wxFAIL;
		break;
	}
}

uint8_t CClientUDPSocket::ServedNatTTransports()
{
#if defined(ENABLE_NATT_SERVER_COORDINATION) && defined(AMULE_UTP_TRANSPORT)
#ifdef AMULE_QUIC_TRANSPORT
	const bool quicEnabled = thePrefs::IsQuicNatTEnabled();
#else
	const bool quicEnabled = false;
#endif
	if (!Ok()) {
		return 0;
	}
	return Kademlia::NatTConnectOptionsFor(m_utp.IsAvailable(), quicEnabled);
#else
	return 0;
#endif
}

void CClientUDPSocket::SendEndpointHint(uint32 ip, uint16 port, const NatRendezvous::EndpointHint &hint)
{
	const auto bytes = NatRendezvous::BuildEndpointHint(hint);
	CMemFile data(bytes.data(), bytes.size());
	CPacket *packet = new CPacket(data, OP_EMULEPROT, OP_NATT_ENDPOINT_HINT);
	theStats::AddUpOverheadOther(packet->GetPacketSize());
	AddDebugLogLineN(logKadBuddy,
		CFormat("Endpoint hint to %s: the served node is at %s") %
			Uint32_16toStringIP_Port(ip, port) %
			Uint32_16toStringIP_Port(hint.ipv4, hint.udpPort));
	SendPacket(packet, ip, port, false, nullptr, false, 0);
}

void CClientUDPSocket::ResendMovedEndpointHints(const CUpDownClient &served)
{
	NatRendezvous::UserHash target;
	std::copy_n(served.GetUserHash().GetHash(), target.size(), target.begin());
	for (const auto &sent :
		m_sentEndpointHints.TakeMoved(target, served.GetServedUdpPort(), ::GetTickCount64())) {
		SendEndpointHint(sent.requesterIP, sent.requesterPort, sent.hint);
	}
}

void CClientUDPSocket::HandleForwardedRendezvous(uint32 ip, uint16 udpPort, const uint8_t *body, size_t size)
{
	Kademlia::CBuddyStats::Own::Rendezvous &counts = theApp->clientlist->GetBuddyStats().own.rendezvous;
	++counts.received;
	const wxString requester = Uint32_16toStringIP_Port(ip, udpPort);
	const uint8_t localTransports = ServedNatTTransports();
	const PeerAddressing::UdpEndpoint observed{ CNetworkAddress::FromIPv4NetworkOrderOrAbsent(ip),
		udpPort };
	const NatRendezvous::ServedRendezvousDecision decision =
		NatRendezvous::DecideServedRendezvous(observed, body, size, localTransports);
	switch (decision.outcome) {
	case NatRendezvous::EServedRendezvous::Malformed:
		++counts.malformed;
		AddDebugLogLineN(
			logKadBuddy, "Dropped a malformed rendezvous our buddy forwarded for " + requester);
		return;
	case NatRendezvous::EServedRendezvous::NoSharedTransport:
		++counts.noSharedTransport;
		AddDebugLogLineN(logKadBuddy,
			CFormat("Dropped a rendezvous for %s: it declared NAT-T 0x%02x, we have 0x%02x") %
				requester % NatRendezvous::RequesterTransports(decision.request) %
				localTransports);
		return;
	case NatRendezvous::EServedRendezvous::Punch:
		break;
	}
#if defined(ENABLE_NATT_SERVER_COORDINATION) && defined(AMULE_UTP_TRANSPORT)
	if (!IsRendezvousPeerAllowed(ip)) {
		AddDebugLogLineN(logKadBuddy, "Ignored a rendezvous for " + requester + ": not allowed");
		return;
	}
	const uint64 now = ::GetTickCount64();
	switch (m_servedRendezvous.Admit(decision.target, now)) {
	case NatRendezvous::EServedAdmission::Start:
		break;
	case NatRendezvous::EServedAdmission::Join:
		AddDebugLogLineN(
			logKadBuddy, "Rendezvous resend for " + requester + " joins the one in flight");
		return;
	case NatRendezvous::EServedAdmission::RequesterLimited:
		AddDebugLogLineN(
			logKadBuddy, "Rendezvous for " + requester + " held back by its address limit");
		return;
	case NatRendezvous::EServedAdmission::OverGlobalLimit:
		AddDebugLogLineN(
			logKadBuddy, "Rendezvous for " + requester + " held back by the global limit");
		return;
	}
	++counts.accepted;
	AddDebugLogLineN(logKadBuddy,
		CFormat("Rendezvous from our buddy: punching %s, NAT-T 0x%02x; the requester dials") %
			requester % decision.sharedTransports);
	// Our buddy may have hinted our UDP port to the requester; a refresh corrects a moved one.
	if (CUpDownClient *buddy = theApp->clientlist->GetBuddy()) {
		buddy->SendBuddyRefresh(true);
	}
	const std::array<std::uint8_t, 16> &requesterHash = decision.request.requesterHash;
	NoteRendezvousAttempt(NatServerHolePunch::RendezvousPath::KadBuddy, ip, &requesterHash);
	// Recorded before the punch: we answer the requester's CAPS only from an expected peer. No
	// server vouches for the hash; the requester's hello decides which client the stream is.
	ExpectNatTPeer(ip, udpPort, &requesterHash);
	NatServerHolePunch::IPv4 peerIP{};
	PokeUInt32(peerIP.data(), ip);
	StartLowIdHolePunchSchedule(
		peerIP, udpPort, now, NatServerHolePunch::RendezvousPath::KadBuddy, requesterHash);
#else
	wxFAIL_MSG("A rendezvous was accepted with no NAT-T transport built in");
#endif
}

#ifdef ENABLE_NATT_SERVER_COORDINATION
bool CClientUDPSocket::IsRendezvousPeerAllowed(uint32 ip)
{
	return IsGoodIP(ip, thePrefs::FilterLanIPs()) && !theApp->ipfilter->IsFiltered(ip) &&
	       !theApp->clientlist->IsBannedClient(CNetworkAddress::FromIPv4NetworkOrderOrAbsent(ip));
}

#ifdef AMULE_UTP_TRANSPORT
void CClientUDPSocket::StartLowIdHolePunchSchedule(const NatServerHolePunch::IPv4 &peerIP,
	uint16 udpPort,
	uint64 now,
	NatServerHolePunch::RendezvousPath path,
	const std::optional<NatServerHolePunch::Hash> &peerHash)
{
	// Every path checks its target first; this is the backstop for the next one.
	if (udpPort == 0 || !IsRendezvousPeerAllowed(PeekUInt32(peerIP.data()))) {
		return;
	}
	m_lowIdHolePunchSchedules.Start(peerIP, udpPort, now, path, peerHash);
	// The first punch is due now, so it leaves with this tick rather than the next one.
	SendDueLowIdHolePunches(now);
}

void CClientUDPSocket::SendDueLowIdHolePunches(uint64 now)
{
	if (!Ok()) {
		return;
	}
	const auto punch = [this](const NatServerHolePunch::IPv4 &peerIP, uint16 udpPort) {
		CPacket *packet = new CPacket(OP_HOLEPUNCH, 0, OP_EMULEPROT);
		theStats::AddUpOverheadOther(packet->GetPacketSize());
		SendPacket(packet, PeekUInt32(peerIP.data()), udpPort, false, nullptr, false, 0);
	};
	for (const NatServerHolePunch::PunchDue &due : m_lowIdHolePunchSchedules.TakeDue(now)) {
		if (due.kind == NatServerHolePunch::PunchDue::Endpoint) {
			punch(due.peerIP, due.udpPort);
			continue;
		}
		// One sweep per destination address every five minutes, ten a minute overall, for each of
		// three budgets: the server paths, the Kad buddy's served side and our own Kad rendezvous.
		NatServerHolePunch::SweepBudget &budget = NatServerHolePunch::SweepBudgetFor(due.path,
			m_lowIdHolePunchSweeps,
			m_kadBuddyHolePunchSweeps,
			m_kadRequesterHolePunchSweeps);
		if (!budget.Admit(due.peerIP, now)) {
			AddDebugLogLineN(logClientUDP,
				CFormat("Punch sweep to %s refused by the sweep budget") %
					Uint32toStringIP(PeekUInt32(due.peerIP.data())));
			continue;
		}
		const std::vector<uint16> ports = NatServerHolePunch::SweptPorts(due.udpPort);
		if (!ports.empty()) {
			AddDebugLogLineN(logClientUDP,
				CFormat("Punch sweep to %s: %u ports %u-%u around %u") %
					Uint32toStringIP(PeekUInt32(due.peerIP.data())) %
					static_cast<unsigned>(ports.size()) % ports.front() % ports.back() %
					due.udpPort);
		}
		for (const uint16 port : ports) {
			punch(due.peerIP, port);
		}
	}
}

bool CClientUDPSocket::StartKadRendezvous(const NatRendezvous::SKadRendezvousTarget &target)
{
	const uint64 now = ::GetTickCount64();
	if (!Ok() || target.udpPort == 0 || !IsRendezvousPeerAllowed(target.ip) ||
		!m_kadRendezvousRequests.Start(target, now)) {
		return false;
	}
	// Recorded before the punch, as on every path: we answer the target's CAPS only from an
	// expected peer, and the hash lets its hello find this attempt.
	ExpectNatTPeer(target.ip, target.udpPort, &target.hash);
	NatServerHolePunch::IPv4 peerIP{};
	PokeUInt32(peerIP.data(), target.ip);
	StartLowIdHolePunchSchedule(
		peerIP, target.udpPort, now, NatServerHolePunch::RendezvousPath::KadRequester, target.hash);
	// The source punches from where it is now, and our NAT drops it from a port we never sent to.
	for (const NatRendezvous::SKadEndpoint &former : target.former) {
		if (former.port == 0 || !IsRendezvousPeerAllowed(former.ip)) {
			continue;
		}
		ExpectNatTPeer(former.ip, former.port, &target.hash);
		NatServerHolePunch::IPv4 formerIP{};
		PokeUInt32(formerIP.data(), former.ip);
		StartLowIdHolePunchSchedule(formerIP,
			former.port,
			now,
			NatServerHolePunch::RendezvousPath::KadRequester,
			target.hash);
	}
	if (!target.former.empty()) {
		AddDebugLogLineN(logClientUDP,
			CFormat("Rendezvous for %s: also punching %u endpoint(s) earlier records named") %
				Uint32_16toStringIP_Port(target.ip, target.udpPort) %
				static_cast<unsigned>(target.former.size()));
	}
	++theApp->clientlist->GetBuddyStats().own.requestedRendezvous.started;
	TickKadRendezvous(now);
	return true;
}

void CClientUDPSocket::TickKadRendezvous(uint64 now)
{
	for (const NatRendezvous::SKadRendezvousAction &action : m_kadRendezvousRequests.TakeDue(now)) {
		const NatRendezvous::SKadRendezvousTarget &target = action.target;
		switch (action.kind) {
		case NatRendezvous::SKadRendezvousAction::Send: {
			const uint8_t localTransports = ServedNatTTransports();
			const std::vector<std::uint8_t> envelope = NatRendezvous::BuildRendezvousEnvelope(
				target.buddyID,
				[] {
					NatRendezvous::UserHash hash{};
					std::copy_n(
						thePrefs::GetUserHash().GetHash(), hash.size(), hash.begin());
					return hash;
				}(),
				NatRendezvous::RequesterConnectOptions(
					Kademlia::CPrefs::GetMyConnectOptions(), localTransports),
				target.fileHash,
				theApp->GetPublicIP(),
				thePrefs::GetEffectiveUDPPort(),
				NatRendezvous::RequesterTransportHint(localTransports));
			CMemFile data(envelope.data(), envelope.size());
			CPacket *packet = new CPacket(data, OP_EMULEPROT, OP_REASKCALLBACKUDP);
			AddDebugLogLineN(logClientUDP,
				"Client UDP socket: send OP_REASKCALLBACKUDP carrying a rendezvous for " +
					Uint32_16toStringIP_Port(target.ip, target.udpPort));
			theStats::AddUpOverheadOther(packet->GetPacketSize());
			SendPacket(packet, target.buddyIP, target.buddyPort, false, nullptr, true, 0);
			++theApp->clientlist->GetBuddyStats().own.requestedRendezvous.sent;
			break;
		}
		case NatRendezvous::SKadRendezvousAction::FallbackDial:
			AddDebugLogLineN(logClientUDP,
				"No punch from " + Uint32_16toStringIP_Port(target.ip, target.udpPort) +
					": dialling its Kad endpoint");
			DialLowIdHolePunchPeer(target.ip,
				target.tcpPort,
				target.udpPort,
				NatServerHolePunch::VouchedPeer{ target.hash, std::nullopt });
			++theApp->clientlist->GetBuddyStats().own.requestedRendezvous.fallbackDials;
			break;
		case NatRendezvous::SKadRendezvousAction::Expired:
			AddDebugLogLineN(logClientUDP,
				"Rendezvous for " + Uint32_16toStringIP_Port(target.ip, target.udpPort) +
					" timed out");
			++theApp->clientlist->GetBuddyStats().own.requestedRendezvous.timedOut;
			break;
		}
	}
}

void CClientUDPSocket::ResetLowIdHolePunchSchedules()
{
	m_lowIdHolePunchSchedules.ClearServerPaths();
}
#endif

uint32 CClientUDPSocket::SendServerWayObfuscated(
	CPacket *packet, uint32 serverIp, uint16 port, uint32 serverKeyUdp)
{
	// Encrypted as CServerUDPSocket::SendPacket does, but sent from this socket: the server reads
	// the source port peers must punch.
	uint16 rawSize = static_cast<uint16>(packet->GetPacketSize() + 2);
	uint8_t *raw = new uint8_t[rawSize];
	memcpy(raw, packet->GetUDPHeader(), 2);
	memcpy(raw + 2, packet->GetDataBuffer(), packet->GetPacketSize());
	delete packet;
	rawSize =
		static_cast<uint16>(CEncryptedDatagramSocket::EncryptSendServer(&raw, rawSize, serverKeyUdp));
	CMemFile encryptedData(raw + 2, rawSize - 2);
	CPacket *encrypted = new CPacket(encryptedData, raw[0], raw[1]);
	delete[] raw;
	const uint32 queuedBytes = encrypted->GetPacketSize();
	SendPacket(encrypted, serverIp, port, false, nullptr, false, 0);
	return queuedBytes;
}
#endif

#if defined(ENABLE_NATT_SERVER_COORDINATION) && defined(AMULE_UTP_TRANSPORT)
bool CClientUDPSocket::DialLowIdHolePunchPeer(
	uint32 ip, uint16 tcpPort, uint16 udpPort, const NatServerHolePunch::VouchedPeer &peer)
{
	if (tcpPort == 0 || udpPort == 0 || !Ok() || !m_utp.IsAvailable() || !IsRendezvousPeerAllowed(ip) ||
		theApp->listensocket->TooManySockets()) {
		return false;
	}
	// This temporary client is not a download source. OP_HELLO, not server-supplied
	// INFO fields, decides which existing LowID source the new stream belongs to.
	CUpDownClient *client = new CUpDownClient(tcpPort, HybridIDOfIPv4(ip), 0, 0, nullptr, false, false);
	// The server-vouched punch dial needs HasValidHash() for the CAPS probe and the QUIC
	// selection in TryServerVouchedLowIdUtpDial(); set it before AddClient() so the client
	// list indexes it immediately, matching the OP_DIRECTCALLBACKREQ pattern. An all-zero hash
	// (a dial with no 0x61 in hand) is not a real one: skip the call rather than running the
	// hash-list/chat-adoption machinery in SetUserHash() for nothing.
	if (peer.userHash != NatServerHolePunch::Hash{}) {
		client->SetUserHash(CMD4Hash(peer.userHash.data()));
		// The peer's crypt options from a 30-byte 0x61 let the dial be obfuscated from its SYN,
		// and let a peer that requires obfuscation accept it.
		if (peer.cryptOptions) {
			client->SetConnectOptions(*peer.cryptOptions, true, false);
		}
	}
	theApp->clientlist->AddClient(client);
	CClientRef ref{ client CLIENT_DEBUGSTRING("LowID hole punch dial") };
	client->SetUDPPort(udpPort);
	client->MarkLowIdHolePunchDial();
	const bool started = client->TryServerVouchedLowIdUtpDial();
	if (!started) {
		if (CUpDownClient *checked = ref.GetClientChecked()) {
			checked->Safe_Delete();
		}
	}
	return started;
}

bool CClientUDPSocket::IsLugdunumNatCallbackCoolingDown(const LugdunumNatTraversal::CallbackFailureKey &key)
{
	return m_lugdunumCallbackFailures.IsCoolingDown(key, ::GetTickCount64());
}

void CClientUDPSocket::RecordLugdunumNatCallbackFailure(const LugdunumNatTraversal::CallbackFailureKey &key)
{
	m_lugdunumCallbackFailures.RecordFailure(key, ::GetTickCount64());
}

void CClientUDPSocket::ClearLugdunumNatCallbackFailure(const LugdunumNatTraversal::CallbackFailureKey &key)
{
	m_lugdunumCallbackFailures.Clear(key);
}

bool CClientUDPSocket::IsLugdunumCallbackRendezvousActive(
	const LugdunumNatTraversal::CallbackFailureKey &key) const
{
	return m_lugdunumCallbackRendezvous.IsActive(key);
}

void CClientUDPSocket::CompleteLugdunumCallbackRendezvous(const LugdunumNatTraversal::CallbackFailureKey &key)
{
	m_lugdunumCallbackRendezvous.Complete(key);
}

void CClientUDPSocket::NoteRendezvousAttempt(NatServerHolePunch::RendezvousPath path,
	uint32 ip,
	const std::array<std::uint8_t, 16> *peerHash,
	bool countAttempt)
{
	std::optional<NatServerHolePunch::Hash> hash;
	if (peerHash != nullptr) {
		hash = *peerHash;
	}
	if (!m_rendezvousOutcomes.NoteAttempt(path, ip, hash, ::GetTickCount64()) || !countAttempt) {
		return;
	}
	switch (path) {
	case NatServerHolePunch::RendezvousPath::ServerPunch:
		theStats::AddServerHolePunchAttempt();
		break;
	case NatServerHolePunch::RendezvousPath::LugdunumCallback:
		theStats::AddLugdunumCallbackAttempt();
		break;
	case NatServerHolePunch::RendezvousPath::KadBuddy:
		// Counted as buddy.own.rendezvous.accepted by the handler, which also counts the drops.
		break;
	case NatServerHolePunch::RendezvousPath::KadRequester:
		// Counted as buddy.own.requested_rendezvous by StartKadRendezvous(); never noted here.
		break;
	}
}

void CClientUDPSocket::NoteLugdunumCallbackRequested(uint32 targetId)
{
	m_requestedCallbackTargets.Note(targetId, ::GetTickCount64());
}

bool CClientUDPSocket::SendForeignNatCallbackRequest(CServer *server,
	uint32 targetId,
	uint8_t ourCrypt,
	uint16 sourceTcpPort,
	const std::optional<NatServerHolePunch::Hash> &knownHash,
	unsigned maxTargets)
{
	if (server == nullptr || !Ok() ||
		!m_foreignCallbacks.TryStart(server->GetIP(),
			server->GetServerKeyUDP(),
			targetId,
			sourceTcpPort,
			knownHash,
			::GetTickCount64(),
			maxTargets)) {
		return false;
	}
	const auto payload = NatServerHolePunch::BuildForeignCallbackRequest(targetId, ourCrypt);
	CMemFile data(payload.data(), payload.size());
	CPacket *packet = new CPacket(data, OP_EDONKEYPROT, OP_NAT_CALLBACKREQUEST);
	AddDebugLogLineN(logClientUDP,
		CFormat("Client UDP socket: cross-server OP_NAT_CALLBACKREQUEST for source %u to %s") %
			targetId %
			Uint32_16toStringIP_Port(server->GetIP(), server->GetObfuscationPortUDP()));
	theStats::AddUpOverheadOther(SendServerWayObfuscated(
		packet, server->GetIP(), server->GetObfuscationPortUDP(), server->GetServerKeyUDP()));
	return true;
}

bool CClientUDPSocket::TryProcessForeignNatCallback(uint32 ip, uint8_t *buffer, size_t length)
{
	const std::uint64_t now = ::GetTickCount64();
	// Decrypted with the key the request was sent with: a failed attempt leaves the buffer as it was.
	uint8_t *decrypted = buffer;
	int decryptedLen = static_cast<int>(length);
	for (const std::uint32_t key : m_foreignCallbacks.PendingKeysFrom(ip, now)) {
		decryptedLen = CEncryptedDatagramSocket::DecryptReceivedServer(
			buffer, static_cast<int>(length), &decrypted, key, ip);
		// Every non-decrypting path returns the input length unchanged; a decrypt strips the header.
		if (decryptedLen < static_cast<int>(length)) {
			break;
		}
	}
	if (decryptedLen >= static_cast<int>(length) || decrypted == nullptr || decryptedLen < 2) {
		return false;
	}
	if (decrypted[0] != OP_EDONKEYPROT || decrypted[1] != OP_NAT_CALLBACKREQUESTED_UDP) {
		AddDebugLogLineN(logClientUDP,
			CFormat("Unexpected server UDP packet 0x%02x/0x%02x from %s on the client socket") %
				decrypted[0] % decrypted[1] % Uint32toStringIP(ip));
		return true;
	}
	const std::optional<NatServerHolePunch::ForeignCallbackAnswer> answer =
		NatServerHolePunch::ParseForeignCallbackAnswer(decrypted + 2, decryptedLen - 2);
	if (!answer || !m_foreignCallbacks.AnswersPending(ip, answer->targetId, answer->targetHash, now)) {
		AddDebugLogLineN(logClientUDP,
			CFormat("Dropped a cross-server 0xA7 from %s: no pending request") %
				Uint32toStringIP(ip));
		return true;
	}
	// The pending request stays: a foreign server names the target, and we punch only a target
	// we would contact anyway.
	if (!IsRendezvousPeerAllowed(answer->targetIp)) {
		AddDebugLogLineN(logClientUDP,
			CFormat("Ignored a cross-server 0xA7 from %s: target %s is not allowed") %
				Uint32toStringIP(ip) % Uint32toStringIP(answer->targetIp));
		return true;
	}
	// Checked before the answer is taken: a refusal here leaves the request for the server's resend.
	CServerConnect *connect = theApp->serverconnect;
	if (connect == nullptr || !connect->LowIdHolePunchActionLimiter().Admit(now)) {
		return true;
	}
	const std::optional<NatServerHolePunch::ForeignCallbackRequests::Pending> pending =
		m_foreignCallbacks.TakeAnswer(ip, answer->targetId, answer->targetHash, now);
	if (!pending) {
		return true;
	}
	AddDebugLogLineN(logClientUDP,
		CFormat("Cross-server answer from %s: source %u at %s; dialling") % Uint32toStringIP(ip) %
			answer->targetId % Uint32_16toStringIP_Port(answer->targetIp, answer->targetUdpPort));
	// Both sides punch; the requester holds the target's hash, so it dials, obfuscated from the start.
	ExpectNatTPeer(answer->targetIp, answer->targetUdpPort, &answer->targetHash);
	NatServerHolePunch::IPv4 targetIP{};
	PokeUInt32(targetIP.data(), answer->targetIp);
	StartLowIdHolePunchSchedule(targetIP,
		answer->targetUdpPort,
		now,
		NatServerHolePunch::RendezvousPath::LugdunumCallback,
		answer->targetHash);
	const NatServerHolePunch::VouchedPeer peer{ answer->targetHash,
		static_cast<std::uint8_t>(answer->crypt & 0x07) };
	DialLowIdHolePunchPeer(answer->targetIp, pending->sourceTcpPort, answer->targetUdpPort, peer);
	return true;
}

bool CClientUDPSocket::NoteRendezvousHello(uint32 ip, const std::array<std::uint8_t, 16> &peerHash)
{
	// The peer is connected, so its punches have nothing left to reach.
	NatServerHolePunch::IPv4 peer{};
	PokeUInt32(peer.data(), ip);
	m_lowIdHolePunchSchedules.CancelPeer(peer, peerHash);
	if (m_kadRendezvousRequests.Complete(ip, peerHash)) {
		++theApp->clientlist->GetBuddyStats().own.requestedRendezvous.connected;
		return true;
	}
	const std::optional<NatServerHolePunch::RendezvousPath> path =
		m_rendezvousOutcomes.TakeSuccess(ip, peerHash, ::GetTickCount64());
	if (!path) {
		return false;
	}
	switch (*path) {
	case NatServerHolePunch::RendezvousPath::ServerPunch:
		theStats::AddServerHolePunchSuccess();
		break;
	case NatServerHolePunch::RendezvousPath::LugdunumCallback:
		theStats::AddLugdunumCallbackSuccess();
		break;
	case NatServerHolePunch::RendezvousPath::KadBuddy:
		++theApp->clientlist->GetBuddyStats().own.rendezvous.connected;
		break;
	case NatServerHolePunch::RendezvousPath::KadRequester:
		// Ended by m_kadRendezvousRequests.Complete() above; never noted as an outcome.
		break;
	}
	return true;
}

void CClientUDPSocket::SendLugdunumNatCallbackRequest(const LugdunumNatTraversal::CallbackFailureKey &key)
{
	if (theApp->serverconnect == nullptr || !Ok()) {
		return;
	}
	CServer *server = theApp->serverconnect->GetCurrentServer();
	if (server == nullptr) {
		return;
	}
	CServer *serverListEntry =
		theApp->serverlist->GetServerByAddress(server->GetAddress(), server->GetPort());
	if (serverListEntry == nullptr) {
		return;
	}
	const LugdunumNatTraversal::CallbackRequestFacts facts{ theApp->serverconnect->GetClientID(),
		key.requesterId,
		server->GetIP() == key.serverIp && server->GetPort() == key.serverPort,
		server->IsLugdunumNatCallbackActive(),
		thePrefs::IsServerCryptLayerUDPEnabled(),
		serverListEntry->GetServerKeyUDP(),
		serverListEntry->SupportsObfuscationUDP(),
		thePrefs::IsLugdunumNatCallbackEnabled() };
	if (!LugdunumNatTraversal::ShouldRequestLugdunumNatCallback(facts)) {
		return;
	}
	const LugdunumNatTraversal::CallbackRequest request{ key.requesterId,
		theApp->serverconnect->GetClientID() };
	const auto payload = LugdunumNatTraversal::BuildCallbackRequest(request);
	CMemFile data(payload.data(), payload.size());
	CPacket *packet = new CPacket(data, OP_EDONKEYPROT, OP_NAT_CALLBACKREQUEST);
	theStats::AddUpOverheadOther(SendServerWayObfuscated(packet,
		server->GetIP(),
		serverListEntry->GetObfuscationPortUDP(),
		serverListEntry->GetServerKeyUDP()));
}

void CClientUDPSocket::TickLugdunumCallbackRendezvous()
{
	for (const LugdunumNatTraversal::CallbackRendezvousAction &action :
		m_lugdunumCallbackRendezvous.Tick(::GetTickCount64())) {
		if (action.requestReverseCallback) {
			SendLugdunumNatCallbackRequest(action.key);
		}
		if (action.sendKeepalive && Ok()) {
			CPacket *punch = new CPacket(OP_HOLEPUNCH, 0, OP_EMULEPROT);
			theStats::AddUpOverheadOther(punch->GetPacketSize());
			SendPacket(punch, action.ip, action.udpPort, false, nullptr, false, 0);
		}
	}
}

void CClientUDPSocket::HandleForwardedCallback(const LugdunumNatTraversal::ForwardedCallback &callback)
{
	CServerConnect *connect = theApp->serverconnect;
	const CServer *server = connect != nullptr ? connect->GetCurrentServer() : nullptr;
	if (server != nullptr &&
		NatServerHolePunch::ShouldAnswerWakeWithKeepalive(connect->IsWakeKeepaliveActive(),
			callback.requesterId,
			server->SupportsLowIdHolePunch())) {
		AnswerWakeWithKeepalive(callback);
	} else {
		DialLugdunumNatCallbackPeer(callback);
	}
}

bool CClientUDPSocket::AnswerWakeWithKeepalive(const LugdunumNatTraversal::ForwardedCallback &callback)
{
	CServerConnect *connect = theApp->serverconnect;
	CServer *server = connect->GetCurrentServer();
	if (server == nullptr ||
		!LugdunumNatTraversal::ShouldDialForwardedCallback(callback,
			server->IsLugdunumNatCallbackActive(),
			thePrefs::IsLugdunumNatCallbackEnabled()) ||
		!Ok() || !m_utp.IsAvailable() || !IsRendezvousPeerAllowed(callback.seenIp)) {
		return false;
	}
	// A wake already waiting for this requester's 0x61 gets its 0x9F burst again, since the 0x9F is
	// what releases the 0x61, but no second punch: that would spend the action budget the other
	// rendezvous share.
	if (connect->PendingWakes().Contains(callback.requesterId)) {
		connect->StartWakeKeepalives();
		return true;
	}
	const std::uint64_t now = ::GetTickCount64();
	if (!connect->LowIdHolePunchActionLimiter().Admit(now)) {
		return false;
	}
	const LugdunumNatTraversal::CallbackDialTarget target =
		LugdunumNatTraversal::BuildCallbackDialTarget(callback);
	ExpectNatTPeer(target.ip, target.udpPort, callback.hasObfuscation ? &callback.userHash : nullptr);
	// Punch now and refresh our port; the dial waits for the 0x61's role, as on any wake-keepalive
	// server, and no reverse callback is needed when the 0x61 gives both sides the endpoints.
	connect->PendingWakes().Add(callback.requesterId, callback, now);
	if (callback.requesterId == 0) {
		// A cross-server requester dials us and no 0x61 follows: its hello is this rendezvous's
		// success.
		NoteRendezvousAttempt(
			NatServerHolePunch::RendezvousPath::LugdunumCallback, target.ip, nullptr);
	}
	connect->StartWakeKeepalives();
	NatServerHolePunch::IPv4 targetIP{};
	PokeUInt32(targetIP.data(), target.ip);
	std::optional<NatServerHolePunch::Hash> vouched;
	if (callback.hasObfuscation) {
		vouched = callback.userHash;
	}
	StartLowIdHolePunchSchedule(
		targetIP, target.udpPort, now, NatServerHolePunch::RendezvousPath::ServerPunch, vouched);
	AddDebugLogLineN(logClientUDP,
		CFormat("Wake keepalive for requester %u: OP_HOLEPUNCH at 0, 0.5, 1 "
			"and 2 s to %s") %
			callback.requesterId % Uint32_16toStringIP_Port(target.ip, target.udpPort));
	return true;
}

bool CClientUDPSocket::DialLugdunumNatCallbackPeer(const LugdunumNatTraversal::ForwardedCallback &callback)
{
	CServerConnect *connect = theApp->serverconnect;
	CServer *server = connect != nullptr ? connect->GetCurrentServer() : nullptr;
	if (server == nullptr || !connect->IsConnected()) {
		return false;
	}
	if (!LugdunumNatTraversal::ShouldDialForwardedCallback(callback,
		    server->IsLugdunumNatCallbackActive(),
		    thePrefs::IsLugdunumNatCallbackEnabled()) ||
		!Ok() || !m_utp.IsAvailable() || !IsRendezvousPeerAllowed(callback.seenIp) ||
		theApp->listensocket->TooManySockets()) {
		return false;
	}

	// The forwarded endpoint is a route hint, not an identity; OP_HELLO supplies the peer ID.
	const LugdunumNatTraversal::CallbackDialTarget target =
		LugdunumNatTraversal::BuildCallbackDialTarget(callback);
	const LugdunumNatTraversal::CallbackFailureKey failureKey{
		server->GetIP(), server->GetPort(), callback.requesterId, target.ip, target.udpPort
	};
	if (IsLugdunumNatCallbackCoolingDown(failureKey) ||
		!m_lugdunumCallbackActions.Admit(failureKey, ::GetTickCount64())) {
		return false;
	}
	ExpectNatTPeer(target.ip, target.udpPort, callback.hasObfuscation ? &callback.userHash : nullptr);

	CUpDownClient *client = new CUpDownClient(
		0, target.provisionalUserId, server->GetIP(), server->GetPort(), nullptr, true, false);
	client->SetUserAddress(CNetworkAddress::FromIPv4NetworkOrder(target.ip));
	// Only the 27-byte forward carries a hash; a shorter one leaves the client HasValidHash()
	// false, same as before, so CAPS/QUIC stay gated on an actually-known identity.
	if (callback.hasObfuscation) {
		client->SetUserHash(CMD4Hash(callback.userHash.data()));
		client->SetConnectOptions(callback.obfuscationSettings, true, false);
	}
	theApp->clientlist->AddClient(client);
	CClientRef ref{ client CLIENT_DEBUGSTRING("Lugdunum callback dial") };
	client->SetUDPPort(target.udpPort);
	client->MarkLowIdHolePunchDial();
	client->MarkLugdunumCallbackDial(failureKey);
	const bool ownRequest = m_requestedCallbackTargets.Take(callback.requesterId, ::GetTickCount64());
	if (!ownRequest) {
		client->NoteCallbackAnswer(EConnectionOpener::AnswerLugdunumCallback);
	}
	// A reverse callback for our own request still records the rendezvous, for its success, but
	// the request already counted the attempt.
	NoteRendezvousAttempt(NatServerHolePunch::RendezvousPath::LugdunumCallback,
		target.ip,
		callback.hasObfuscation ? &callback.userHash : nullptr,
		!ownRequest);
	const bool started = LugdunumNatTraversal::RunCallbackDialApplication(
		failureKey,
		[client]() { return client->TryServerVouchedLowIdUtpDial(); },
		[this](const LugdunumNatTraversal::CallbackFailureKey &key) {
			RecordLugdunumNatCallbackFailure(key);
		},
		[client](std::uint32_t requesterId) {
			client->TrySendLugdunumNatCallbackRequest(requesterId);
		},
		[&ref]() {
			if (CUpDownClient *checked = ref.GetClientChecked()) {
				checked->Safe_Delete();
			}
		});
	if (started) {
		m_lugdunumCallbackRendezvous.Start(failureKey, target.ip, target.udpPort, ::GetTickCount64());
	}
	return started;
}

void CClientUDPSocket::HandleLowIdHolePunch(uint32 host, uint16 port)
{
	NatServerHolePunch::IPv4 source{};
	PokeUInt32(source.data(), host);
	// A punch from the peer is what its schedule was for, whatever path started it.
	m_lowIdHolePunchSchedules.Cancel(source, port);
	// A Kad rendezvous we asked for needs no server: the target's punch is the dial's cue.
	if (const auto target = m_kadRendezvousRequests.TakePunch(host, port, ::GetTickCount64())) {
		AddDebugLogLineN(logClientUDP,
			"OP_HOLEPUNCH from " + Uint32_16toStringIP_Port(host, port) +
				" answers our rendezvous; dialling");
		DialLowIdHolePunchPeer(host,
			target->tcpPort,
			port,
			NatServerHolePunch::VouchedPeer{ target->hash, std::nullopt });
		++theApp->clientlist->GetBuddyStats().own.requestedRendezvous.punched;
		return;
	}
	CServerConnect *connect = theApp->serverconnect;
	if (connect == nullptr) {
		return;
	}
	if (!connect->IsConnected() || !thePrefs::IsLowIdHolePunchEnabled() ||
		thePrefs::GetEffectiveUDPPort() == 0) {
		return;
	}
	CServer *server = connect->GetCurrentServer();
	if (server == nullptr || !server->SupportsLowIdHolePunch() || !IsRendezvousPeerAllowed(host)) {
		return;
	}
	const std::uint64_t now = ::GetTickCount64();
	const NatServerHolePunch::PunchAction action =
		connect->LowIdHolePunchExpectedEndpoints().Take(source, port, now);
	if (action.kind == NatServerHolePunch::PunchAction::DialNow) {
		if (connect->LowIdHolePunchActionLimiter().Admit(now)) {
			AddDebugLogLineN(logClientUDP,
				CFormat("OP_HOLEPUNCH from %s; dialling its observed UDP port") %
					Uint32_16toStringIP_Port(host, port));
			// OP_HOLEPUNCH carries no user hash; the 0x61 that announced this endpoint does.
			DialLowIdHolePunchPeer(host, action.tcpPort, port, action.peer);
		}
	} else if (action.kind == NatServerHolePunch::PunchAction::DialLater) {
		connect->LowIdHolePunchDialBacks().Schedule(source, action.tcpPort, port, now, action.peer);
	}
}
#endif

void CClientUDPSocket::ProcessPacket(
	uint8_t *packet, int16 size, int8 opcode, uint32 host, uint16 port, [[maybe_unused]] bool obfuscated)
{
	switch (opcode) {
#if defined(AMULE_UTP_TRANSPORT) || defined(AMULE_QUIC_TRANSPORT)
	case OP_NATT_SOURCE_ECHO:
		theStats::AddDownOverheadOther(size);
		HandleSourceEcho(packet, size, host, port, obfuscated);
		break;
#endif
#if defined(ENABLE_NATT_SERVER_COORDINATION) && defined(AMULE_UTP_TRANSPORT)
	case OP_HOLEPUNCH:
		theStats::AddDownOverheadOther(size);
		if (size == 0) {
			HandleLowIdHolePunch(host, port);
		}
		break;
#endif
	case OP_REASKCALLBACKUDP: {
		AddDebugLogLineN(logClientUDP, "Client UDP socket; OP_REASKCALLBACKUDP");
		theStats::AddDownOverheadOther(size);
		Kademlia::CBuddyRelayCounts &relays = theApp->clientlist->GetBuddyStats().serving.relays;
		// It is relayed whole and its source can be forged, so nothing larger than a reask can be
		// is relayed.
		if (size < 17 || static_cast<std::size_t>(size) > Kademlia::MaxReaskRelaySize) {
			++relays.reasksMalformed;
			break;
		}
		CUpDownClient *buddy = theApp->clientlist->FindServedBuddy(packet);
		if (buddy == nullptr) {
			theApp->clientlist->RecordBuddyRelayDrop(false, packet);
			break;
		}
		if (buddy->GetSocket() == nullptr) {
			break;
		}
		CMemFile mem_packet;
		std::optional<NatRendezvous::EndpointHint> hint;
		if (size > NatRendezvous::kRendezvousPrefixSize &&
			packet[NatRendezvous::kRendezvousPrefixSize] == NatRendezvous::OP_RENDEZVOUS &&
			std::all_of(packet + NatRendezvous::kHashSize,
				packet + NatRendezvous::kRendezvousPrefixSize,
				[](uint8 byte) { return byte == 0; })) {
			// A rendezvous is checked and rebuilt from what parsed, never relayed as received.
			const auto envelope = NatRendezvous::ParseRendezvousEnvelope(packet, size);
			if (!envelope) {
				++relays.reasksMalformed;
				break;
			}
			if (!Kademlia::ForwardsRendezvous(buddy->GetServedBuddyOptions(),
				    NatRendezvous::RequesterTransports(envelope->request))) {
				++relays.rendezvousNoSharedTransport;
				break;
			}
			const std::vector<std::uint8_t> body =
				NatRendezvous::BuildForwardedRendezvous(envelope->request);
			mem_packet.WriteUInt32(host);
			mem_packet.WriteUInt16(port);
			mem_packet.Write(body.data(), body.size());
			if ((envelope->request.connectOptions & NatRendezvous::kEndpointHintBit) != 0 &&
				envelope->request.fileHash) {
				NatRendezvous::UserHash target;
				std::copy_n(buddy->GetUserHash().GetHash(), target.size(), target.begin());
				hint = NatRendezvous::MakeServedEndpointHint(target,
					envelope->servingBuddyId,
					*envelope->request.fileHash,
					buddy->GetIP(),
					buddy->GetServedUdpPort(),
					buddy->GetKadPort(),
					buddy->GetCryptOptions(),
					buddy->GetServedBuddyOptions());
			}
		} else {
			/* The packet starts with a 16-byte key for the buddy. It is currently
			   unused, so the transformation discards the first 10 bytes below and
			   overwrites the other 6 with ip/port. */
			mem_packet.Write(packet + 10, size - 10);
			// Change the ip and port while leaving the rest untouched
			mem_packet.Seek(0, wxFromStart);
			mem_packet.WriteUInt32(host);
			mem_packet.WriteUInt16(port);
		}
		CPacket *response = new CPacket(mem_packet, OP_EMULEPROT, OP_REASKCALLBACKTCP);
		const uint32 responseSize = response->GetPacketSize();
		if (!theApp->clientlist->HasBuddyRelayBudget(buddy, responseSize)) {
			delete response;
			++relays.reasksOverBudget;
			break;
		}
		AddDebugLogLineN(logClientUDP, "Client UDP socket: send OP_REASKCALLBACKTCP");
		theStats::AddUpOverheadFileRequest(responseSize);
		buddy->GetSocket()->SendPacket(response);
		theApp->clientlist->RecordBuddyRelay(buddy, false, responseSize);
		if (hint) {
			// One punch with it, as eMuleQt's buddy sends.
			SendEndpointHint(host, port, *hint);
			CPacket *punch = new CPacket(OP_HOLEPUNCH, 0, OP_EMULEPROT);
			theStats::AddUpOverheadOther(punch->GetPacketSize());
			SendPacket(punch, host, port, false, nullptr, false, 0);
			m_sentEndpointHints.Record(host, port, *hint, ::GetTickCount64());
		}
		break;
	}
	case OP_REASKFILEPING: {
		AddDebugLogLineN(logClientUDP, "Client UDP socket: OP_REASKFILEPING");
		theStats::AddDownOverheadFileRequest(size);

		// A reask always carries the 16-byte file hash. Log a shorter datagram here
		// rather than through the reader's end-of-buffer exception.
		if (size < 16) {
			AddDebugLogLineN(logClientUDP,
				CFormat("Client UDP socket: rejected short OP_REASKFILEPING (%u bytes) from "
					"%s:%u") %
					static_cast<unsigned>(size) % Uint32toStringIP(host) %
					static_cast<unsigned>(port));
			break;
		}

		CMemFile data_in(packet, size);
		CMD4Hash reqfilehash = data_in.ReadHash();
		CKnownFile *reqfile = theApp->sharedfiles->GetFileByID(reqfilehash);
		bool bSenderMultipleIpUnknown = false;
		CUpDownClient *sender = theApp->uploadqueue->GetWaitingClientByIP_UDP(
			CNetworkAddress::FromIPv4NetworkOrderOrAbsent(host),
			port,
			true,
			&bSenderMultipleIpUnknown);

		if (!reqfile) {
			CPacket *response = new CPacket(OP_FILENOTFOUND, 0, OP_EMULEPROT);
			theStats::AddUpOverheadFileRequest(response->GetPacketSize());
			if (sender) {
				SendPacket(response,
					host,
					port,
					sender->ShouldReceiveCryptUDPPackets(),
					sender->GetUserHash().GetHash(),
					false,
					0);
			} else {
				SendPacket(response, host, port, false, NULL, false, 0);
			}

			break;
		}

		if (sender) {
			sender->CheckForAggressive();
			if (sender->IsBanned()) {
				// CheckForAggressive can call Ban() on score >= 10. Mirror the TCP
				// file-request path at ClientTCPSocket.cpp:539 and short-circuit,
				// so a freshly banned client cannot keep the seeder processing UDP
				// file-info packets.
				break;
			}

			// Make sure we are still thinking about the same file
			if (reqfilehash == sender->GetUploadFileID()) {
				sender->AddAskedCount();
				sender->SetUDPPort(port);
				sender->SetLastUpRequest();

				if (sender->GetUDPVersion() > 3) {
					sender->ProcessExtendedInfo(&data_in, reqfile);
				} else if (sender->GetUDPVersion() > 2) {
					// Legacy reask layout is file hash (16) + complete-source count (2).
					if (size < 18) {
						AddDebugLogLineN(logClientUDP,
							CFormat("Client UDP socket: rejected short "
								"OP_REASKFILEPING source count (%u bytes) "
								"from %s:%u") %
								static_cast<unsigned>(size) %
								Uint32toStringIP(host) %
								static_cast<unsigned>(port));
						break;
					}
					uint16 nCompleteCountLast = sender->GetUpCompleteSourcesCount();
					uint16 nCompleteCountNew = data_in.ReadUInt16();
					sender->SetUpCompleteSourcesCount(nCompleteCountNew);
					if (nCompleteCountLast != nCompleteCountNew) {
						reqfile->UpdatePartsInfo();
					}
				}

				CMemFile data_out(128);
				if (sender->GetUDPVersion() > 3) {
					reqfile->WritePartStatus(&data_out);
				}

				data_out.WriteUInt16(sender->GetUploadQueueWaitingPosition());
				CPacket *response = new CPacket(data_out, OP_EMULEPROT, OP_REASKACK);
				theStats::AddUpOverheadFileRequest(response->GetPacketSize());
				AddDebugLogLineN(logClientUDP,
					"Client UDP socket: OP_REASKACK to " + sender->GetFullIP());
				SendPacket(response,
					host,
					port,
					sender->ShouldReceiveCryptUDPPackets(),
					sender->GetUserHash().GetHash(),
					false,
					0);
			} else {
				AddDebugLogLineN(logClientUDP,
					"Client UDP socket; ReaskFilePing; reqfile does not match");
			}
		} else {
			if (!bSenderMultipleIpUnknown) {
				if ((theStats::GetWaitingUserCount() + 50) > thePrefs::GetQueueSize()) {
					CPacket *response = new CPacket(OP_QUEUEFULL, 0, OP_EMULEPROT);
					theStats::AddUpOverheadFileRequest(response->GetPacketSize());
					SendPacket(response,
						host,
						port,
						false,
						NULL,
						false,
						0); // we cannot answer this one encrypted since we dont know
						    // this client
				}
			} else {
				AddDebugLogLineN(logClientUDP,
					CFormat("UDP Packet received - multiple clients with the same IP but "
						"different UDP port found. Possible UDP Portmapping problem, "
						"enforcing TCP connection. IP: %s, Port: %u") %
						Uint32toStringIP(host) % port);
			}
		}
		break;
	}
	case OP_QUEUEFULL: {
		AddDebugLogLineN(logClientUDP, "Client UDP socket: OP_QUEUEFULL");
		theStats::AddDownOverheadOther(size);
		CUpDownClient *sender = theApp->downloadqueue->GetDownloadClientByIP_UDP(host, port);
		if (sender) {
			sender->SetRemoteQueueFull(true);
			sender->UDPReaskACK(0);
		}
		break;
	}
	case OP_REASKACK: {
		theStats::AddDownOverheadFileRequest(size);
		CUpDownClient *sender = theApp->downloadqueue->GetDownloadClientByIP_UDP(host, port);
		if (sender) {
			CMemFile data_in(packet, size);
			if (sender->GetUDPVersion() > 3) {
				sender->ProcessFileStatus(true, &data_in, sender->GetRequestFile());
			}
			if (data_in.GetAvailable() < 2) {
				AddDebugLogLineN(logClientUDP,
					CFormat("Client UDP socket: rejected OP_REASKACK without queue rank "
						"from %s:%u") %
						Uint32toStringIP(host) % static_cast<unsigned>(port));
				break;
			}
			uint16 nRank = data_in.ReadUInt16();
			sender->SetRemoteQueueFull(false);
			sender->UDPReaskACK(nRank);
			// UDP cannot carry the hashset request.
			if (sender->GetRequestFile() && sender->GetRequestFile()->IsHashSetNeeded()) {
				sender->AskAgainSoon();
			}
		}
		break;
	}
	case OP_FILENOTFOUND: {
		AddDebugLogLineN(logClientUDP, "Client UDP socket: OP_FILENOTFOUND");
		theStats::AddDownOverheadFileRequest(size);
		CUpDownClient *sender = theApp->downloadqueue->GetDownloadClientByIP_UDP(host, port);
#if defined(ENABLE_NATT_SERVER_COORDINATION) && defined(AMULE_UTP_TRANSPORT)
		// A target that read our 0xA0 as a reask for a zero file hash does not speak the
		// rendezvous. The source is not dead: leave it, and never ask it this way again. A known
		// source at exactly this endpoint is the target only with the attempt's hash: a target
		// that connected before keeps its address. Any other one answers its own reask.
		std::optional<NatRendezvous::KadRendezvousHash> senderHash;
		if (sender != nullptr) {
			senderHash.emplace();
			std::copy_n(sender->GetUserHash().GetHash(), senderHash->size(), senderHash->begin());
		}
		if (m_kadRendezvousRequests.NoRendezvous(host, port, senderHash)) {
			AddDebugLogLineN(logClientUDP,
				"No rendezvous at " + Uint32_16toStringIP_Port(host, port) +
					": it answered not found");
			++theApp->clientlist->GetBuddyStats().own.requestedRendezvous.noRendezvous;
			break;
		}
#endif
		if (sender) {
			sender->UDPReaskFNF(); // may delete 'sender'!
			sender = NULL;
		}
		break;
	}
	case OP_DIRECTCALLBACKREQ: {
		AddDebugLogLineN(logClientUDP, "Client UDP socket: OP_DIRECTCALLBACKREQ");
		theStats::AddDownOverheadOther(size);
		if (!theApp->clientlist->AllowCallbackRequest(host)) {
			AddDebugLogLineN(logClientUDP,
				"Ignored DirectCallback Request because this IP (" + Uint32toStringIP(host) +
					") has sent too many requests within a short time");
			break;
		}
		// do we accept callbackrequests at all?
		if (Kademlia::CKademlia::IsRunning() && Kademlia::CKademlia::IsFirewalled()) {
			theApp->clientlist->AddTrackCallbackRequests(host);
			// Short requests consume the same per-address cooldown as parser failures.
			// Layout is TCP port (2) + user hash (16) + connect options (1).
			if (size < 19) {
				AddDebugLogLineN(logClientUDP,
					CFormat("Client UDP socket: rejected short "
						"OP_DIRECTCALLBACKREQ (%u bytes) from %s:%u") %
						static_cast<unsigned>(size) % Uint32toStringIP(host) %
						static_cast<unsigned>(port));
				break;
			}
			CMemFile data(packet, size);
			uint16_t remoteTCPPort = data.ReadUInt16();
			CMD4Hash userHash(data.ReadHash());
			uint8_t connectOptions = data.ReadUInt8();
			CUpDownClient *requester = NULL;
			CClientList::SourceList clients = theApp->clientlist->GetClientsByHash(userHash);
			for (CClientList::SourceList::iterator it = clients.begin(); it != clients.end();
				++it) {
				if ((host == 0 || it->GetIP() == host) &&
					(remoteTCPPort == 0 || it->GetUserPort() == remoteTCPPort)) {
					requester = it->GetClient();
					break;
				}
			}
			if (requester == NULL) {
				requester = new CUpDownClient(
					remoteTCPPort, HybridIDOfIPv4(host), 0, 0, nullptr, false, true);
				requester->SetUserHash(CMD4Hash(userHash));
				theApp->clientlist->AddClient(requester);
			}
			requester->SetConnectOptions(connectOptions, true, false);
			requester->SetDirectUDPCallbackSupport(false);
			requester->SetIP(host);
			requester->SetUserPort(remoteTCPPort);
			AddDebugLogLineN(logClientUDP,
				"Accepting incoming DirectCallback Request from " + Uint32toStringIP(host));
			requester->NoteCallbackAnswer(EConnectionOpener::AnswerDirectCallback);
			requester->TryToConnect();
		} else {
			AddDebugLogLineN(logClientUDP,
				"Ignored DirectCallback Request because we do not accept Direct Callbacks at "
				"all (" +
					Uint32toStringIP(host) + ")");
		}
		break;
	}
	default:
		theStats::AddDownOverheadOther(size);
	}
}
// File_checked_for_headers
