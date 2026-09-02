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
#include "SearchJson.h"      // WriteSearchResultFields, shared with GET /search/{id}/results
#include "ServerFlagNames.h" // Shared server capability-bit tables, decoded to JSON

#include <JsonWriter.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <thread>
#include <type_traits>

namespace webapi
{

namespace
{

// Minimal JSON string escaper. JsonWriter (libwebcommon) is the
// canonical formatter for response bodies, but the event-data
// payloads we emit here are small and predictable — a few KB at
// most — and keeping the diff path independent of CJsonWriter
// avoids dragging wxString into the bus path. (The one exception is
// `search_result_added`, which is documented as carrying exactly a
// results-list entry and so is built by the shared writer in
// SearchJson.h rather than restated here.) Quote-escape only the
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
	  << ",\"size_bytes\":" << f.size << ",\"completed_bytes\":" << f.download.completed_bytes
	  << ",\"transferred_bytes\":" << f.download.transferred_bytes
	  << ",\"speed_bytes_per_second\":" << f.download.speed_bytes_per_second << ",\"status\":\""
	  << EscJson(f.download.status) << "\""
	  << ",\"priority\":\"" << EscJson(f.download.priority) << "\""
	  << ",\"priority_auto\":" << (f.download.priority_auto ? "true" : "false")
	  << ",\"category_index\":" << f.download.category << ",\"sources\":{"
	  << "\"total\":" << f.download.sources_total << ",\"unavailable\":" << f.download.sources_unavailable
	  << ",\"transferring\":" << f.download.sources_transferring
	  << ",\"a4af\":" << f.download.sources_a4af << "}"
	  << ",\"progress\":{\"percent\":" << JsonDoubleToString(f.download.percent) << "}"
	  << ",\"kad_comment_lookup_running\":" << (f.download.kad_comment_searching ? "true" : "false")
	  << ",\"hashed_part_count\":" << f.download.hashed_part_count
	  << ",\"parts_total_count\":" << webapi::PartCountForSize(f.size) << ",\"source_ecids\":[";
	bool first_a4af = true;
	for (const std::uint32_t ecid : f.download.a4af_sources) {
		if (!first_a4af)
			o << ",";
		first_a4af = false;
		o << ecid;
	}
	o << "]}";
	return o.str();
}

// comments_updated event payload — the GET /downloads/{hash}/comments body
// plus `hash`. Covers both retrieved Kad notes and comments reported by
// connected ed2k sources (they share source_comments).
//
// A strict superset of the endpoint, deliberately: the event needs `hash`
// because nothing else in the frame identifies the file, and it needs
// `kad_comment_lookup_running` because that flag is exactly what a client
// wants while a POST /downloads/{hash}/comments lookup is in flight. It used
// to carry the first and not the second, so a client that followed the docs
// and fed the event into the view it built from the endpoint silently lost
// the in-flight indicator.
std::string ToJsonCommentsEvent(const FileSnapshot &f)
{
	std::ostringstream o;
	o << "{\"hash\":\"" << EscJson(f.hash) << "\""
	  << ",\"kad_comment_lookup_running\":" << (f.download.kad_comment_searching ? "true" : "false")
	  << ",\"total\":" << f.download.source_comments.size() << ",\"comments\":[";
	bool first = true;
	for (const auto &c : f.download.source_comments) {
		if (!first)
			o << ",";
		first = false;
		o << "{\"username\":\"" << EscJson(c.username) << "\""
		  << ",\"filename\":\"" << EscJson(c.filename) << "\""
		  << ",\"rating\":" << c.rating << ",\"comment\":\"" << EscJson(c.comment) << "\"}";
	}
	o << "]}";
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
	  << ",\"size_bytes\":" << f.size << ",\"priority\":\"" << EscJson(f.shared.priority) << "\""
	  << ",\"priority_auto\":"
	  << (f.shared.priority_auto ? "true" : "false")
	  // Nested to match the REST row: a stated exception to R11, so that
	  // `sources.complete` is one access path across every endpoint that has
	  // the concept. The list shape carries `complete` only; the range is
	  // detail-only and does not ride the event.
	  << ",\"sources\":{\"complete\":" << f.shared.complete_sources
	  << "}"
	  // Flattened (R11), same as the REST row this promises key parity with.
	  << ",\"uploaded_bytes_session\":" << f.shared.uploaded_bytes_session
	  << ",\"uploaded_bytes_total\":" << f.shared.uploaded_bytes_total
	  << ",\"request_count_session\":" << f.shared.request_count_session
	  << ",\"request_count_total\":" << f.shared.request_count_total
	  << ",\"accepted_request_count_session\":" << f.shared.accepted_request_count_session
	  << ",\"accepted_request_count_total\":" << f.shared.accepted_request_count_total
	  << ",\"upload_speed_bytes_per_second\":" << f.shared.upload_speed_bytes_per_second
	  << ",\"uploading_client_count\":"
	  << f.shared.uploading_client_count
	  // Unix seconds, null when unknown -- never uploaded, or a known.met entry
	  // that predates the field. 0 reads as 1970 rather than "no idea", and the
	  // REST row this event promises key parity with has always sent null here
	  // (WriteIntOrNull in the shared list writer). A subscriber that hydrates
	  // from REST and live-updates from this saw its null flip to 0 on the
	  // first tick the file changed. Never-uploaded is the common case, so this
	  // was the routine reading, not an edge one.
	  << ",\"last_upload_at\":";
	if (f.shared.last_upload != 0)
		o << f.shared.last_upload;
	else
		o << "null";
	o << ",\"shared_since_at\":";
	if (f.shared.shared_since != 0)
		o << f.shared.shared_since;
	else
		o << "null";
	o << ",\"hashed_part_count\":" << SharedHashingProgress(f);
	// Media metadata rides the event because a metadata re-extraction is
	// otherwise invisible to a subscriber: the refresh endpoints answer 202
	// with no result, so this is how a client learns a probe landed. Six
	// small scalars, unlike the per-part arrays the list endpoints omit.
	//
	// null rather than absent when the file has none, matching the REST row
	// this event promises key parity with -- a subscriber diffing the two
	// must not find a key on one side only.
	o << ",\"media\":";
	if (f.has_media) {
		o << "{\"duration_seconds\":" << f.media.duration_seconds
		  << ",\"bitrate_kilobits_per_second\":" << f.media.bitrate_kilobits_per_second
		  << ",\"codec\":\"" << EscJson(f.media.codec) << "\""
		  << ",\"artist\":\"" << EscJson(f.media.artist) << "\""
		  << ",\"album\":\"" << EscJson(f.media.album) << "\""
		  << ",\"title\":\"" << EscJson(f.media.title) << "\"}";
	} else {
		o << "null";
	}
	o << "}";
	return o.str();
}

std::string ToJson(const ServerSnapshot &s)
{
	std::ostringstream o;
	o << "{"
	  << "\"ecid\":" << s.ecid << ",\"name\":\"" << EscJson(s.name) << "\""
	  << ",\"description\":\"" << EscJson(s.description) << "\""
	  << ",\"version\":\"" << EscJson(s.version) << "\""
	  << ",\"address\":\"" << EscJson(s.address)
	  << "\""
	  // The bare IP beside the "ip:port" form, matching the REST row.
	  << ",\"ip\":\"" << EscJson(s.address.substr(0, s.address.rfind(':'))) << "\""
	  << ",\"country_code\":"
	  << (s.country_code.empty() ? std::string("null") : "\"" + EscJson(s.country_code) + "\"")
	  << ",\"port\":" << s.port << ",\"user_count\":" << s.users << ",\"max_user_count\":" << s.max_users
	  << ",\"file_count\":" << s.files << ",\"soft_file_limit\":" << s.soft_file_limit
	  << ",\"hard_file_limit\":" << s.hard_file_limit << ",\"priority\":\"" << EscJson(s.priority) << "\""
	  << ",\"ping_ms\":" << s.ping_ms << ",\"failed_count\":" << s.failed_count << ",\"permanent\":"
	  << (s.is_static ? "true" : "false")
	  // Same fragment builder WriteServerObject uses, so the event payload and
	  // the REST object stay byte-identical here by construction.
	  << ",\"tcp_flags\":" << ServerTcpFlagsJson(s.tcp_flags)
	  << ",\"udp_flags\":" << ServerUdpFlagsJson(s.udp_flags) << "}";
	return o.str();
}

std::string ToJson(const FriendSnapshot &f)
{
	std::ostringstream o;
	o << "{"
	  << "\"ecid\":" << f.ecid << ",\"name\":\"" << EscJson(f.name) << "\""
	  << ",\"user_hash\":\"" << EscJson(f.user_hash)
	  << "\""
	  // null, not "" / 0, when the daemon has not reported an address: the
	  // REST row this event promises key parity with emits null for both
	  // (WriteFriendObject), and a subscriber hydrating from GET /friends
	  // would otherwise see ip flip null -> "" on the first tick that
	  // touches the row, with no real change behind it.
	  << ",\"ip\":" << (f.ip.empty() ? std::string("null") : "\"" + EscJson(f.ip) + "\"")
	  << ",\"port\":" << (f.ip.empty() ? std::string("null") : std::to_string(f.port))
	  << ",\"client_ecid\":" << (f.client_ecid ? std::to_string(f.client_ecid) : std::string("null"))
	  << ",\"online\":" << (f.client_ecid != 0 ? "true" : "false")
	  << ",\"friend_slot\":" << (f.friend_slot ? "true" : "false") << "}";
	return o.str();
}

std::string ToJson(const ClientSnapshot &c)
{
	std::ostringstream o;
	o << "{"
	  << "\"ecid\":" << c.ecid << ",\"name\":\"" << EscJson(c.client_name) << "\""
	  << ",\"user_hash\":\"" << EscJson(c.user_hash)
	  << "\""
	  // Same guard as country_code below and as WriteKnownClientObject's
	  // has_addr, which nulls ip/port/kad_port together.
	  << ",\"ip\":" << (c.ip.empty() ? std::string("null") : "\"" + EscJson(c.ip) + "\"")
	  << ",\"country_code\":"
	  // null, not "", when the lookup has not resolved -- the REST row this
	  // event promises key parity with emits null here.
	  << (c.country_code.empty() ? std::string("null") : "\"" + EscJson(c.country_code) + "\"")
	  << ",\"port\":" << (c.ip.empty() ? std::string("null") : std::to_string(c.port))
	  << ",\"software\":\"" << EscJson(c.software) << "\""
	  << ",\"software_version\":\"" << EscJson(c.software_version) << "\""
	  << ",\"reported_os\":\"" << EscJson(c.reported_os) << "\""
	  << ",\"upload_state\":\"" << EscJson(c.upload_state) << "\""
	  << ",\"download_state\":\"" << EscJson(c.download_state) << "\""
	  << ",\"ident_state\":\"" << EscJson(c.ident_state) << "\""
	  << ",\"download_file_name\":\"" << EscJson(c.download_file_name) << "\""
	  << ",\"upload_file_name\":\"" << EscJson(c.upload_file_name) << "\""
	  << ",\"upload_file_hash\":\"" << EscJson(c.upload_file_hash) << "\""
	  << ",\"download_file_hash\":\"" << EscJson(c.download_file_hash)
	  << "\""
	  // Flattened out of the old `xfer` wrapper (R11), same as the REST row
	  // this payload promises key parity with.
	  << ",\"uploaded_bytes_session\":" << c.uploaded_bytes_session
	  << ",\"downloaded_bytes_session\":" << c.downloaded_bytes_session
	  << ",\"uploaded_bytes_total\":" << c.uploaded_bytes_total
	  << ",\"downloaded_bytes_total\":" << c.downloaded_bytes_total
	  << ",\"upload_speed_bytes_per_second\":" << c.upload_speed_bytes_per_second
	  << ",\"download_speed_bytes_per_second\":" << c.download_speed_bytes_per_second
	  << ",\"upload_queue_position\":" << c.upload_queue_position << ",\"remote_queue_position\":"
	  << (c.remote_queue_position == kRemoteQueueFullSentinel ? std::string("null")
								  : std::to_string(c.remote_queue_position))
	  << ",\"upload_queue_score\":" << c.score << ",\"obfuscation_state\":\""
	  << EscJson(c.obfuscation_state) << "\""
	  << ",\"friend_slot\":" << (c.friend_slot ? "true" : "false") << ",\"source_origin\":\""
	  << EscJson(c.source_origin) << "\""
	  << ",\"parts_offered_count\":"
	  << (c.has_parts_offered_count ? std::to_string(c.parts_offered_count) : std::string("null"))
	  << ",\"client_mod_name\":\"" << EscJson(c.client_mod_name) << "\""
	  << ",\"shared_files_browsable\":" << (c.view_shared_disabled ? "false" : "true");
	// null, not omitted, matching the REST row: the field only means
	// something for a peer we are downloading from, and -1 is the
	// in-process sentinel that must never reach the wire. Formatted
	// through the shared writer rather than `<<`: the stream default is 6
	// significant digits (so SSE read 33.3333 where REST read
	// 33.333333333333336) and it honours LC_NUMERIC, which on an it/de/fr
	// locale would emit a comma and break the frame's JSON outright.
	o << ",\"part_progress_percent\":"
	  << (c.part_progress_percent >= 0.0 ? JsonDoubleToString(c.part_progress_percent)
					     : std::string("null"));
	o << "}";
	return o.str();
}

// Status event payload mirrors the REST /status envelope nesting
// (ed2k.*, kad.* including the kad.network rollup, speeds.*, queue.*,
// plus the top-level ec_connected flag). Takes a triple because the
// REST nesting groups data from StatusSnapshot AND KadSnapshot AND
// the dashboard's ec_connected bit — all three are read in one
// shared_lock by state.Dashboard() at the call site.
// A free-space figure renders as a JSON number, or as null when the daemon
// has none (-1). Kept beside the REST handler's identical rule so the SSE
// payload and the REST body cannot drift apart.
std::string JsonFreeSpace(std::int64_t v)
{
	return v < 0 ? std::string("null") : std::to_string(v);
}

// `null` when the value was never measured, matching WriteIntOrNull /
// WriteBoolOrNull on the REST side. The two bodies are promised to be
// byte-identical, so the disconnected fields have to print `null` here too --
// and the comparators below have to treat null<->value as a change, or the
// event stops firing on the very edge that flips them.
std::string JsonNumOrNull(bool known, std::uint64_t v)
{
	return known ? std::to_string(v) : std::string("null");
}

std::string JsonBoolOrNull(bool known, bool v)
{
	return known ? std::string(v ? "true" : "false") : std::string("null");
}

// Mirrors HandleStatus key for key -- EVENTS.md promises this payload is
// identical to the REST /status envelope, and 22-sse-diff-emission.sh asserts
// it. Both connected_since_at values are 0 while not connected, same rule as
// there: gate on state rather than trusting a 0 timestamp.
std::string ToJsonStatusEvent(const StatusSnapshot &s, const KadSnapshot &k, bool ec_connected)
{
	std::ostringstream o;
	o << "{"
	  << "\"ec_connected\":" << (ec_connected ? "true" : "false") << ",\"ed2k\":{"
	  << "\"state\":\"" << EscJson(s.ed2k_state) << "\""
	  << ",\"high_id\":" << (s.ed2k_high_id ? "true" : "false") << ",\"user_id\":" << s.ed2k_user_id
	  << ",\"public_ip\":\"" << EscJson(s.ed2k_public_ip) << "\""
	  << ",\"connected_since_at\":" << s.ed2k_connected_since << ",\"server_name\":\""
	  << EscJson(s.server_name) << "\""
	  << ",\"server_ip\":\"" << EscJson(s.server_ip) << "\""
	  << ",\"server_port\":" << s.server_port << ",\"network\":{"
	  << "\"user_count\":" << JsonNumOrNull(s.has_ed2k_network, s.ed2k_users)
	  << ",\"file_count\":" << JsonNumOrNull(s.has_ed2k_network, s.ed2k_files) << "}}"
	  << ",\"kad\":{"
	  << "\"state\":\"" << EscJson(s.kad_state) << "\""
	  << ",\"firewalled_tcp\":" << JsonBoolOrNull(s.has_kad_firewalled_tcp, s.kad_firewalled_tcp)
	  << ",\"connected_since_at\":" << s.kad_connected_since << ",\"network\":{"
	  << "\"user_count\":" << JsonNumOrNull(k.has_network, k.users)
	  << ",\"file_count\":" << JsonNumOrNull(k.has_network, k.files)
	  << ",\"node_count\":" << JsonNumOrNull(k.has_network, k.nodes) << "}"
	  << "}"
	  << ",\"speeds\":{"
	  << "\"download_bytes_per_second\":" << s.download_bytes_per_second
	  << ",\"upload_bytes_per_second\":" << s.upload_bytes_per_second
	  << ",\"download_overhead_bytes_per_second\":" << s.download_overhead_bytes_per_second
	  << ",\"upload_overhead_bytes_per_second\":" << s.upload_overhead_bytes_per_second << "}"
	  << ",\"disk\":{"
	  // null, not the -1 sentinel and not 0 -- same reasoning as the REST body.
	  << "\"temp_free_bytes\":" << JsonFreeSpace(s.temp_free_bytes)
	  << ",\"incoming_free_bytes\":" << JsonFreeSpace(s.incoming_free_bytes) << "}"
	  << ",\"queue\":{"
	  << "\"waiting_upload_client_count\":" << s.ul_queue_len
	  << ",\"download_source_count\":" << s.total_src_count << "}"
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
	       a.download.completed_bytes == b.download.completed_bytes &&
	       a.download.transferred_bytes == b.download.transferred_bytes &&
	       a.download.speed_bytes_per_second == b.download.speed_bytes_per_second &&
	       a.download.status == b.download.status &&
	       a.download.priority_auto == b.download.priority_auto &&
	       a.download.category == b.download.category &&
	       a.download.sources_total == b.download.sources_total &&
	       a.download.sources_unavailable == b.download.sources_unavailable &&
	       a.download.sources_transferring == b.download.sources_transferring &&
	       a.download.sources_a4af == b.download.sources_a4af &&
	       a.download.percent == b.download.percent &&
	       a.download.kad_comment_searching == b.download.kad_comment_searching &&
	       a.download.hashed_part_count == b.download.hashed_part_count &&
	       // The membership, not the `sources_a4af` count beside it: a swap
	       // moves one client out and another in, so the count never budges
	       // and comparing it would publish nothing.
	       a.download.a4af_sources == b.download.a4af_sources;
}

// Comment list equality (deliberately NOT part of EqualDownload — a comment
// change drives the separate comments_updated event, not download_updated).
bool EqualComments(const FileSnapshot &a, const FileSnapshot &b)
{
	// The in-flight flag is part of the payload, so it has to be part of
	// the comparison: without it the true->false edge at the end of a Kad
	// lookup fires no event at all, and a `?channels=comments` subscriber
	// is left with its spinner stuck on. Every field the event emits must
	// be compared here or the event cannot announce it changing.
	if (a.download.kad_comment_searching != b.download.kad_comment_searching)
		return false;
	const auto &ca = a.download.source_comments;
	const auto &cb = b.download.source_comments;
	if (ca.size() != cb.size())
		return false;
	for (std::size_t i = 0; i < ca.size(); ++i) {
		if (ca[i].username != cb[i].username || ca[i].filename != cb[i].filename ||
			ca[i].rating != cb[i].rating || ca[i].comment != cb[i].comment)
			return false;
	}
	return true;
}
bool EqualShared(const FileSnapshot &a, const FileSnapshot &b)
{
	return a.ecid == b.ecid && a.hash == b.hash && a.name == b.name && a.ed2k_link == b.ed2k_link &&
	       a.size == b.size && a.shared.priority == b.shared.priority &&
	       a.shared.priority_auto == b.shared.priority_auto &&
	       a.shared.complete_sources == b.shared.complete_sources &&
	       a.shared.uploaded_bytes_session == b.shared.uploaded_bytes_session &&
	       a.shared.uploaded_bytes_total == b.shared.uploaded_bytes_total &&
	       a.shared.request_count_session == b.shared.request_count_session &&
	       a.shared.request_count_total == b.shared.request_count_total &&
	       a.shared.accepted_request_count_session == b.shared.accepted_request_count_session &&
	       a.shared.accepted_request_count_total == b.shared.accepted_request_count_total &&
	       a.shared.upload_speed_bytes_per_second == b.shared.upload_speed_bytes_per_second &&
	       a.shared.uploading_client_count == b.shared.uploading_client_count &&
	       a.shared.last_upload == b.shared.last_upload &&
	       a.shared.shared_since == b.shared.shared_since &&
	       // Media metadata, so a re-extraction emits shared_updated at all.
	       // Without these a file whose metadata just changed compares EQUAL
	       // and the refresh is invisible to every subscriber -- which is the
	       // only progress signal the 202-returning refresh endpoints have.
	       // These change once per probe, not per tick, so they cost nothing
	       // in event volume.
	       a.has_media == b.has_media && a.media.duration_seconds == b.media.duration_seconds &&
	       a.media.bitrate_kilobits_per_second == b.media.bitrate_kilobits_per_second &&
	       a.media.codec == b.media.codec && a.media.artist == b.media.artist &&
	       a.media.album == b.media.album && a.media.title == b.media.title &&
	       // Through the accessor, not the raw field: a shared download's
	       // progress lives on the download side, and comparing the raw
	       // field would hold every tick of it back from shared_updated.
	       SharedHashingProgress(a) == SharedHashingProgress(b);
}
bool Equal(const ServerSnapshot &a, const ServerSnapshot &b)
{
	return a.name == b.name && a.description == b.description && a.version == b.version &&
	       a.address == b.address && a.country_code == b.country_code && a.port == b.port &&
	       a.users == b.users && a.max_users == b.max_users && a.files == b.files &&
	       a.soft_file_limit == b.soft_file_limit && a.hard_file_limit == b.hard_file_limit &&
	       a.tcp_flags == b.tcp_flags && a.udp_flags == b.udp_flags && a.priority == b.priority &&
	       a.ping_ms == b.ping_ms && a.failed_count == b.failed_count && a.is_static == b.is_static;
}
bool Equal(const FriendSnapshot &a, const FriendSnapshot &b)
{
	// client_ecid is part of the identity here on purpose: it going to 0 is
	// the friend going offline, which is exactly what a subscriber watching
	// the connected indicator needs to hear about.
	return a.name == b.name && a.user_hash == b.user_hash && a.ip == b.ip && a.port == b.port &&
	       a.client_ecid == b.client_ecid && a.friend_slot == b.friend_slot;
}
bool Equal(const ClientSnapshot &a, const ClientSnapshot &b)
{
	return a.client_name == b.client_name && a.user_hash == b.user_hash && a.ip == b.ip &&
	       a.country_code == b.country_code && a.port == b.port && a.software == b.software &&
	       a.software_version == b.software_version && a.reported_os == b.reported_os &&
	       a.upload_state == b.upload_state && a.download_state == b.download_state &&
	       a.ident_state == b.ident_state && a.download_file_name == b.download_file_name &&
	       a.upload_file_name == b.upload_file_name && a.upload_file_hash == b.upload_file_hash &&
	       a.download_file_hash == b.download_file_hash &&
	       a.uploaded_bytes_session == b.uploaded_bytes_session &&
	       a.downloaded_bytes_session == b.downloaded_bytes_session &&
	       a.uploaded_bytes_total == b.uploaded_bytes_total &&
	       a.downloaded_bytes_total == b.downloaded_bytes_total &&
	       a.upload_speed_bytes_per_second == b.upload_speed_bytes_per_second &&
	       a.download_speed_bytes_per_second == b.download_speed_bytes_per_second &&
	       a.upload_queue_position == b.upload_queue_position &&
	       a.remote_queue_position == b.remote_queue_position && a.score == b.score &&
	       a.obfuscation_state == b.obfuscation_state && a.friend_slot == b.friend_slot &&
	       a.source_origin == b.source_origin && a.parts_offered_count == b.parts_offered_count &&
	       // Without the flag, null -> 0 (the part map arriving and reporting
	       // zero) compares equal and the row never updates.
	       a.has_parts_offered_count == b.has_parts_offered_count &&
	       a.client_mod_name == b.client_mod_name && a.view_shared_disabled == b.view_shared_disabled &&
	       // Derived from parts_offered_count and the linked file's part count,
	       // so it normally moves only when a compared field does. The case
	       // that needs it in its own right is the file going away: the
	       // percent drops back to its sentinel while every other field
	       // holds, and without this the payload would change with no event.
	       a.part_progress_percent == b.part_progress_percent;
}
bool Equal(const StatusSnapshot &a, const StatusSnapshot &b)
{
	// public_ip is derived from ed2k_user_id, so comparing the id covers it.
	return a.ed2k_state == b.ed2k_state && a.kad_state == b.kad_state &&
	       a.ed2k_high_id == b.ed2k_high_id && a.ed2k_user_id == b.ed2k_user_id &&
	       a.ed2k_connected_since == b.ed2k_connected_since &&
	       a.kad_connected_since == b.kad_connected_since &&
	       a.kad_firewalled_tcp == b.kad_firewalled_tcp && a.server_name == b.server_name &&
	       a.server_ip == b.server_ip && a.server_port == b.server_port &&
	       a.download_bytes_per_second == b.download_bytes_per_second &&
	       a.upload_bytes_per_second == b.upload_bytes_per_second &&
	       a.download_overhead_bytes_per_second == b.download_overhead_bytes_per_second &&
	       a.upload_overhead_bytes_per_second == b.upload_overhead_bytes_per_second &&
	       a.temp_free_bytes == b.temp_free_bytes && a.incoming_free_bytes == b.incoming_free_bytes &&
	       a.ul_queue_len == b.ul_queue_len && a.total_src_count == b.total_src_count &&
	       // The has_ flags are part of the comparison, not just the values: a
	       // disconnect flips these to null while the underlying ints keep
	       // their last reading, so comparing the ints alone would miss the
	       // edge and the event would stop firing exactly when it matters.
	       a.has_ed2k_network == b.has_ed2k_network && a.ed2k_users == b.ed2k_users &&
	       a.ed2k_files == b.ed2k_files && a.has_kad_firewalled_tcp == b.has_kad_firewalled_tcp;
}
bool Equal(const KadSnapshot &a, const KadSnapshot &b)
{
	// This is the SEPARATE gate for the kad half of status_changed -- the
	// status comparator above does not cover these. has_network is compared
	// first because it is the field that changes on a connect/disconnect edge
	// while users/files/nodes keep their last values underneath.
	return a.has_network == b.has_network && a.users == b.users && a.files == b.files &&
	       a.nodes == b.nodes;
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
std::string RemovedHashPayload(const std::string &hash)
{
	return "{\"hash\":\"" + EscJson(hash) + "\"}";
}

// Every ECID-keyed collection identifies a removed entry the same way, now
// that each object names its own handle `ecid` (issue #976): one shape, one
// function, rather than a per-type overload that only differed in the key it
// spelled.
template <class Snapshot> std::string RemovedEcidPayload(const Snapshot &item)
{
	// The per-type overloads this replaced could only be called with an
	// ECID-keyed snapshot; an unconstrained template accepts anything with
	// an `.ecid` member, and FileSnapshot has one (State.h) while its
	// collections are hash-keyed and their consumers expect {"hash":...}.
	// Wiring one through DiffMap would otherwise compile and emit the wrong
	// shape at runtime.
	static_assert(!std::is_same<Snapshot, FileSnapshot>::value,
		"file collections are hash-keyed -- use RemovedHashPayload");
	std::ostringstream o;
	o << "{\"ecid\":" << item.ecid << "}";
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

// Every file event resolves its payload through `prev.files` after the locked
// walk, which holds only because nothing erases from that map in between: the
// `gone` sweep runs after the batch is built, and `gone` is disjoint from
// everything the walk recorded. Unreachable today -- but a dropped event is
// invisible, and a lost `shared_removed` leaves a ghost row on every client
// until something else happens to touch that file. Hard-abort for the same
// reason EnforceSinglePublisher does: the check has to survive -DNDEBUG,
// because that is where the ordering will actually get broken.
[[noreturn]] void AbortOnMissingBaseline(const char *event_name, std::uint32_t ecid)
{
	std::cerr << "amuleapi: file diff lost the baseline entry for ECID " << ecid << " while building "
		  << event_name << "; the prev.files erase has moved ahead of the batch build.\n";
	std::abort();
}

} // namespace

// One chat message as the `message` object both the SSE payload and
// GET /chats/{client_address}/messages expose. Written here in the same string-building
// style as the other event payloads in this file; the REST side renders the
// identical shape through CJsonWriter.
std::string ChatMessageJson(const ChatMessageSnapshot &msg)
{
	return "{\"id\":" + std::to_string(msg.id) + ",\"direction\":\"" + (msg.outgoing ? "out" : "in") +
	       "\",\"text\":\"" + EscJson(msg.text) + "\",\"sent_at\":" + std::to_string(msg.timestamp) + "}";
}

void PublishChatEvents(CEventBus &bus,
	const std::vector<ChatSessionSnapshot> &new_messages,
	const std::vector<std::uint64_t> &closed)
{
	if (new_messages.empty() && closed.empty())
		return;

	std::vector<std::pair<std::string, std::string>> batch;
	for (const ChatSessionSnapshot &session : new_messages) {
		const std::string peer = session.PeerKey();
		for (const ChatMessageSnapshot &msg : session.messages) {
			std::string payload = "{\"client_address\":\"" + EscJson(peer) + "\",\"ip\":\"" +
					      EscJson(session.ip) +
					      "\",\"port\":" + std::to_string(session.port) + ",\"name\":\"" +
					      EscJson(session.DisplayName()) +
					      // client_ecid / friend_ecid are null rather than the 0
					      // sentinel, matching the REST row (R10).
					      "\",\"client_ecid\":" +
					      (session.client_ecid ? std::to_string(session.client_ecid)
								   : std::string("null")) +
					      ",\"friend_ecid\":" +
					      (session.friend_ecid ? std::to_string(session.friend_ecid)
								   : std::string("null")) +
					      ",\"message\":" + ChatMessageJson(msg) + "}";
			batch.emplace_back("chat_message", std::move(payload));
		}
	}
	for (std::uint64_t gui_id : closed) {
		const std::string peer = ChatPeerKeyFromGuiId(gui_id);
		batch.emplace_back("chat_session_closed", "{\"client_address\":\"" + EscJson(peer) + "\"}");
	}
	bus.PublishBatch(batch);
}

void EmitDiffsAndUpdate(CEventBus &bus, LastSeenState &prev, const CState &state)
{
	EnforceSinglePublisher();
	// Snapshot the current state under its read locks. Each accessor takes the
	// shared_timed_mutex shared, copies, and returns. Files are the exception,
	// walked in place further down: the diff needs the unified map, not a
	// role-filtered view of it, so it can see a file that flipped is_shared
	// false→true on an existing ECID and fire `shared_added` for it even though
	// the entry was there all along.
	auto new_servers = ByEcid(state.Servers());
	auto new_friends = ByEcid(state.Friends());
	auto new_clients = ByEcid(state.Clients());
	// part_progress_percent is derived, not refreshed: it needs the part count
	// of the file the peer is a source for, which lives in a different
	// snapshot, so the refresher leaves it at its sentinel and the REST
	// handlers fill it in per request. Do the same here, or the event would
	// be the one payload a subscriber has to re-GET to complete -- the exact
	// thing EVENTS.md promises it never has to. Computed on the copies before
	// both the Equal() comparison and the serialiser see them, so the
	// baseline and the payload always agree.
	for (auto &kv : new_clients) {
		ComputePartProgressPercent(state, kv.second);
	}
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

	// Files: role-flag-aware diff, run against the live map rather than a copy
	// of it. download_* fires on is_downloading transitions, shared_* on
	// is_shared transitions, and a single tick can fire both for the same file
	// (a partfile becoming shared while its download side also moved).
	//
	// prev.files is a comparison baseline, not a mirror: an entry is rewritten
	// exactly when a predicate below reports a difference, so the fields those
	// predicates read stay fresh and others may lag. A new predicate has to go
	// into the write-back condition too, not only the emit condition.
	{
		// Decided under the read lock, serialised after it. The payloads are
		// the full snapshot shape, so a cold-start tick or a shared-files
		// reload builds one per file; doing that inside the lock would queue
		// the refresher's own writer and every reader behind it, which is the
		// cost this change exists to remove. What the walk records instead is
		// the event and its subject: a hash for a removal, an ECID for
		// everything else, resolved against `prev.files` once the lock is
		// released -- the write-back below leaves that entry equal to the live
		// one, so it is the same object the payload would have been built from.
		// Removals name their subject by ECID for the same reason: copying the
		// hash out would put one string allocation per removed file back under
		// the lock, and `prev.files` still holds the entry -- the `gone` erase
		// is deferred until the batch is built.
		enum class Change
		{
			DownloadAdded,
			DownloadUpdated,
			SharedAdded,
			SharedUpdated,
			CommentsUpdated,
		};
		std::vector<std::pair<const char *, std::uint32_t>> removed;
		std::vector<std::pair<Change, std::uint32_t>> changed;
		// ECIDs to drop from the baseline once the batch is built -- erasing
		// during the walk would invalidate the iterator, and erasing before
		// the batch would take the removal payloads' hashes with it.
		std::vector<std::uint32_t> gone;
		state.WithFiles([&](const FileMap &files) {
			// _removed first — clients can tear down their cache slot
			// before the _added/_updated for the same ECID lands.
			for (const auto &kv : prev.files) {
				const auto it = files.find(kv.first);
				const bool absent = (it == files.end());
				if (kv.second.is_downloading && (absent || !it->second.is_downloading)) {
					removed.emplace_back("download_removed", kv.first);
				}
				if (kv.second.is_shared && (absent || !it->second.is_shared)) {
					removed.emplace_back("shared_removed", kv.first);
				}
				if (absent)
					gone.push_back(kv.first);
			}
			// _added / _updated — gated by the role-flag transition against
			// the previous tick's is_downloading / is_shared value.
			for (const auto &entry : files) {
				const FileSnapshot &now = entry.second;
				const auto it = prev.files.find(entry.first);
				const bool known = (it != prev.files.end());
				const bool was_downloading = known && it->second.is_downloading;
				const bool was_shared = known && it->second.is_shared;
				bool moved = !known || was_downloading != now.is_downloading ||
					     was_shared != now.is_shared;
				if (now.is_downloading) {
					if (!was_downloading) {
						changed.emplace_back(Change::DownloadAdded, entry.first);
						// The flag counts as comment state, exactly as
						// it does in EqualComments. Gating on the list
						// alone means a download first seen with a Kad
						// lookup already in flight never announces the
						// lookup at all -- the mirror of the edge where
						// a finished lookup never announced its end,
						// leaving the same indicator wrong in the
						// opposite direction.
						if (!now.download.source_comments.empty() ||
							now.download.kad_comment_searching) {
							changed.emplace_back(
								Change::CommentsUpdated, entry.first);
						}
					} else {
						if (!EqualDownload(it->second, now)) {
							changed.emplace_back(
								Change::DownloadUpdated, entry.first);
							moved = true;
						}
						// Independent of download_updated: fires for Kad notes AND
						// comments reported by connected sources (issue #434 / #419).
						if (!EqualComments(it->second, now)) {
							changed.emplace_back(
								Change::CommentsUpdated, entry.first);
							moved = true;
						}
					}
				}
				if (now.is_shared) {
					if (!was_shared) {
						changed.emplace_back(Change::SharedAdded, entry.first);
					} else if (!EqualShared(it->second, now)) {
						changed.emplace_back(Change::SharedUpdated, entry.first);
						moved = true;
					}
				}
				if (!moved)
					continue;
				if (known)
					it->second = now;
				else
					prev.files.emplace(entry.first, now);
			}
		});
		std::vector<std::pair<std::string, std::string>> batch;
		batch.reserve(removed.size() + changed.size());
		for (const auto &r : removed) {
			// Still in the baseline: `gone` is erased below, once every
			// payload that reads through it has been built.
			const auto it = prev.files.find(r.second);
			if (it == prev.files.end())
				AbortOnMissingBaseline(r.first, r.second);
			batch.emplace_back(r.first, RemovedHashPayload(it->second.hash));
		}
		for (const auto &c : changed) {
			// Every recorded change set `moved`, so its entry was written
			// back; `gone` holds only ECIDs absent from the live map, which
			// these are not.
			const auto it = prev.files.find(c.second);
			if (it == prev.files.end())
				AbortOnMissingBaseline("a file event", c.second);
			const FileSnapshot &f = it->second;
			switch (c.first) {
			case Change::DownloadAdded:
				batch.emplace_back("download_added", ToJsonDownloadEvent(f));
				break;
			case Change::DownloadUpdated:
				batch.emplace_back("download_updated", ToJsonDownloadEvent(f));
				break;
			case Change::SharedAdded:
				batch.emplace_back("shared_added", ToJsonSharedEvent(f));
				break;
			case Change::SharedUpdated:
				batch.emplace_back("shared_updated", ToJsonSharedEvent(f));
				break;
			case Change::CommentsUpdated:
				batch.emplace_back("comments_updated", ToJsonCommentsEvent(f));
				break;
			}
		}
		for (const std::uint32_t ecid : gone)
			prev.files.erase(ecid);
		bus.PublishBatch(batch);
	}
	DiffMap(bus, "server", prev.servers, new_servers, [](const ServerSnapshot &s) {
		return RemovedEcidPayload(s);
	});
	DiffMap(bus, "client", prev.clients, new_clients, [](const ClientSnapshot &c) {
		return RemovedEcidPayload(c);
	});
	// Note for consumers: a single PATCH of the friend slot can produce two
	// friend_updated events, because granting it to one friend clears it on
	// whoever held it before.
	DiffMap(bus, "friend", prev.friends, new_friends, [](const FriendSnapshot &f) {
		return RemovedEcidPayload(f);
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
	prev.servers = std::move(new_servers);
	prev.clients = std::move(new_clients);
	prev.friends = std::move(new_friends);
	prev.status = new_status;
	prev.kad = new_kad;
	prev.ec_connected = new_ec;

	// The stable-but-mutable field set for `search_result_updated`. A hit's
	// identity fields (hash, name, size, type, directory, media, children)
	// never change for a given ECID, and its source counts churn every tick
	// while the search runs -- where `search_progress` is already the re-read
	// cue. What is left is the set that can change AFTER a search finishes,
	// when no other signal exists: download state (`status` /
	// `already_downloaded`), and the Kad-notes cluster (`comments[]`, the
	// in-flight flag, and `rating`, which aggregates from the comments).
	// Comparing only these keeps the search channel quiet on a running
	// search instead of firing per-result frames on source-count churn.
	const auto result_mutated = [](const SearchResult &a, const SearchResult &b) {
		if (a.status != b.status || a.already_downloaded != b.already_downloaded ||
			a.rating != b.rating || a.kad_comment_searching != b.kad_comment_searching ||
			a.comments.size() != b.comments.size())
			return true;
		for (std::size_t i = 0; i < a.comments.size(); ++i) {
			const auto &ca = a.comments[i];
			const auto &cb = b.comments[i];
			if (ca.username != cb.username || ca.filename != cb.filename ||
				ca.rating != cb.rating || ca.comment != cb.comment)
				return true;
		}
		return false;
	};

	// Search events. `search_result_added` per new ECID in the results
	// map; `search_result_updated` when one of a held result's
	// stable-but-mutable fields changes (see result_mutated above);
	// `search_progress` on any percent change while running and on
	// the running→finished edge. The finished frame (state="finished",
	// percent=100) is just the terminal search_progress — there is no
	// separate search_finished event. The refresher's state machine
	// (AdvanceSearchProgress) drives both — POST /search seeds the active
	// flag; subsequent ticks either grow the results map, advance the
	// percent, or flip complete. First tick after MarkSearchStarted
	// bootstraps the baseline so we don't double-emit on first observation.
	{
		// Multi-search: diff every open search independently, keyed by
		// search_id, and stamp that id on each event so subscribers demux.
		const auto ids = state.AllSearchIds();
		for (std::uint32_t sid : ids) {
			const auto search_now = ByEcid(state.Search(sid));
			const auto progress_now = state.SearchProgress(sid);

			// Cold start (first tick ever): baseline every pre-existing
			// search silently so history isn't replayed as events. A
			// search that appears LATER has no prev entry, so its
			// generation (0) differs from the live one — the progress
			// edge below fires its initial "running" frame, and its
			// results stream in as ordinary additions.
			if (!prev.search_initialised) {
				auto &b = prev.searches[sid];
				b.results = search_now;
				b.complete = progress_now.complete;
				b.percent = progress_now.percent;
				b.generation = progress_now.generation;
				continue;
			}

			auto &pstate = prev.searches[sid];
			// New and mutated result entries for this search.
			for (const auto &kv : search_now) {
				const auto pit = pstate.results.find(kv.first);
				const bool is_new = pit == pstate.results.end();
				if (!is_new && !result_mutated(pit->second, kv.second))
					continue;
				// `search_id` routes the event to a tab/view; every
				// field after it comes from the same writer
				// GET /search/{id}/results uses, which is what makes
				// the documented "byte-for-byte identical to a
				// results-list entry" promise hold by construction
				// rather than by review.
				//
				// `search_result_updated` carries the identical payload
				// under its own name, rather than re-firing _added with
				// upsert semantics: a consumer that only handles _added
				// keeps exactly the behaviour it had, and one that wants
				// live rows opts in by handling the new event. It is the
				// close of the one window where a client could not know:
				// a finished search stops emitting search_progress, yet a
				// hit downloaded from it flips status / already_downloaded, and
				// a Kad notes lookup lands comments / rating after the
				// fact. (Those fields are polled at all because the union
				// keeps finished searches in the per-tick poll set.)
				CJsonWriter w;
				w.BeginObject();
				w.Key("search_id");
				w.ValueInt(static_cast<int64_t>(sid));
				WriteSearchResultFields(w, kv.second);
				w.EndObject();
				bus.Publish(is_new ? "search_result_added" : "search_result_updated",
					w.TakeBuffer());
			}
			// search_progress: a percent change while running, the
			// running→finished edge (complete false→true), or a
			// generation bump (new POST /search, or the first observation
			// of this search_id). The generation trigger catches
			// back-to-back searches whose whole lifecycle fits inside one
			// refresher tick — the percent+complete comparison would see
			// 100→100 / true→true and emit nothing.
			const bool generation_bumped = progress_now.generation != pstate.generation;
			const bool finished_edge = progress_now.complete && !pstate.complete;
			const bool percent_moved = progress_now.percent != pstate.percent;
			if (generation_bumped || finished_edge || percent_moved) {
				std::ostringstream payload;
				payload << "{\"search_id\":" << sid << ",\"state\":\""
					<< (progress_now.complete ? "finished" : "running") << "\""
					<< ",\"percent\":"
					<< progress_now.percent
					// `result_count`: a plural key held an integer while `results` is an
					// array everywhere else, and GET /search already calls this number
					// result_count.
					<< ",\"result_count\":" << search_now.size() << ",\"type\":\""
					<< EscJson(progress_now.kind) << "\""
					<< "}";
				bus.Publish("search_progress", payload.str());
			}
			pstate.results = search_now;
			pstate.complete = progress_now.complete;
			pstate.percent = progress_now.percent;
			pstate.generation = progress_now.generation;
		}
		prev.search_initialised = true;
		// Prune baselines for searches that vanished (closed / EC reset) so
		// prev.searches can't grow without bound, and tell subscribers.
		// Without the event a consumer holding one tab per search only finds
		// out on its next read, and with SSE live it may never read again.
		//
		// This fires only when the SLOT is gone -- DELETE /search/{id}, the
		// slot cap evicting an old finished search, or an EC reset. A search
		// the daemon evicted from its own ring is retired as finished and
		// kept locally for late reads, so that case is a terminal
		// search_progress above, never a search_closed.
		for (auto it = prev.searches.begin(); it != prev.searches.end();) {
			if (std::find(ids.begin(), ids.end(), it->first) == ids.end()) {
				std::ostringstream payload;
				payload << "{\"search_id\":" << it->first << "}";
				bus.Publish("search_closed", payload.str());
				it = prev.searches.erase(it);
			} else {
				++it;
			}
		}
	}

	// log_appended. The refresher only ever appends, so a size that grew means
	// the tail is new. First tick records the baseline silently — clients GET
	// /api/v0/logs/amule for the history; this channel is the live tail only.
	//
	// A size that shrank means `DELETE /logs/amule` cleared the buffer, and the
	// only thing this does about it is re-point the counter. Lines appended
	// between that DELETE and this tick are NOT published, and if the counter
	// was below the new size the append branch publishes a mid-buffer slice as
	// though it were a tail. Only the client that issued the DELETE knows to
	// refetch; other subscribers are not told. Pre-existing, and fixable by
	// publishing `resync` on the reset edge -- which needs the bus-published
	// resync to bypass the `?channels=` filter, so it is not this change.
	//
	// Size and tail in one read: the history is uncapped, so asking AmuleLog()
	// for a `.size()` that is unchanged on almost every tick copies all of it
	// -- and splitting the two would let that DELETE land in between, pairing
	// a pre-truncation size with an empty tail.
	std::size_t log_size = 0;
	const auto tail = state.AmuleLogFrom(prev.amule_log_count, log_size);
	if (!prev.amule_log_initialised) {
		prev.amule_log_count = log_size;
		prev.amule_log_initialised = true;
	} else if (log_size < prev.amule_log_count) {
		prev.amule_log_count = log_size;
	} else if (!tail.empty()) {
		std::ostringstream payload;
		payload << "{\"lines\":[";
		bool first = true;
		for (const std::string &line : tail) {
			if (!first)
				payload << ",";
			first = false;
			payload << "\"" << EscJson(line) << "\"";
		}
		payload << "]}";
		bus.Publish("log_appended", payload.str());
		prev.amule_log_count = prev.amule_log_count + tail.size();
	}
}

} // namespace webapi
