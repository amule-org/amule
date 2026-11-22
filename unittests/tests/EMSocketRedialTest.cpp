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

#include <muleunit/test.h>

#include <ClientList.h>
#include <EMSocket.h>
#include <GuiEvents.h>
#include <Logger.h>
#include <Packet.h>
#include <Preferences.h>
#include <ServerConnect.h>
#include <UploadBandwidthThrottler.h>
#include <UploadDiskIOThread.h>
#include <amule.h>
#include <protocol/Protocols.h>

#include <optional>
#include <vector>

using namespace muleunit;

DECLARE_SIMPLE(EMSocketRedial)

namespace
{
int g_throttlerRemovals = 0;
// The place in the upload slot list the throttler reports for a socket it removes.
std::optional<uint32> g_uploadSlot;
struct SReAdd
{
	uint32 index;
	ThrottledFileSocket *socket;
	size_t queuedControlPackets;
};
std::vector<SReAdd> g_reAdds;
} // namespace

// Only the application boundary is stubbed; the socket's state machine is production code.
CamuleDaemonApp *theApp = nullptr;
CamuleDaemonApp::CamuleDaemonApp() = default;
bool CamuleDaemonApp::OnInit()
{
	FAIL_M("Unexpected daemon startup");
	return false;
}
int CamuleDaemonApp::OnRun()
{
	FAIL_M("Unexpected daemon run");
	return 0;
}
int CamuleDaemonApp::OnExit()
{
	FAIL_M("Unexpected daemon shutdown");
	return 0;
}
int CamuleDaemonApp::InitGui(bool, wxString &)
{
	FAIL_M("Unexpected daemon GUI");
	return 0;
}
bool CamuleDaemonApp::Initialize(int &, wxChar **)
{
	FAIL_M("Unexpected daemon initialization");
	return false;
}
int CamuleDaemonApp::ShowAlert(wxString, wxString, int)
{
	FAIL_M("Unexpected alert");
	return 0;
}
wxBEGIN_EVENT_TABLE(CamuleDaemonApp, CamuleApp)
wxEND_EVENT_TABLE()

UploadBandwidthThrottler::UploadBandwidthThrottler()
: wxThread(wxTHREAD_JOINABLE)
{
}
UploadBandwidthThrottler::~UploadBandwidthThrottler() = default;
void *UploadBandwidthThrottler::Entry()
{
	return nullptr;
}
void UploadBandwidthThrottler::QueueForSendingControlPacket(ThrottledControlSocket *, bool) {}
std::optional<uint32> UploadBandwidthThrottler::RemoveFromAllQueues(ThrottledFileSocket *)
{
	++g_throttlerRemovals;
	return g_uploadSlot;
}
void UploadBandwidthThrottler::AddToStandardList(uint32 index, ThrottledFileSocket *socket)
{
	g_reAdds.push_back(
		{ index, socket, dynamic_cast<CEMSocket &>(*socket).GetPendingControlPacketCount() });
}
void CUploadDiskIOThread::SocketNeedsMoreData() {}
bool CClientList::IsKadFirewallCheckIP(uint32) const
{
	return false;
}
bool CServerConnect::AwaitingTestFromIP(uint32)
{
	return false;
}
namespace MuleNotify
{
void HandleNotificationAlways(const CMuleNotiferBase &) {}
} // namespace MuleNotify

CMD4Hash CPreferences::s_userhash;
wxString CPreferences::s_Addr;
bool CPreferences::s_IsClientCryptLayerSupported = false;
bool CPreferences::s_IsClientCryptLayerRequired = false;
bool CPreferences::s_bCryptLayerRequested = false;
uint8 CPreferences::s_byCryptTCPPaddingLength = 0;

namespace
{
class CRedialSocket : public CEMSocket
{
public:
	using CEMSocket::OnClose;
	using CEMSocket::QuiesceForRedial;
	void OnConnect(int) override {}
};

bool Send(CRedialSocket &socket, bool control)
{
	return socket.TrySendPacket(new CPacket(0x01, 0, OP_EDONKEYPROT), true, control, 0);
}

class CRedialFixture
{
public:
	CRedialFixture()
	{
		// Constructing an app makes it wx's running one; muleunit's must stay that.
		wxAppConsole::SetInstance(m_appGuard.original);
		theApp = &m_app;
		m_app.uploadBandwidthThrottler = &m_throttler;
		g_throttlerRemovals = 0;
		g_uploadSlot.reset();
		g_reAdds.clear();
	}
	~CRedialFixture()
	{
		m_app.uploadBandwidthThrottler = nullptr;
		theApp = nullptr;
	}

private:
	struct AppInstanceGuard
	{
		wxAppConsole *original = wxAppConsole::GetInstance();
		~AppInstanceGuard() { wxAppConsole::SetInstance(original); }
	} m_appGuard;
	CamuleDaemonApp m_app;
	UploadBandwidthThrottler m_throttler;
};
} // namespace

TEST(EMSocketRedial, AClosedSocketSendsAgainOnceQuiescedForRedial)
{
	CRedialFixture fixture;
	CRedialSocket socket;
	socket.OnClose(0);
	// OnClose() leaves a state no send path clears.
	ASSERT_FALSE(Send(socket, true));

	socket.QuiesceForRedial();

	ASSERT_TRUE(Send(socket, true));
	ASSERT_EQUALS(1u, (unsigned)socket.GetPendingControlPacketCount());
}

TEST(EMSocketRedial, QuiescingDropsWhatTheDeadStreamLeftQueued)
{
	CRedialFixture fixture;
	CRedialSocket socket;
	ASSERT_TRUE(Send(socket, true));
	ASSERT_EQUALS(1u, (unsigned)socket.GetPendingControlPacketCount());

	socket.QuiesceForRedial();

	// Replayed onto the new connection, the old packet would be read as a bogus header.
	ASSERT_EQUALS(0u, (unsigned)socket.GetPendingControlPacketCount());
	// It leaves the throttler before its queues go, so no send is still running in them.
	ASSERT_TRUE(g_throttlerRemovals > 0);
	// It held no upload slot, so it gets none.
	ASSERT_EQUALS(0u, (unsigned)g_reAdds.size());
}

TEST(EMSocketRedial, AnUploadSlotIsGivenBackOnceTheDeadStreamIsGone)
{
	CRedialFixture fixture;
	CRedialSocket socket;
	ASSERT_TRUE(Send(socket, true));
	g_uploadSlot = 3;

	socket.QuiesceForRedial();

	// Only sockets in that list are sent file data: without it, the redialled socket
	// accepts the upload and then sends none of the file.
	ASSERT_EQUALS(1u, (unsigned)g_reAdds.size());
	ASSERT_EQUALS(3u, (unsigned)g_reAdds[0].index);
	ASSERT_TRUE(g_reAdds[0].socket == &socket);
	// Not before the queues go, or the throttler sends the old stream's leftovers.
	ASSERT_EQUALS(0u, (unsigned)g_reAdds[0].queuedControlPackets);
}
