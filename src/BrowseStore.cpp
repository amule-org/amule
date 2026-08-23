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

#include "BrowseStore.h"

namespace browse
{

bool Store::Start(ClientKey client, std::uint32_t searchId, std::uint32_t peerEcid, std::uint64_t now)
{
	if (client == nullptr || searchId == 0 || FindFor(client) != m_records.end()) {
		return false;
	}
	Held held;
	held.rec.searchId = searchId;
	held.rec.peerEcid = peerEcid;
	held.rec.state = State::InProgress;
	held.rec.outstanding = kFlatBrowse;
	held.rec.deadline = now + kSilenceTimeoutMs;
	held.client = client;
	m_records[searchId] = held;
	return true;
}

void Store::Touch(ClientKey client, std::uint64_t now)
{
	const auto it = FindFor(client);
	if (it != m_records.end() && it->second.rec.state == State::InProgress) {
		it->second.rec.deadline = now + kSilenceTimeoutMs;
	}
}

Effect Store::OnDirectoryList(ClientKey client, int dirCount, std::uint64_t now)
{
	const auto it = FindFor(client);
	if (it == m_records.end()) {
		return Effect::Nothing;
	}
	it->second.rec = browse::OnDirectoryList(it->second.rec, dirCount, now + kSilenceTimeoutMs);
	// A peer answering "no directories" has said everything it means to, so
	// this may complete the browse outright.
	return ApplyTo(it->second, ::browse::Tick(it->second.rec, now));
}

Effect Store::OnListingReceived(ClientKey client, std::uint64_t now)
{
	const auto it = FindFor(client);
	if (it == m_records.end()) {
		return Effect::Nothing;
	}
	it->second.rec = browse::OnListingReceived(it->second.rec, now + kSilenceTimeoutMs);
	// Both protocol forms complete here, from the outstanding count -- the
	// only thing that knows whether anything is still expected.
	return ApplyTo(it->second, ::browse::Tick(it->second.rec, now));
}

Effect Store::Fail(ClientKey client)
{
	const auto it = FindFor(client);
	return it == m_records.end() ? Effect::Nothing : ApplyTo(it->second, Action::Expire);
}

Effect Store::Finish(ClientKey client)
{
	const auto it = FindFor(client);
	return it == m_records.end() ? Effect::Nothing : ApplyTo(it->second, Action::Complete);
}

std::vector<Effect> Store::Forget(ClientKey client)
{
	std::vector<Effect> out;
	const auto it = FindFor(client);
	if (it == m_records.end()) {
		return out;
	}
	if (it->second.rec.state == State::InProgress) {
		// Going away mid-browse is a failure, and has to be reported like any
		// other rather than quietly vanishing.
		out.push_back(ApplyTo(it->second, Action::Expire));
	}
	it->second.client = nullptr;
	out.push_back(Effect::ReleaseClient);
	return out;
}

void Store::Remove(std::uint32_t searchId)
{
	m_records.erase(searchId);
}

std::vector<std::pair<std::uint32_t, Effect>> Store::Tick(std::uint64_t now)
{
	std::vector<std::pair<std::uint32_t, Effect>> out;
	for (auto &kv : m_records) {
		const Effect effect = ApplyTo(kv.second, ::browse::Tick(kv.second.rec, now));
		if (effect != Effect::Nothing) {
			out.emplace_back(kv.first, effect);
		}
	}
	return out;
}

Effect Store::ApplyTo(Held &held, Action action)
{
	if (action == Action::Drop) {
		// Terminal and already announced. Releasing the peer is all that is
		// left; the record itself outlives it, because the search ID is still
		// listed and every consumer asks here what state it is in. Dropping it
		// now would send them back to guessing from whether results were
		// retained, which reports a failed browse as idle, forever.
		if (held.client == nullptr) {
			return Effect::Nothing;
		}
		held.client = nullptr;
		return Effect::ReleaseClient;
	}
	const State before = held.rec.state;
	held.rec = ApplyAction(held.rec, action);
	if (held.rec.state == before) {
		// Refused, because it had already ended. Nothing to announce -- and
		// nothing to log, which is what keeps the disconnect that follows a
		// completed browse from being reported as a failure.
		return Effect::Nothing;
	}
	return held.rec.state == State::Failed ? Effect::AnnounceFailure : Effect::Announce;
}

std::map<std::uint32_t, Store::Held>::iterator Store::FindFor(ClientKey client)
{
	if (client == nullptr) {
		return m_records.end();
	}
	for (auto it = m_records.begin(); it != m_records.end(); ++it) {
		if (it->second.client == client) {
			return it;
		}
	}
	return m_records.end();
}

std::map<std::uint32_t, Store::Held>::const_iterator Store::FindFor(ClientKey client) const
{
	if (client == nullptr) {
		return m_records.end();
	}
	for (auto it = m_records.begin(); it != m_records.end(); ++it) {
		if (it->second.client == client) {
			return it;
		}
	}
	return m_records.end();
}

std::uint32_t Store::SearchIdFor(ClientKey client) const
{
	const auto it = FindFor(client);
	return (it != m_records.end() && it->second.rec.state == State::InProgress) ? it->first : 0;
}

bool Store::Has(std::uint32_t searchId) const
{
	return m_records.find(searchId) != m_records.end();
}

State Store::StateOf(std::uint32_t searchId) const
{
	const auto it = m_records.find(searchId);
	return it != m_records.end() ? it->second.rec.state : State::Failed;
}

std::uint16_t Store::BarValue(std::uint32_t searchId) const
{
	const auto it = m_records.find(searchId);
	return it != m_records.end() ? browse::BarValue(it->second.rec) : 0xffff;
}

std::vector<std::uint32_t> Store::Ids() const
{
	std::vector<std::uint32_t> out;
	out.reserve(m_records.size());
	for (const auto &kv : m_records) {
		out.push_back(kv.first);
	}
	return out;
}

} // namespace browse
