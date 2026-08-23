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

#include "BrowseManager.h"

#include "GuiEvents.h"
#include "Logger.h"
#include "updownclient.h"

#include <common/Format.h>

#include <wx/intl.h> // _()
#include <protocol/ed2k/Constants.h>

#include <vector>

bool CBrowseManager::Start(
	CUpDownClient *client, std::uint32_t searchId, std::uint32_t peerEcid, std::uint64_t now)
{
	if (client == nullptr || searchId == 0) {
		return false;
	}
	if (FindFor(client) != m_browses.end()) {
		// One browse per peer: there is a single exchange with that client to
		// report on, so a second record could only ever describe the same one.
		return false;
	}
	Entry entry;
	entry.rec.searchId = searchId;
	entry.rec.peerEcid = peerEcid;
	entry.rec.state = browse::State::InProgress;
	entry.rec.outstanding = browse::kFlatBrowse;
	entry.rec.deadline = now + BROWSE_SILENCE_TIMEOUT;
	entry.client.Link(client CLIENT_DEBUGSTRING("CBrowseManager::Start"));
	m_browses[searchId] = entry;
	return true;
}

void CBrowseManager::OnRequestSent(CUpDownClient *client, std::uint64_t now)
{
	const auto it = FindFor(client);
	if (it != m_browses.end() && it->second.rec.state == browse::State::InProgress) {
		it->second.rec.deadline = now + BROWSE_SILENCE_TIMEOUT;
	}
}

void CBrowseManager::OnDirectoryList(CUpDownClient *client, int dirCount, std::uint64_t now)
{
	const auto it = FindFor(client);
	if (it == m_browses.end()) {
		return;
	}
	it->second.rec = browse::OnDirectoryList(it->second.rec, dirCount, now + BROWSE_SILENCE_TIMEOUT);
	// A peer answering "0 directories" has told us everything it intends to,
	// so this may already complete the browse.
	Apply(it, browse::Tick(it->second.rec, now));
}

void CBrowseManager::OnListingReceived(CUpDownClient *client, std::uint64_t now)
{
	const auto it = FindFor(client);
	if (it == m_browses.end()) {
		return;
	}
	it->second.rec = browse::OnListingReceived(it->second.rec, now + BROWSE_SILENCE_TIMEOUT);
	// Both protocol forms complete here -- the directory form when its count
	// reaches zero, the flat form on its single answer. Marking completion in
	// one place is what the packet handlers could not do, since only one of
	// them knew it was the last.
	Apply(it, browse::Tick(it->second.rec, now));
}

void CBrowseManager::Fail(CUpDownClient *client)
{
	const auto it = FindFor(client);
	if (it != m_browses.end()) {
		Apply(it, browse::Action::Expire);
	}
}

void CBrowseManager::Finish(CUpDownClient *client)
{
	const auto it = FindFor(client);
	if (it != m_browses.end()) {
		Apply(it, browse::Action::Complete);
	}
}

void CBrowseManager::Remove(std::uint32_t searchId)
{
	m_browses.erase(searchId);
}

void CBrowseManager::Forget(CUpDownClient *client)
{
	const auto it = FindFor(client);
	if (it == m_browses.end()) {
		return;
	}
	if (it->second.rec.state == browse::State::InProgress) {
		// The peer is going away mid-browse: that is a failure, and it has to
		// be reported like any other rather than vanishing.
		Apply(it, browse::Action::Expire);
	}
	// Let go of the client; the record outlives it, until the search is freed.
	if (it->second.client.IsLinked()) {
		it->second.client.Unlink();
	}
}

void CBrowseManager::Process(std::uint64_t now)
{
	for (auto it = m_browses.begin(); it != m_browses.end();) {
		const auto current = it++;
		// Apply may erase `current`; `it` already points past it.
		Apply(current, browse::Tick(current->second.rec, now));
	}
}

bool CBrowseManager::Apply(std::map<std::uint32_t, Entry>::iterator it, browse::Action action)
{
	if (action == browse::Action::None) {
		return false;
	}
	if (action != browse::Action::Drop) {
		const browse::State before = it->second.rec.state;
		it->second.rec = browse::ApplyAction(it->second.rec, action);
		if (it->second.rec.state == before) {
			// Already terminal; the transition was refused. Nothing to
			// report, and nothing to log -- the disconnect that follows a
			// completed browse must not be announced as a failure.
			return false;
		}
	}
	switch (action) {
	case browse::Action::None:
	case browse::Action::Complete:
	case browse::Action::Expire:
		break;
	case browse::Action::Drop:
		// Terminal and already reported. The RECORD stays: the search ID is
		// still listed, and every consumer asks this manager what state it is
		// in -- drop it here and a failed browse would fall back to "results
		// retained?" and report idle again, which is the bug this ownership
		// was meant to end. It is released with the search itself, in
		// Remove(). What can go now is the client reference, so a peer that
		// is finished with is free to be reaped.
		if (it->second.client.IsLinked()) {
			it->second.client.Unlink();
		}
		return false;
	}
	NotifyTransition(it->second);
	if (action == browse::Action::Expire) {
		AddLogLineC(CFormat(_("Failed to retrieve shared files from user '%s'")) %
			    (it->second.client.IsLinked() ? it->second.client.GetClient()->GetUserName()
							  : wxString()));
	}
	// Terminal now: hold it one more tick so readers that poll between the
	// transition and the next tick still see the final state, then Drop.
	return false;
}

void CBrowseManager::NotifyTransition(const Entry &entry)
{
	// The GUI's tab marker, and nothing else: every other consumer -- the EC
	// progress reply, the EC search listing, the monolithic bar -- reads this
	// manager rather than holding a copy to be kept in step.
	Notify_Browse_Status(static_cast<std::uint64_t>(entry.rec.searchId),
		entry.rec.state == browse::State::Finished ? BROWSE_FINISHED : BROWSE_FAILED);
}

std::map<std::uint32_t, CBrowseManager::Entry>::iterator CBrowseManager::FindFor(const CUpDownClient *client)
{
	if (client == nullptr) {
		return m_browses.end();
	}
	for (auto it = m_browses.begin(); it != m_browses.end(); ++it) {
		if (it->second.client.GetClient() == client) {
			return it;
		}
	}
	return m_browses.end();
}

std::uint32_t CBrowseManager::SearchIdFor(const CUpDownClient *client) const
{
	for (const auto &kv : m_browses) {
		if (kv.second.client.GetClient() == client &&
			kv.second.rec.state == browse::State::InProgress) {
			return kv.first;
		}
	}
	return 0;
}

bool CBrowseManager::Has(std::uint32_t searchId) const
{
	return m_browses.find(searchId) != m_browses.end();
}

browse::State CBrowseManager::StateOf(std::uint32_t searchId) const
{
	const auto it = m_browses.find(searchId);
	return it != m_browses.end() ? it->second.rec.state : browse::State::Failed;
}

std::uint16_t CBrowseManager::BarValue(std::uint32_t searchId) const
{
	const auto it = m_browses.find(searchId);
	return it != m_browses.end() ? browse::BarValue(it->second.rec) : 0xffff;
}

int CBrowseManager::Outstanding(std::uint32_t searchId) const
{
	const auto it = m_browses.find(searchId);
	return it != m_browses.end() ? it->second.rec.outstanding : 0;
}

std::vector<std::uint32_t> CBrowseManager::Ids() const
{
	std::vector<std::uint32_t> out;
	out.reserve(m_browses.size());
	for (const auto &kv : m_browses) {
		out.push_back(kv.first);
	}
	return out;
}
