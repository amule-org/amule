// This file is part of the aMule Project.
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#include <muleunit/test.h>
#include <amule.h>
#include <ClientList.h>
#include <Preferences.h>
#include <Statistics.h>
#include <GetTickCount.h>
#include <cstdlib>
#include <kademlia/kademlia/UDPFirewallTester.h>
#include <kademlia/kademlia/SearchManager.h>
#include <kademlia/routing/RoutingZone.h>
#include <KadCallbackPolicy.h>

using namespace muleunit;
using namespace Kademlia;

namespace
{
uint64_t now = 1000000;
unsigned notifications = 0;
unsigned clockReads = 0;
bool queryDuringNotification = false;
bool verifiedDuringNotification = true;
bool lanMode = false;
constexpr uint16_t internalPort = 4665;
constexpr uint16_t externalPort = 5678;
constexpr uint32_t peer = 0x01020304;
} // namespace

// Only the application/network boundary is stubbed. The tester, expiry logic and
// verification queries are production code, driven through their normal entry points.
uint64 GetTickCount64()
{
	++clockReads;
	return now;
}
CamuleDaemonApp *theApp = nullptr;
void CamuleApp::ShowConnectionState(bool)
{
	++notifications;
	if (queryDuringNotification) {
		verifiedDuringNotification = CUDPFirewallTester::IsVerified();
	}
}
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

uint16_t CStatistics::s_kadNodesCur = 0;
uint16 CPreferences::s_udpport = internalPort;
CKademlia *CKademlia::instance = nullptr;
bool CKademlia::m_running = false;
bool CKademlia::IsRunningInLANMode()
{
	return lanMode;
}
void CKademlia::Start(CPrefs *prefs)
{
	instance = new CKademlia;
	instance->m_prefs = prefs;
	instance->m_routingZone = new CRoutingZone;
	instance->m_udpListener = nullptr;
	instance->m_indexed = nullptr;
	m_running = true;
}
void CKademlia::Stop()
{
	CUDPFirewallTester::Reset();
	delete instance->m_routingZone;
	delete instance->m_prefs;
	delete instance;
	instance = nullptr;
	m_running = false;
}
CPrefs::CPrefs()
{
	m_lastContact = time(nullptr);
	m_externKadPort = externalPort;
	m_useExternKadPort = true;
}
CPrefs::~CPrefs() = default;
bool CPrefs::FindExternKadPort(bool)
{
	return false;
}
bool CSearchManager::FindNodeFWCheckUDP()
{
	return true;
}
void CSearchManager::CancelNodeFWCheckUDPSearch() {}
bool CClientList::DoRequestFirewallCheckUDP(const CContact &)
{
	FAIL_M("Unexpected live client request");
	return false;
}
CRoutingZone::CRoutingZone() = default;
CRoutingZone::~CRoutingZone() = default;
CContact *CRoutingZone::GetContact(uint32_t, uint16_t, bool) const noexcept
{
	std::abort();
}

namespace Kademlia
{
class CUDPFirewallTesterFixture
{
public:
	CUDPFirewallTesterFixture()
	{
		now = 1000000;
		notifications = 0;
		lanMode = false;
		queryDuringNotification = false;
		wxAppConsole::SetInstance(appGuard.original);
		theApp = &app;
		CKademlia::Start(new CPrefs);
		CUDPFirewallTester::Reset();
		CUDPFirewallTester::m_usedTestClients.clear();
	}
	~CUDPFirewallTesterFixture()
	{
		CKademlia::Stop();
		theApp = nullptr;
	}
	void Request(uint32_t ip = peer)
	{
		CContact contact(CUInt128(false), ip, 1, 1, 6, CKadUDPKey(), true, CUInt128(false));
		CUDPFirewallTester::m_usedTestClients.push_front({ contact, false });
		++CUDPFirewallTester::m_fwChecksRunningUDP;
	}
	void Open()
	{
		CUDPFirewallTester::ReCheckFirewallUDP(false);
		Request();
		CUDPFirewallTester::SetUDPFWCheckResult(true, false, peer, externalPort);
		ASSERT_TRUE(CUDPFirewallTester::IsVerified());
	}
	void Firewalled()
	{
		CUDPFirewallTester::ReCheckFirewallUDP(false);
		Request(peer + 10);
		Request(peer + 11);
		CUDPFirewallTester::SetUDPFWCheckResult(false, false, peer + 10, 0);
		CUDPFirewallTester::SetUDPFWCheckResult(false, false, peer + 11, 0);
		ASSERT_TRUE(CUDPFirewallTester::IsVerified());
		ASSERT_TRUE(CUDPFirewallTester::IsFirewalledUDP(false));
	}
	// The next recheck, with two testers asked.
	void StartRecheckRound()
	{
		now += CUDPVerificationExpiry::kRecheckIntervalMs;
		CUDPFirewallTester::ReCheckFirewallUDP(false);
		Request(peer + 1);
		Request(peer + 2);
	}
	uint64_t LastSuccess() const { return CUDPFirewallTester::m_lastSucceededTime; }

private:
	// wxAppConsole's constructor/destructor changes the global app instance.
	// Restore the unit runner after the genuine application fixture is destroyed.
	struct AppInstanceGuard
	{
		wxAppConsole *original = wxAppConsole::GetInstance();
		~AppInstanceGuard() { wxAppConsole::SetInstance(original); }
	} appGuard;
	CamuleDaemonApp app;
};
} // namespace Kademlia

DECLARE_SIMPLE(KadUDPFirewallIntegration)

TEST(KadUDPFirewallIntegration, VerificationQueryExpiresWithoutFirewallQuery)
{
	CUDPFirewallTesterFixture fixture;
	fixture.Open();
	now += CUDPVerificationExpiry::kMaxVerificationAgeMs - 1;
	ASSERT_TRUE(CUDPFirewallTester::IsVerified());
	++now;
	// No IsFirewalledUDP call may refresh the flag before this assertion.
	ASSERT_FALSE(CUDPFirewallTester::IsVerified());
	ASSERT_FALSE(CUDPFirewallTester::IsFirewalledUDP(true)); // stored open result is preserved
	ASSERT_TRUE(NeedsBuddy(true, false, CUDPFirewallTester::IsVerified()));
	ASSERT_FALSE(CanPublishSource(true, 0, 0, 0, 1, 0, 0));
	ASSERT_EQUALS(2u, notifications); // successful result, then one expiry
	ASSERT_FALSE(CUDPFirewallTester::IsVerified());
	ASSERT_EQUALS(2u, notifications);
}

TEST(KadUDPFirewallIntegration, UnpolledOverdueRoundsExpireOnRestart)
{
	CUDPFirewallTesterFixture fixture;
	fixture.Open();
	now += CUDPVerificationExpiry::kRecheckIntervalMs;
	CUDPFirewallTester::ReCheckFirewallUDP(false);
	now += CUDPVerificationExpiry::kRecheckIntervalMs;
	CUDPFirewallTester::ReCheckFirewallUDP(false); // counts the first overdue round
	ASSERT_TRUE(CUDPFirewallTester::IsVerified());
	now += CUDPVerificationExpiry::kRoundTimeoutMs + 1;
	CUDPFirewallTester::ReCheckFirewallUDP(false); // counts the second before restarting
	ASSERT_FALSE(CUDPFirewallTester::IsVerified());
}

TEST(KadUDPFirewallIntegration, LateSuccessRenewsExpiredVerification)
{
	CUDPFirewallTesterFixture fixture;
	fixture.Open();
	now += CUDPVerificationExpiry::kRecheckIntervalMs;
	CUDPFirewallTester::ReCheckFirewallUDP(false);
	fixture.Request(peer + 1);
	now += CUDPVerificationExpiry::kMaxVerificationAgeMs;
	ASSERT_FALSE(CUDPFirewallTester::IsVerified());
	CUDPFirewallTester::SetUDPFWCheckResult(true, false, peer + 1, externalPort);
	ASSERT_TRUE(CUDPFirewallTester::IsVerified());
	ASSERT_EQUALS(now, fixture.LastSuccess());
	now += CUDPVerificationExpiry::kMaxVerificationAgeMs - 1;
	ASSERT_TRUE(CUDPFirewallTester::IsVerified());
}

TEST(KadUDPFirewallIntegration, CancellationDoesNotRenewVerification)
{
	CUDPFirewallTesterFixture fixture;
	fixture.Open();
	now += CUDPVerificationExpiry::kRecheckIntervalMs;
	CUDPFirewallTester::ReCheckFirewallUDP(false);
	fixture.Request(peer + 1);
	CUDPFirewallTester::SetUDPFWCheckResult(false, true, peer + 1, 0);
	now = 1000000 + CUDPVerificationExpiry::kMaxVerificationAgeMs;
	ASSERT_FALSE(CUDPFirewallTester::IsVerified());
}

TEST(KadUDPFirewallIntegration, FirewalledResultRenewsVerification)
{
	CUDPFirewallTesterFixture fixture;
	fixture.Open();
	fixture.StartRecheckRound();
	CUDPFirewallTester::SetUDPFWCheckResult(false, false, peer + 1, 0);
	CUDPFirewallTester::SetUDPFWCheckResult(false, false, peer + 2, 0);
	ASSERT_TRUE(CUDPFirewallTester::IsVerified());
	ASSERT_TRUE(CUDPFirewallTester::IsFirewalledUDP(true));
	now += CUDPVerificationExpiry::kMaxVerificationAgeMs - 1;
	ASSERT_TRUE(CUDPFirewallTester::IsVerified());
}

TEST(KadUDPFirewallIntegration, InternalPortCorrectionUsesSuccessTimestamp)
{
	CUDPFirewallTesterFixture fixture;
	fixture.Open();
	now += 9999;
	CUDPFirewallTester::SetUDPFWCheckResult(true, false, peer, internalPort);
	ASSERT_FALSE(CKademlia::GetPrefs()->GetUseExternKadPort());
}

TEST(KadUDPFirewallIntegration, ResetAndLANMode)
{
	CUDPFirewallTesterFixture fixture;
	fixture.Open();
	CUDPFirewallTester::Reset();
	ASSERT_FALSE(CUDPFirewallTester::IsVerified());
	now += CUDPVerificationExpiry::kMaxVerificationAgeMs;
	lanMode = true;
	clockReads = 0;
	ASSERT_TRUE(CUDPFirewallTester::IsVerified());
	ASSERT_EQUALS(0u, clockReads); // LAN bypass precedes clock/expiry work
	ASSERT_FALSE(CUDPFirewallTester::IsFirewalledUDP(true));
	ASSERT_FALSE(CKademlia::GetPrefs()->GetUseExternKadPort());
	lanMode = false;
	ASSERT_FALSE(CUDPFirewallTester::IsVerified());
}

TEST(KadUDPFirewallIntegration, ResultSamplesClockOnce)
{
	CUDPFirewallTesterFixture fixture;
	CUDPFirewallTester::ReCheckFirewallUDP(false);
	fixture.Request();
	clockReads = 0;
	CUDPFirewallTester::SetUDPFWCheckResult(true, false, peer, externalPort);
	ASSERT_EQUALS(1u, clockReads);
	ASSERT_EQUALS(now, fixture.LastSuccess());
}

TEST(KadUDPFirewallIntegration, UnrequestedResultCannotRenewExpiredState)
{
	CUDPFirewallTesterFixture fixture;
	fixture.Open();
	now += CUDPVerificationExpiry::kMaxVerificationAgeMs;
	CUDPFirewallTester::SetUDPFWCheckResult(true, false, peer + 1, externalPort);
	ASSERT_FALSE(CUDPFirewallTester::IsVerified());
}

TEST(KadUDPFirewallIntegration, InternalPortCorrectionEndsAtTenSeconds)
{
	CUDPFirewallTesterFixture fixture;
	fixture.Open();
	now += 10000;
	CUDPFirewallTester::SetUDPFWCheckResult(true, false, peer, internalPort);
	ASSERT_TRUE(CKademlia::GetPrefs()->GetUseExternKadPort());
}

TEST(KadUDPFirewallIntegration, StopStartDropsPreviousVerification)
{
	CUDPFirewallTesterFixture fixture;
	fixture.Open();
	CKademlia::Stop();
	ASSERT_FALSE(CKademlia::IsRunning());
	ASSERT_FALSE(CUDPFirewallTester::IsVerified());
	CKademlia::Start(new CPrefs);
	CUDPFirewallTester::Connected();
	ASSERT_FALSE(CUDPFirewallTester::IsVerified());
	now += CUDPVerificationExpiry::kRoundTimeoutMs + 1;
	ASSERT_FALSE(CUDPFirewallTester::IsVerified());
}

TEST(KadUDPFirewallIntegration, FrequentRestartsCannotRetainVerification)
{
	CUDPFirewallTesterFixture fixture;
	fixture.Open();
	const uint64_t start = now;
	for (uint64_t elapsed = 300000; elapsed < CUDPVerificationExpiry::kMaxVerificationAgeMs;
		elapsed += 300000) {
		now = start + elapsed;
		CUDPFirewallTester::ReCheckFirewallUDP(false);
		ASSERT_TRUE(CUDPFirewallTester::IsVerified());
	}
	now = start + CUDPVerificationExpiry::kMaxVerificationAgeMs;
	ASSERT_FALSE(CUDPFirewallTester::IsVerified());
}

TEST(KadUDPFirewallIntegration, FullSecondRecheckWindowCanRecover)
{
	CUDPFirewallTesterFixture fixture;
	CUDPFirewallTester::ReCheckFirewallUDP(false);
	fixture.Request();
	const uint64_t start = now;
	now += 60000;
	CUDPFirewallTester::SetUDPFWCheckResult(true, false, peer, externalPort);
	now = start + CUDPVerificationExpiry::kRecheckIntervalMs;
	CUDPFirewallTester::ReCheckFirewallUDP(false);
	now += CUDPVerificationExpiry::kRecheckIntervalMs;
	CUDPFirewallTester::ReCheckFirewallUDP(false);
	fixture.Request(peer + 1);
	now += CUDPVerificationExpiry::kRoundTimeoutMs;
	ASSERT_TRUE(CUDPFirewallTester::IsVerified());
	CUDPFirewallTester::SetUDPFWCheckResult(true, false, peer + 1, externalPort);
	now += CUDPVerificationExpiry::kRoundTimeoutMs + 1;
	ASSERT_TRUE(CUDPFirewallTester::IsVerified());
}

TEST(KadUDPFirewallIntegration, PreviousSessionResultCannotVerify)
{
	CUDPFirewallTesterFixture fixture;
	CUDPFirewallTester::ReCheckFirewallUDP(false);
	fixture.Request();
	CKademlia::Stop();
	CKademlia::Start(new CPrefs);
	CUDPFirewallTester::Connected();
	CUDPFirewallTester::SetUDPFWCheckResult(true, false, peer, externalPort);
	ASSERT_FALSE(CUDPFirewallTester::IsVerified());
}

TEST(KadUDPFirewallIntegration, GetterOrderDoesNotChangeExpiredState)
{
	for (bool verifiedFirst : { false, true }) {
		CUDPFirewallTesterFixture fixture;
		fixture.Open();
		now += CUDPVerificationExpiry::kMaxVerificationAgeMs;
		bool verified;
		bool firewalled;
		if (verifiedFirst) {
			verified = CUDPFirewallTester::IsVerified();
			firewalled = CUDPFirewallTester::IsFirewalledUDP(true);
		} else {
			firewalled = CUDPFirewallTester::IsFirewalledUDP(true);
			verified = CUDPFirewallTester::IsVerified();
		}
		ASSERT_FALSE(verified);
		ASSERT_FALSE(firewalled);
		ASSERT_FALSE(DirectCallbackAvailable(true, firewalled, verified));
		ASSERT_TRUE(NeedsBuddy(true, firewalled, verified));
		ASSERT_EQUALS(2u, notifications);
	}
}

TEST(KadUDPFirewallIntegration, LANBypassesExpiryUntilPublicMode)
{
	CUDPFirewallTesterFixture fixture;
	fixture.Open();
	now += CUDPVerificationExpiry::kMaxVerificationAgeMs;
	lanMode = true;
	clockReads = 0;
	ASSERT_TRUE(CUDPFirewallTester::IsVerified());
	ASSERT_EQUALS(0u, clockReads);
	ASSERT_EQUALS(1u, notifications);
	lanMode = false;
	ASSERT_FALSE(CUDPFirewallTester::IsVerified());
	ASSERT_EQUALS(2u, notifications);
}

TEST(KadUDPFirewallIntegration, ExpiryNotificationCanReenterVerificationQuery)
{
	CUDPFirewallTesterFixture fixture;
	fixture.Open();
	now += CUDPVerificationExpiry::kMaxVerificationAgeMs;
	queryDuringNotification = true;
	ASSERT_FALSE(CUDPFirewallTester::IsVerified());
	ASSERT_FALSE(verifiedDuringNotification);
	ASSERT_EQUALS(2u, notifications);
}

TEST(KadUDPFirewallIntegration, PreviousSessionFailuresCannotFinishCurrentRound)
{
	CUDPFirewallTesterFixture fixture;
	CUDPFirewallTester::ReCheckFirewallUDP(false);
	fixture.Request();
	fixture.Request(peer + 1);
	CKademlia::Stop();
	CKademlia::Start(new CPrefs);
	CUDPFirewallTester::Connected();
	fixture.Request(peer + 2);
	fixture.Request(peer + 3);
	CUDPFirewallTester::SetUDPFWCheckResult(false, false, peer, 0);
	CUDPFirewallTester::SetUDPFWCheckResult(false, false, peer + 1, 0);
	ASSERT_FALSE(CUDPFirewallTester::IsVerified());
	CUDPFirewallTester::SetUDPFWCheckResult(false, false, peer + 2, 0);
	CUDPFirewallTester::SetUDPFWCheckResult(false, false, peer + 3, 0);
	ASSERT_TRUE(CUDPFirewallTester::IsVerified());
	ASSERT_TRUE(CUDPFirewallTester::IsFirewalledUDP(true));
}

TEST(KadUDPFirewallIntegration, PreviousRoundSuccessCannotVerifyCurrentRound)
{
	CUDPFirewallTesterFixture fixture;
	CUDPFirewallTester::ReCheckFirewallUDP(false);
	fixture.Request();
	CUDPFirewallTester::ReCheckFirewallUDP(false);
	fixture.Request(peer + 1);
	CUDPFirewallTester::SetUDPFWCheckResult(true, false, peer, externalPort);
	ASSERT_FALSE(CUDPFirewallTester::IsVerified());
	CUDPFirewallTester::SetUDPFWCheckResult(true, false, peer + 1, externalPort);
	ASSERT_TRUE(CUDPFirewallTester::IsVerified());
}

TEST(KadUDPFirewallIntegration, PortCorrectionMustBelongToCurrentRound)
{
	CUDPFirewallTesterFixture fixture;
	fixture.Open();
	++now;
	CUDPFirewallTester::ReCheckFirewallUDP(false);
	fixture.Request(peer + 1);
	CUDPFirewallTester::SetUDPFWCheckResult(true, false, peer + 1, externalPort);
	++now;
	CUDPFirewallTester::SetUDPFWCheckResult(true, false, peer, internalPort);
	ASSERT_TRUE(CKademlia::GetPrefs()->GetUseExternKadPort());
	CUDPFirewallTester::SetUDPFWCheckResult(true, false, peer + 1, internalPort);
	ASSERT_FALSE(CKademlia::GetPrefs()->GetUseExternKadPort());
}

TEST(KadUDPFirewallIntegration, OpenAfterFirewalledNeedsSecondTester)
{
	CUDPFirewallTesterFixture fixture;
	fixture.Firewalled();
	fixture.StartRecheckRound();
	CUDPFirewallTester::SetUDPFWCheckResult(true, false, peer + 1, externalPort);
	ASSERT_TRUE(CUDPFirewallTester::IsFWCheckUDPRunning());
	ASSERT_TRUE(CUDPFirewallTester::IsFirewalledUDP(true));
	CUDPFirewallTester::SetUDPFWCheckResult(true, false, peer + 2, externalPort);
	ASSERT_TRUE(CUDPFirewallTester::IsVerified());
	ASSERT_FALSE(CUDPFirewallTester::IsFirewalledUDP(false));
}

TEST(KadUDPFirewallIntegration, UnconfirmedOpenEndsFirewalled)
{
	CUDPFirewallTesterFixture fixture;
	fixture.Firewalled();
	fixture.StartRecheckRound();
	CUDPFirewallTester::SetUDPFWCheckResult(true, false, peer + 1, externalPort);
	CUDPFirewallTester::SetUDPFWCheckResult(false, false, peer + 2, 0);
	ASSERT_FALSE(CUDPFirewallTester::IsFWCheckUDPRunning());
	ASSERT_TRUE(CUDPFirewallTester::IsVerified());
	ASSERT_TRUE(CUDPFirewallTester::IsFirewalledUDP(false));
}

TEST(KadUDPFirewallIntegration, OpenAfterFailureEndsFirewalled)
{
	CUDPFirewallTesterFixture fixture;
	fixture.Firewalled();
	fixture.StartRecheckRound();
	CUDPFirewallTester::SetUDPFWCheckResult(false, false, peer + 1, 0);
	CUDPFirewallTester::SetUDPFWCheckResult(true, false, peer + 2, externalPort);
	ASSERT_FALSE(CUDPFirewallTester::IsFWCheckUDPRunning());
	ASSERT_TRUE(CUDPFirewallTester::IsFirewalledUDP(false));
}

TEST(KadUDPFirewallIntegration, SameTesterCannotConfirmOpen)
{
	CUDPFirewallTesterFixture fixture;
	fixture.Firewalled();
	fixture.StartRecheckRound();
	CUDPFirewallTester::SetUDPFWCheckResult(true, false, peer + 1, externalPort);
	CUDPFirewallTester::SetUDPFWCheckResult(true, false, peer + 1, internalPort);
	ASSERT_TRUE(CUDPFirewallTester::IsFWCheckUDPRunning());
	ASSERT_TRUE(CUDPFirewallTester::IsFirewalledUDP(true));
}

TEST(KadUDPFirewallIntegration, NewSessionTrustsFirstOpen)
{
	CUDPFirewallTesterFixture fixture;
	fixture.Firewalled();
	CKademlia::Stop();
	CKademlia::Start(new CPrefs);
	fixture.Open();
	ASSERT_FALSE(CUDPFirewallTester::IsFirewalledUDP(false));
}

TEST(KadUDPFirewallIntegration, OpenAfterTimeoutNeedsSecondTester)
{
	CUDPFirewallTesterFixture fixture;
	CUDPFirewallTester::ReCheckFirewallUDP(false);
	now += CUDPVerificationExpiry::kRoundTimeoutMs + 1;
	CUDPFirewallTester::IsFirewalledUDP(true);
	ASSERT_TRUE(CUDPFirewallTester::IsFirewalledUDP(true));
	ASSERT_FALSE(CUDPFirewallTester::IsVerified());
	CUDPFirewallTester::ReCheckFirewallUDP(false);
	fixture.Request(peer + 1);
	fixture.Request(peer + 2);
	CUDPFirewallTester::SetUDPFWCheckResult(true, false, peer + 1, externalPort);
	ASSERT_TRUE(CUDPFirewallTester::IsFWCheckUDPRunning());
	ASSERT_FALSE(CUDPFirewallTester::IsVerified());
	CUDPFirewallTester::SetUDPFWCheckResult(true, false, peer + 2, externalPort);
	ASSERT_TRUE(CUDPFirewallTester::IsVerified());
	ASSERT_FALSE(CUDPFirewallTester::IsFirewalledUDP(false));
}

TEST(KadUDPFirewallIntegration, UnconfirmedOpenRoundsExpireVerification)
{
	CUDPFirewallTesterFixture fixture;
	fixture.Firewalled();
	for (uint32_t round = 1; round <= CUDPVerificationExpiry::kFailedRoundsToExpire; ++round) {
		now += CUDPVerificationExpiry::kRecheckIntervalMs;
		CUDPFirewallTester::ReCheckFirewallUDP(false);
		fixture.Request(peer + round);
		CUDPFirewallTester::SetUDPFWCheckResult(true, false, peer + round, externalPort);
		now += CUDPVerificationExpiry::kRoundTimeoutMs + 1;
		ASSERT_TRUE(CUDPFirewallTester::IsFWCheckUDPRunning());
		ASSERT_TRUE(CUDPFirewallTester::IsFirewalledUDP(false));
	}
	ASSERT_FALSE(CUDPFirewallTester::IsVerified());
}

TEST(KadUDPFirewallIntegration, ConfirmedOpenKeepsUnconfirmedInternPort)
{
	CUDPFirewallTesterFixture fixture;
	fixture.Firewalled();
	fixture.StartRecheckRound();
	CUDPFirewallTester::SetUDPFWCheckResult(true, false, peer + 1, internalPort);
	CUDPFirewallTester::SetUDPFWCheckResult(true, false, peer + 2, externalPort);
	ASSERT_FALSE(CUDPFirewallTester::IsFirewalledUDP(false));
	ASSERT_FALSE(CKademlia::GetPrefs()->GetUseExternKadPort());
}

TEST(KadUDPFirewallIntegration, ConfirmedOpenKeepsUnconfirmedTestersSecondPacket)
{
	CUDPFirewallTesterFixture fixture;
	fixture.Firewalled();
	fixture.StartRecheckRound();
	CUDPFirewallTester::SetUDPFWCheckResult(true, false, peer + 1, externalPort);
	CUDPFirewallTester::SetUDPFWCheckResult(true, false, peer + 1, internalPort);
	CUDPFirewallTester::SetUDPFWCheckResult(true, false, peer + 2, externalPort);
	ASSERT_FALSE(CUDPFirewallTester::IsFirewalledUDP(false));
	ASSERT_FALSE(CKademlia::GetPrefs()->GetUseExternKadPort());
}
