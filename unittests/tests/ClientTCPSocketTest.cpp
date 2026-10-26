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
#include <IPFilter.h>
#include <GuiEvents.h>
#include <kademlia/kademlia/Kademlia.h>
#include <kademlia/net/KademliaUDPListener.h>
#include <LugdunumNatTraversal.h>
#include <ClientUDPSocket.h>
#include <UploadDiskIOThread.h>
#include <ServerConnect.h>
#include <PartFile.h>
#include <KnownFile.h>
#include <StateMachine.h>
#include <Packet.h>

using namespace muleunit;
DECLARE_SIMPLE(ClientTCPSocket)

// This target links the real production socket stack (ClientTCPSocket.cpp, EMSocket.cpp,
// EncryptedStreamSocket.cpp, Proxy.cpp, LibSocketAsio.cpp, ListenSocket.cpp) so the test drives
// ApplyUtpCryptParameters() through an actually-constructed CClientTCPSocket, not a reimplemented
// stand-in. --gc-sections (set on this target in CMakeLists.txt) discards the application-wide
// code this test's own call graph never reaches. CClientTCPSocket is polymorphic, though, and its
// virtual overrides (PacketReceived, ProcessPacket, ...) are pinned alive by its vtable even though
// this test never calls them through a live connection -- so every production collaborator they
// reference still needs a symbol. None of the definitions below are ever actually invoked; they
// exist only so the never-exercised code compiled into those methods still links, matching this
// codebase's existing narrow-application-boundary-stub pattern (see ApplicationTestStubs.cpp).

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

// CPreferences statics the linked socket chain reads; this test never configures a real prefs
// store, so each stays at its zero-value default.
wxString CPreferences::s_Addr;
CMD4Hash CPreferences::s_userhash;
uint8 CPreferences::s_byCryptTCPPaddingLength;
bool CPreferences::s_IsClientCryptLayerSupported;
bool CPreferences::s_IsClientCryptLayerRequired;
bool CPreferences::s_bCryptLayerRequested;
bool CPreferences::s_bVerbose;

// CListenSocket::AddSocket()/RemoveSocket() bump the real stats tree CStatistics builds in its
// own constructor; this test links the real Statistics.cpp/StatTree.cpp and constructs one real
// CStatistics rather than hand-wiring each counter node it declares. A function-local static,
// not a namespace-scope global: wx's own globals are not ready before wxEntry() runs it.
namespace
{
void EnsureStatsTree()
{
	static CStatistics statistics;
	(void)statistics;
}

} // namespace

// --- Application-boundary stand-ins (never invoked; see the block comment above) ---

Kademlia::CKademlia *Kademlia::CKademlia::instance = nullptr;
time_t Kademlia::CKademlia::m_bootstrap = 0;
void Kademlia::CKademliaUDPListener::Bootstrap(uint32_t, uint16_t, uint8_t, const Kademlia::CUInt128 *) {}

CUpDownClient::CUpDownClient(uint16, uint32, uint32, uint16, CPartFile *, bool, bool)
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
// CPreferences statics the deep (dead-code) application graph above reads.
uint16 CPreferences::s_maxconnections;
uint16 CPreferences::s_maxsourceperfile;
bool CPreferences::s_paranoidfilter;
uint8 CPreferences::s_iSeeShares;
uint16 CPreferences::s_MaxConperFive;
uint8 CPreferences::s_iQueueSize;
wxString CPreferences::s_configDir;
uint8 CPreferences::s_statsAverageMinutes;

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
CNetworkAddress CUpDownClient::GetDialAddress() const
{
	return CNetworkAddress();
}
#ifdef ENABLE_IPV6
void CUpDownClient::MarkIPv6ChangeConnectionReady() {}
void CUpDownClient::UpdateModIPv6(const CNetworkAddress &) {}
#endif
bool CUpDownClient::Disconnected(const wxString &, bool)
{
	return false;
}
void CUpDownClient::ProcessFirewallCheckUDPRequest(CMemFile *) {}
void CUpDownClient::ProcessCaptchaReqRes(uint8) {}
void CUpDownClient::ProcessCaptchaRequest(CMemFile *) {}
#ifdef ENABLE_NATT_SERVER_COORDINATION
void CUpDownClient::TrySendLugdunumNatCallbackRequest(uint32) const {}
#endif
bool CClientList::IsBannedClient(const CNetworkAddress &)
{
	return false;
}
#ifdef ENABLE_NATT_SERVER_COORDINATION
void CClientUDPSocket::RecordLugdunumNatCallbackFailure(const LugdunumNatTraversal::CallbackFailureKey &) {}
bool CClientUDPSocket::IsLugdunumCallbackRendezvousActive(
	const LugdunumNatTraversal::CallbackFailureKey &) const
{
	return false;
}
#endif
std::unique_ptr<IStreamTransport> CLibSocket::DetachTransport()
{
	return nullptr;
}
CUpDownClient::CUpDownClient(CClientTCPSocket *)
: CECID()
{
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
bool CUploadQueue::RemoveFromUploadQueue(CUpDownClient *)
{
	return false;
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
void CBrowseManager::Fail(CUpDownClient *) {}
bool CBrowseManager::AwaitingDirectoryList(const CUpDownClient *) const
{
	return false;
}
void CBrowseManager::OnDirectoryList(CUpDownClient *, int, std::uint64_t) {}
CKnownFile *CSharedFileList::GetFileByID(const CMD4Hash &)
{
	return nullptr;
}
CServer *CServerList::GetServerByIP(uint32) const
{
	return nullptr;
}
bool CIPFilter::IsFiltered(const CNetworkAddress &, bool)
{
	return false;
}
#ifdef ENABLE_NATT_SERVER_COORDINATION
void CClientUDPSocket::ClearLugdunumNatCallbackFailure(const LugdunumNatTraversal::CallbackFailureKey &) {}
void CClientUDPSocket::CompleteLugdunumCallbackRendezvous(const LugdunumNatTraversal::CallbackFailureKey &) {}
#endif
void CMuleUDPSocket::SendPacket(CPacket *packet, uint32, uint16, bool, const uint8 *, bool, uint32)
{
	delete packet;
}

void CUploadQueue::AddClientToQueue(CUpDownClient *) {}
CUpDownClient *CUploadQueue::GetWaitingClientByIP_UDP(const CNetworkAddress &, uint16_t, bool, bool *)
{
	return nullptr;
}
bool CClientList::TakeKadFirewallReply(uint32, Kademlia::CFirewallCheckRequests::Reply)
{
	return false;
}
CUpDownClient *CClientList::FindClientByIP(uint32, uint16)
{
	return nullptr;
}
void CPartFile::SetLastAnsweredTime() {}
void CPartFile::AddClientSources(CMemFile *, uint32, uint8, bool, const CUpDownClient *) {}
void CPartFile::AddDeadSource(const CUpDownClient *) {}
void CKnownFile::CreateOfferedFilePacket(CMemFile *, CServer *, CUpDownClient *) {}
void CSharedFileList::CopyFileList(std::vector<CKnownFile *> &) const {}
std::uint32_t CBrowseManager::SearchIdFor(const CUpDownClient *) const
{
	return 0;
}
bool Kademlia::CKademlia::m_running = false;
void Kademlia::CPrefs::IncFirewalled() {}
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

namespace
{
// A stream-operations double is enough to build a real CQuicSocketTransport: this test only
// needs CClientTCPSocket to see a genuine CQuicSocketTransport through IStreamTransport, not a
// working QUIC connection underneath it.
struct NullStreamOperations : IQuicStreamOperations
{
	std::ptrdiff_t WriteStream(Handle, const uint8_t *, size_t, uint64_t) override { return 0; }
	void CloseStream(Handle, uint64_t) override {}
	void AbortStream(Handle) override {}
	void ExtendReadWindow(Handle, size_t) override {}
};

CListenSocket *MakeLoopbackListenSocket()
{
	amuleIPV4Address addr;
	addr.Hostname(wxT("127.0.0.1"));
	addr.Service(0);
	return new CListenSocket(addr, nullptr);
}
} // namespace

// CClientTCPSocket::AddSocket()/RemoveSocket() go through theApp->listensocket unconditionally;
// this fixture gives every test a real (loopback-bound) one, matching production wiring, since
// CListenSocket has no application-free construction path to stub around.
class ClientTCPSocketTestFixture
{
public:
	ClientTCPSocketTestFixture()
	{
		// wxAppConsole's constructor/destructor changes the global app instance (CamuleDaemonApp
		// is one under AMULE_DAEMON, same as muleunit's own runner); AppInstanceGuard restores the
		// unit runner once m_app is destroyed, matching KadUDPFirewallIntegrationTest.cpp.
		wxAppConsole::SetInstance(m_appGuard.original);
		EnsureStatsTree();
		theApp = &m_app;
		// Every real CLibSocket (CClientTCPSocket's base) owns an ASIO socket that production code
		// always closes through a live CAsioService thread pool; without one, tearing a real
		// socket down hits wx threading primitives that were never started.
		theApp->m_AsioService = new CAsioService();
		theApp->listensocket = MakeLoopbackListenSocket();
	}
	~ClientTCPSocketTestFixture()
	{
		// CListenSocket's constructor posts a real async_accept() to the shared ASIO io_context;
		// deleted here, while that service is still running, so the pending operation cancels
		// cleanly instead of being torn down later by global io_context destruction.
		delete theApp->listensocket;
		// Stopped and joined here, before wxEntry()'s own shutdown tears down wx's threading
		// subsystem: a CAsioService destructed later (e.g. at process-exit static teardown)
		// joins a wxThread after that subsystem is already gone.
		theApp->m_AsioService->Stop();
		delete theApp->m_AsioService;
		theApp = nullptr;
	}

	// SetClient() is private: the real constructor dereferences a non-null in_client
	// (GetUserIDHybrid()), so an opaque identity-only test client is attached afterward instead.
	static void AttachClient(CClientTCPSocket &socket, CUpDownClient *client)
	{
		socket.SetClient(client);
	}

private:
	struct AppInstanceGuard
	{
		wxAppConsole *original = wxAppConsole::GetInstance();
		~AppInstanceGuard() { wxAppConsole::SetInstance(original); }
	} m_appGuard;
	CamuleDaemonApp m_app;
};

TEST(ClientTCPSocket, ApplyUtpCryptParametersOnQuicTransportIsANoOp)
{
	ClientTCPSocketTestFixture fixture;

	// The fixed bug: an unchecked static_cast once called CUtpSocketTransport::SetCryptParameters()
	// on whatever IStreamTransport was attached, including a CQuicSocketTransport -- corrupting
	// memory because the objects are unrelated types with different layouts. m_client only needs
	// to be non-null to reach that call site; ApplyUtpCryptParameters() never dereferences it on
	// this path, because the dynamic_cast to CUtpSocketTransport fails first for a QUIC transport.
	// Constructed with a null client and attached afterward through the fixture's friend access:
	// the real constructor itself dereferences a non-null in_client (GetUserIDHybrid()), which an
	// opaque identity-only pointer cannot survive.
	auto *opaqueClient = reinterpret_cast<CUpDownClient *>(static_cast<uintptr_t>(1));
	// Leaked on purpose: Connect() is never called, so (unlike the fixture's CListenSocket) this
	// socket has no pending ASIO operation for a real destructor to cancel.
	auto *socket = new CClientTCPSocket(nullptr, nullptr);
	ClientTCPSocketTestFixture::AttachClient(*socket, opaqueClient);

	NullStreamOperations ops;
	static int handleStorage;
	auto transport = std::make_unique<CQuicSocketTransport>(
		ops, &handleStorage, CNetworkAddress::FromString("192.0.2.1"), 4662, nullptr, false);
	socket->AttachTransport(std::move(transport));
	ASSERT_TRUE(socket->HasTransport());

	// No crash, no reaching CUtpSocketTransport::SetCryptParameters(): the fix's dynamic_cast
	// returns null for a QUIC transport and the function returns immediately.
	socket->ApplyUtpCryptParameters();

	ASSERT_TRUE(socket->HasTransport());
}

TEST(ClientTCPSocket, ApplyUtpCryptParametersWithoutAClientIsANoOp)
{
	ClientTCPSocketTestFixture fixture;

	auto *socket = new CClientTCPSocket(nullptr, nullptr);

	NullStreamOperations ops;
	static int handleStorage;
	auto transport = std::make_unique<CQuicSocketTransport>(
		ops, &handleStorage, CNetworkAddress::FromString("192.0.2.1"), 4662, nullptr, false);
	socket->AttachTransport(std::move(transport));

	socket->ApplyUtpCryptParameters();

	ASSERT_TRUE(socket->HasTransport());
}
