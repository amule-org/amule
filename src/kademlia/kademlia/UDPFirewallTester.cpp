//
// This file is part of the aMule Project.
//
// Copyright (c) 2008-2011 Dévai Tamás ( gonosztopi@amule.org )
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

#include "UDPFirewallTester.h"
#include "../utils/UInt128.h"
#include "../utils/KadUDPKey.h"
#include "../routing/RoutingZone.h"
#include <common/Macros.h>
#include "Prefs.h"
#include "SearchManager.h"
#include "../../Logger.h"
#include "../../amule.h"
#include "../../ClientList.h"
#include "../../GetTickCount.h"
#include "../../NetworkFunctions.h"

using namespace Kademlia;

CUDPVerificationExpiry CUDPFirewallTester::m_verificationExpiry;

bool CUDPFirewallTester::m_firewalledUDP = false;
bool CUDPFirewallTester::m_firewalledLastStateUDP = false;
bool CUDPFirewallTester::m_isFWVerifiedUDP = false;
bool CUDPFirewallTester::m_nodeSearchStarted = false;
bool CUDPFirewallTester::m_timedOut = false;
bool CUDPFirewallTester::m_openNeedsConfirmation = false;
uint32_t CUDPFirewallTester::m_unconfirmedOpenIP = 0;
bool CUDPFirewallTester::m_unconfirmedOpenInternPort = false;
uint8_t CUDPFirewallTester::m_fwChecksRunningUDP = 0;
uint8_t CUDPFirewallTester::m_fwChecksFinishedUDP = 0;
uint64_t CUDPFirewallTester::m_testStart = 0;
uint64_t CUDPFirewallTester::m_lastSucceededTime = 0;
CUDPFirewallTester::PossibleClientList CUDPFirewallTester::m_possibleTestClients;
CUDPFirewallTester::UsedClientList CUDPFirewallTester::m_usedTestClients;

bool CUDPFirewallTester::IsVerified()
{
	if (CKademlia::IsRunningInLANMode()) {
		return true;
	}
	CheckVerificationExpiry(::GetTickCount64());
	return m_isFWVerifiedUDP;
}

void CUDPFirewallTester::CheckVerificationExpiry(uint64_t now)
{
	if (m_verificationExpiry.ShouldExpire(now) && m_isFWVerifiedUDP) {
		m_isFWVerifiedUDP = false;
		AddDebugLogLineN(logKadUdpFwTester,
			"UDP verification expired: consecutive inconclusive rechecks or maximum age reached");
		if (m_lastSucceededTime != 0) {
			AddDebugLogLineN(logKadUdpFwTester,
				CFormat("Last successful UDP check was %u seconds ago") %
					((now - m_lastSucceededTime) / 1000));
		}
		theApp->ShowConnectionState();
	}
}

bool CUDPFirewallTester::IsFirewalledUDP(bool lastStateIfTesting)
{
	if (CKademlia::IsRunningInLANMode()) {
		return false;
	}
	const uint64_t now = ::GetTickCount64();
	CheckVerificationExpiry(now);
	if (!m_timedOut && IsFWCheckUDPRunning()) {
		if (!m_firewalledUDP && CKademlia::IsFirewalled() && m_testStart != 0 &&
			now - m_testStart > CUDPVerificationExpiry::kRoundTimeoutMs && !m_isFWVerifiedUDP) {
			AddDebugLogLineN(logKadUdpFwTester,
				"Timeout: Setting UDP status to firewalled after being unable to get results "
				"for 6 minutes");
			m_timedOut = true;
			theApp->ShowConnectionState();
		}
	} else if (m_timedOut && IsFWCheckUDPRunning()) {
		return true; // firewallstate by timeout
	} else if (m_timedOut) {
		wxFAIL;
	}

	if (lastStateIfTesting && IsFWCheckUDPRunning()) {
		return m_firewalledLastStateUDP;
	} else {
		return m_firewalledUDP;
	}
}

void CUDPFirewallTester::SetUDPFWCheckResult(
	bool succeeded, bool testCancelled, uint32_t fromIP, uint16_t incomingPort)
{
	// can be called on shutdown after KAD has been stopped
	if (!CKademlia::IsRunning()) {
		return;
	}

	const uint64_t now = ::GetTickCount64();

	// check if we actually requested a firewallcheck from this client
	bool requested = false;
	for (UsedClientList::iterator it = m_usedTestClients.begin(); it != m_usedTestClients.end(); ++it) {
		if (it->contact.GetIPAddress() == fromIP && it->currentRound) {
			if (!IsFWCheckUDPRunning() && !m_firewalledUDP && m_isFWVerifiedUDP &&
				m_lastSucceededTime + SEC2MS(10) > now &&
				incomingPort == CKademlia::GetPrefs()->GetInternKadPort() &&
				CKademlia::GetPrefs()->GetUseExternKadPort()) {
				// Our test already finished in the last 10 seconds as open, but
				// another answer packet has now arrived on our incoming port. Not
				// unusual, since both result packets are sent at nearly the same
				// time and UDP does not preserve order. A properly forwarded intern
				// port is more reliable than an extern port set by the NAT, so
				// prefer it.
				CKademlia::GetPrefs()->SetUseExternKadPort(false);
				AddDebugLogLineN(logKadUdpFwTester,
					CFormat("Corrected UDP firewall result: Using open internal (%u) "
						"instead of open external port") %
						incomingPort);
				theApp->ShowConnectionState();
				return;
			} else if (it->answered) {
				// We already received an answer. This may happen since every test
				// consists of two answer packets, but the answer could also be too
				// late and already counted as a failure.
				if (fromIP == m_unconfirmedOpenIP &&
					incomingPort == CKademlia::GetPrefs()->GetInternKadPort()) {
					m_unconfirmedOpenInternPort = true;
				}
				return;
			} else {
				it->answered = true;
			}
			requested = true;
			break;
		}
	}

	if (!requested) {
		AddDebugLogLineN(
			logKadUdpFwTester, "Unrequested UDPFWCheckResult from " + KadIPToString(fromIP));
		return;
	}

	if (!IsFWCheckUDPRunning()) {
		// it's all over already
		return;
	}

	if (m_fwChecksRunningUDP == 0) {
		// A response arrived for a check we no longer believe to be running, most commonly
		// a late UDP packet straddling a Kad restart (#384), where ReCheckFirewallUDP
		// zeroed the counter while the previous test's responses were still in flight.
		// Dropping it avoids mutating m_firewalledUDP / m_fwChecksFinishedUDP from stale
		// state.
		AddDebugLogLineN(logKadUdpFwTester,
			wxString::Format("Ignoring late UDP FW result from %s",
				(const char *)KadIPToString(fromIP).mb_str()));
		return;
	}
	m_fwChecksRunningUDP--;

	if (!testCancelled) {
		m_fwChecksFinishedUDP++;
		// One answer can be wrong: a NAT may still hold a mapping for that tester from
		// earlier Kad traffic. Dropping a working buddy on it costs a long-lived link, so
		// a change from firewalled to open needs a second tester in the same round.
		const bool unconfirmed = succeeded && m_openNeedsConfirmation;
		if (unconfirmed) {
			m_openNeedsConfirmation = false;
			// Keep its port: an open intern port is preferred once a second tester agrees.
			m_unconfirmedOpenIP = fromIP;
			m_unconfirmedOpenInternPort =
				incomingPort == CKademlia::GetPrefs()->GetInternKadPort();
			AddDebugLogLineN(logKadUdpFwTester,
				"Kad UDP firewalltest from " + KadIPToString(fromIP) +
					" result: Open, waiting for a second tester to confirm");
		}
		if (succeeded && !unconfirmed) {
			m_lastSucceededTime = now;
			m_testStart = 0;
			m_firewalledUDP = false;
			m_isFWVerifiedUDP = true;
			m_verificationExpiry.RecordResult(now);
			m_timedOut = false;
			m_fwChecksFinishedUDP = UDP_FIREWALLTEST_CLIENTSTOASK; // don't do any more tests
			m_fwChecksRunningUDP = 0;      // all other tests are cancelled
			m_possibleTestClients.clear(); // clear list, keep used clients list though
			CSearchManager::CancelNodeFWCheckUDPSearch(); // cancel firewallnode searches if any
								      // are still active
			// if this packet came to our internal port, explicitly set the internal port as used
			// port from now on
			if (incomingPort == CKademlia::GetPrefs()->GetInternKadPort() ||
				m_unconfirmedOpenInternPort) {
				CKademlia::GetPrefs()->SetUseExternKadPort(false);
				AddDebugLogLineN(logKadUdpFwTester,
					"New Kad Firewallstate (UDP): Open, using intern port");
			} else if (incomingPort == CKademlia::GetPrefs()->GetExternalKadPort() &&
				   incomingPort != 0) {
				CKademlia::GetPrefs()->SetUseExternKadPort(true);
				AddDebugLogLineN(logKadUdpFwTester,
					"New Kad Firewallstate (UDP): Open, using extern port");
			}
			theApp->ShowConnectionState();
			return;
		} else if (m_fwChecksFinishedUDP >= UDP_FIREWALLTEST_CLIENTSTOASK) {
			// seems we are firewalled
			m_testStart = 0;
			AddDebugLogLineN(logKadUdpFwTester, "New KAD Firewallstate (UDP): Firewalled");
			m_firewalledUDP = true;
			m_isFWVerifiedUDP = true;
			m_verificationExpiry.RecordResult(now);
			m_timedOut = false;
			theApp->ShowConnectionState();
			m_possibleTestClients.clear(); // clear list, keep used clients list though
			CSearchManager::CancelNodeFWCheckUDPSearch(); // cancel firewallnode searches if any
								      // are still active
			return;
		} else if (!unconfirmed) {
			AddDebugLogLineN(logKadUdpFwTester,
				"Kad UDP firewalltest from " + KadIPToString(fromIP) +
					" result: Firewalled, continue testing");
		}
	} else {
		AddDebugLogLineN(logKadUdpFwTester,
			"Kad UDP firewalltest from " + KadIPToString(fromIP) + " cancelled");
	}
	QueryNextClient();
}

void CUDPFirewallTester::RetireTestClients()
{
	for (auto &client : m_usedTestClients) {
		client.currentRound = false;
	}
}

void CUDPFirewallTester::ReCheckFirewallUDP(bool setUnverified)
{
	const uint64_t now = ::GetTickCount64();
	// Account for an overdue previous round even if no state query polled it.
	CheckVerificationExpiry(now);
	RetireTestClients();
	if (setUnverified) {
		m_verificationExpiry.Reset();
	}
	m_verificationExpiry.Start(now);
	if (m_fwChecksRunningUDP != 0) {
		// Entering a fresh UDP firewall test while the previous one is still bookkeeping
		// in-flight requests. Common on Kad restart after a system suspend/resume (#384):
		// the previous check's responses may still be travelling. We forcibly reset; any
		// late responses for the old run are dropped by SetUDPFWCheckResult's stale-
		// response guard.
		AddDebugLogLineN(logKadUdpFwTester,
			wxString::Format("ReCheckFirewallUDP: resetting in-flight counter %u -> 0",
				(unsigned)m_fwChecksRunningUDP));
	}
	m_fwChecksRunningUDP = 0;
	m_fwChecksFinishedUDP = 0;
	m_testStart = now;
	// A timed-out round presents as firewalled too, and keeps the buddy.
	m_openNeedsConfirmation = m_firewalledUDP || m_timedOut;
	m_unconfirmedOpenIP = 0;
	m_unconfirmedOpenInternPort = false;
	m_timedOut = false;
	m_firewalledLastStateUDP = m_firewalledUDP;
	m_isFWVerifiedUDP = (m_isFWVerifiedUDP && !setUnverified);
	CSearchManager::FindNodeFWCheckUDP(); // start a lookup for a random node to find suitable IPs
	m_nodeSearchStarted = true;
	CKademlia::GetPrefs()->FindExternKadPort(true);
}

void CUDPFirewallTester::Connected()
{
	if (!m_nodeSearchStarted && IsFWCheckUDPRunning()) {
		CSearchManager::FindNodeFWCheckUDP(); // start a lookup for a random node to find suitable IPs
		m_nodeSearchStarted = true;
		m_testStart = ::GetTickCount64();
		m_verificationExpiry.Start(m_testStart);
		m_timedOut = false;
	}
}

void CUDPFirewallTester::Reset()
{
	RetireTestClients();
	m_verificationExpiry.Reset();
	m_firewalledUDP = false;
	m_firewalledLastStateUDP = false;
	m_isFWVerifiedUDP = false;
	m_nodeSearchStarted = false;
	m_timedOut = false;
	m_openNeedsConfirmation = false;
	m_unconfirmedOpenIP = 0;
	m_unconfirmedOpenInternPort = false;
	m_fwChecksRunningUDP = 0;
	m_fwChecksFinishedUDP = 0;
	m_testStart = 0;
	m_lastSucceededTime = 0;
	CSearchManager::CancelNodeFWCheckUDPSearch(); // cancel firewallnode searches if any are still active
	m_possibleTestClients.clear();
	CKademlia::GetPrefs()->SetUseExternKadPort(true);
	// keep the list of used clients
}

void CUDPFirewallTester::QueryNextClient()
{ // try the next available client for the firewallcheck
	if (!IsFWCheckUDPRunning() || !GetUDPCheckClientsNeeded() ||
		CKademlia::GetPrefs()->FindExternKadPort(false)) {
		return; // check if more tests are needed and wait till we know our extern port
	}

	if (!CKademlia::IsRunning() || CKademlia::GetRoutingZone() == NULL) {
		wxFAIL;
		return;
	}

	while (!m_possibleTestClients.empty()) {
		CContact curContact = m_possibleTestClients.front();
		m_possibleTestClients.pop_front();
		// udp firewallchecks are not supported by clients with kadversion < 6
		if (curContact.GetVersion() <= 5) {
			continue;
		}

		// sanity - do not test ourself
		if (wxUINT32_SWAP_ALWAYS(curContact.GetIPAddress()) == theApp->GetPublicIP() ||
			curContact.GetClientID() == CKademlia::GetPrefs()->GetKadID()) {
			continue;
		}

		// check if we actually requested a firewallcheck from this client at some point
		bool alreadyRequested = false;
		for (UsedClientList::const_iterator it = m_usedTestClients.begin();
			it != m_usedTestClients.end();
			++it) {
			if (it->contact.GetIPAddress() == curContact.GetIPAddress()) {
				alreadyRequested = true;
				break;
			}
		}

		// check if we know its IP already from kademlia - we need an IP which was never used for UDP
		// yet
		if (!alreadyRequested && CKademlia::GetRoutingZone()->GetContact(
						 curContact.GetIPAddress(), 0, false) == NULL) {
			// ok, tell the clientlist to do the same search and start the check if ok
			if (theApp->clientlist->DoRequestFirewallCheckUDP(curContact)) {
				UsedClient_Struct sAdd = { curContact, false };
				m_usedTestClients.push_front(sAdd);
				m_fwChecksRunningUDP++;
				break;
			}
		}
	}
}
