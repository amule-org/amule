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
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
//

#include "State.h"

#include <cstdlib>  // std::abort
#include <iostream> // std::cerr

#include <algorithm>
#include <cstdio>
#include <ctime>

#include <mutex>
#include <shared_mutex>
#include <utility>

namespace webapi
{

std::uint64_t PartCountForSize(std::uint64_t size)
{
	// Ceiling division. An empty file falls out as 0 without a special case,
	// since kPartSizeBytes - 1 < kPartSizeBytes.
	return (size + kPartSizeBytes - 1) / kPartSizeBytes;
}

std::uint16_t SharedHashingProgress(const FileSnapshot &f)
{
	return f.shared.hashing_progress ? f.shared.hashing_progress : f.download.hashed_part_count;
}

// Completeness of the file we download FROM this peer: parts the peer has over
// that file's part count. Only the download link carries a meaningful
// denominator -- a peer that merely downloads from us has no percent. Left at
// its < 0 sentinel when not computable, which is how the writers know to omit
// the field.
void ComputePartProgressPercent(const CState &state, ClientSnapshot &cli)
{
	if (!cli.has_parts_offered_count || cli.download_file_hash.empty()) {
		return;
	}
	// DownloadPartCount, not FindDownload: this runs once per source per
	// tick on the SSE path and once per row on two REST paths, and the part
	// count is the only thing wanted. FindDownload would deep-copy the whole
	// FileSnapshot each time, repeating the identical copy for every source
	// of the same file.
	const std::uint64_t part_count = state.DownloadPartCount(cli.download_file_hash);
	if (part_count == 0) {
		return;
	}
	double pct = 100.0 * static_cast<double>(cli.parts_offered_count) / static_cast<double>(part_count);
	if (pct > 100.0)
		pct = 100.0;
	cli.part_progress_percent = pct;
}

bool CState::HasFirstSnapshot() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_has_first_snapshot;
}

std::time_t CState::SnapshotAt() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_snapshot_at;
}

bool CState::EcConnected() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_ec_connected;
}

StatusSnapshot CState::Status() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_status;
}

KadSnapshot CState::Kad() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_kad;
}

CState::DashboardSnapshot CState::Dashboard() const
{
	// Single shared_lock acquisition: callers of /api/v0/status get
	// a coherent (status, kad, snapshot_at, ec_connected) tuple
	// instead of the four-separate-lock dance, which can interleave
	// with a refresher tick and make `kad.network` describe a
	// different tick than `ed2k.*` / `speeds.*`.
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	DashboardSnapshot out;
	out.status = m_status;
	out.kad = m_kad;
	out.snapshot_at = m_snapshot_at;
	out.ec_connected = m_ec_connected;
	return out;
}

PreferencesSnapshot CState::Preferences() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_preferences;
}

std::vector<CategorySnapshot> CState::Categories() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_categories;
}

std::vector<std::string> CState::AmuleLog() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_amule_log_lines;
}

std::vector<std::string> CState::AmuleLogFrom(std::size_t first, std::size_t &total) const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	total = m_amule_log_lines.size();
	if (first >= total)
		return {};
	return std::vector<std::string>(
		m_amule_log_lines.begin() + static_cast<std::ptrdiff_t>(first), m_amule_log_lines.end());
}

ServerInfoLog CState::ServerInfo() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_server_info;
}

StatsTreeNode CState::StatsTree() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_stats_tree;
}

StatsGraphs CState::Graphs() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_graphs;
}

std::vector<SearchResult> CState::Search(std::uint32_t search_id) const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	std::vector<SearchResult> out;
	auto it = m_searches.find(search_id);
	if (it == m_searches.end())
		return out;
	out.reserve(it->second.results.size());
	for (const auto &kv : it->second.results)
		out.push_back(kv.second);
	return out;
}

// See CState::ReentryGuard in State.h for why this aborts rather than
// returning, and why it runs before the lock rather than after it.
thread_local const CState *CState::t_in_callback = nullptr;

CState::ReentryGuard::ReentryGuard(const CState *self)
: m_prev(t_in_callback)
{
	if (t_in_callback == self) {
		std::cerr << "amuleapi: FATAL CState callback re-entered the same CState; "
			     "this would deadlock on a non-recursive lock\n";
		std::abort();
	}
	t_in_callback = self;
}

CState::ReentryGuard::~ReentryGuard()
{
	t_in_callback = m_prev;
}

void CState::MutateSearch(
	std::uint32_t search_id, const std::function<void(std::map<std::uint32_t, SearchResult> &)> &fn)
{
	const ReentryGuard guard(this);
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	auto it = m_searches.find(search_id);
	if (it != m_searches.end())
		fn(it->second.results);
}

SearchProgressSnapshot CState::SearchProgress(std::uint32_t search_id) const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	auto it = m_searches.find(search_id);
	if (it == m_searches.end())
		return SearchProgressSnapshot{};
	return it->second.progress;
}

bool CState::HasSearch(std::uint32_t search_id) const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_searches.count(search_id) != 0;
}

std::string CState::SearchQuery(std::uint32_t search_id) const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	auto it = m_searches.find(search_id);
	return it == m_searches.end() ? std::string() : it->second.query;
}

std::time_t CState::SearchStartedAt(std::uint32_t search_id) const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	auto it = m_searches.find(search_id);
	return it == m_searches.end() ? 0 : it->second.started_at;
}

bool CState::ClaimSearchRefresh(std::uint32_t search_id, std::chrono::milliseconds ttl)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	auto it = m_searches.find(search_id);
	if (it == m_searches.end())
		return false;
	// An active search is already refreshed by the tick every second;
	// claiming it here would just double the EC traffic on a running search.
	if (it->second.progress.active)
		return false;
	const auto now = std::chrono::steady_clock::now();
	if (now - it->second.last_fetch < ttl)
		return false;
	it->second.last_fetch = now;
	return true;
}

std::vector<std::uint32_t> CState::ActiveSearchIds() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	std::vector<std::uint32_t> out;
	for (const auto &kv : m_searches)
		if (kv.second.progress.active)
			out.push_back(kv.first);
	return out;
}

std::vector<std::uint32_t> CState::AttachedSearchIds() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	std::vector<std::uint32_t> out;
	for (const auto &kv : m_searches)
		if (!kv.second.detached)
			out.push_back(kv.first);
	return out;
}

std::vector<std::uint32_t> CState::AllSearchIds() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	std::vector<std::uint32_t> out;
	out.reserve(m_searches.size());
	for (const auto &kv : m_searches)
		out.push_back(kv.first);
	return out;
}

bool CState::FindSearchResultByHash(
	const std::string &hash_hex, SearchResult &out, std::uint32_t *owner_search_id) const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	for (const auto &skv : m_searches) {
		for (const auto &rkv : skv.second.results) {
			if (rkv.second.hash == hash_hex) {
				out = rkv.second;
				if (owner_search_id)
					*owner_search_id = skv.first;
				return true;
			}
		}
	}
	return false;
}

void CState::MarkSearchStarted(std::uint32_t search_id, const std::string &kind, const std::string &query)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	SearchSlot &slot = m_searches[search_id];
	if (slot.seq == 0)
		slot.seq = ++m_search_seq;
	// generation is per-slot and monotonic: a restart of the same id (rare —
	// the daemon allocates fresh ids) keeps it climbing so EventDiff still fires.
	const auto next_generation = slot.progress.generation + 1;
	// Both maps, and the ECID index that points at them. `raw` is the merge
	// target the union applies diffed tags to, so a stale entry left here
	// would have the previous search's fields show through wherever the new
	// one's tag happens not to carry that field -- and RebuildFoldedResults
	// would then put the ghost straight back into `results`, however many
	// times that gets cleared.
	for (const auto &entry : slot.raw)
		m_resultOwner.erase(entry.first);
	slot.raw.clear();
	slot.results.clear();
	slot.detached = false;
	slot.progress = SearchProgressSnapshot{};
	slot.progress.active = true;
	slot.progress.kind = kind;
	slot.progress.generation = next_generation;
	slot.query = query;
	slot.started_at = std::time(nullptr);
	slot.last_fetch = {};
	EvictSurplusSearchSlotsLocked(search_id);
}

void CState::MarkSearchDiscovered(std::uint32_t search_id,
	const std::string &kind,
	const std::string &query,
	bool active,
	bool complete,
	int reported_percent)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	auto known = m_searches.find(search_id);
	if (known != m_searches.end()) {
		// Already known (self-started, or discovered on an earlier
		// cache-miss check): leave its accumulated results/progress
		// alone. Re-seeding here would stomp whatever
		// WriteSearchProgress/ApplySearchUnion already recorded for it
		// this session. The query is the one exception — a slot seeded
		// before the daemon reported a name has an empty one, and
		// filling it in loses nothing.
		if (known->second.query.empty())
			known->second.query = query;
		return;
	}
	SearchSlot &slot = m_searches[search_id];
	slot.seq = ++m_search_seq;
	// The daemon's own lifecycle state for this search, not an assumption.
	// A finished search seeded as active reads as running until the next
	// tick corrects it, and POST /search/{id}/more gates on exactly that.
	// `started_at` stays 0: this session did not start it, and the daemon
	// ships no timestamp to borrow.
	slot.progress.active = active;
	slot.progress.complete = complete;
	// Seeded here or never. A slot discovered as finished is not in
	// ActiveSearchIds(), so the tick never polls it and WriteSearchProgress
	// is never called for it -- the percent would sit at its 0 default for
	// the life of the slot, contradicting the "finished" state in the very
	// same envelope.
	//
	// Prefer the daemon's own number when the listing carried one. Without it
	// (an older daemon) derive: 100 for a finished search, true by
	// definition; 0 for a running one, which IS polled and is corrected
	// within a tick, where inventing a number would flash a wrong one.
	slot.progress.percent =
		reported_percent >= 0 ? static_cast<std::uint32_t>(reported_percent) : (complete ? 100u : 0u);
	slot.progress.kind = kind;
	slot.query = query;
	EvictSurplusSearchSlotsLocked(search_id);
}

void CState::DetachSearch(std::uint32_t search_id)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	auto it = m_searches.find(search_id);
	if (it != m_searches.end())
		it->second.detached = true;
}

void CState::MarkAllSearchesNeedResync()
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	for (auto &kv : m_searches) {
		// A detached slot is frozen and the daemon holds nothing for it, so
		// there is nothing to re-seed and the FULL would come back expired.
		if (!kv.second.detached)
			kv.second.needs_resync = true;
	}
}

std::vector<std::uint32_t> CState::SearchesNeedingResync() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	std::vector<std::pair<std::uint64_t, std::uint32_t>> pending;
	for (const auto &kv : m_searches) {
		// Same exemption the flag's writer applies, and for the same reason:
		// a slot detached after it was flagged has had its search dropped by
		// the daemon, so a full re-read would only come back expired. Asking
		// anyway costs a roundtrip per slot, and a replace-mode apply against
		// a detached slot would clear the results the detach exists to keep.
		if (kv.second.needs_resync && !kv.second.detached)
			pending.emplace_back(kv.second.seq, kv.first);
	}
	std::sort(pending.begin(), pending.end());
	std::vector<std::uint32_t> out;
	out.reserve(pending.size());
	for (const auto &p : pending)
		out.push_back(p.second);
	return out;
}

void CState::ClearSearchResyncFlag(std::uint32_t search_id)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	auto it = m_searches.find(search_id);
	if (it != m_searches.end())
		it->second.needs_resync = false;
}

void CState::EvictSurplusSearchSlotsLocked(std::uint32_t exempt_id)
{
	while (m_searches.size() > kMaxSearchSlots) {
		auto victim = m_searches.end();
		for (auto it = m_searches.begin(); it != m_searches.end(); ++it) {
			// Never an active one: it is still being polled, and the surplus
			// is always made of slots that have stopped moving.
			if (it->second.progress.active)
				continue;
			// Never the slot the caller is in the middle of seeding.
			if (exempt_id != 0 && it->first == exempt_id)
				continue;
			// A detached slot always outranks an attached one, however much
			// younger: the daemon no longer holds it, so evicting it drops
			// nothing that could still be re-read.
			if (victim == m_searches.end() || (it->second.detached && !victim->second.detached) ||
				(it->second.detached == victim->second.detached &&
					it->second.seq < victim->second.seq)) {
				victim = it;
			}
		}
		if (victim == m_searches.end())
			break; // every remaining slot is still active — nothing to evict
		// The index has to go with the slot, or a later result reusing one of
		// these ECIDs would be attributed to a search that is no longer here.
		for (const auto &entry : victim->second.raw)
			m_resultOwner.erase(entry.first);
		m_searches.erase(victim);
	}
}

void CState::WriteSearchProgress(std::uint32_t search_id, SearchProgressSnapshot s)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	auto it = m_searches.find(search_id);
	if (it != m_searches.end())
		it->second.progress = std::move(s);
}

void CState::MutateAllSearches(const std::function<void(
		std::map<std::uint32_t, SearchSlot> &, std::map<std::uint32_t, std::uint32_t> &)> &fn)
{
	const ReentryGuard guard(this);
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	fn(m_searches, m_resultOwner);
}

void CState::CloseSearch(std::uint32_t search_id)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	const auto it = m_searches.find(search_id);
	if (it == m_searches.end())
		return;
	// The index mirrors this slot's result map, so it has to lose the same
	// ECIDs in the same locked step. Walking the slot's own results is what
	// keeps that exact: erasing by value over the whole index would be O(n)
	// in every search rather than this one.
	for (const auto &kv : it->second.raw)
		m_resultOwner.erase(kv.first);
	m_searches.erase(it);
}

void CState::WriteStatsTree(StatsTreeNode t)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	m_stats_tree = std::move(t);
}

void CState::WriteGraphs(StatsGraphs g)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	m_graphs = std::move(g);
}

void CState::AppendAmuleLog(std::vector<std::string> new_lines)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	// No cap — see State.h comment above the `m_amule_log_lines`
	// declaration. Operators can truncate via DELETE /logs/amule
	// .
	m_amule_log_lines.insert(m_amule_log_lines.end(),
		std::make_move_iterator(new_lines.begin()),
		std::make_move_iterator(new_lines.end()));
}

void CState::ClearAmuleLog()
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	m_amule_log_lines.clear();
}

void CState::WriteServerInfo(ServerInfoLog s)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	m_server_info = std::move(s);
}

std::vector<ServerSnapshot> CState::Servers() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	std::vector<ServerSnapshot> out;
	out.reserve(m_servers.size());
	for (const auto &kv : m_servers)
		out.push_back(kv.second);
	return out;
}

void CState::WriteStatus(StatusSnapshot s)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	m_status = std::move(s);
}

void CState::WriteKad(KadSnapshot k)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	m_kad = std::move(k);
}

void CState::WritePreferences(PreferencesSnapshot p)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	m_preferences = std::move(p);
}

void CState::WriteCategories(std::vector<CategorySnapshot> c)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	m_categories = std::move(c);
}

void CState::MutateServers(const std::function<void(std::map<std::uint32_t, ServerSnapshot> &)> &fn)
{
	const ReentryGuard guard(this);
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	fn(m_servers);
}
std::vector<FriendSnapshot> CState::Friends() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	std::vector<FriendSnapshot> out;
	out.reserve(m_friends.size());
	for (const auto &kv : m_friends)
		out.push_back(kv.second);
	return out;
}
void CState::MutateFriends(const std::function<void(std::map<std::uint32_t, FriendSnapshot> &)> &fn)
{
	const ReentryGuard guard(this);
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	fn(m_friends);
}

std::string IPv4ToDotted(std::uint32_t ip_lsb_first)
{
	char buf[16];
	std::snprintf(buf,
		sizeof(buf),
		"%u.%u.%u.%u",
		static_cast<unsigned>((ip_lsb_first) & 0xFFu),
		static_cast<unsigned>((ip_lsb_first >> 8) & 0xFFu),
		static_cast<unsigned>((ip_lsb_first >> 16) & 0xFFu),
		static_cast<unsigned>((ip_lsb_first >> 24) & 0xFFu));
	return std::string(buf);
}

// See State.h. Matches on the path; the query string selects a page, not a
// different resource.
bool MemoizableTarget(const std::string &target)
{
	const std::string path = target.substr(0, target.find('?'));
	// OPT-IN, and deliberately so. This was an exclusion list, and an
	// exclusion list has to be right about every route that exists now and
	// every route anyone adds later -- it was wrong four separate times,
	// each for a different reason. Inverting it makes the failure mode
	// "we hash a body we did not have to", which costs microseconds,
	// instead of "we serve a 304 for content that changed".
	//
	// Only the two collections the memo was built for are listed. They are
	// the multi-MB bodies where skipping an MD5 is worth anything; every
	// other target hashes per request and is immune by construction.
	// Before adding one, it must be BOTH governed by the refresher
	// snapshot AND identical for every caller -- see State.h.
	return path == "/api/v0/downloads" || path == "/api/v0/shared";
}

// See State.h. Ordered cheap-test-first: the revision comparison is two
// integer loads, the target match copies and scans a string.
bool MemoUsable(const std::string &target, std::uint64_t rev_before, std::uint64_t rev_after)
{
	return rev_before == rev_after && MemoizableTarget(target);
}

// See State.h.
bool ShouldStampEtag(bool is_safe_method, bool handler_set_etag, unsigned status, bool body_empty)
{
	return is_safe_method && !handler_set_etag && status == 200 && !body_empty;
}

std::string ChatPeerKeyFromGuiId(std::uint64_t gui_id)
{
	const std::uint32_t ip = static_cast<std::uint32_t>(gui_id >> 16);
	const std::uint16_t port = static_cast<std::uint16_t>(gui_id & 0xFFFFu);
	return IPv4ToDotted(ip) + ":" + std::to_string(port);
}

std::vector<ChatSessionSnapshot> CState::Chats() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_chats;
}

std::uint32_t CState::ChatCursor() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_chat_cursor;
}

void CState::MutateChats(const std::function<void(std::vector<ChatSessionSnapshot> &, std::uint32_t &)> &fn)
{
	const ReentryGuard guard(this);
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	fn(m_chats, m_chat_cursor);
}

std::vector<ClientSnapshot> CState::Clients() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	std::vector<ClientSnapshot> out;
	out.reserve(m_clients.size());
	for (const auto &kv : m_clients)
		out.push_back(kv.second);
	return out;
}

bool CState::KnownClientsLoaded() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_known_loaded;
}

void CState::SetKnownClients(std::vector<KnownClientSnapshot> &&rows)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	m_known_clients = std::move(rows);
	m_known_of_hash.clear();
	m_known_online.clear();
	for (std::size_t i = 0; i < m_known_clients.size(); ++i)
		m_known_of_hash[m_known_clients[i].user_hash] = i;
	m_known_loaded = true;
	// The fetch describes the store as of a moment ago; the peers connected
	// right now are already more current than parts of it.
	ReconcileKnownClientsLocked();
}

void CState::ReconcileKnownClients()
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	if (!m_known_loaded)
		return;
	ReconcileKnownClientsLocked();
}

void CState::ReconcileKnownClientsLocked()
{
	const std::time_t now = std::time(nullptr);
	std::set<std::size_t> still_online;

	for (const auto &kv : m_clients) {
		const ClientSnapshot &c = kv.second;
		// An all-zero hash is not an identity: it is what a peer that has not
		// sent its hash yet reports, and every such peer would otherwise
		// collapse into one fabricated record -- sharing a session count and
		// a first-seen between unrelated clients. The daemon never writes one
		// either; it creates a credit record from the hash in the hello. They
		// get a row of their own once they identify.
		if (c.user_hash.empty() || c.user_hash.find_first_not_of('0') == std::string::npos)
			continue;

		auto it = m_known_of_hash.find(c.user_hash);
		if (it == m_known_of_hash.end()) {
			// A peer met since the store was read. The daemon wrote its
			// credit record when the peer said hello, stamping first-seen
			// and counting the session, so this reconstructs what it wrote
			// rather than inventing anything.
			//
			// sessions is left at zero deliberately: the offline-to-online
			// transition below is what counts it, and this record is about
			// to make that transition. Setting it here too would count the
			// same arrival twice.
			KnownClientSnapshot k;
			k.user_hash = c.user_hash;
			k.first_seen_at = now;
			k.last_seen_at = now;
			m_known_clients.push_back(std::move(k));
			it = m_known_of_hash.emplace(c.user_hash, m_known_clients.size() - 1).first;
		}

		KnownClientSnapshot &k = m_known_clients[it->second];
		still_online.insert(it->second);
		// Offline to online is a new session, which is what the daemon counts:
		// UpdateMeta() bumps it once per client object, at the hello. Counted
		// on the transition rather than per tick for the same reason.
		//
		// It can over-count by one if a peer drops out of the update for a
		// tick and returns -- an EC hiccup rather than a real reconnect. The
		// daemon's own figure replaces this at the next fetch, so any drift
		// lives no longer than the connection to that core.
		if (!k.online)
			k.session_count++;
		k.online = true;
		// A peer in front of us was last seen now, not whenever it previously
		// disconnected. Leaving the stored value would report a peer that is
		// connected as last seen months ago, and now is what the core writes
		// to the record at its own disconnect handling anyway.
		k.last_seen_at = now;
		k.uploaded_bytes_total = c.uploaded_bytes_total;
		k.downloaded_bytes_total = c.downloaded_bytes_total;
		// Identity, when the peer in front of us knows more than the record.
		// A record only gains a name once the core writes its metadata, so a
		// peer we have never finished a session with is otherwise nameless.
		// Guarded on the live name being known: a peer mid-handshake has none
		// and must not blank a stored one.
		if (!c.client_name.empty()) {
			k.client_name = c.client_name;
			k.ip = c.ip;
			k.port = c.port;
			k.kad_port = c.kad_port;
			k.country_code = c.country_code;
			k.software = c.software;
			k.software_version = c.software_version;
			k.source_origin = c.source_origin;
			k.obfuscation_state = c.obfuscation_state;
		}
	}

	// Whoever was online last tick and is not in this one has gone. Found
	// through the online set, so this costs the number of departures rather
	// than a walk of the store.
	for (const std::size_t idx : m_known_online) {
		if (still_online.count(idx) != 0)
			continue;
		m_known_clients[idx].online = false;
		// Seen until this moment, which is what the core writes to the record
		// at its own disconnect handling. The stored value is the *previous*
		// disconnect, so leaving it would show a peer that was here a second
		// ago as last seen months back.
		m_known_clients[idx].last_seen_at = now;
	}
	m_known_online.swap(still_online);
}

bool CState::FindDownload(const std::string &hash_hex, FileSnapshot &out) const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	std::uint32_t ecid = 0;
	// Role-keyed: a share with the same content must not shadow the download
	// (#1161). The is_downloading check below is now belt-and-braces rather
	// than the thing standing between the caller and the wrong entry.
	if (!m_files.FindDownloadEcidByHash(hash_hex, ecid))
		return false;
	const auto it = m_files.find(ecid);
	if (it == m_files.end() || !it->second.is_downloading)
		return false;
	out = it->second;
	return true;
}

std::uint64_t CState::DownloadPartCount(const std::string &hash_hex) const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	std::uint32_t ecid = 0;
	// Same role-keyed resolution as FindDownload(): this answers a question
	// about the download, so a share with the same hash must not answer it.
	if (!m_files.FindDownloadEcidByHash(hash_hex, ecid))
		return 0;
	const auto it = m_files.find(ecid);
	if (it == m_files.end() || !it->second.is_downloading)
		return 0;
	return PartCountForSize(it->second.size);
}

bool CState::FindDownloadByEcid(std::uint32_t ecid, FileSnapshot &out) const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	auto it = m_files.find(ecid);
	if (it == m_files.end() || !it->second.is_downloading)
		return false;
	out = it->second;
	return true;
}

bool CState::FindShared(const std::string &hash_hex, FileSnapshot &out) const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	std::uint32_t ecid = 0;
	// Role-keyed, mirroring FindDownload(): the part file being downloaded
	// must not shadow the share.
	if (!m_files.FindSharedEcidByHash(hash_hex, ecid))
		return false;
	const auto it = m_files.find(ecid);
	if (it == m_files.end() || !it->second.is_shared)
		return false;
	out = it->second;
	return true;
}

bool CState::FindSharedByEcid(std::uint32_t ecid, FileSnapshot &out) const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	auto it = m_files.find(ecid);
	if (it == m_files.end() || !it->second.is_shared)
		return false;
	out = it->second;
	return true;
}

// MutateDownloads + MutateShared both lock + hand out m_files. Both
// walkers operate on the same unified map (and the same lock acquisition,
// when chained from a single tick); the callback decides which role
// flag to set or clear. The FileMap wrapper keeps its hash→ECID index
// in sync as the walker emplaces / erases, so there's no rebuild pass
// at the end of the mutate window.
void CState::MutateDownloads(const std::function<void(FileMap &)> &fn)
{
	const ReentryGuard guard(this);
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	fn(m_files);
	// Bumped by the writer, not by its callers. The ETag memo keys on this,
	// and every previous attempt to advance it from the outside missed a
	// path: first the inline refreshes that mutating handlers run, then a
	// tick that failed partway after already writing. A writer cannot
	// forget to say that it wrote.
	++m_snapshot_rev;
}

void CState::MutateShared(const std::function<void(FileMap &)> &fn)
{
	const ReentryGuard guard(this);
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	fn(m_files);
	// See MutateDownloads: the memo key is advanced by the writer.
	++m_snapshot_rev;
}

void CState::MutateClients(const std::function<void(std::map<std::uint32_t, ClientSnapshot> &)> &fn)
{
	// Forwards rather than repeating the guard-plus-lock body: the two differ
	// only in what they hand the callback. Still exactly one acquisition, so
	// a caller that does not need the files pays nothing for the convenience.
	MutateClientsWithFiles(
		[&fn](std::map<std::uint32_t, ClientSnapshot> &clients, const FileMap &) { fn(clients); });
}

void CState::MutateClientsWithFiles(
	const std::function<void(std::map<std::uint32_t, ClientSnapshot> &, const FileMap &)> &fn)
{
	const ReentryGuard guard(this);
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	fn(m_clients, m_files);
}

void CState::ResetLists()
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	// A wholesale wipe on a failed tick is as much a body change as any
	// mutation, and it runs on the failure path -- exactly where the key
	// used to freeze while the bodies moved underneath it.
	++m_snapshot_rev;
	m_files.clear();
	m_clients.clear();
	m_servers.clear();
	m_categories.clear();
	// Search slots and the ECID->search_id index that mirrors them are
	// deliberately NOT cleared, for the same reason as the credit store
	// below and then some. This runs when a tick failed against a socket
	// that is still up -- an actual dropped connection sets
	// g_shutdownRequested via HandleEcConnectionLost() and this loop exits
	// instead -- so the daemon's per-connection search registry is very much
	// alive, along with its record of which results it has already sent us
	// and the valuemap it diffs against. Wiping our side of that would not
	// resync anything: results the daemon considers delivered are elided from
	// then on, so every search would come back permanently short by whatever
	// it held at the moment one tick returned null. The collections above are
	// safe to wipe precisely because their EC_DETAIL_UPDATE streams resend in
	// full; this one does not.
	//
	// The credit store is dropped for the same reason -- refetching the whole
	// thing after one null tick is the cost this endpoint exists to avoid. It
	// cannot go stale across a daemon restart either: amuleapi shuts down the
	// moment the socket drops, so the process never attaches to a second
	// core.
	// Logs + stats_tree + graphs survive EC reconnects on purpose —
	// operator can see "EC disconnected at HH:MM" alongside earlier
	// graph traffic; stats_tree's counters are amuled-uptime not
	// amuleapi-tick scoped.
}

void CState::BumpSnapshotRevision()
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	++m_snapshot_rev;
}

std::uint64_t CState::SnapshotRevision() const
{
	std::shared_lock<std::shared_timed_mutex> lock(m_mu);
	return m_snapshot_rev;
}

void CState::MarkTickSuccess()
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	m_has_first_snapshot = true;
	m_ec_connected = true;
	// `m_snapshot_at` is stamped at tick-END (here), not tick-start.
	// Clients reading `snapshot_at` therefore see "the wall-clock
	// moment the daemon finished assembling this snapshot", with the
	// tick's own duration as the implicit skew (typically 50-200 ms,
	// up to multi-second under EC-mutex contention). For coarse
	// freshness checks ("is this stale by more than 5 s?") that's
	// fine; if a future caller wants sub-second precision, document
	// the skew or stamp both tick_started_at and tick_ended_at.
	m_snapshot_at = std::time(nullptr);
}

void CState::MarkTickFailure()
{
	std::unique_lock<std::shared_timed_mutex> lock(m_mu);
	// Deliberately preserve m_snapshot_at — clients see stale
	// `snapshot_at` next to `ec_connected=false`, so they can tell
	// how stale the cache is. Resetting it to `now` would lie.
	//
	// Tick-atomicity: on failure CState may hold partial mutations
	// from earlier in the tick. The "tick = transaction" model is
	// atomic for events (EmitDiffsForEventBus is skipped on failure,
	// next-tick diff is against the prior-success baseline in
	// LastSeenState) but NOT atomic for state — no rollback. CState
	// is a best-effort cache for /status freshness; LastSeenState
	// is the authoritative event baseline.
	m_ec_connected = false;
}

} // namespace webapi
