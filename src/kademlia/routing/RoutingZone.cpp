//
// This file is part of aMule Project
//
// Copyright (c) 2004-2011 Angel Vidal ( kry@amule.org )
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2003-2011 Barry Dunne ( http://www.emule-project.net )
// Copyright (C)2007-2011 Merkur ( strEmail.Format("%s@%s", "devteam", "emule-project.net") /
// http://www.emule-project.net )

// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either
// version 2 of the License, or (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA

// This work is based on the java implementation of the Kademlia protocol.
// Kademlia: Peer-to-peer routing based on the XOR metric
// Copyright (c) 2002-2011  Petar Maymounkov ( petar@maymounkov.org )
// https://pdos.csail.mit.edu/~petar/papers/maymounkov-kademlia-lncs.pdf

// Note To Mods //
/*
Please do not change anything here and release it..
There is going to be a new forum created just for the Kademlia side of the client..
If you feel there is an error or a way to improve something, please
post it in the forum first and let us look at it.. If it is a real improvement,
it will be added to the official client.. Changing something without knowing
what all it does can cause great harm to the network if released in mass form..
Any mod that changes anything within the Kademlia side will not be allowed to advertise
there client on the eMule forum..
*/

/**
 * A *Zone* is a node in a binary tree of *Zone*s, either internal or a leaf. Internal nodes have
 * "bin == null" and "subZones[i] != null"; leaf nodes have "subZones[i] == null" and
 * "bin != null". All key unique ids are relative to the center (self), taken as 000..000.
 */
#include "RoutingZone.h"

#include <protocol/kad/Client2Client/UDP.h>
#include <protocol/kad2/Client2Client/UDP.h>
#include <common/Macros.h>

#include "Contact.h"
#include "RoutingBin.h"
#include "../kademlia/Defines.h"
#include "../kademlia/SearchManager.h"
#include "../kademlia/UDPFirewallTester.h"
#include "../net/KademliaUDPListener.h"
#ifdef ENABLE_KAD_NODE_PROTECTION
#include "../net/SafeKad.h"
#endif
#include "../utils/KadUDPKey.h"
#include "../../amule.h"
#include "../../CFile.h"
#include "../../Logger.h"
#include "../../NetworkFunctions.h"
#include "../../IPFilter.h"
#include "../../RandomFunctions.h"

#include <cmath>

////////////////////////////////////////
using namespace Kademlia;
////////////////////////////////////////

// This is just a safety precaution
#define CONTACT_FILE_LIMIT 500

wxString CRoutingZone::m_filename;
CUInt128 CRoutingZone::me((uint32_t)0);

CRoutingZone::CRoutingZone()
{
	// Can only create routing zone after prefs
	// Set our KadID for creating the contact tree
	me = CKademlia::GetPrefs()->GetKadID();
	m_filename = thePrefs::GetConfigDir() + "nodes.dat";
	Init(NULL, 0, CUInt128((uint32_t)0));
}

void CRoutingZone::Init(CRoutingZone *super_zone, int level, const CUInt128 &zone_index)
{
	// Set this zone's parent
	m_superZone = super_zone;
	m_level = level;
	m_zoneIndex = zone_index;
	m_subZones[0] = NULL;
	m_subZones[1] = NULL;
	m_bin = new CRoutingBin();

	// Set timer so that zones closer to the root are processed earlier.
	m_nextSmallTimer = time(NULL) + m_zoneIndex.Get32BitChunk(3);

	StartTimer();

	// If we are initializing the root node, read in our saved contact list.
	if ((m_superZone == NULL) && (m_filename.Length() > 0)) {
		ReadFile();
	}
}

CRoutingZone::~CRoutingZone()
{
	// Root node is processed first so that we can write our contact list and delete all branches.
	if ((m_superZone == NULL) && (m_filename.Length() > 0)) {
		WriteFile();
	}

	if (IsLeaf()) {
		delete m_bin;
	} else {
		delete m_subZones[0];
		delete m_subZones[1];
	}
}

void CRoutingZone::ReadFile(const wxString &specialNodesdat)
{
	if (m_superZone != NULL || (m_filename.IsEmpty() && specialNodesdat.IsEmpty())) {
		wxFAIL;
		return;
	}

	bool doHaveVerifiedContacts = false;
	try {
		uint32_t validContacts = 0;
		CFile file;
		if (CPath::FileExists(specialNodesdat.IsEmpty() ? m_filename : specialNodesdat) &&
			file.Open(m_filename, CFile::read)) {
			// How many contacts are in the saved list. Older clients put the count
			// here; newer ones always write 0, to stop older clients reading it.
			uint32_t numContacts = file.ReadUInt32();
			uint32_t fileVersion = 0;
			if (numContacts == 0) {
				if (file.GetLength() >= 8) {
					fileVersion = file.ReadUInt32();
					if (fileVersion == 3) {
						uint32_t bootstrapEdition = file.ReadUInt32();
						if (bootstrapEdition == 1) {
							// this is a special bootstrap-only nodes.dat, handle
							// it in a separate reading function
							ReadBootstrapNodesDat(file);
							file.Close();
							return;
						}
					}
					if (fileVersion >= 1 && fileVersion <= 3) {
						numContacts = file.ReadUInt32();
					}
				}
			} else {
				// Don't read version 0 nodes.dat files, because they can't tell the kad
				// version of the contacts stored.
				AddLogLineC(_("Failed to read nodes.dat file - too old. This version (0) is "
					      "not supported anymore."));
				numContacts = 0;
			}
			DEBUG_ONLY(unsigned kad1Count = 0;)
			if (numContacts != 0 && numContacts * 25 <= (file.GetLength() - file.GetPosition())) {
				for (uint32_t i = 0; i < numContacts; i++) {
					CUInt128 id = file.ReadUInt128();
					uint32_t ip = file.ReadUInt32();
					uint16_t udpPort = file.ReadUInt16();
					uint16_t tcpPort = file.ReadUInt16();
					uint8_t contactVersion = 0;
					contactVersion = file.ReadUInt8();
					CKadUDPKey kadUDPKey;
					bool verified = false;
					if (fileVersion >= 2) {
						kadUDPKey.ReadFromFile(file);
						verified = file.ReadUInt8() != 0;
						if (verified) {
							doHaveVerifiedContacts = true;
						}
					}
					// IP appears valid
					if (contactVersion > 1) {
						if (IsGoodIPPort(wxUINT32_SWAP_ALWAYS(ip), udpPort)) {
							if (!theApp->ipfilter->IsFiltered(
								    wxUINT32_SWAP_ALWAYS(ip)) &&
								!(
									udpPort == 53 && contactVersion <= 5 /*No DNS Port without encryption*/)) {
								// This was not a dead contact, inc counter if
								// add was successful
								if (AddUnfiltered(id,
									    ip,
									    udpPort,
									    tcpPort,
									    contactVersion,
									    kadUDPKey,
									    verified,
									    false,
									    false)) {
									validContacts++;
								}
							}
						}
					} else {
						DEBUG_ONLY(kad1Count++;)
					}
				}
			}
			file.Close();
			AddLogLineN(CFormat(wxPLURAL(
					    "Read %u Kad contact", "Read %u Kad contacts", validContacts)) %
				    validContacts);
#ifdef __DEBUG__
			if (kad1Count > 0) {
				AddDebugLogLineN(logKadRouting,
					CFormat("Ignored %u kad1 %s in nodes.dat file.") % kad1Count %
						(kad1Count > 1 ? "contacts" : "contact"));
			}
#endif
			if (!doHaveVerifiedContacts) {
				AddDebugLogLineN(logKadRouting,
					"No verified contacts found in nodes.dat - might be an old file "
					"version. Setting all contacts verified for this time to speed up "
					"Kad bootstrapping.");
				SetAllContactsVerified();
			}
		}
		if (validContacts == 0) {
			AddLogLineC(_("No contacts found, please bootstrap, or download a nodes.dat file."));
		}
	} catch (const CSafeIOException &DEBUG_ONLY(e)) {
		AddDebugLogLineN(logKadRouting, "IO error in CRoutingZone::readFile: " + e.what());
	}
}

void CRoutingZone::ReadBootstrapNodesDat(CFileDataIO &file)
{
	// Bootstrap versions of nodes.dat are in the style of version 1 nodes.dats, but hold
	// 500-1000 contacts instead of 50, and those contacts are not added to the routing table --
	// they are only sent Bootstrap packets. On a list with a high ratio of dead nodes that
	// bootstraps faster, and it avoids the DDOS that shipping a normal nodes.dat would cause,
	// where everyone adds the same 50 nodes to their routing table. Here we ask one of the 1000
	// contacts, until one is alive.
	if (!CKademlia::s_bootstrapList.empty()) {
		wxFAIL;
		return;
	}
	uint32_t numContacts = file.ReadUInt32();
	if (numContacts != 0 && numContacts * 25 == (file.GetLength() - file.GetPosition())) {
		uint32_t validContacts = 0;
		while (numContacts) {
			CUInt128 id = file.ReadUInt128();
			uint32_t ip = file.ReadUInt32();
			uint16_t udpPort = file.ReadUInt16();
			uint16_t tcpPort = file.ReadUInt16();
			uint8_t contactVersion = file.ReadUInt8();

			if (::IsGoodIPPort(wxUINT32_SWAP_ALWAYS(ip), udpPort)) {
				if (!theApp->ipfilter->IsFiltered(wxUINT32_SWAP_ALWAYS(ip)) &&
					!(udpPort == 53 && contactVersion <= 5) &&
					(contactVersion > 1)) // only kad2 nodes
				{
					// The 50 nodes closest to our own ID: that gives randomness
					// between users and a good chance of bootstrapping with
					// close nodes.
					CUInt128 distance = me;
					distance ^= id;
					validContacts++;
					// don't bother if we already have 50 and the farthest distance is
					// smaller than this contact
					if (CKademlia::s_bootstrapList.size() < 50 ||
						CKademlia::s_bootstrapList.back()->GetDistance() > distance) {
						// look where to put this contact into the proper position
						bool inserted = false;
						CContact *contact = new CContact(id,
							ip,
							udpPort,
							tcpPort,
							contactVersion,
							0,
							false,
							me);
						for (ContactList::iterator it =
								CKademlia::s_bootstrapList.begin();
							it != CKademlia::s_bootstrapList.end();
							++it) {
							if ((*it)->GetDistance() > distance) {
								CKademlia::s_bootstrapList.insert(
									it, contact);
								inserted = true;
								break;
							}
						}
						if (!inserted) {
							CKademlia::s_bootstrapList.push_back(contact);
						} else if (CKademlia::s_bootstrapList.size() > 50) {
							delete CKademlia::s_bootstrapList.back();
							CKademlia::s_bootstrapList.pop_back();
						}
					}
				}
			}
			numContacts--;
		}
		AddLogLineN(CFormat(wxPLURAL("Read %u Kad contact",
				    "Read %u Kad contacts",
				    CKademlia::s_bootstrapList.size())) %
			    CKademlia::s_bootstrapList.size());
		AddDebugLogLineN(logKadRouting,
			CFormat("Loaded Bootstrap nodes.dat, selected %u out of %u valid contacts") %
				CKademlia::s_bootstrapList.size() % validContacts);
	}
	if (CKademlia::s_bootstrapList.size() == 0) {
		AddLogLineC(_("No contacts found, please bootstrap, or download a nodes.dat file."));
	}
}

void CRoutingZone::WriteFile()
{
	// don't overwrite a bootstrap nodes.dat with an empty one, if we didn't finish probing
	if (!CKademlia::s_bootstrapList.empty() && GetNumContacts() == 0) {
		AddDebugLogLineN(logKadRouting,
			"Skipped storing nodes.dat, because we have an unfinished bootstrap of the nodes.dat "
			"version and no contacts in our routing table");
		return;
	}

	// The bootstrap method gets a very nice sample of contacts to save.
	ContactList contacts;
	GetBootstrapContacts(&contacts, 200);
	ContactList::size_type numContacts = contacts.size();
	numContacts = std::min<ContactList::size_type>(
		numContacts, CONTACT_FILE_LIMIT); // safety precaution, should not be above
	if (numContacts < 25) {
		AddLogLineN(CFormat(wxPLURAL("Only %d Kad contact available, nodes.dat not written",
				    "Only %d Kad contacts available, nodes.dat not written",
				    numContacts)) %
			    numContacts);
		return;
	}
	try {
		unsigned int count = 0;
		CFile file;
		if (file.Open(m_filename, CFile::write_safe)) {
			file.WriteUInt32(0);
			file.WriteUInt32(2);
			// file.WriteUInt32(0); // if we would use version >= 3 this would mean that this is a
			// normal nodes.dat
			file.WriteUInt32(numContacts);
			for (ContactList::const_iterator it = contacts.begin(); it != contacts.end(); ++it) {
				CContact *c = *it;
				count++;
				if (count > CONTACT_FILE_LIMIT) {
					// This should never happen
					wxFAIL;
					break;
				}
				file.WriteUInt128(c->GetClientID());
				file.WriteUInt32(c->GetIPAddress());
				file.WriteUInt16(c->GetUDPPort());
				file.WriteUInt16(c->GetTCPPort());
				file.WriteUInt8(c->GetVersion());
				c->GetUDPKey().StoreToFile(file);
				file.WriteUInt8(c->IsIPVerified() ? 1 : 0);
			}
			file.Close();
		}
		AddLogLineN(
			CFormat(wxPLURAL("Wrote %d Kad contact", "Wrote %d Kad contacts", count)) % count);
	} catch (const CIOFailureException &e) {
		AddDebugLogLineC(logKadRouting, "IO failure in CRoutingZone::writeFile: " + e.what());
	}
}

#if 0
void CRoutingZone::WriteBootstrapFile()
{
	AddDebugLogLineC(logKadRouting, "Writing special bootstrap nodes.dat - not intended for normal use");
	try {
		CUInt128 id;
		CFile file;
		if (file.Open(m_filename, CFile::write)) {
			ContactMap mapContacts;
			CUInt128 random(CUInt128((uint32_t)0), 0);
			CUInt128 distance = random;
			distance ^= me;
			GetClosestTo(2, random, distance, 1200, &mapContacts, false, false);
			// filter out Kad1 nodes
			for (ContactMap::iterator it = mapContacts.begin(); it != mapContacts.end(); ) {
				ContactMap::iterator itCur = it++;
				CContact* contact = itCur->second;
				if (contact->GetVersion() <= 1) {
					mapContacts.erase(itCur);
				}
			}
			file.WriteUInt32(0);
			file.WriteUInt32(3);
			file.WriteUInt32(1); // using version >= 3, this means that this is not a normal nodes.dat
			file.WriteUInt32((uint32_t)mapContacts.size());
			for (ContactMap::const_iterator it = mapContacts.begin(); it != mapContacts.end(); ++it)
			{
				CContact* contact = it->second;
				file.WriteUInt128(contact->GetClientID());
				file.WriteUInt32(contact->GetIPAddress());
				file.WriteUInt16(contact->GetUDPPort());
				file.WriteUInt16(contact->GetTCPPort());
				file.WriteUInt8(contact->GetVersion());
			}
			file.Close();
			AddDebugLogLineN(logKadRouting, CFormat("Wrote %u contacts to bootstrap file.") % mapContacts.size());
		} else {
			AddDebugLogLineC(logKadRouting, "Unable to store Kad file: " + m_filename);
		}
	} catch (const CIOFailureException& e) {
		AddDebugLogLineC(logKadRouting, "CFileException in CRoutingZone::writeFile" + e.what());
	}
}
#endif

bool CRoutingZone::CanSplit() const noexcept
{
	if (m_level >= 127) {
		return false;
	}

	return ((m_zoneIndex < KK || m_level < KBASE) && m_bin->GetSize() == K);
}

// Returns true if a contact was added or updated, false if the routing table was not touched.
bool CRoutingZone::Add(const CUInt128 &id,
	uint32_t ip,
	uint16_t port,
	uint16_t tport,
	uint8_t version,
	const CKadUDPKey &key,
	bool &ipVerified,
	bool update,
	bool fromHello)
{
	if (IsGoodIPPort(wxUINT32_SWAP_ALWAYS(ip), port)) {
		if (!theApp->ipfilter->IsFiltered(wxUINT32_SWAP_ALWAYS(ip)) &&
			!(port == 53 && version <= 5) /*No DNS Port without encryption*/) {
			return AddUnfiltered(
				id, ip, port, tport, version, key, ipVerified, update, fromHello);
		}
	}
	return false;
}

// Returns true if a contact was added or updated, false if the routing table was not touched.
bool CRoutingZone::AddUnfiltered(const CUInt128 &id,
	uint32_t ip,
	uint16_t port,
	uint16_t tport,
	uint8_t version,
	const CKadUDPKey &key,
	bool &ipVerified,
	bool update,
	bool fromHello)
{
	if (id != me) {
#ifdef ENABLE_KAD_NODE_PROTECTION
		// Kad identity protections. This is the routing table's front door, so it is where
		// an address that rotates Kad IDs faster than once an hour, or one banned for
		// having done so, has to be turned away.
		//
		// No upstream counterpart: eMuleAI and emule-qt each call IsBadNode() in exactly
		// one place, the search answer, and neither gates routing table admission with it.
		// That is why the switch must not default to ON without evidence: the table
		// deciding who may enter is what Kad health rests on, and a heuristic even slightly
		// too eager fails quietly, as a node that gradually stops finding peers. Measure
		// routing table size and contact churn against a control node before changing the
		// default.
		//
		// onlyOneNodePerIP is deliberately off: CRoutingBin already caps the table at
		// MAX_CONTACTS_IP Kad ID per address plus MAX_CONTACTS_SUBNET per /24, and
		// duplicating that here would be a second, weaker copy of the same rule.
		if (safeKad.IsBadNode(ip, port, id, version, ipVerified, false, time(nullptr))) {
			AddDebugLogLineN(logKadRouting,
				"Ignored kad contact (IP=" + KadIPPortToString(ip, port) +
					") - rejected by the Kad identity protections");
			return false;
		}
#endif

		CContact *contact = new CContact(id, ip, port, tport, version, key, ipVerified);
		if (fromHello) {
			contact->SetReceivedHelloPacket();
		}
		if (Add(contact, update, ipVerified)) {
			wxASSERT(!update);
			return true;
		} else {
			delete contact;
			return update;
		}
	}
	return false;
}

bool CRoutingZone::Add(CContact *contact, bool &update, bool &outIpVerified)
{
	if (!IsLeaf()) {
		return m_subZones[contact->GetDistance().GetBitNumber(m_level)]->Add(
			contact, update, outIpVerified);
	} else {
		CContact *contactUpdate = m_bin->GetContact(contact->GetClientID());
		if (contactUpdate) {
			if (update) {
				if (contactUpdate->GetUDPKey().GetKeyValue(theApp->GetPublicIP(false)) != 0 &&
					contactUpdate->GetUDPKey().GetKeyValue(theApp->GetPublicIP(false)) !=
						contact->GetUDPKey().GetKeyValue(
							theApp->GetPublicIP(false))) {
					// If the existing contact has a UDPSender-Key -- which
					// every >= 0.49a client should, unless our IP changed
					// recently -- demand that it matches the key from the
					// packet wanting to update it, so this is not a hijack
					// attempt.
					AddDebugLogLineN(logKadRouting,
						"Sender (" + KadIPToString(contact->GetIPAddress()) +
							") tried to update contact entry but failed to "
							"provide the proper sender key (Sent Empty: " +
							(contact->GetUDPKey().GetKeyValue(
								 theApp->GetPublicIP(false)) == 0
									? "Yes"
									: "No") +
							") for the entry (" +
							KadIPToString(contactUpdate->GetIPAddress()) +
							") - denying update");
					update = false;
				} else if (contactUpdate->GetVersion() >= 1 &&
					   contactUpdate->GetVersion() < 6 &&
					   contactUpdate->GetReceivedHelloPacket()) {
					// Legacy kad2 contacts may only update their RefreshTimer,
					// so an attacker cannot hijack or corrupt them. kad1
					// contacts have no such restriction, as they might turn out
					// to be kad2 later on; the only other exception is not
					// having received a HELLO from this client yet.
					if (contactUpdate->GetIPAddress() == contact->GetIPAddress() &&
						contactUpdate->GetTCPPort() == contact->GetTCPPort() &&
						contactUpdate->GetVersion() == contact->GetVersion() &&
						contactUpdate->GetUDPPort() == contact->GetUDPPort()) {
						wxASSERT(
							!contact->IsIPVerified()); // legacy kad2 nodes should
										   // be unable to verify
										   // their IP on a HELLO
						outIpVerified = contactUpdate->IsIPVerified();
						m_bin->SetAlive(contactUpdate);
						AddDebugLogLineN(logKadRouting,
							CFormat("Updated kad contact refreshtimer only for "
								"legacy kad2 contact (%s, %u)") %
								KadIPToString(contactUpdate->GetIPAddress()) %
								contactUpdate->GetVersion());
					} else {
						AddDebugLogLineN(logKadRouting,
							CFormat("Rejected value update for legacy kad2 "
								"contact (%s -> %s, %u -> %u)") %
								KadIPToString(contactUpdate->GetIPAddress()) %
								KadIPToString(contact->GetIPAddress()) %
								contactUpdate->GetVersion() %
								contact->GetVersion());
						update = false;
					}
				} else {
#ifdef __DEBUG__
					// just for outlining, gets removed anyway
					if (contact->GetUDPKey().GetKeyValue(theApp->GetPublicIP(false)) ==
						0) {
						if (contact->GetVersion() >= 6 && contact->GetType() < 2) {
							AddDebugLogLineN(logKadRouting,
								"Updating > 0.49a + type < 2 contact without "
								"valid key stored " +
									KadIPToString(
										contact->GetIPAddress()));
						}
					} else {
						AddDebugLogLineN(logKadRouting,
							"Updating contact, passed key check " +
								KadIPToString(contact->GetIPAddress()));
					}

					if (contactUpdate->GetVersion() >= 1 &&
						contactUpdate->GetVersion() < 6) {
						wxASSERT(!contactUpdate->GetReceivedHelloPacket());
						AddDebugLogLineN(logKadRouting,
							CFormat("Accepted update for legacy kad2 contact, "
								"because of first HELLO (%s -> %s, %u -> "
								"%u)") %
								KadIPToString(contactUpdate->GetIPAddress()) %
								KadIPToString(contact->GetIPAddress()) %
								contactUpdate->GetVersion() %
								contact->GetVersion());
					}
#endif
					// All other nodes (Kad1, Kad2 > 0.49a with UDPKey checked
					// or not set, first hello updates) may do full updates. Do
					// not let Kad1 responses overwrite Kad2 ones.
					if (m_bin->ChangeContactIPAddress(
						    contactUpdate, contact->GetIPAddress()) &&
						contact->GetVersion() >= contactUpdate->GetVersion()) {
						contactUpdate->SetUDPPort(contact->GetUDPPort());
						contactUpdate->SetTCPPort(contact->GetTCPPort());
						contactUpdate->SetVersion(contact->GetVersion());
						contactUpdate->SetUDPKey(contact->GetUDPKey());
						// don't unset the verified flag (will clear itself on
						// ipchanges)
						if (!contactUpdate->IsIPVerified()) {
							contactUpdate->SetIPVerified(contact->IsIPVerified());
						}
						outIpVerified = contactUpdate->IsIPVerified();
						m_bin->SetAlive(contactUpdate);
						if (contact->GetReceivedHelloPacket()) {
							contactUpdate->SetReceivedHelloPacket();
						}
					} else {
						update = false;
					}
				}
			}
			return false;
		} else if (m_bin->GetRemaining()) {
			update = false;
			return m_bin->AddContact(contact);
		} else if (CanSplit()) {
			Split();
			return m_subZones[contact->GetDistance().GetBitNumber(m_level)]->Add(
				contact, update, outIpVerified);
		} else {
			update = false;
			return false;
		}
	}
}

CContact *CRoutingZone::GetContact(const CUInt128 &id) const noexcept
{
	if (IsLeaf()) {
		return m_bin->GetContact(id);
	} else {
		CUInt128 distance = CKademlia::GetPrefs()->GetKadID();
		distance ^= id;
		return m_subZones[distance.GetBitNumber(m_level)]->GetContact(id);
	}
}

CContact *CRoutingZone::GetContact(uint32_t ip, uint16_t port, bool tcpPort) const noexcept
{
	if (IsLeaf()) {
		return m_bin->GetContact(ip, port, tcpPort);
	} else {
		CContact *contact = m_subZones[0]->GetContact(ip, port, tcpPort);
		return (contact != NULL) ? contact : m_subZones[1]->GetContact(ip, port, tcpPort);
	}
}

CContact *CRoutingZone::GetRandomContact(uint32_t maxType, uint32_t minKadVersion) const
{
	if (IsLeaf()) {
		return m_bin->GetRandomContact(maxType, minKadVersion);
	} else {
		unsigned zone = GetRandomUint16() & 1 /* GetRandomUint16() % 2 */;
		CContact *contact = m_subZones[zone]->GetRandomContact(maxType, minKadVersion);
		return (contact != NULL) ? contact
					 : m_subZones[1 - zone]->GetRandomContact(maxType, minKadVersion);
	}
}

void CRoutingZone::GetClosestTo(uint32_t maxType,
	const CUInt128 &target,
	const CUInt128 &distance,
	uint32_t maxRequired,
	ContactMap *result,
	bool emptyFirst,
	bool inUse) const
{
	if (IsLeaf()) {
		m_bin->GetClosestTo(maxType, target, maxRequired, result, emptyFirst, inUse);
		return;
	}

	// otherwise, recurse in the closer-to-the-target subzone first
	int closer = distance.GetBitNumber(m_level);
	m_subZones[closer]->GetClosestTo(maxType, target, distance, maxRequired, result, emptyFirst, inUse);

	// if still not enough tokens found, recurse in the other subzone too
	if (result->size() < maxRequired) {
		m_subZones[1 - closer]->GetClosestTo(
			maxType, target, distance, maxRequired, result, false, inUse);
	}
}

void CRoutingZone::GetAllEntries(ContactList *result, bool emptyFirst) const
{
	if (IsLeaf()) {
		m_bin->GetEntries(result, emptyFirst);
	} else {
		m_subZones[0]->GetAllEntries(result, emptyFirst);
		m_subZones[1]->GetAllEntries(result, false);
	}
}

void CRoutingZone::TopDepth(int depth, ContactList *result, bool emptyFirst) const
{
	if (IsLeaf()) {
		m_bin->GetEntries(result, emptyFirst);
	} else if (depth <= 0) {
		RandomBin(result, emptyFirst);
	} else {
		m_subZones[0]->TopDepth(depth - 1, result, emptyFirst);
		m_subZones[1]->TopDepth(depth - 1, result, false);
	}
}

void CRoutingZone::RandomBin(ContactList *result, bool emptyFirst) const
{
	if (IsLeaf()) {
		m_bin->GetEntries(result, emptyFirst);
	} else {
		m_subZones[rand() & 1]->RandomBin(result, emptyFirst);
	}
}

uint32_t CRoutingZone::GetMaxDepth() const noexcept
{
	if (IsLeaf()) {
		return 0;
	}
	return 1 + std::max(m_subZones[0]->GetMaxDepth(), m_subZones[1]->GetMaxDepth());
}

void CRoutingZone::Split()
{
	StopTimer();

	m_subZones[0] = GenSubZone(0);
	m_subZones[1] = GenSubZone(1);

	ContactList entries;
	m_bin->GetEntries(&entries);
	m_bin->m_dontDeleteContacts = true;
	delete m_bin;
	m_bin = NULL;

	for (ContactList::const_iterator it = entries.begin(); it != entries.end(); ++it) {
		if (!m_subZones[(*it)->GetDistance().GetBitNumber(m_level)]->m_bin->AddContact(*it)) {
			delete *it;
		}
	}
}

uint32_t CRoutingZone::Consolidate()
{
	uint32_t mergeCount = 0;

	if (IsLeaf()) {
		return mergeCount;
	}

	wxASSERT(m_bin == NULL);

	if (!m_subZones[0]->IsLeaf()) {
		mergeCount += m_subZones[0]->Consolidate();
	}
	if (!m_subZones[1]->IsLeaf()) {
		mergeCount += m_subZones[1]->Consolidate();
	}

	if (m_subZones[0]->IsLeaf() && m_subZones[1]->IsLeaf() && GetNumContacts() < K / 2) {
		m_bin = new CRoutingBin();

		m_subZones[0]->StopTimer();
		m_subZones[1]->StopTimer();

		ContactList list0;
		ContactList list1;
		m_subZones[0]->m_bin->GetEntries(&list0);
		m_subZones[1]->m_bin->GetEntries(&list1);

		m_subZones[0]->m_bin->m_dontDeleteContacts = true;
		m_subZones[1]->m_bin->m_dontDeleteContacts = true;

		delete m_subZones[0];
		delete m_subZones[1];

		m_subZones[0] = NULL;
		m_subZones[1] = NULL;

		for (ContactList::const_iterator it = list0.begin(); it != list0.end(); ++it) {
			m_bin->AddContact(*it);
		}
		for (ContactList::const_iterator it = list1.begin(); it != list1.end(); ++it) {
			m_bin->AddContact(*it);
		}

		StartTimer();

		mergeCount++;
	}
	return mergeCount;
}

CRoutingZone *CRoutingZone::GenSubZone(unsigned side)
{
	wxASSERT(side <= 1);

	CUInt128 newIndex(m_zoneIndex);
	newIndex <<= 1;
	newIndex += side;
	return new CRoutingZone(this, m_level + 1, newIndex);
}

void CRoutingZone::StartTimer()
{
	// Start filling the tree, closest bins first.
	m_nextBigTimer = time(NULL) + SEC(10);
	CKademlia::AddEvent(this);
}

void CRoutingZone::StopTimer()
{
	CKademlia::RemoveEvent(this);
}

bool CRoutingZone::OnBigTimer() const
{
	if (IsLeaf() && (m_zoneIndex < KK || m_level < KBASE || m_bin->GetRemaining() >= (K * 0.8))) {
		RandomLookup();
		return true;
	}

	return false;
}

// Used when we find a leaf and want to know what this sample looks like. We fall back two levels
// and take a sample, to minimize areas of the tree that would give very bad results.
uint32_t CRoutingZone::EstimateCount() const
{
	if (!IsLeaf()) {
		return 0;
	}

	if (m_level < KBASE) {
		return (uint32_t)(pow(2.0, (int)m_level) * K);
	}

	CRoutingZone *curZone = m_superZone->m_superZone->m_superZone;

	float modify = ((float)curZone->GetNumContacts()) / (float)(K * 2);

	// First calculate users assuming the tree is full, then modify the count by bin size and by
	// how full the tree actually is.

	// LowIDModififier. Assume 20% of users are firewalled and cannot be a contact for < 0.49b
	// nodes; for >= 0.49b use the actual firewalled ratio when we are not firewalled ourselves,
	// or 40% when we are (the real figure on Kad is 35-55%).
	const float firewalledModifyOld = 1.20f;
	float firewalledModifyNew = 0;
	if (CUDPFirewallTester::IsFirewalledUDP(true)) {
		firewalledModifyNew = 1.40f; // we are firewalled and can't get the real statistics, assume
					     // 40% firewalled >=0.49b nodes
	} else if (CKademlia::GetPrefs()->StatsGetFirewalledRatio(true) > 0) {
		firewalledModifyNew = 1.0 + (CKademlia::GetPrefs()->StatsGetFirewalledRatio(
						    true)); // apply the firewalled ratio to the modify
		wxASSERT(firewalledModifyNew > 1.0 && firewalledModifyNew < 1.90);
	}
	float newRatio = CKademlia::GetPrefs()->StatsGetKadV8Ratio();
	float firewalledModifyTotal = 0;
	if (newRatio > 0 &&
		firewalledModifyNew >
			0) { // weight the old and the new modifier based on how many new contacts we have
		firewalledModifyTotal =
			(newRatio * firewalledModifyNew) + ((1 - newRatio) * firewalledModifyOld);
	} else {
		firewalledModifyTotal = firewalledModifyOld;
	}
	wxASSERT(firewalledModifyTotal > 1.0 && firewalledModifyTotal < 1.90);

	return (uint32_t)(pow(2.0, (int)m_level - 2) * (float)K * modify * firewalledModifyTotal);
}

void CRoutingZone::OnSmallTimer()
{
	if (!IsLeaf()) {
		return;
	}

	CContact *c = NULL;
	time_t now = time(NULL);
	ContactList entries;

	// Remove dead entries
	m_bin->GetEntries(&entries);
	for (ContactList::iterator it = entries.begin(); it != entries.end(); ++it) {
		c = *it;
#ifdef ENABLE_KAD_NODE_PROTECTION
		// A banned address is swept out of the table, not merely refused re-entry. Without
		// this the ban only applies to contacts we have yet to learn, and one already
		// sitting in the table keeps being asked -- which is the node the ban was about.
		// Folded into the dead-entry pass rather than given a sweep of its own, because
		// this loop already walks every entry once a minute and already owns the InUse()
		// rule that keeps a contact alive while a search holds it.
		//
		// Safe only because escalation now requires a verified identity: while an
		// unverified flip could ban, this removal would have let two fabricated mentions
		// evict an honest contact rather than merely block its return.
		if (safeKad.IsBanned(c->GetIPAddress(), now)) {
			if (!c->InUse()) {
				m_bin->RemoveContact(c);
				delete c;
			}
			continue;
		}
#endif
		if (c->GetType() == 4) {
			if ((c->GetExpireTime() > 0) && (c->GetExpireTime() <= now)) {
				if (!c->InUse()) {
					m_bin->RemoveContact(c);
					delete c;
				}
				continue;
			}
		}
		if (c->GetExpireTime() == 0) {
			c->SetExpireTime(now);
		}
	}

	c = m_bin->GetOldest();
	if (c != NULL) {
		if (c->GetExpireTime() >= now || c->GetType() == 4) {
			m_bin->PushToBottom(c);
			c = NULL;
		}
	}

	if (c != NULL) {
		c->CheckingType();
		if (c->GetVersion() >= 6) {
			DebugSend(Kad2HelloReq, c->GetIPAddress(), c->GetUDPPort());
			CUInt128 clientID = c->GetClientID();
			CKademlia::GetUDPListener()->SendMyDetails(KADEMLIA2_HELLO_REQ,
				c->GetIPAddress(),
				c->GetUDPPort(),
				c->GetVersion(),
				c->GetUDPKey(),
				&clientID,
				false);
			if (c->GetVersion() >= 8) {
				// FIXME: a work-around for statistic values. Normally we only count
				// values from incoming HELLO_REQs for the firewalled statistics, to
				// get numbers from nodes which have us in their routing table; but
				// if we send a HELLO on the timer, the remote node sends no
				// HELLO_REQ of its own (only a HELLO_RES, which we do not count),
				// so count those statistics here. Not really accurate, but fair
				// enough. Could be improved later, for example by flagging the
				// contact and counting the answer.
				CKademlia::GetPrefs()->StatsIncUDPFirewalledNodes(false);
				CKademlia::GetPrefs()->StatsIncTCPFirewalledNodes(false);
			}
		} else if (c->GetVersion() >= 2) {
			DebugSend(Kad2HelloReq, c->GetIPAddress(), c->GetUDPPort());
			CKademlia::GetUDPListener()->SendMyDetails(KADEMLIA2_HELLO_REQ,
				c->GetIPAddress(),
				c->GetUDPPort(),
				c->GetVersion(),
				0,
				NULL,
				false);
			wxASSERT(c->GetUDPKey() == CKadUDPKey(0));
		} else {
			AddDebugLogLineN(logKadRouting,
				CFormat("Ignoring Kad contact %s version %d.") %
					KadIPToString(c->GetIPAddress()) % c->GetVersion());
			// wxFAIL;	// thanks, I'm having enough problems without any Kad asserts
		}
	}
}

void CRoutingZone::RandomLookup() const
{
	// Look-up a random client in this zone
	CUInt128 prefix(m_zoneIndex);
	prefix <<= 128 - m_level;
	CUInt128 random(prefix, m_level);
	random ^= me;
	CSearchManager::FindNode(random, false);
}

uint32_t CRoutingZone::GetNumContacts() const noexcept
{
	if (IsLeaf()) {
		return m_bin->GetSize();
	} else {
		return m_subZones[0]->GetNumContacts() + m_subZones[1]->GetNumContacts();
	}
}

void CRoutingZone::GetNumContacts(
	uint32_t &nInOutContacts, uint32_t &nInOutFilteredContacts, uint8_t minVersion) const noexcept
{
	if (IsLeaf()) {
		m_bin->GetNumContacts(nInOutContacts, nInOutFilteredContacts, minVersion);
	} else {
		m_subZones[0]->GetNumContacts(nInOutContacts, nInOutFilteredContacts, minVersion);
		m_subZones[1]->GetNumContacts(nInOutContacts, nInOutFilteredContacts, minVersion);
	}
}

uint32_t CRoutingZone::GetBootstrapContacts(ContactList *results, uint32_t maxRequired) const
{
	wxASSERT(m_superZone == NULL);

	results->clear();

	uint32_t count = 0;
	ContactList top;
	TopDepth(LOG_BASE_EXPONENT, &top);
	if (!top.empty()) {
		for (ContactList::const_iterator it = top.begin(); it != top.end(); ++it) {
			results->push_back(*it);
			count++;
			if (count == maxRequired) {
				break;
			}
		}
	}

	return count;
}

bool CRoutingZone::VerifyContact(const CUInt128 &id, uint32_t ip)
{
	CContact *contact = GetContact(id);
	if (contact == NULL) {
		return false;
	} else if (ip != contact->GetIPAddress()) {
		return false;
	} else {
		if (contact->IsIPVerified()) {
			AddDebugLogLineN(
				logKadRouting, "Sender already verified (sender: " + KadIPToString(ip) + ")");
		} else {
			contact->SetIPVerified(true);
		}
#ifdef ENABLE_KAD_NODE_PROTECTION
		// The three-way handshake has just proved that this address stands behind this Kad
		// ID. Recording it verified is what makes a later unverified claim of a different
		// ID for the same address rejectable rather than merely rate-limited.
		bool newlyBanned = false;
		safeKad.TrackNode(ip, contact->GetUDPPort(), id, true, time(nullptr), &newlyBanned);
		if (newlyBanned) {
			// The one event in this subsystem worth a line without debug logging on: a
			// ban is why a peer stops appearing, and until now it left no trace
			// anywhere. Logged here rather than in CSafeKad because that class links
			// against nothing.
			AddDebugLogLineN(logKadNodeTracking,
				CFormat("Kad: banned %s after a second rejected identity change; "
					"%u address(es) now banned") %
					KadIPToString(ip) % (unsigned)safeKad.GetBannedAddressCount());
		}
#endif
		return true;
	}
}

void CRoutingZone::SetAllContactsVerified()
{
	if (IsLeaf()) {
		m_bin->SetAllContactsVerified();
	} else {
		m_subZones[0]->SetAllContactsVerified();
		m_subZones[1]->SetAllContactsVerified();
	}
}

bool CRoutingZone::IsAcceptableContact(const CContact *toCheck) const
{
	// Check whether we know a contact with the same ID or IP but a non-matching IP/ID, and the
	// other limitations -- similar checks to adding a node to the table, except duplicates are
	// allowed. Used to check KADEMLIA_RES routing answers on searches.
	if (toCheck->GetVersion() <= 1) {
		// No Kad1 contacts allowed
		return false;
	}
	CContact *duplicate = GetContact(toCheck->GetClientID());
	if (duplicate != NULL) {
		if ((duplicate->IsIPVerified() && duplicate->GetIPAddress() != toCheck->GetIPAddress()) ||
			duplicate->GetUDPPort() != toCheck->GetUDPPort()) {
			// already existing verified node with different IP
			return false;
		} else {
			// node exists already in our routing table, that's fine
			return true;
		}
	}
	// if the node is not yet known, check if our IP limitations would hit
	return CRoutingBin::CheckGlobalIPLimits(toCheck->GetIPAddress(), toCheck->GetUDPPort());
}

bool CRoutingZone::HasOnlyLANNodes() const noexcept
{
	if (IsLeaf()) {
		return m_bin->HasOnlyLANNodes();
	} else {
		return m_subZones[0]->HasOnlyLANNodes() && m_subZones[1]->HasOnlyLANNodes();
	}
}
