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

#include "EventDiff.h"

#include "EventBus.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <thread>

namespace webapi
{

namespace
{

// Minimal JSON string escaper. JsonWriter (libwebcommon) is the
// canonical formatter for response bodies, but the event-data
// payloads we emit here are small and predictable — a few KB at
// most — and keeping the diff path independent of CJsonWriter
// avoids dragging wxString into the bus path. Quote-escape only the
// characters JSON disallows: backslash, double-quote, and the C0
// controls. Tab/CR/LF appear in amule log lines so we encode them
// explicitly.
std::string EscJson(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 8);
	for (unsigned char c : s) {
		switch (c) {
		case '\\':
			out += "\\\\";
			break;
		case '"':
			out += "\\\"";
			break;
		case '\b':
			out += "\\b";
			break;
		case '\f':
			out += "\\f";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c < 0x20) {
				char buf[8];
				std::snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else {
				out += static_cast<char>(c);
			}
		}
	}
	return out;
}

// Each ToJson emits the SAME shape as the corresponding REST list-item
// writer in Api.cpp (WriteDownloadObject / WriteSharedObject /
// WriteServerObject / WriteClientObject / HandleStatus). The contract
// is "an SSE _added/_updated event carries the full resource — clients
// don't need to re-GET to see the moved counters". The matching Equal
// functions below compare every field included here so any movement
// fires `_updated`. If REST or SSE drifts in the future, the doc-
// alignment check in run-all.sh phase11 should catch it.

// download_* event payload — mirrors WriteDownloadObject (Api.cpp)
// at the wire level. Reads the download sub-block of FileSnapshot.
std::string ToJsonDownloadEvent(const FileSnapshot &f)
{
	std::ostringstream o;
	o << "{"
	  << "\"hash\":\"" << EscJson(f.hash) << "\""
	  << ",\"name\":\"" << EscJson(f.name) << "\""
	  << ",\"ed2k_link\":\"" << EscJson(f.ed2k_link) << "\""
	  << ",\"size\":" << f.size << ",\"size_done\":" << f.download.size_done
	  << ",\"size_xfer\":" << f.download.size_xfer << ",\"speed_bps\":" << f.download.speed_bps
	  << ",\"status\":\"" << EscJson(f.download.status) << "\""
	  << ",\"priority\":\"" << EscJson(f.download.priority) << "\""
	  << ",\"priority_auto\":" << (f.download.priority_auto ? "true" : "false")
	  << ",\"category\":" << f.download.category << ",\"sources\":{"
	  << "\"total\":" << f.download.sources_total << ",\"not_current\":" << f.download.sources_not_current
	  << ",\"transferring\":" << f.download.sources_transferring
	  << ",\"a4af\":" << f.download.sources_a4af << "}"
	  << ",\"progress\":{\"percent\":" << f.download.percent << "}"
	  << "}";
	return o.str();
}

// shared_* event payload — mirrors WriteSharedObject. Reads the
// shared sub-block of FileSnapshot.
std::string ToJsonSharedEvent(const FileSnapshot &f)
{
	std::ostringstream o;
	o << "{"
	  << "\"hash\":\"" << EscJson(f.hash) << "\""
	  << ",\"name\":\"" << EscJson(f.name) << "\""
	  << ",\"ed2k_link\":\"" << EscJson(f.ed2k_link) << "\""
	  << ",\"size\":" << f.size << ",\"priority\":\"" << EscJson(f.shared.priority) << "\""
	  << ",\"priority_auto\":" << (f.shared.priority_auto ? "true" : "false")
	  << ",\"complete_sources\":" << f.shared.complete_sources
	  << ",\"xfer\":{\"session\":" << f.shared.xfer_session << ",\"total\":" << f.shared.xfer_total << "}"
	  << ",\"requests\":{\"session\":" << f.shared.requests_session
	  << ",\"total\":" << f.shared.requests_total << "}"
	  << ",\"accepts\":{\"session\":" << f.shared.accepts_session
	  << ",\"total\":" << f.shared.accepts_total << "}"
	  << "}";
	return o.str();
}

std::string ToJson(const ServerSnapshot &s)
{
	std::ostringstream o;
	o << "{"
	  << "\"ecid\":" << s.ecid << ",\"name\":\"" << EscJson(s.name) << "\""
	  << ",\"description\":\"" << EscJson(s.description) << "\""
	  << ",\"version\":\"" << EscJson(s.version) << "\""
	  << ",\"address\":\"" << EscJson(s.address) << "\""
	  << ",\"port\":" << s.port << ",\"users\":" << s.users << ",\"max_users\":" << s.max_users
	  << ",\"files\":" << s.files << ",\"priority\":\"" << EscJson(s.priority) << "\""
	  << ",\"ping_ms\":" << s.ping_ms << ",\"failed\":" << s.failed
	  << ",\"static\":" << (s.is_static ? "true" : "false") << "}";
	return o.str();
}

std::string ToJson(const ClientSnapshot &c)
{
	std::ostringstream o;
	o << "{"
	  << "\"client_ecid\":" << c.ecid << ",\"client_name\":\"" << EscJson(c.client_name) << "\""
	  << ",\"user_hash\":\"" << EscJson(c.user_hash) << "\""
	  << ",\"ip\":\"" << EscJson(c.ip) << "\""
	  << ",\"port\":" << c.port << ",\"software\":\"" << EscJson(c.software) << "\""
	  << ",\"software_version\":\"" << EscJson(c.software_version) << "\""
	  << ",\"os_info\":\"" << EscJson(c.os_info) << "\""
	  << ",\"upload_state\":\"" << EscJson(c.upload_state) << "\""
	  << ",\"download_state\":\"" << EscJson(c.download_state) << "\""
	  << ",\"ident_state\":\"" << EscJson(c.ident_state) << "\""
	  << ",\"download_file_name\":\"" << EscJson(c.download_file_name) << "\""
	  << ",\"upload_file_hash\":\"" << EscJson(c.upload_file_hash) << "\""
	  << ",\"download_file_hash\":\"" << EscJson(c.download_file_hash) << "\""
	  << ",\"xfer\":{"
	  << "\"up_session\":" << c.xfer_up_session << ",\"down_session\":" << c.xfer_down_session
	  << ",\"up_total\":" << c.xfer_up_total << ",\"down_total\":" << c.xfer_down_total << "}"
	  << ",\"upload_speed_bps\":" << c.upload_speed_bps
	  << ",\"download_speed_bps\":" << c.download_speed_bps
	  << ",\"queue_waiting_position\":" << c.queue_waiting_position
	  << ",\"remote_queue_rank\":" << c.remote_queue_rank << ",\"score\":" << c.score
	  << ",\"obfuscation_status\":\"" << EscJson(c.obfuscation_status) << "\""
	  << ",\"friend_slot\":" << (c.friend_slot ? "true" : "false") << "}";
	return o.str();
}

// Status event payload mirrors the REST /status envelope nesting
// (ed2k.*, kad.* including the kad.network rollup, speeds.*, queue.*,
// plus the top-level ec_connected flag). Takes a triple because the
// REST nesting groups data from StatusSnapshot AND KadSnapshot AND
// the dashboard's ec_connected bit — all three are read in one
// shared_lock by state.Dashboard() at the call site.
std::string ToJsonStatusEvent(const StatusSnapshot &s, const KadSnapshot &k, bool ec_connected)
{
	std::ostringstream o;
	o << "{"
	  << "\"ec_connected\":" << (ec_connected ? "true" : "false") << ",\"ed2k\":{"
	  << "\"state\":\"" << EscJson(s.ed2k_state) << "\""
	  << ",\"low_id\":" << (s.ed2k_lowid ? "true" : "false") << ",\"server_name\":\""
	  << EscJson(s.server_name) << "\""
	  << ",\"server_ip\":\"" << EscJson(s.server_ip) << "\""
	  << ",\"server_port\":" << s.server_port << ",\"network\":{"
	  << "\"users\":" << s.ed2k_users << ",\"files\":" << s.ed2k_files << "}}"
	  << ",\"kad\":{"
	  << "\"state\":\"" << EscJson(s.kad_state) << "\""
	  << ",\"firewalled\":" << (s.kad_firewalled ? "true" : "false") << ",\"network\":{"
	  << "\"users\":" << k.users << ",\"files\":" << k.files << ",\"nodes\":" << k.nodes << "}"
	  << "}"
	  << ",\"speeds\":{"
	  << "\"download_bps\":" << s.download_bps << ",\"upload_bps\":" << s.upload_bps << "}"
	  << ",\"queue\":{"
	  << "\"upload_queue_length\":" << s.ul_queue_len << ",\"total_source_count\":" << s.total_src_count
	  << "}"
	  << "}";
	return o.str();
}

// Coarse equality — every field. For we treat any change as
// "_updated" (emit the full new snapshot). v0.2 could introduce
// per-field deltas if a real consumer reports wanting them.
// Equal compares every field that ToJson emits. Any movement fires
// `_updated`. Field sets here are the same as the matching ToJson
// above; if one drifts from the other clients will see stale
// values until the next ROW-level field changes.
// download_* / shared_* event diffs compare the FIELDS THAT THE
// CORRESPONDING ToJson emits, not the full FileSnapshot. The download
// side ignores shared.* and is_shared, the shared side ignores
// download.* and is_downloading — a tick that flips one role doesn't
// fire the other role's _updated.
//
// ecid is in both JSON shapes; if amuled gets restarted while
// amuleapi keeps running, the same hash will surface with a fresh
// ECID, and clients keyed on ECID need the _updated to invalidate
// their cached id.
bool EqualDownload(const FileSnapshot &a, const FileSnapshot &b)
{
	return a.ecid == b.ecid && a.hash == b.hash && a.name == b.name && a.ed2k_link == b.ed2k_link &&
	       a.size == b.size && a.download.priority == b.download.priority &&
	       a.download.size_done == b.download.size_done && a.download.size_xfer == b.download.size_xfer &&
	       a.download.speed_bps == b.download.speed_bps && a.download.status == b.download.status &&
	       a.download.priority_auto == b.download.priority_auto &&
	       a.download.category == b.download.category &&
	       a.download.sources_total == b.download.sources_total &&
	       a.download.sources_not_current == b.download.sources_not_current &&
	       a.download.sources_transferring == b.download.sources_transferring &&
	       a.download.sources_a4af == b.download.sources_a4af && a.download.percent == b.download.percent;
}
bool EqualShared(const FileSnapshot &a, const FileSnapshot &b)
{
	return a.ecid == b.ecid && a.hash == b.hash && a.name == b.name && a.ed2k_link == b.ed2k_link &&
	       a.size == b.size && a.shared.priority == b.shared.priority &&
	       a.shared.priority_auto == b.shared.priority_auto &&
	       a.shared.complete_sources == b.shared.complete_sources &&
	       a.shared.xfer_session == b.shared.xfer_session && a.shared.xfer_total == b.shared.xfer_total &&
	       a.shared.requests_session == b.shared.requests_session &&
	       a.shared.requests_total == b.shared.requests_total &&
	       a.shared.accepts_session == b.shared.accepts_session &&
	       a.shared.accepts_total == b.shared.accepts_total;
}
bool Equal(const ServerSnapshot &a, const ServerSnapshot &b)
{
	return a.name == b.name && a.description == b.description && a.version == b.version &&
	       a.address == b.address && a.port == b.port && a.users == b.users &&
	       a.max_users == b.max_users && a.files == b.files && a.priority == b.priority &&
	       a.ping_ms == b.ping_ms && a.failed == b.failed && a.is_static == b.is_static;
}
bool Equal(const ClientSnapshot &a, const ClientSnapshot &b)
{
	return a.client_name == b.client_name && a.user_hash == b.user_hash && a.ip == b.ip &&
	       a.port == b.port && a.software == b.software && a.software_version == b.software_version &&
	       a.os_info == b.os_info && a.upload_state == b.upload_state &&
	       a.download_state == b.download_state && a.ident_state == b.ident_state &&
	       a.download_file_name == b.download_file_name && a.upload_file_hash == b.upload_file_hash &&
	       a.download_file_hash == b.download_file_hash && a.xfer_up_session == b.xfer_up_session &&
	       a.xfer_down_session == b.xfer_down_session && a.xfer_up_total == b.xfer_up_total &&
	       a.xfer_down_total == b.xfer_down_total && a.upload_speed_bps == b.upload_speed_bps &&
	       a.download_speed_bps == b.download_speed_bps &&
	       a.queue_waiting_position == b.queue_waiting_position &&
	       a.remote_queue_rank == b.remote_queue_rank && a.score == b.score &&
	       a.obfuscation_status == b.obfuscation_status && a.friend_slot == b.friend_slot;
}
bool Equal(const StatusSnapshot &a, const StatusSnapshot &b)
{
	return a.ed2k_state == b.ed2k_state && a.kad_state == b.kad_state && a.ed2k_lowid == b.ed2k_lowid &&
	       a.kad_firewalled == b.kad_firewalled && a.server_name == b.server_name &&
	       a.server_ip == b.server_ip && a.server_port == b.server_port &&
	       a.download_bps == b.download_bps && a.upload_bps == b.upload_bps &&
	       a.ul_queue_len == b.ul_queue_len && a.total_src_count == b.total_src_count &&
	       a.ed2k_users == b.ed2k_users && a.ed2k_files == b.ed2k_files;
}
bool Equal(const KadSnapshot &a, const KadSnapshot &b)
{
	return a.users == b.users && a.files == b.files && a.nodes == b.nodes;
}

// Generic map-diff helper. Walks both old and new, emitting:
//  - `<base>_removed` for keys in old missing from new (identity-only)
//  - `<base>_added`   for keys in new missing from old (full ToJson)
//  - `<base>_updated` for shared keys whose values differ (full ToJson)
//
// `removed_id_payload_fn` formats the identity-only `_removed` payload
// — `{"hash": "..."}` for hash-keyed (downloads, shared) or
// `{"ecid": N}` for ECID-keyed (servers, clients).
//
// Coalesced into one PublishBatch (one lock acquisition, one
// notify_all) so a cold-start diff on a 5K-download library doesn't
// fire 5K notify_all cycles inside the refresher loop.
template <class Map, class IdentityFn>
void DiffMap(CEventBus &bus,
	const std::string &base,
	const Map &old_items,
	const Map &new_items,
	IdentityFn removed_id_payload_fn)
{
	std::vector<std::pair<std::string, std::string>> batch;
	batch.reserve(old_items.size() + new_items.size());
	const std::string removed_name = base + "_removed";
	const std::string added_name = base + "_added";
	const std::string updated_name = base + "_updated";
	for (const auto &kv : old_items) {
		if (new_items.find(kv.first) == new_items.end()) {
			batch.emplace_back(removed_name, removed_id_payload_fn(kv.second));
		}
	}
	for (const auto &kv : new_items) {
		const auto it = old_items.find(kv.first);
		if (it == old_items.end()) {
			batch.emplace_back(added_name, ToJson(kv.second));
		} else if (!Equal(it->second, kv.second)) {
			batch.emplace_back(updated_name, ToJson(kv.second));
		}
	}
	bus.PublishBatch(batch);
}

// For hash-keyed file events emit removed payloads as
// `{"hash":"..."}` so consumers can drop the cache entry without
// needing the old object.
std::string RemovedHashPayload(const FileSnapshot &f)
{
	return "{\"hash\":\"" + EscJson(f.hash) + "\"}";
}
// ECID-keyed types (servers / clients): same shape, ECID payload.
std::string RemovedEcidPayload(const ServerSnapshot &s)
{
	std::ostringstream o;
	o << "{\"ecid\":" << s.ecid << "}";
	return o.str();
}
std::string RemovedEcidPayload(const ClientSnapshot &c)
{
	std::ostringstream o;
	o << "{\"client_ecid\":" << c.ecid << "}";
	return o.str();
}

// Build an ECID-keyed map from the vector view that CState exposes.
// The cache's internal layout is std::map<ECID, Snapshot>; the public
// accessor returns std::vector<Snapshot>. For diffing we want
// random-access-by-ECID, so we lift it back into a map. Cheap — O(N)
// with N typically <1000 per substruct.
template <class Snap> std::map<std::uint32_t, Snap> ByEcid(const std::vector<Snap> &v)
{
	std::map<std::uint32_t, Snap> m;
	for (const auto &x : v)
		m.emplace(x.ecid, x);
	return m;
}

} // namespace

namespace
{

// Single-writer invariant: only the wxApp refresher tick mutates
// LastSeenState + publishes diffs. Anything else (a future inline-
// refresh-then-publish, a debug recompute, etc.) is a silent
// concurrency bug — events get duplicated/dropped depending on
// which order the threads landed. Capture the first caller's
// thread id and abort hard on any subsequent caller from a
// different thread. Hard-abort (not assert) so the check survives
// -DNDEBUG and ships in every Release / RelWithDebInfo binary.
std::atomic<std::thread::id> g_publisher_thread;

void EnforceSinglePublisher()
{
	const std::thread::id self = std::this_thread::get_id();
	std::thread::id expected;
	if (g_publisher_thread.compare_exchange_strong(expected, self)) {
		return; // first caller — claimed it
	}
	if (expected == self)
		return;
	std::cerr << "amuleapi: EmitDiffsAndUpdate called from two "
		     "different threads; this breaks the single-writer "
		     "invariant on LastSeenState and the EventBus.\n";
	std::abort();
}

} // namespace

void EmitDiffsAndUpdate(CEventBus &bus, LastSeenState &prev, const CState &state)
{
	EnforceSinglePublisher();
	// Snapshot the current state under its read locks. Each accessor
	// takes the shared_timed_mutex shared, copies, and returns. For
	// files we use the unfiltered view (Files() — not the role-filtered
	// Downloads/Shared) so the diff below sees role-flag transitions:
	// a file that flipped is_shared false→true on an existing ECID
	// must fire `shared_added` even though it's been in the unified
	// map all along.
	auto new_files = ByEcid(state.Files());
	auto new_servers = ByEcid(state.Servers());
	auto new_clients = ByEcid(state.Clients());
	// Read the full dashboard for status_changed — the event payload
	// mirrors the REST /status nested envelope which pulls from
	// StatusSnapshot + KadSnapshot + ec_connected. Dashboard() takes
	// the State lock once for all three, so the rollup is coherent
	// (kad.network can't be from tick N+1 while ed2k.* is from tick
	// N).
	auto new_dashboard = state.Dashboard();
	const StatusSnapshot &new_status = new_dashboard.status;
	const KadSnapshot &new_kad = new_dashboard.kad;
	const bool new_ec = new_dashboard.ec_connected;

	// Files: role-flag-aware diff. download_* fires on is_downloading
	// transitions; shared_* on is_shared transitions. A single tick
	// can fire both for the same file (e.g. partfile becoming shared
	// + receiving a stat update on the download side).
	{
		std::vector<std::pair<std::string, std::string>> batch;
		batch.reserve(new_files.size());
		const auto push = [&](const char *name, const std::string &payload) {
			batch.emplace_back(name, payload);
		};
		// _removed first — clients can tear down their cache slot
		// before the _added/_updated for the same ECID lands.
		for (const auto &kv : prev.files) {
			const auto it = new_files.find(kv.first);
			if (it == new_files.end()) {
				if (kv.second.is_downloading) {
					push("download_removed", RemovedHashPayload(kv.second));
				}
				if (kv.second.is_shared) {
					push("shared_removed", RemovedHashPayload(kv.second));
				}
			} else {
				if (kv.second.is_downloading && !it->second.is_downloading) {
					push("download_removed", RemovedHashPayload(kv.second));
				}
				if (kv.second.is_shared && !it->second.is_shared) {
					push("shared_removed", RemovedHashPayload(kv.second));
				}
			}
		}
		// _added / _updated — gate by role flag transition vs the
		// previous tick's is_downloading / is_shared value.
		for (const auto &kv : new_files) {
			const auto it = prev.files.find(kv.first);
			const bool was_downloading = (it != prev.files.end() && it->second.is_downloading);
			const bool was_shared = (it != prev.files.end() && it->second.is_shared);
			if (kv.second.is_downloading) {
				if (!was_downloading) {
					push("download_added", ToJsonDownloadEvent(kv.second));
				} else if (!EqualDownload(it->second, kv.second)) {
					push("download_updated", ToJsonDownloadEvent(kv.second));
				}
			}
			if (kv.second.is_shared) {
				if (!was_shared) {
					push("shared_added", ToJsonSharedEvent(kv.second));
				} else if (!EqualShared(it->second, kv.second)) {
					push("shared_updated", ToJsonSharedEvent(kv.second));
				}
			}
		}
		bus.PublishBatch(batch);
	}
	DiffMap(bus, "server", prev.servers, new_servers, [](const ServerSnapshot &s) {
		return RemovedEcidPayload(s);
	});
	DiffMap(bus, "client", prev.clients, new_clients, [](const ClientSnapshot &c) {
		return RemovedEcidPayload(c);
	});

	// /status: one event when anything in the dashboard envelope
	// changes (StatusSnapshot fields OR Kad network rollup OR
	// ec_connected). Cold-start gates on `status_initialised` so we
	// don't blast a status_changed on the very first tick (SSE
	// subscribers already see the current state via REST; the
	// *change* events are what they're here for).
	if (!prev.status_initialised) {
		bus.Publish("status_changed", ToJsonStatusEvent(new_status, new_kad, new_ec));
		prev.status_initialised = true;
	} else if (!Equal(prev.status, new_status) || !Equal(prev.kad, new_kad) ||
		   prev.ec_connected != new_ec) {
		bus.Publish("status_changed", ToJsonStatusEvent(new_status, new_kad, new_ec));
	}

	// Snapshot the new state for next tick's diff baseline.
	prev.files = std::move(new_files);
	prev.servers = std::move(new_servers);
	prev.clients = std::move(new_clients);
	prev.status = new_status;
	prev.kad = new_kad;
	prev.ec_connected = new_ec;

	// Search events. `search_result_added` per new ECID in the results
	// map; `search_progress` on any percent change while running and on
	// the running→finished edge. The finished frame (state="finished",
	// percent=100) is just the terminal search_progress — there is no
	// separate search_finished event. The refresher's state machine
	// (AdvanceSearchProgress) drives both — POST /search seeds the active
	// flag; subsequent ticks either grow the results map, advance the
	// percent, or flip complete. First tick after MarkSearchStarted
	// bootstraps the baseline so we don't double-emit on first observation.
	{
		const auto search_now = ByEcid(state.Search());
		const auto progress_now = state.SearchProgress();
		if (!prev.search_initialised) {
			prev.search = search_now;
			prev.search_complete = progress_now.complete;
			prev.search_percent = progress_now.percent;
			prev.search_generation = progress_now.generation;
			prev.search_initialised = true;
		} else {
			// New result entries.
			for (const auto &kv : search_now) {
				if (prev.search.find(kv.first) == prev.search.end()) {
					std::ostringstream payload;
					// Byte-for-byte identical to WriteSearchObject (Api.cpp):
					// sources is a nested {total, complete} object, matching
					// the /search/results[] entry rather than flattening it.
					payload << "{\"hash\":\"" << EscJson(kv.second.hash) << "\""
						<< ",\"name\":\"" << EscJson(kv.second.name) << "\""
						<< ",\"size\":" << kv.second.size
						<< ",\"sources\":{\"total\":" << kv.second.source_count
						<< ",\"complete\":" << kv.second.complete_source_count << "}"
						<< ",\"already_have\":"
						<< (kv.second.already_have ? "true" : "false")
						<< ",\"rating\":" << static_cast<int>(kv.second.rating)
						<< ",\"status\":\"" << EscJson(kv.second.status) << "\""
						<< ",\"type\":\"" << EscJson(kv.second.type) << "\""
						<< "}";
					bus.Publish("search_result_added", payload.str());
				}
			}
			// search_progress: a percent change while running, the
			// running→finished edge (complete false→true), or a
			// generation bump from a new POST /search. The generation
			// trigger is what catches back-to-back searches whose
			// entire lifecycle fits inside one refresher tick — the
			// percent+complete comparison would see 100→100 / true→true
			// on such runs and emit nothing. MarkSearchStarted also
			// resets complete=false + percent=0 so ordinary runs still
			// get fresh edges too.
			const bool generation_bumped = progress_now.generation != prev.search_generation;
			const bool finished_edge = progress_now.complete && !prev.search_complete;
			const bool percent_moved = progress_now.percent != prev.search_percent;
			if (generation_bumped || finished_edge || percent_moved) {
				std::ostringstream payload;
				payload << "{\"state\":\"" << (progress_now.complete ? "finished" : "running")
					<< "\""
					<< ",\"percent\":" << progress_now.percent
					<< ",\"results\":" << search_now.size() << ",\"kind\":\""
					<< EscJson(progress_now.kind) << "\""
					<< "}";
				bus.Publish("search_progress", payload.str());
			}
			prev.search = search_now;
			prev.search_complete = progress_now.complete;
			prev.search_percent = progress_now.percent;
			prev.search_generation = progress_now.generation;
		}
	}

	// log_appended. CState::AmuleLog() is append-only
	// (CState.cpp:142-151) so a strictly-increasing size means the
	// refresher just appended the tail. First tick records the
	// size baseline silently — clients GET /api/v0/logs/amule for
	// the historical buffer; the event channel is for live tail
	// only. A truncation (size decreased) silently resyncs the
	// counter; the only path that truncates today is a future
	// `DELETE /logs/amule` mutation, and clients refetch on that
	// regardless.
	const auto amule_log = state.AmuleLog();
	if (!prev.amule_log_initialised) {
		prev.amule_log_count = amule_log.size();
		prev.amule_log_initialised = true;
	} else if (amule_log.size() < prev.amule_log_count) {
		prev.amule_log_count = amule_log.size();
	} else if (amule_log.size() > prev.amule_log_count) {
		std::ostringstream payload;
		payload << "{\"lines\":[";
		bool first = true;
		for (std::size_t i = prev.amule_log_count; i < amule_log.size(); ++i) {
			if (!first)
				payload << ",";
			first = false;
			payload << "\"" << EscJson(amule_log[i]) << "\"";
		}
		payload << "]}";
		bus.Publish("log_appended", payload.str());
		prev.amule_log_count = amule_log.size();
	}
}

} // namespace webapi
