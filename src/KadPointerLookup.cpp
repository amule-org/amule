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

#include "KadPointerLookup.h"

#include "KadPointerPolicy.h" // Needed for KadPointerKey
#include "Logger.h"
#include "MD4Hash.h"
#include "NetworkFunctions.h" // Needed for Uint32_16toStringIP_Port
#include "Preferences.h"
#include "kademlia/kademlia/Search.h"
#include "kademlia/kademlia/SearchManager.h"
#include "kademlia/utils/UInt128.h"

#include <common/Format.h>

#include <ctime>

namespace
{
CKadPointerCache::Key SourceKeyOfTarget(const Kademlia::CUInt128 &target)
{
	CKadPointerCache::Key key;
	(target ^ Kademlia::CUInt128(true)).ToByteArray(key.data());
	return key;
}

wxString KeyToString(const CKadPointerCache::Key &key)
{
	return CMD4Hash(key.data()).Encode();
}

const char *VerdictName(EKadPointerVerdict verdict)
{
	switch (verdict) {
	case EKadPointerVerdict::Accepted:
		return "accepted";
	case EKadPointerVerdict::NotTypeThree:
		return "not type 3";
	case EKadPointerVerdict::OtherSource:
		return "another source";
	case EKadPointerVerdict::OtherSourceIP:
		return "another source IP";
	case EKadPointerVerdict::NoBuddyRoute:
		return "no buddy route";
	}
	return "?";
}
} // namespace

CKadPointerCache &KadPointerLookup::Cache()
{
	static CKadPointerCache cache;
	return cache;
}

CKadPointerCache::Key KadPointerLookup::KeyOf(const CMD4Hash &userHash)
{
	CKadPointerCache::Key key;
	std::copy_n(userHash.GetHash(), key.size(), key.begin());
	return key;
}

bool KadPointerLookup::Start(const CMD4Hash &userHash, uint32_t recordIP)
{
	Cache().Prune(static_cast<uint32_t>(time(nullptr)));
	uint8_t target[16];
	KadPointerKey(userHash.GetHash(), target);
	if (!Kademlia::CSearchManager::PrepareLookup(
		    Kademlia::CSearch::FINDPOINTER, true, Kademlia::CUInt128(target))) {
		AddDebugLogLineN(logKadBuddy,
			"Kad buddy pointer lookup for source " + userHash.Encode() + " did not start");
		return false;
	}
	Cache().Started(KeyOf(userHash), recordIP);
	AddDebugLogLineN(logKadBuddy, "Kad buddy pointer lookup started for source " + userHash.Encode());
	return true;
}

void KadPointerLookup::AddRecord(const Kademlia::CUInt128 &target, const SKadPointerRecord &record)
{
	const CKadPointerCache::Key key = SourceKeyOfTarget(target);
	const EKadPointerVerdict verdict = Cache().Add(key, record);
	AddDebugLogLineN(logKadBuddy,
		CFormat("Kad buddy pointer for source %s names buddy %s: %s") % KeyToString(key) %
			Uint32_16toStringIP_Port(record.buddy.ip, record.buddy.port) % VerdictName(verdict));
}

void KadPointerLookup::Finished(const Kademlia::CUInt128 &target)
{
	const CKadPointerCache::Key key = SourceKeyOfTarget(target);
	const uint32_t missSeconds = static_cast<uint32_t>(thePrefs::GetSourceReaskTime() / 1000);
	const CKadPointerCache::SOutcome outcome =
		Cache().Finished(key, static_cast<uint32_t>(time(nullptr)), missSeconds);
	AddDebugLogLineN(logKadBuddy,
		CFormat("Kad buddy pointer lookup for source %s ended: %s, buddies %u, records accepted %u, "
			"refused %u") %
			KeyToString(key) % (outcome.found ? "found" : "missed") % outcome.buddies %
			outcome.accepted % outcome.refused);
}
