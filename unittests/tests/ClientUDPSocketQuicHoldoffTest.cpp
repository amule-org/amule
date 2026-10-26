//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// Any parts of this program contributed by third-party developers are copyrighted
// by their respective authors.
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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
//

#include <muleunit/test.h>
#include <amule.h>
#include <ClientTCPSocket.h>
#include <ClientUDPSocket.h>
#include <QuicSocketTransport.h>
#include <LibSocket.h>
#include <Preferences.h>
#include <Statistics.h>
#include <StatTree.h>
#include <ListenSocket.h>
#include <updownclient.h>
#include <DownloadQueue.h>
#include <UploadQueue.h>
#include <ClientList.h>
#include <BrowseManager.h>
#include <SharedFileList.h>
#include <ServerList.h>
#include <Server.h>
#include <IPFilter.h>
#include <GuiEvents.h>
#include <kademlia/kademlia/Kademlia.h>
#include <kademlia/kademlia/Prefs.h>
#include <kademlia/net/KademliaUDPListener.h>
#include <LugdunumNatTraversal.h>
#include <UploadDiskIOThread.h>
#include <ServerConnect.h>
#include <PartFile.h>
#include <KnownFile.h>
#include <StateMachine.h>
#include <Packet.h>
#include <ClientRef.h>
#include <Proxy.h>
#include <EncryptedDatagramSocket.h>
#include <NetworkFunctions.h>
#include <UploadBandwidthThrottler.h>

using namespace muleunit;
DECLARE_SIMPLE(ClientUDPSocketQuicHoldoff)

void QuicClientRefTestForget(CUpDownClient *client);

// This target links the real production UDP socket stack (ClientUDPSocket.cpp and everything
// CClientTCPSocketTest.cpp already needed for the sibling TCP socket, since CClientUDPSocket's
// constructor builds a QUIC outbound-handoff callback that references CClientTCPSocket's own
// constructor even though this test never triggers it) to drive ServiceQuicCapsHoldoffs() itself,
// not only the pure DecideQuicCapsHoldoff() helper QuicCapsHoldoffTest.cpp already covers.
// --gc-sections discards the application-wide code this test's call graph never reaches; the
// definitions below exist only so the never-exercised code compiled into CClientUDPSocket's and
// CClientTCPSocket's vtables and lambdas still links, matching ClientTCPSocketTest.cpp's pattern.

CamuleDaemonApp *theApp = nullptr;

bool CamuleDaemonApp::OnInit()
{
	return false;
}
int CamuleDaemonApp::OnRun()
{
	return 0;
}
int CamuleDaemonApp::OnExit()
{
	return 0;
}
int CamuleDaemonApp::InitGui(bool, wxString &)
{
	return 0;
}
bool CamuleDaemonApp::Initialize(int &, wxChar **)
{
	return false;
}
int CamuleDaemonApp::ShowAlert(wxString, wxString, int)
{
	return 0;
}
wxBEGIN_EVENT_TABLE(CamuleDaemonApp, CamuleApp)
wxEND_EVENT_TABLE()
CamuleDaemonApp::CamuleDaemonApp() = default;

// This test controls elapsed time directly (ServiceQuicCapsHoldoffs() takes nowMs explicitly),
// so the real wall clock is never linked; TryQuicDial()'s own internal ::GetTickCount64() calls
// read this instead, keeping holdoff timestamps on the same clock the test advances.
namespace
{
uint64_t g_now = 1000000;
}
uint64 GetTickCount64()
{
	return g_now;
}

// CPreferences statics the linked socket chain reads; this test never configures a real prefs
// store, so each stays at its zero-value default except where a TEST() below opts in.
wxString CPreferences::s_Addr;
CMD4Hash CPreferences::s_userhash;
uint8 CPreferences::s_byCryptTCPPaddingLength;
bool CPreferences::s_IsClientCryptLayerSupported;
bool CPreferences::s_IsClientCryptLayerRequired;
bool CPreferences::s_bCryptLayerRequested;
uint16 CPreferences::s_maxconnections;
uint16 CPreferences::s_maxsourceperfile;
bool CPreferences::s_paranoidfilter;
uint8 CPreferences::s_iSeeShares;
uint16 CPreferences::s_MaxConperFive;
uint8 CPreferences::s_iQueueSize;
wxString CPreferences::s_configDir;
uint8 CPreferences::s_statsAverageMinutes;
bool CPreferences::s_UDPEnable;
// Set at definition, not by later assignment: protected, unlike the private statics above.
bool CPreferences::s_QuicNatTEnabled = true;
CProxyData CPreferences::s_ProxyData;
bool CPreferences::s_filterLanIP;
bool CPreferences::s_LowIdHolePunchEnabled;
uint16 CPreferences::s_udpport;
bool CPreferences::s_LugdunumNatCallbackEnabled;

namespace
{
void EnsureStatsTree()
{
	static CStatistics statistics;
	(void)statistics;
}
} // namespace

// A narrow, non-threading stand-in: the real class's constructor starts a live wxThread, and
// theApp->uploadBandwidthThrottler is dereferenced unconditionally by CMuleUDPSocket::SendPacket()
// (TryQuicDial()'s own SendQuicCapsRequest() reaches it), so it must be non-null, but nothing in
// this test needs real upload scheduling.
UploadBandwidthThrottler::UploadBandwidthThrottler()
: wxThread(wxTHREAD_JOINABLE)
, m_newDataCondition(m_newDataMutex)
{
	m_SentBytesSinceLastCall = 0;
	m_SentBytesSinceLastCallOverhead = 0;
}
UploadBandwidthThrottler::~UploadBandwidthThrottler() {}
void UploadBandwidthThrottler::QueueForSendingControlPacket(ThrottledControlSocket *, bool) {}
void UploadBandwidthThrottler::RemoveFromAllQueues(ThrottledControlSocket *) {}
void UploadBandwidthThrottler::RemoveFromAllQueues(ThrottledFileSocket *) {}
void UploadBandwidthThrottler::EndThread() {}
void UploadBandwidthThrottler::NewUploadDataAvailable() {}
uint64 UploadBandwidthThrottler::GetNumberOfSentBytesSinceLastCallAndReset()
{
	return 0;
}
uint64 UploadBandwidthThrottler::GetNumberOfSentBytesOverheadSinceLastCallAndReset()
{
	return 0;
}
void UploadBandwidthThrottler::AddToStandardList(uint32, ThrottledFileSocket *) {}
bool UploadBandwidthThrottler::RemoveFromStandardList(ThrottledFileSocket *)
{
	return false;
}
void *UploadBandwidthThrottler::Entry()
{
	return nullptr;
}

// --- Application-boundary stand-ins (never invoked at runtime; see the block comment above) ---

Kademlia::CKademlia *Kademlia::CKademlia::instance = nullptr;
time_t Kademlia::CKademlia::m_bootstrap = 0;
bool Kademlia::CKademlia::m_running = false;
void Kademlia::CKademliaUDPListener::Bootstrap(uint32_t, uint16_t, uint8_t, const Kademlia::CUInt128 *) {}
void Kademlia::CPrefs::IncFirewalled() {}
bool Kademlia::CKademlia::IsRunningInLANMode()
{
	return false;
}
void Kademlia::CKademlia::ProcessPacket(
	const uint8_t *, uint32_t, uint32_t, uint16_t, bool, const Kademlia::CKadUDPKey &)
{
}
uint32_t Kademlia::CPrefs::GetUDPVerifyKey(uint32_t)
{
	return 0;
}
// The real options byte this would compute depends on Kademlia/firewall/server state no test in
// this file exercises; ServiceQuicCapsHoldoffs()'s own behavior never reads it back.
uint8_t Kademlia::CPrefs::GetMyConnectOptions(bool, bool)
{
	return 0;
}

CUpDownClient::CUpDownClient(uint16, uint32, uint32, uint16, CPartFile *, bool, bool)
: CECID()
{
}
CUpDownClient::CUpDownClient(CClientTCPSocket *)
: CECID()
{
}
bool CUpDownClient::SendPacket(CPacket *packet, bool, bool)
{
	delete packet;
	return false;
}
void CUpDownClient::ProcessBlockPacket(const unsigned char *, unsigned int, bool, bool) {}
void CUpDownClient::SetDownloadState(uint8) {}
void CUpDownClient::SendSignaturePacket() {}
void CUpDownClient::InfoPacketsReceived() {}
bool CUpDownClient::IsBanned() const
{
	return false;
}
EUtf8Str CUpDownClient::GetUnicodeSupport() const
{
	return utf8strNone;
}
void CUpDownClient::SendSharedFilesOfDirectory(const wxString &) {}
void CUpDownClient::ProcessChatMessage(wxString) {}
int CUpDownClient::GetHashType() const
{
	return 0;
}
void CUpDownClient::SendHelloAnswer() {}
void CUpDownClient::ConnectionEstablished() {}
void CUpDownClient::SetUserIDHybrid(uint32) {}
void CUpDownClient::SetWaitStartTime() {}
void CUpDownClient::SendSharedDirectories() {}
void CUpDownClient::SendMuleInfoPacket(bool, bool) {}
void CUpDownClient::Safe_Delete() {}
void CUpDownClient::SendHashsetPacket(const CMD4Hash &) {}
void CUpDownClient::SendBlockRequests() {}
void CUpDownClient::ProcessRequestPartsPacket(const unsigned char *, unsigned int, bool) {}
void CUpDownClient::ProcessSharedFileList(const unsigned char *, unsigned int, wxString &) {}
void CUpDownClient::ProcessFileStatus(bool, const CMemFile *, const CPartFile *) {}
uint64 CUpDownClient::GetWaitStartTime() const
{
	return 0;
}
void CUpDownClient::SetUploadFileID(CKnownFile *) {}
bool CUpDownClient::ProcessHelloPacket(const uint8_t *, uint32)
{
	return false;
}
bool CUpDownClient::ShouldReceiveCryptUDPPackets() const
{
	return false;
}
void CUpDownClient::ProcessExtendedInfo(const CMemFile *, CKnownFile *) {}
void CUpDownClient::ProcessPublicIPAnswer(const uint8_t *, uint32) {}
bool CUpDownClient::CheckHandshakeFinished() const
{
	return false;
}
void CUpDownClient::ProcessSecIdentStatePacket(const uint8_t *, uint32) {}
uint8 CUpDownClient::GetSecureIdentState()
{
	return 0;
}
void CUpDownClient::SendPublicKeyPacket() {}
bool CUpDownClient::ProcessMuleInfoPacket(const uint8_t *, uint32)
{
	return false;
}
void CUpDownClient::ProcessMuleCommentPacket(const uint8_t *, uint32) {}
void CUpDownClient::ProcessPublicKeyPacket(const uint8_t *, uint32) {}
void CUpDownClient::ProcessSignaturePacket(const uint8_t *, uint32) {}
void CUpDownClient::CheckForAggressive() {}
void CUpDownClient::ProcessAICHRequest(const uint8_t *, uint32) {}
bool CUpDownClient::TryToConnect(bool)
{
	return false;
}
void CUpDownClient::ProcessAICHAnswer(const uint8_t *, uint32) {}
void CUpDownClient::ProcessAICHFileHash(CMemFile *, const CPartFile *) {}
void CUpDownClient::ProcessFileInfo(const CMemFile *, const CPartFile *) {}
void CUpDownClient::ProcessHashSet(const uint8_t *, uint32) {}
void CUpDownClient::SendCommentInfo(CKnownFile *) {}
void CUpDownClient::SetRemoteQueueRank(uint16) {}
bool CUpDownClient::ProcessHelloAnswer(const uint8_t *, uint32)
{
	return false;
}
bool CUpDownClient::SwapToAnotherFile(bool, bool, bool, CPartFile *)
{
	return false;
}
void CUpDownClient::SetSocket(CClientTCPSocket *) {}
bool CUpDownClient::SendHelloPacket()
{
	return false;
}
bool CUpDownClient::IsRedialAllowed() const
{
	return false;
}
bool CUpDownClient::Connect()
{
	return false;
}
bool CUpDownClient::Disconnected(const wxString &, bool)
{
	return false;
}
void CUpDownClient::ProcessFirewallCheckUDPRequest(CMemFile *) {}
void CUpDownClient::ProcessCaptchaReqRes(uint8) {}
void CUpDownClient::ProcessCaptchaRequest(CMemFile *) {}
void CUpDownClient::TrySendLugdunumNatCallbackRequest(uint32) const {}
bool CUpDownClient::ReplaceSocket(CClientTCPSocket *)
{
	return true;
}
void CUpDownClient::UDPReaskFNF() {}
void CUpDownClient::UDPReaskACK(uint16) {}
void CUpDownClient::SetConnectOptions(uint8_t, bool, bool) {}
void CUpDownClient::SetIP(uint32) {}
void CUpDownClient::SetUserAddress(const CNetworkAddress &) {}
void CUpDownClient::SetUserHash(const CMD4Hash &) {}
bool CUpDownClient::TryServerVouchedLowIdUtpDial()
{
	return false;
}
// The one CUpDownClient method this test's own assertions observe: recorded, not just a no-op.
namespace
{
unsigned g_quicDialFailedCalls = 0;
const CUpDownClient *g_quicDialFailedOwner = nullptr;
} // namespace
void CUpDownClient::QuicDialFailed()
{
	++g_quicDialFailedCalls;
	g_quicDialFailedOwner = this;
}

bool CClientList::IsBannedClient(const CNetworkAddress &)
{
	return false;
}
// CClientUDPSocket's own Lugdunum methods are real, defined in the real ClientUDPSocket.cpp this
// test links -- unlike CClientTCPSocket's, which stays a narrow stand-in (see below).
std::unique_ptr<IStreamTransport> CLibSocket::DetachTransport()
{
	return nullptr;
}

CPartFile *CDownloadQueue::GetFileByID(const CMD4Hash &) const
{
	return nullptr;
}
bool CDownloadQueue::RemoveSource(CUpDownClient *, bool, bool)
{
	return false;
}
void CDownloadQueue::CheckAndAddKnownSource(CPartFile *, CUpDownClient *) {}
CUpDownClient *CDownloadQueue::GetDownloadClientByIP_UDP(uint32, uint16) const
{
	return nullptr;
}
bool CUploadQueue::RemoveFromUploadQueue(CUpDownClient *)
{
	return false;
}
void CUploadQueue::AddClientToQueue(CUpDownClient *) {}
CUpDownClient *CUploadQueue::GetWaitingClientByIP_UDP(const CNetworkAddress &, uint16_t, bool, bool *)
{
	return nullptr;
}
void CClientList::AddClient(CUpDownClient *) {}
bool CClientList::AttachToAlreadyKnown(CUpDownClient **, CClientTCPSocket *, bool *, bool)
{
	return false;
}
bool CClientList::IsKadFirewallCheckIP(uint32) const
{
	return false;
}
bool CClientList::TakeKadFirewallReply(uint32, Kademlia::CFirewallCheckRequests::Reply)
{
	return false;
}
CUpDownClient *CClientList::FindClientByIP(uint32, uint16)
{
	return nullptr;
}
bool CClientList::AllowCallbackRequest(uint32) const
{
	return false;
}
void CClientList::AddTrackCallbackRequests(uint32_t) {}
CClientList::SourceList CClientList::GetClientsByHash(const CMD4Hash &)
{
	return CClientList::SourceList();
}
void CBrowseManager::Fail(CUpDownClient *) {}
bool CBrowseManager::AwaitingDirectoryList(const CUpDownClient *) const
{
	return false;
}
void CBrowseManager::OnDirectoryList(CUpDownClient *, int, unsigned long) {}
std::uint32_t CBrowseManager::SearchIdFor(const CUpDownClient *) const
{
	return 0;
}
CKnownFile *CSharedFileList::GetFileByID(const CMD4Hash &)
{
	return nullptr;
}
void CSharedFileList::CopyFileList(std::vector<CKnownFile *> &) const {}
CServer *CServerList::GetServerByIP(uint32) const
{
	return nullptr;
}
CServer *CServerList::GetServerByIPUDP(uint32, uint16, bool) const
{
	return nullptr;
}
CServer *CServerList::GetServerByAddress(const wxString &, uint16) const
{
	return nullptr;
}
uint32 CServer::GetServerKeyUDP(bool) const
{
	return 0;
}
CServer *CServerConnect::GetCurrentServer()
{
	return nullptr;
}
bool CIPFilter::IsFiltered(const CNetworkAddress &, bool)
{
	return false;
}
bool CIPFilter::IsFiltered(uint32, bool)
{
	return false;
}
void CPartFile::SetLastAnsweredTime() {}
void CPartFile::AddClientSources(CMemFile *, uint32, uint8, bool, const CUpDownClient *) {}
void CPartFile::AddDeadSource(const CUpDownClient *) {}
void CKnownFile::CreateOfferedFilePacket(CMemFile *, CServer *, CUpDownClient *) {}
void CAICHHash::Write(CFileDataIO *) const {}

CStateMachine::CStateMachine(const wxString &, unsigned int maxStates, t_sm_state)
: m_maxStates(maxStates)
{
}
CStateMachine::~CStateMachine() {}
void CStateMachine::Clock() {}
void CStateMachine::Schedule(t_sm_event) {}

namespace MuleNotify
{
void HandleNotification(const CMuleNotiferBase &) {}
void HandleNotificationAlways(const CMuleNotiferBase &) {}
void SharedCtrlRefreshClient(uint32, SourceItemType) {}
} // namespace MuleNotify

void CUploadDiskIOThread::SocketNeedsMoreData() {}
bool CServerConnect::AwaitingTestFromIP(uint32)
{
	return false;
}

bool StringIPtoUint32(const wxString &, uint32 &)
{
	return false;
}
wxString EncodeBase64(const char *, unsigned int)
{
	return wxEmptyString;
}
bool IsGoodIP(uint32, bool) noexcept
{
	return false;
}

// CClientTCPSocket itself: never actually constructed at runtime in this test (the QUIC
// outbound-handoff lambda that references it is only invoked once a dialled connection's EAQN1
// proof exchange completes, which this test never drives), but CClientUDPSocket's constructor
// unconditionally builds that lambda as a std::function, so its body -- including this
// constructor call -- must still link.
CClientTCPSocket::CClientTCPSocket(CUpDownClient *, const CProxyData *) {}
CClientTCPSocket::~CClientTCPSocket() {}
void CClientTCPSocket::OnConnect(int) {}
void CClientTCPSocket::OnSend(int) {}
void CClientTCPSocket::OnReceive(int) {}
void CClientTCPSocket::OnClose(int) {}
void CClientTCPSocket::OnError(int) {}
void CClientTCPSocket::SendPacket(CPacket *packet, bool, bool, uint32)
{
	delete packet;
}
SocketSentBytes CClientTCPSocket::SendControlData(uint32, uint32)
{
	return SocketSentBytes{};
}
SocketSentBytes CClientTCPSocket::SendFileAndControlData(uint32, uint32)
{
	return SocketSentBytes{};
}
bool CClientTCPSocket::IsDownloadThrottled() const
{
	return false;
}
bool CClientTCPSocket::PacketReceived(CPacket *)
{
	return false;
}
void CClientTCPSocket::Safe_Delete() {}
void CClientTCPSocket::Safe_Delete_Client() {}
bool CClientTCPSocket::InitNetworkData(AdmissionTransport)
{
	return false;
}

namespace
{
CListenSocket *MakeLoopbackListenSocket()
{
	amuleIPV4Address addr;
	addr.Hostname(wxT("127.0.0.1"));
	addr.Service(0);
	return new CListenSocket(addr, nullptr);
}
} // namespace

// The real libutp-backed uTP acceptor and the QUIC stream acceptor: both admit a transport only
// once a peer's stream actually opens, which this test (no real wire traffic) never reaches.
#include <UtpStreamAcceptor.h>
#include <QuicStreamAcceptor.h>
SUtpIncomingSynDecision CUtpStreamAcceptor::DecideIncomingSyn(
	uint32_t, uint16_t, const SUtpSynCandidateInfo &)
{
	return SUtpIncomingSynDecision{};
}
bool CUtpStreamAcceptor::AcceptStream(std::unique_ptr<IStreamTransport> &, uint32_t, uint16_t)
{
	return false;
}
bool CUtpStreamAcceptor::AcceptStream(
	std::unique_ptr<IStreamTransport> &, uint32_t, uint16_t, const SUtpIncomingSynDecision &)
{
	return false;
}
bool CQuicStreamAcceptor::AcceptStream(std::unique_ptr<IStreamTransport> &, const CNetworkAddress &, uint16_t)
{
	return false;
}

// --- The fixture: a real CClientUDPSocket, driven through ServiceQuicCapsHoldoffs() directly ---

// Grants access to ServiceQuicCapsHoldoffs(), m_quicCapsHoldoffs, m_capabilityCache and m_quic:
// the integration this test covers that QuicCapsHoldoffTest.cpp's DecideQuicCapsHoldoff() unit
// test cannot, since those are private members tightly coupled to the real socket object.
class ClientUDPSocketQuicHoldoffTestFixture
{
public:
	ClientUDPSocketQuicHoldoffTestFixture()
	{
		wxAppConsole::SetInstance(m_appGuard.original);
		EnsureStatsTree();
		theApp = &m_app;
		theApp->m_AsioService = new CAsioService();
		theApp->listensocket = MakeLoopbackListenSocket();
		theApp->uploadBandwidthThrottler = new UploadBandwidthThrottler();
		CPreferences::SetUDPDisable(false); // Ok() (real bound socket) gates the QUIC-dial branch.

		amuleIPV4Address addr;
		addr.Hostname(wxT("127.0.0.1"));
		addr.Service(0);
		m_socket = new CClientUDPSocket(addr);
	}
	~ClientUDPSocketQuicHoldoffTestFixture()
	{
		delete m_socket;
		delete theApp->uploadBandwidthThrottler;
		delete theApp->listensocket;
		theApp->m_AsioService->Stop();
		delete theApp->m_AsioService;
		theApp = nullptr;
	}

	CClientUDPSocket &Socket() { return *m_socket; }

	bool TryQuicDial(const std::array<uint8_t, 16> &peer,
		uint32_t ipv4,
		uint16_t udpPort,
		const std::array<uint8_t, 16> &fileHash,
		CUpDownClient *owner)
	{
		return m_socket->TryQuicDial(peer, ipv4, udpPort, fileHash, owner);
	}
	void ServiceQuicCapsHoldoffs(uint64_t nowMs) { m_socket->ServiceQuicCapsHoldoffs(nowMs); }
	std::size_t HoldoffCount() const { return m_socket->m_quicCapsHoldoffs.size(); }
	void RememberCompatible(const std::array<uint8_t, 16> &peer, uint64_t nowMs)
	{
		m_socket->m_capabilityCache.Remember(peer, QuicNatt::EAQC_CAP_QUIC, nowMs, 29 * 60 * 1000);
	}

private:
	struct AppInstanceGuard
	{
		wxAppConsole *original = wxAppConsole::GetInstance();
		~AppInstanceGuard() { wxAppConsole::SetInstance(original); }
	} m_appGuard;
	CamuleDaemonApp m_app;
	CClientUDPSocket *m_socket = nullptr;
};

namespace
{
const std::array<uint8_t, 16> kPeer{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
const std::array<uint8_t, 16> kFileHash{ 0 };
constexpr uint32_t kPeerIPv4 = 0x0100007f; // 127.0.0.1, network order
constexpr uint16_t kPeerPort = 12345;
} // namespace

TEST(ClientUDPSocketQuicHoldoff, AckArrivingResolvesToQuicDialAndRemovesTheHoldoff)
{
	ClientUDPSocketQuicHoldoffTestFixture fixture;
	g_quicDialFailedCalls = 0;

	// CClientRef needs the real owner to be reachable through the shim's map; the dial itself
	// never dereferences owner before the handoff this test does not drive (see the comment on
	// the QUIC outbound-handoff lambda above), so an opaque identity pointer is enough here too.
	auto *owner = reinterpret_cast<CUpDownClient *>(static_cast<uintptr_t>(1));
	ASSERT_TRUE(fixture.TryQuicDial(kPeer, kPeerIPv4, kPeerPort, kFileHash, owner));
	ASSERT_EQUALS(static_cast<std::size_t>(1), fixture.HoldoffCount());

	// The real CAPS_ACK handler's own effect on capability state: QuicCapabilityCache::Remember()
	// with the QUIC bit set, exactly what ClientUDPSocket.cpp's OP_NATT_FRAME_CAPS_ACK case calls.
	fixture.RememberCompatible(kPeer, g_now);
	fixture.ServiceQuicCapsHoldoffs(g_now);

	ASSERT_EQUALS(static_cast<std::size_t>(0), fixture.HoldoffCount());
	ASSERT_EQUALS(0u, g_quicDialFailedCalls);
	QuicClientRefTestForget(owner);
}

TEST(ClientUDPSocketQuicHoldoff, DeadlineWithNoAckResolvesViaQuicDialFailedAndRemovesTheHoldoff)
{
	ClientUDPSocketQuicHoldoffTestFixture fixture;
	g_quicDialFailedCalls = 0;
	g_quicDialFailedOwner = nullptr;

	auto *owner = reinterpret_cast<CUpDownClient *>(static_cast<uintptr_t>(2));
	ASSERT_TRUE(fixture.TryQuicDial(kPeer, kPeerIPv4, kPeerPort, kFileHash, owner));
	ASSERT_EQUALS(static_cast<std::size_t>(1), fixture.HoldoffCount());

	// Past kQuicCapsHoldoffDeadlineMs with the cache still Unknown: DecideQuicCapsHoldoff()
	// resolves uTP (QuicCapsHoldoffTest.cpp proves this threshold in isolation).
	fixture.ServiceQuicCapsHoldoffs(g_now + 1500);

	ASSERT_EQUALS(static_cast<std::size_t>(0), fixture.HoldoffCount());
	ASSERT_EQUALS(1u, g_quicDialFailedCalls);
	ASSERT_TRUE(g_quicDialFailedOwner == owner);
	QuicClientRefTestForget(owner);
}

TEST(ClientUDPSocketQuicHoldoff, OwnerDeletedWhileHoldoffIsPendingDropsItSilently)
{
	ClientUDPSocketQuicHoldoffTestFixture fixture;
	g_quicDialFailedCalls = 0;

	auto *owner = reinterpret_cast<CUpDownClient *>(static_cast<uintptr_t>(3));
	ASSERT_TRUE(fixture.TryQuicDial(kPeer, kPeerIPv4, kPeerPort, kFileHash, owner));
	ASSERT_EQUALS(static_cast<std::size_t>(1), fixture.HoldoffCount());

	// Simulates the owner CUpDownClient being deleted: CClientRef::GetClientChecked() returns
	// null, matching a real dead reference, without ever constructing a real CUpDownClient.
	QuicClientRefTestForget(owner);

	fixture.ServiceQuicCapsHoldoffs(g_now);

	ASSERT_EQUALS(static_cast<std::size_t>(0), fixture.HoldoffCount());
	// No call through the dead CClientRef: QuicDialFailed() is never reached for a forgotten owner.
	ASSERT_EQUALS(0u, g_quicDialFailedCalls);
}
