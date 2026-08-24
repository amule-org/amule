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

bool CBrowseManager::Start(CUpDownClient *client, std::uint32_t searchId, std::uint64_t now)
{
	const browse::Store::StartResult result = m_store.Start(client, searchId, now);
	if (result.started) {
		m_clients[searchId].Link(client CLIENT_DEBUGSTRING("CBrowseManager::Start"));
	}
	// Only now release whatever this peer was attached to before, so the
	// reference map never holds two entries for one peer. Deliberately after
	// the link: CClientRef::Unlink deletes at a refcount of zero, so releasing
	// first would free `client` here if this map ever held the last reference.
	// It does not today -- clientlist keeps one for any browsable peer -- but
	// that is another class's invariant, and this order does not need it.
	Perform(result.displaced);
	return result.started;
}

void CBrowseManager::OnRequestSent(CUpDownClient *client, std::uint64_t now)
{
	m_store.Touch(client, now);
}

void CBrowseManager::OnDirectoryList(CUpDownClient *client, int dirCount, std::uint64_t now)
{
	Perform(m_store.OnDirectoryList(client, dirCount, now));
}

void CBrowseManager::OnListingReceived(CUpDownClient *client, std::uint64_t now)
{
	Perform(m_store.OnListingReceived(client, now));
}

void CBrowseManager::Fail(CUpDownClient *client)
{
	Perform(m_store.Fail(client));
}

void CBrowseManager::Forget(CUpDownClient *client)
{
	// Ordered: the failure is reported while the reference that names the peer
	// is still held, and only then released.
	for (const browse::Outcome &outcome : m_store.Forget(client)) {
		Perform(outcome);
	}
}

void CBrowseManager::Remove(std::uint32_t searchId)
{
	m_store.Remove(searchId);
	m_clients.erase(searchId);
}

void CBrowseManager::Process(std::uint64_t now)
{
	for (const browse::Outcome &outcome : m_store.Tick(now)) {
		Perform(outcome);
	}
}

void CBrowseManager::Perform(const browse::Outcome &outcome)
{
	// The ID travels with the effect, so there is no way for the two to
	// disagree -- which is how a release went missing when the manager looked
	// the ID up separately and the lookup excluded terminal browses.
	switch (outcome.effect) {
	case browse::Effect::Nothing:
		break;
	case browse::Effect::ReleaseClient:
		// Terminal and announced; the peer is free to be reaped. The record
		// stays until its search is freed.
		m_clients.erase(outcome.searchId);
		break;
	case browse::Effect::AnnounceFailure: {
		const auto it = m_clients.find(outcome.searchId);
		AddLogLineC(CFormat(_("Failed to retrieve shared files from user '%s'")) %
			    (it != m_clients.end() && it->second.IsLinked()
					    ? it->second.GetClient()->GetUserName()
					    : wxString()));
		Announce(outcome.searchId);
		break;
	}
	case browse::Effect::Announce:
		Announce(outcome.searchId);
		break;
	}
}

void CBrowseManager::Announce(std::uint32_t searchId)
{
	// The GUI's tab marker, and nothing else: every other consumer -- the EC
	// progress reply, the EC search listing, the monolithic bar -- reads this
	// manager rather than holding a copy to be kept in step.
	Notify_Browse_Status(static_cast<std::uint64_t>(searchId),
		m_store.StateOf(searchId) == browse::State::Finished ? BROWSE_FINISHED : BROWSE_FAILED);
}

std::uint32_t CBrowseManager::SearchIdFor(const CUpDownClient *client) const
{
	return m_store.SearchIdFor(client);
}

bool CBrowseManager::AwaitingDirectoryList(const CUpDownClient *client) const
{
	return m_store.AwaitingDirectoryList(client);
}

bool CBrowseManager::Has(std::uint32_t searchId) const
{
	return m_store.Has(searchId);
}

browse::State CBrowseManager::StateOf(std::uint32_t searchId) const
{
	return m_store.StateOf(searchId);
}

std::uint16_t CBrowseManager::BarValue(std::uint32_t searchId) const
{
	return m_store.BarValue(searchId);
}
