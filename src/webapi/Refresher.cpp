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
// Pure EC-tag-to-State translation layer. No CamuleapiApp dependency
// — the per-tick orchestration (`RefresherTick` + `TwoPhaseRefresh`)
// lives in RefresherTick.cpp so the unit tests can link these
// transformations in isolation.

#include "Refresher.h"

#include "State.h"

#include "Constants.h"                            // PS_* / PR_* / US_* / DS_* / OBST_* enums
#include "OtherFunctions.h"                       // GetFiletypeByName for the search-result `type`
#include <common/Path.h>                          // CPath
#include "ClientList.h"                           // buddyState enum (Disconnected/Connecting/Connected)
#include "ClientCredits.h"                        // EIdentState (IS_NOTAVAILABLE / IS_IDENTIFIED / ...)
#include "Server.h"                               // SRV_PR_* server priority constants
#include "RLE.h"                                  // PartFileEncoderData (stateful gap/part decoder)
#include "Types.h"                                // ArrayOfUInts16 / ArrayOfUInts64
#include "include/protocol/ed2k/ClientSoftware.h" // SO_* client-software enum

#include <ec/cpp/ECSpecialTags.h>
#include <ec/cpp/ECPacket.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace webapi
{

namespace
{

const char *Ed2kStateString(const CEC_ConnState_Tag *conn)
{
	if (!conn)
		return "disconnected";
	if (conn->IsConnectedED2K())
		return "connected";
	if (conn->IsConnectingED2K())
		return "connecting";
	return "disconnected";
}

const char *KadStateString(const CEC_ConnState_Tag *conn)
{
	// Kad has a "running but disconnected" mode (peer-discovery active,
	// no contact-routing yet); we collapse that into "connecting" so
	// the API surface uses three states uniformly for both networks.
	if (!conn || !conn->IsKadRunning())
		return "disabled";
	if (conn->IsConnectedKademlia())
		return "connected";
	return "connecting";
}

} // namespace

void ParseStatusFromPacket(const CECPacket *resp, StatusSnapshot &out)
{
	if (!resp)
		return;

	const CEC_ConnState_Tag *conn =
		static_cast<const CEC_ConnState_Tag *>(resp->GetTagByName(EC_TAG_CONNSTATE));

	out.ed2k_state = Ed2kStateString(conn);
	out.kad_state = KadStateString(conn);

	if (conn) {
		out.ed2k_lowid = conn->HasLowID();
		out.kad_firewalled = conn->IsKadFirewalled();
		if (conn->IsConnectedED2K()) {
			const CECTag *server = conn->GetTagByName(EC_TAG_SERVER);
			if (server) {
				const CECTag *name = server->GetTagByName(EC_TAG_SERVER_NAME);
				if (name) {
					out.server_name = std::string(name->GetStringData().utf8_str());
				}
				out.server_ip = std::string(server->GetIPv4Data().StringIP().utf8_str());
				out.server_port = server->GetIPv4Data().m_port;
			}
		}
	}

	if (const CECTag *t = resp->GetTagByName(EC_TAG_STATS_DL_SPEED)) {
		out.download_bps = static_cast<std::uint64_t>(t->GetInt());
	}
	if (const CECTag *t = resp->GetTagByName(EC_TAG_STATS_UL_SPEED)) {
		out.upload_bps = static_cast<std::uint64_t>(t->GetInt());
	}
	if (const CECTag *t = resp->GetTagByName(EC_TAG_STATS_UL_QUEUE_LEN)) {
		out.ul_queue_len = static_cast<std::uint32_t>(t->GetInt());
	}
	if (const CECTag *t = resp->GetTagByName(EC_TAG_STATS_TOTAL_SRC_COUNT)) {
		out.total_src_count = static_cast<std::uint32_t>(t->GetInt());
	}
	// ed2k network aggregate — the same EC_OP_STAT_REQ response
	// already carries KAD_USERS / KAD_FILES (parsed further down in
	// ParseKadFromPacket), plus ED2K_USERS / ED2K_FILES sitting right
	// next to them (ExternalConn.cpp:762-768). Read them here so
	// /status can surface ed2k.network.{users,files} symmetric with
	// kad.network.{users,files,nodes} — no extra EC round-trip.
	if (const CECTag *t = resp->GetTagByName(EC_TAG_STATS_ED2K_USERS)) {
		out.ed2k_users = static_cast<std::uint32_t>(t->GetInt());
	}
	if (const CECTag *t = resp->GetTagByName(EC_TAG_STATS_ED2K_FILES)) {
		out.ed2k_files = static_cast<std::uint32_t>(t->GetInt());
	}
	// Version-check result (present only once the daemon has completed a
	// check). LATEST carries the release string; its presence means a check
	// is done. OUTDATED is an empty marker present only for a newer release.
	if (const CECTag *t = resp->GetTagByName(EC_TAG_GENERAL_VERSION_CHECK_LATEST)) {
		out.version_check_done = true;
		out.version_check_latest = std::string(t->GetStringData().utf8_str());
		out.version_check_outdated =
			resp->GetTagByName(EC_TAG_GENERAL_VERSION_CHECK_OUTDATED) != nullptr;
		if (const CECTag *ts = resp->GetTagByName(EC_TAG_GENERAL_VERSION_CHECK_TIMESTAMP)) {
			out.version_check_timestamp = static_cast<std::uint64_t>(ts->GetInt());
		}
	}
	// Nickname intentionally absent: it isn't shipped in the
	// EC_OP_STAT_REQ response. amuled returns it from
	// EC_OP_GET_PREFERENCES / EC_OP_GET_STATSTREE@DETAIL_WEB; the
	// /preferences endpoint exposes it instead.
}

namespace
{

// PartFile status code (PS_*, see Constants.h) → wire string. amule
// has more codes than the API surface — we collapse "completing"/
// "complete"/"hashing" etc. to the names a curl-tests reader would
// recognise. "downloading" is overloaded: it covers PS_READY (the
// daemon's "transferring" state) AND PS_EMPTY (no sources right now
// but the file isn't paused) — clients distinguish by reading
// `speed_bps` and `sources.transferring`.
const char *DownloadStatusName(std::uint8_t ps_code, bool stopped)
{
	// PS_COMPLETE / PS_COMPLETING take priority over `stopped` —
	// amuled holds finished downloads in `m_completedDownloads` with
	// the EC_TAG_PARTFILE_STOPPED flag set, so a naive `if (stopped)
	// return "paused"` early-out masks every cleared-pending file
	// as still-paused. The "completed" wire string is reserved for
	// the precise semantic "in m_completedDownloads, awaiting clear"
	// — consumers (and the /downloads default filter) rely on it.
	if (ps_code == PS_COMPLETE)
		return "completed";
	if (ps_code == PS_COMPLETING)
		return "completing";

	if (stopped)
		return "stopped"; // stop = pause + drop all sources +
				  // stop searching; the daemon reports it as
				  // PS_PAUSED with EC_TAG_PARTFILE_STOPPED set,
				  // surfaced here as a distinct wire status so
				  // clients can tell it apart from a plain pause
				  // (see /downloads PATCH status="stopped")
	switch (ps_code) {
	case PS_READY:
		return "downloading";
	case PS_EMPTY:
		return "downloading";
	case PS_WAITING_FOR_HASH:
		return "waiting";
	case PS_HASHING:
		return "hashing";
	case PS_ERROR:
		return "erroneous";
	case PS_INSUFFICIENT:
		return "insufficient_disk";
	case PS_PAUSED:
		return "paused";
	case PS_ALLOCATING:
		return "allocating";
	default:
		return "unknown";
	}
}

// The auto-priority flag is encoded as `prio + 10`, NOT bit-7
// (`& 0x80`). Pattern lifted from amule-remote-gui.cpp:1424:
//
// if (m_iUpPriorityEC >= 10) {
//      m_iUpPriority    = m_iUpPriorityEC - 10;
//      m_bAutoUpPriority = true;
//  }
//
// Same encoding for `EC_TAG_KNOWNFILE_PRIO` (shared, up-side) and
// `EC_TAG_PARTFILE_PRIO` (downloads, down-side). Using bit-7 here
// silently mis-labels every auto-priority entry as "normal" because
// the PR_* enum values are tiny and never overlap with the 0x80 bit.
constexpr std::uint8_t kAutoPriorityOffset = 10;

// Decodes the shared `+ 10` auto-flag offset carried by both
// `EC_TAG_PARTFILE_PRIO` (download, down-side) and
// `EC_TAG_KNOWNFILE_PRIO` (shared, up-side): a raw code >= 10 is an
// auto entry whose base level is `raw - 10`. Returns the base level as
// a wire string and reports the auto flag via `auto_out`, so downloads
// and shared surface priority identically (`priority` + `priority_auto`).
const char *PriorityName(std::uint8_t pr_code_raw, bool &auto_out)
{
	std::uint8_t pr;
	if (pr_code_raw >= kAutoPriorityOffset) {
		pr = pr_code_raw - kAutoPriorityOffset;
		auto_out = true;
	} else {
		pr = pr_code_raw;
		auto_out = false;
	}
	switch (pr) {
	case PR_VERY_LOW:
		return "very_low";
	case PR_LOW:
		return "low";
	case PR_NORMAL:
		return "normal";
	case PR_HIGH:
		return "high";
	case PR_VERYHIGH:
		return "release";
	case PR_AUTO:
		auto_out = true;
		return "auto";
	default:
		return "normal";
	}
}

} // namespace

namespace
{

// Lowercase 32-char hex MD4 from a tag.
std::string TagHashLower(const CEC_SharedFile_Tag *sf)
{
	std::string h(sf->FileHashString().utf8_str());
	std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return std::tolower(c); });
	return h;
}

// Merge a CEC_PartFile_Tag's PRESENT child tags into an existing
// FileSnapshot. Absent tags leave the corresponding field unchanged
// — that's the point of INC mode.
//
// Identity (name, ed2k_link, size, priority) lives at the top level
// because both walkers populate it; download-specific stats land in
// `f.download`. The caller is responsible for setting f.ecid + f.hash
// on first encounter and for flipping f.is_downloading=true.
//
// `is_new` distinguishes first-encounter from INC update — used only
// for the status-string re-derive (idle-on-status-suppressed shouldn't
// silently lose the prior status).

// Decode the base CKnownFile detail tags carried by BOTH EC_TAG_PARTFILE
// and EC_TAG_KNOWNFILE (via the CEC_SharedFile_Tag base ctor), so the
// download and shared detail endpoints share one decode. Detail-only;
// absent from the list payloads. INC-safe: only assigns when a tag is
// present this frame, otherwise the prior value is retained.
void MergeKnownFileDetail(const CECTag *t, FileSnapshot &f)
{
	if (const CECTag *aich = t->GetTagByName(EC_TAG_KNOWNFILE_AICH_MASTERHASH))
		f.aich_hash = std::string(aich->GetStringData().utf8_str());
	std::uint32_t q = 0;
	if (t->AssignIfExist(EC_TAG_KNOWNFILE_ON_QUEUE, q))
		f.queued_count = q;
	if (const CECTag *fn = t->GetTagByName(EC_TAG_KNOWNFILE_FILENAME))
		f.knownfile_filename = std::string(fn->GetStringData().utf8_str());
	// The user's own comment + rating (issue #419).
	if (const CECTag *cm = t->GetTagByName(EC_TAG_KNOWNFILE_COMMENT))
		f.comment = std::string(cm->GetStringData().utf8_str());
	std::uint32_t rt = 0;
	if (t->AssignIfExist(EC_TAG_KNOWNFILE_RATING, rt))
		f.rating = static_cast<std::int32_t>(rt);
	// Audio/video media metadata (issue #418). amuled emits these only
	// for probed files, so any one present this frame marks the file as
	// having media (INC-safe: absent tags keep the prior value).
	{
		std::uint32_t v = 0;
		if (t->AssignIfExist(EC_TAG_KNOWNFILE_MEDIA_LENGTH, v)) {
			f.media.length_s = v;
			f.has_media = true;
		}
		if (t->AssignIfExist(EC_TAG_KNOWNFILE_MEDIA_BITRATE, v)) {
			f.media.bitrate = v;
			f.has_media = true;
		}
	}
	if (const CECTag *x = t->GetTagByName(EC_TAG_KNOWNFILE_MEDIA_CODEC)) {
		f.media.codec = std::string(x->GetStringData().utf8_str());
		f.has_media = true;
	}
	if (const CECTag *x = t->GetTagByName(EC_TAG_KNOWNFILE_MEDIA_ARTIST)) {
		f.media.artist = std::string(x->GetStringData().utf8_str());
		f.has_media = true;
	}
	if (const CECTag *x = t->GetTagByName(EC_TAG_KNOWNFILE_MEDIA_ALBUM)) {
		f.media.album = std::string(x->GetStringData().utf8_str());
		f.has_media = true;
	}
	if (const CECTag *x = t->GetTagByName(EC_TAG_KNOWNFILE_MEDIA_TITLE)) {
		f.media.title = std::string(x->GetStringData().utf8_str());
		f.has_media = true;
	}
}

void MergePartFileTag(const CEC_PartFile_Tag *pf, FileSnapshot &f, bool is_new)
{
	wxString fn;
	if (pf->FileName(fn)) {
		f.name = std::string(fn.utf8_str());
	}
	{
		const wxString link = pf->FileEd2kLink();
		if (!link.IsEmpty()) {
			f.ed2k_link = std::string(link.utf8_str());
		}
	}
	{
		std::uint64_t v = f.size;
		if (pf->AssignIfExist(EC_TAG_PARTFILE_SIZE_FULL, v))
			f.size = v;
	}
	{
		std::uint64_t v = f.download.size_done;
		if (pf->AssignIfExist(EC_TAG_PARTFILE_SIZE_DONE, v))
			f.download.size_done = v;
	}
	{
		std::uint64_t v = f.download.size_xfer;
		if (pf->AssignIfExist(EC_TAG_PARTFILE_SIZE_XFER, v))
			f.download.size_xfer = v;
	}
	{
		std::uint32_t v = f.download.speed_bps;
		if (pf->AssignIfExist(EC_TAG_PARTFILE_SPEED, v))
			f.download.speed_bps = v;
	}
	{
		// Status + stopped flag interact — re-derive the wire string
		// whenever either changed.
		std::uint8_t fs = 0;
		bool stopped = false;
		const bool fs_present = pf->AssignIfExist(EC_TAG_PARTFILE_STATUS, fs);
		const bool stop_present = pf->AssignIfExist(EC_TAG_PARTFILE_STOPPED, stopped);
		if (fs_present || stop_present || is_new) {
			f.download.status = DownloadStatusName(
				fs_present ? fs : pf->FileStatus(), stop_present ? stopped : pf->Stopped());
		}
	}
	{
		std::uint8_t pr_raw = 0;
		if (pf->AssignIfExist(EC_TAG_PARTFILE_PRIO, pr_raw)) {
			bool prio_auto = false;
			f.download.priority = PriorityName(pr_raw, prio_auto);
			f.download.priority_auto = prio_auto;
		}
	}
	{
		// Upload priority also rides on the partfile tag (the base
		// CEC_SharedFile_Tag ctor adds EC_TAG_KNOWNFILE_PRIO). Capture
		// it here, from the file's first downloading tick, so a partfile
		// that starts sharing only later still has a shared `priority`:
		// by then amuled has CValueMap-suppressed the unchanged tag and
		// the shared walker would never see it (empty-priority bug).
		std::uint8_t up_raw = 0;
		if (pf->AssignIfExist(EC_TAG_KNOWNFILE_PRIO, up_raw)) {
			bool up_auto = false;
			f.shared.priority = PriorityName(up_raw, up_auto);
			f.shared.priority_auto = up_auto;
		}
	}
	{
		std::uint8_t cat = 0;
		if (pf->AssignIfExist(EC_TAG_PARTFILE_CAT, cat))
			f.download.category = cat;
	}
	{
		std::uint16_t v = 0;
		if (pf->AssignIfExist(EC_TAG_PARTFILE_SOURCE_COUNT, v))
			f.download.sources_total = v;
		if (pf->AssignIfExist(EC_TAG_PARTFILE_SOURCE_COUNT_NOT_CURRENT, v))
			f.download.sources_not_current = v;
		if (pf->AssignIfExist(EC_TAG_PARTFILE_SOURCE_COUNT_XFER, v))
			f.download.sources_transferring = v;
		if (pf->AssignIfExist(EC_TAG_PARTFILE_SOURCE_COUNT_A4AF, v))
			f.download.sources_a4af = v;
	}
	// Detail-only fields (surfaced on GET /downloads/{hash} only).
	{
		std::uint32_t v = 0;
		if (pf->AssignIfExist(EC_TAG_PARTFILE_LAST_SEEN_COMP, v))
			f.download.last_seen_complete = v;
		if (pf->AssignIfExist(EC_TAG_PARTFILE_LAST_RECV, v))
			f.download.last_changed = v;
		if (pf->AssignIfExist(EC_TAG_PARTFILE_DOWNLOAD_ACTIVE, v))
			f.download.download_active_time = v;
		if (pf->AssignIfExist(EC_TAG_PARTFILE_SAVED_ICH, v))
			f.download.saved_by_ich = v;
		if (pf->AssignIfExist(EC_TAG_PARTFILE_PARTMETID, v))
			f.download.partmet_id = v;
	}
	{
		std::uint16_t v = 0;
		if (pf->AssignIfExist(EC_TAG_PARTFILE_AVAILABLE_PARTS, v))
			f.download.available_part_count = v;
		if (pf->AssignIfExist(EC_TAG_PARTFILE_HASHED_PART_COUNT, v))
			f.download.hashing_progress = v;
	}
	{
		std::uint64_t v = 0;
		if (pf->AssignIfExist(EC_TAG_PARTFILE_LOST_CORRUPTION, v))
			f.download.lost_to_corruption = v;
		if (pf->AssignIfExist(EC_TAG_PARTFILE_GAINED_COMPRESSION, v))
			f.download.gained_by_compression = v;
	}
	// Per-source comments/ratings (issue #419). The EC container packs
	// four children per source, evaluated by index: username, filename,
	// rating (int8; -1 = unrated), comment. Rebuild the list whenever the
	// container is present (CValueMap-suppressed when unchanged, so an
	// absent container keeps the prior list).
	if (const CECTag *cont = pf->GetTagByName(EC_TAG_PARTFILE_COMMENTS)) {
		std::vector<const CECTag *> kids;
		for (const CECTag &kid : *cont)
			kids.push_back(&kid);
		f.download.source_comments.clear();
		for (std::size_t i = 0; i + 3 < kids.size(); i += 4) {
			FileSnapshot::DownloadSide::SourceComment c;
			c.username = std::string(kids[i]->GetStringData().utf8_str());
			c.filename = std::string(kids[i + 1]->GetStringData().utf8_str());
			c.rating =
				static_cast<std::int32_t>(static_cast<std::int64_t>(kids[i + 2]->GetInt()));
			c.comment = std::string(kids[i + 3]->GetStringData().utf8_str());
			f.download.source_comments.push_back(std::move(c));
		}
	}
	// Source-reported filenames (issue #420). amuled delta-encodes the
	// container keyed by a stable per-name id: a child carrying a name
	// subtag is a new/updated entry; a child with COUNTS==0 and no name
	// is a removal; otherwise it's a count update. The container is only
	// sent when something changed (HasChildTags gate on the daemon), so
	// an absent container keeps the accumulated map.
	if (const CECTag *names = pf->GetTagByName(EC_TAG_PARTFILE_SOURCE_NAMES)) {
		for (const CECTag &child : *names) {
			const std::uint32_t id = static_cast<std::uint32_t>(child.GetInt());
			const CECTag *name_tag = child.GetTagByName(EC_TAG_PARTFILE_SOURCE_NAMES);
			const CECTag *count_tag = child.GetTagByName(EC_TAG_PARTFILE_SOURCE_NAMES_COUNTS);
			const std::uint32_t count =
				count_tag ? static_cast<std::uint32_t>(count_tag->GetInt()) : 0;
			if (name_tag) {
				FileSnapshot::DownloadSide::SourceName sn;
				sn.name = std::string(name_tag->GetStringData().utf8_str());
				sn.count = count;
				f.download.source_names[id] = std::move(sn);
			} else if (count == 0) {
				f.download.source_names.erase(id);
			} else {
				auto mit = f.download.source_names.find(id);
				if (mit != f.download.source_names.end())
					mit->second.count = count;
			}
		}
	}
	// A4AF (issue #421): the auto flag + the full source-ECID list.
	{
		bool v = false;
		if (pf->AssignIfExist(EC_TAG_PARTFILE_A4AFAUTO, v))
			f.download.a4af_auto = v;
	}
	// amuled rebuilds the whole A4AF-source container when it changes, so
	// replace the list wholesale when present (absent = unchanged, keep).
	if (const CECTag *a4af = pf->GetTagByName(EC_TAG_PARTFILE_A4AF_SOURCES)) {
		f.download.a4af_sources.clear();
		for (const CECTag &src : *a4af)
			f.download.a4af_sources.push_back(static_cast<std::uint32_t>(src.GetInt()));
	}
	// Base CKnownFile detail tags (aich_hash, queued_count, met_file).
	MergeKnownFileDetail(pf, f);
	// Recompute percent unconditionally — both inputs may have moved.
	f.download.percent =
		(f.size > 0)
			? (static_cast<double>(f.download.size_done) * 100.0 / static_cast<double>(f.size))
			: 0.0;
}

// State-code → wire-string decoders for the four enums amule ships
// on `EC_TAG_CLIENT_*_STATE`. Wire forms match the names amule uses
// in its UI (Constants.h enums, lowercased). All decoders fall back
// to "unknown" for codes outside the enum.

const char *ClientUploadStateName(std::uint8_t code)
{
	switch (code) {
	case US_UPLOADING:
		return "uploading";
	case US_ONUPLOADQUEUE:
		return "queued";
	case US_WAITCALLBACK:
		return "waitcallback";
	case US_CONNECTING:
		return "connecting";
	case US_PENDING:
		return "pending";
	case US_LOWTOLOWIP:
		return "lowtolowip";
	case US_BANNED:
		return "banned";
	case US_ERROR:
		return "error";
	case US_NONE:
		return "idle";
	default:
		return "unknown";
	}
}

const char *ClientDownloadStateName(std::uint8_t code)
{
	switch (code) {
	case DS_DOWNLOADING:
		return "downloading";
	case DS_ONQUEUE:
		return "onqueue";
	case DS_CONNECTED:
		return "connected";
	case DS_CONNECTING:
		return "connecting";
	case DS_WAITCALLBACK:
		return "waitcallback";
	case DS_WAITCALLBACKKAD:
		return "waitcallbackkad";
	case DS_REQHASHSET:
		return "reqhashset";
	case DS_NONEEDEDPARTS:
		return "noneededparts";
	case DS_TOOMANYCONNS:
		return "toomanyconns";
	case DS_TOOMANYCONNSKAD:
		return "toomanyconnskad";
	case DS_LOWTOLOWIP:
		return "lowtolowip";
	case DS_BANNED:
		return "banned";
	case DS_ERROR:
		return "error";
	case DS_NONE:
		return "idle";
	case DS_REMOTEQUEUEFULL:
		return "remotequeuefull";
	default:
		return "unknown";
	}
}

const char *ClientIdentStateName(std::uint8_t code)
{
	switch (code) {
	case IS_NOTAVAILABLE:
		return "not_available";
	case IS_IDNEEDED:
		return "id_needed";
	case IS_IDENTIFIED:
		return "identified";
	case IS_IDFAILED:
		return "id_failed";
	case IS_IDBADGUY:
		return "bad_guy";
	default:
		return "unknown";
	}
}

const char *ClientSoftwareName(std::uint32_t code)
{
	// Subset that covers the bulk of the live ed2k population —
	// every client we'd ever realistically meet on the wire. SO_UNKNOWN
	// and SO_COMPAT_UNK collapse to "unknown" / "compat" so consumers
	// see a stable label even when amuled couldn't fingerprint the
	// peer's software.
	switch (code) {
	case SO_EMULE:
		return "emule";
	case SO_CDONKEY:
		return "cdonkey";
	case SO_LXMULE:
		return "lxmule";
	case SO_AMULE:
		return "amule";
	case SO_SHAREAZA:
	case SO_NEW2_SHAREAZA:
	case SO_NEW_SHAREAZA:
		return "shareaza";
	case SO_EMULEPLUS:
		return "emule_plus";
	case SO_HYDRANODE:
		return "hydranode";
	case SO_NEW2_MLDONKEY:
	case SO_MLDONKEY:
	case SO_NEW_MLDONKEY:
		return "mldonkey";
	case SO_LPHANT:
		return "lphant";
	case SO_EDONKEYHYBRID:
		return "edonkey_hybrid";
	case SO_EDONKEY:
		return "edonkey";
	case SO_OLDEMULE:
		return "old_emule";
	case SO_UNKNOWN:
		return "unknown";
	case SO_COMPAT_UNK:
		return "compat";
	default:
		return "unknown";
	}
}

const char *ClientObfuscationName(std::uint8_t code)
{
	switch (code) {
	case OBST_UNDEFINED:
		return "undefined";
	case OBST_ENABLED:
		return "enabled";
	case OBST_SUPPORTED:
		return "supported";
	case OBST_NOT_SUPPORTED:
		return "not_supported";
	case OBST_DISABLED:
		return "disabled";
	default:
		return "unknown";
	}
}

// Format an IP from EC_TAG_CLIENT_USER_IP. The EC tag holds a
// 32-bit host-order IPv4; we render it dotted-quad. Returns "" for
// zero IPs (commonly the case for clients we've never confirmed).
std::string FormatClientIpv4(std::uint32_t ip_he)
{
	if (ip_he == 0)
		return std::string();
	char buf[16];
	std::snprintf(buf,
		sizeof(buf),
		"%u.%u.%u.%u",
		static_cast<unsigned>((ip_he) & 0xFFu),
		static_cast<unsigned>((ip_he >> 8) & 0xFFu),
		static_cast<unsigned>((ip_he >> 16) & 0xFFu),
		static_cast<unsigned>((ip_he >> 24) & 0xFFu));
	return std::string(buf);
}

// Map EC_TAG_CLIENT_FROM (ESourceFrom, Constants.h) to a stable
// lowercase token, mirroring the GUI's Origin column without leaking
// the daemon locale. Local/remote server both collapse to "server".
std::string SourceOriginName(std::uint32_t from)
{
	switch (from) {
	case SF_LOCAL_SERVER:
	case SF_REMOTE_SERVER:
		return "server";
	case SF_KADEMLIA:
		return "kad";
	case SF_SOURCE_EXCHANGE:
		return "source_exchange";
	case SF_PASSIVE:
		return "passive";
	case SF_LINK:
		return "link";
	case SF_SOURCE_SEEDS:
		return "source_seeds";
	case SF_SEARCH_RESULT:
		return "search_result";
	default:
		return "unknown";
	}
}

// Merge a `CEC_UpDownClient_Tag` into an existing ClientSnapshot.
// On a cache-miss the caller pre-populates ecid + hashes; on a hit
// the AssignIfExist pattern leaves cached values intact when the
// tag is CValueMap-suppressed by amuled.
void MergeClientTag(const CEC_UpDownClient_Tag *c,
	ClientSnapshot &cs,
	bool is_new,
	const std::map<std::uint32_t, std::string> &file_hash_by_ecid)
{
	if (const CECTag *t = c->GetTagByName(EC_TAG_CLIENT_NAME)) {
		cs.client_name = std::string(t->GetStringData().utf8_str());
	}
	if (const CECTag *t = c->GetTagByName(EC_TAG_CLIENT_HASH)) {
		cs.user_hash = std::string(t->GetMD4Data().Encode().Lower().utf8_str());
	}
	{
		std::uint32_t v = 0;
		if (c->AssignIfExist(EC_TAG_CLIENT_USER_IP, v))
			cs.ip = FormatClientIpv4(v);
	}
	{
		std::uint16_t v = 0;
		if (c->AssignIfExist(EC_TAG_CLIENT_USER_PORT, v))
			cs.port = v;
	}
	std::uint32_t soft_code = static_cast<std::uint32_t>(SO_UNKNOWN);
	if (c->AssignIfExist(EC_TAG_CLIENT_SOFTWARE, soft_code))
		cs.software = ClientSoftwareName(soft_code);

	// software_version: the daemon formats this with gettext -- an
	// unidentified peer yields _("Unknown"), which is "Desconocido" on a
	// Spanish daemon and would leak the daemon locale into the English-only
	// API (#359). amuleapi runs in its own process and can't reverse the
	// translation, so we key off the locale-independent numeric software
	// code instead of the string: a client the daemon couldn't identify
	// (SO_UNKNOWN, which is exactly the branch that sets the translated
	// "Unknown") gets the lowercase "unknown" sentinel, matching the other
	// enum-like string fields. A known client with an absent/empty version
	// string falls through to the same sentinel.
	if (soft_code != static_cast<std::uint32_t>(SO_UNKNOWN)) {
		if (const CECTag *t = c->GetTagByName(EC_TAG_CLIENT_SOFT_VER_STR)) {
			cs.software_version = std::string(t->GetStringData().utf8_str());
		}
	}
	if (cs.software_version.empty()) {
		cs.software_version = "unknown";
	}
	// os_info is the peer's own self-reported OS string (raw external data,
	// not gettext-translated by our daemon), so it carries no locale-leak;
	// it is frequently empty because most clients don't send it.
	if (const CECTag *t = c->GetTagByName(EC_TAG_CLIENT_OS_INFO)) {
		cs.os_info = std::string(t->GetStringData().utf8_str());
	}
	{
		std::uint8_t v = 0;
		if (c->AssignIfExist(EC_TAG_CLIENT_UPLOAD_STATE, v)) {
			cs.upload_state = ClientUploadStateName(v);
		} else if (is_new) {
			cs.upload_state = "unknown";
		}
	}
	{
		std::uint8_t v = 0;
		if (c->AssignIfExist(EC_TAG_CLIENT_DOWNLOAD_STATE, v)) {
			cs.download_state = ClientDownloadStateName(v);
		} else if (is_new) {
			cs.download_state = "unknown";
		}
	}
	{
		std::uint8_t v = 0;
		if (c->AssignIfExist(EC_TAG_CLIENT_IDENT_STATE, v)) {
			cs.ident_state = ClientIdentStateName(v);
		} else if (is_new) {
			cs.ident_state = "unknown";
		}
	}
	// REMOTE_FILENAME = the file we are downloading from this peer
	// (`m_clientFilename` is set from OP_REQFILENAMEANSWER; see
	// DownloadClient.cpp:350). Live only at INC_UPDATE detail.
	wxString fn;
	if (c->RemoteFilename(fn)) {
		cs.download_file_name = std::string(fn.utf8_str());
	}
	// UPLOAD_FILE / REQUEST_FILE carry amuled-side ECIDs (the unified
	// m_FileEncoder map's IDs). Resolve to MD4 hashes via the
	// file_hash_by_ecid snapshot the caller built from m_files this
	// tick. Empty hash if the ECID isn't in the map — file may have
	// been removed between the file walkers and this client walker.
	{
		std::uint32_t v = 0;
		if (c->AssignIfExist(EC_TAG_CLIENT_UPLOAD_FILE, v) && v != 0) {
			const auto it = file_hash_by_ecid.find(v);
			cs.upload_file_hash = (it != file_hash_by_ecid.end()) ? it->second : std::string();
		}
	}
	{
		std::uint32_t v = 0;
		if (c->AssignIfExist(EC_TAG_CLIENT_REQUEST_FILE, v) && v != 0) {
			const auto it = file_hash_by_ecid.find(v);
			cs.download_file_hash = (it != file_hash_by_ecid.end()) ? it->second : std::string();
		}
	}
	{
		std::uint64_t v = cs.xfer_up_session;
		if (c->AssignIfExist(EC_TAG_CLIENT_UPLOAD_SESSION, v))
			cs.xfer_up_session = v;
	}
	{
		std::uint64_t v = cs.xfer_down_session;
		if (c->AssignIfExist(EC_TAG_PARTFILE_SIZE_XFER, v))
			cs.xfer_down_session = v;
	}
	{
		std::uint64_t v = cs.xfer_up_total;
		if (c->AssignIfExist(EC_TAG_CLIENT_UPLOAD_TOTAL, v))
			cs.xfer_up_total = v;
	}
	{
		std::uint64_t v = cs.xfer_down_total;
		if (c->AssignIfExist(EC_TAG_CLIENT_DOWNLOAD_TOTAL, v))
			cs.xfer_down_total = v;
	}
	{
		std::uint32_t v = cs.upload_speed_bps;
		if (c->AssignIfExist(EC_TAG_CLIENT_UP_SPEED, v))
			cs.upload_speed_bps = v;
	}
	{
		// EC_TAG_CLIENT_DOWN_SPEED is emitted as a double-encoded
		// CECTag (see ECSpecialCoreTags.cpp:289-291 — KBps as a
		// double). AssignIfExist with a uint won't pick it up cleanly;
		// extract via the typed read and convert.
		if (const CECTag *t = c->GetTagByName(EC_TAG_CLIENT_DOWN_SPEED)) {
			const double kBps = t->GetDoubleData();
			cs.download_speed_bps = static_cast<std::uint32_t>(kBps * 1024.0);
		}
	}
	{
		std::uint32_t v = cs.queue_waiting_position;
		if (c->AssignIfExist(EC_TAG_CLIENT_WAITING_POSITION, v))
			cs.queue_waiting_position = v;
	}
	{
		std::uint16_t v = cs.remote_queue_rank;
		if (c->AssignIfExist(EC_TAG_CLIENT_REMOTE_QUEUE_RANK, v))
			cs.remote_queue_rank = v;
	}
	{
		std::uint32_t v = cs.score;
		if (c->AssignIfExist(EC_TAG_CLIENT_SCORE, v))
			cs.score = v;
	}
	{
		std::uint8_t v = 0;
		if (c->AssignIfExist(EC_TAG_CLIENT_OBFUSCATION_STATUS, v)) {
			cs.obfuscation_status = ClientObfuscationName(v);
		}
	}
	{
		bool v = false;
		if (c->AssignIfExist(EC_TAG_CLIENT_FRIEND_SLOT, v))
			cs.friend_slot = v;
	}

	// --- Detail-only fields (issue #422) -----------------------------
	// All already on the INC_UPDATE wire (ECSpecialCoreTags.cpp) but not
	// surfaced by the list; the detail endpoint serializes them.
	{
		std::uint32_t v = 0;
		if (c->AssignIfExist(EC_TAG_CLIENT_USER_ID, v)) {
			cs.user_id_hybrid = v;
			// A LowID peer has a hybrid id below 0x1000000 (IsLowID(),
			// NetworkFunctions.h); inline the ed2k-stable ceiling rather
			// than drag the core header into the webapi decoder.
			const std::uint32_t kLowIdCeiling = 16777216u;
			cs.high_id = v >= kLowIdCeiling;
		}
	}
	{
		std::uint32_t v = 0;
		if (c->AssignIfExist(EC_TAG_CLIENT_SERVER_IP, v))
			cs.server_ip = FormatClientIpv4(v);
	}
	{
		std::uint16_t v = 0;
		if (c->AssignIfExist(EC_TAG_CLIENT_SERVER_PORT, v))
			cs.server_port = v;
	}
	if (const CECTag *t = c->GetTagByName(EC_TAG_CLIENT_SERVER_NAME)) {
		cs.server_name = std::string(t->GetStringData().utf8_str());
	}
	{
		std::uint16_t v = 0;
		if (c->AssignIfExist(EC_TAG_CLIENT_KAD_PORT, v))
			cs.kad_port = v;
	}
	{
		std::uint32_t v = 0;
		if (c->AssignIfExist(EC_TAG_CLIENT_FROM, v))
			cs.source_origin = SourceOriginName(v);
	}
	// PARTFILE_NAME rides inside the client tag only while the peer is
	// downloading from us (ECSpecialCoreTags.cpp:331); leave the cached
	// value when absent.
	if (const CECTag *t = c->GetTagByName(EC_TAG_PARTFILE_NAME)) {
		cs.upload_file_name = std::string(t->GetStringData().utf8_str());
	}
	{
		std::uint32_t v = 0;
		if (c->AssignIfExist(EC_TAG_CLIENT_AVAILABLE_PARTS, v)) {
			cs.available_parts = v;
			cs.has_available_parts = true;
		}
	}
	if (const CECTag *t = c->GetTagByName(EC_TAG_CLIENT_MOD_VERSION)) {
		cs.mod_version = std::string(t->GetStringData().utf8_str());
	}
	{
		bool v = false;
		if (c->AssignIfExist(EC_TAG_CLIENT_DISABLE_VIEW_SHARED, v))
			cs.view_shared_disabled = v;
	}

	// --- Friend status + DL/UP modifier (issue #423, new EC tags) ----
	// Absent when talking to a core built before #423 (older peers just
	// don't send them); the AssignIfExist / GetTagByName guards leave
	// the defaults in place.
	{
		bool v = false;
		if (c->AssignIfExist(EC_TAG_CLIENT_IS_FRIEND, v))
			cs.is_friend = v;
	}
	if (const CECTag *t = c->GetTagByName(EC_TAG_CLIENT_SCORE_RATIO)) {
		cs.dl_up_modifier = t->GetDoubleData();
	}
}

void MergeSharedTag(const CEC_SharedFile_Tag *sf, FileSnapshot &f)
{
	wxString fn;
	if (sf->FileName(fn)) {
		f.name = std::string(fn.utf8_str());
	}
	{
		const wxString link = sf->FileEd2kLink();
		if (!link.IsEmpty()) {
			f.ed2k_link = std::string(link.utf8_str());
		}
	}
	{
		std::uint64_t v = f.size;
		if (sf->AssignIfExist(EC_TAG_PARTFILE_SIZE_FULL, v))
			f.size = v;
	}
	{
		std::uint64_t v = f.shared.xfer_session;
		if (sf->AssignIfExist(EC_TAG_KNOWNFILE_XFERRED, v))
			f.shared.xfer_session = v;
	}
	{
		std::uint64_t v = f.shared.xfer_total;
		if (sf->AssignIfExist(EC_TAG_KNOWNFILE_XFERRED_ALL, v))
			f.shared.xfer_total = v;
	}
	{
		std::uint32_t v = f.shared.requests_session;
		if (sf->AssignIfExist(EC_TAG_KNOWNFILE_REQ_COUNT, v))
			f.shared.requests_session = v;
	}
	{
		std::uint32_t v = f.shared.requests_total;
		if (sf->AssignIfExist(EC_TAG_KNOWNFILE_REQ_COUNT_ALL, v))
			f.shared.requests_total = v;
	}
	{
		std::uint32_t v = f.shared.accepts_session;
		if (sf->AssignIfExist(EC_TAG_KNOWNFILE_ACCEPT_COUNT, v))
			f.shared.accepts_session = v;
	}
	{
		std::uint32_t v = f.shared.accepts_total;
		if (sf->AssignIfExist(EC_TAG_KNOWNFILE_ACCEPT_COUNT_ALL, v))
			f.shared.accepts_total = v;
	}
	{
		std::uint16_t v = 0;
		if (sf->AssignIfExist(EC_TAG_KNOWNFILE_COMPLETE_SOURCES, v))
			f.shared.complete_sources = v;
		// Detail-only complete-sources range (GET /shared/{hash}).
		if (sf->AssignIfExist(EC_TAG_KNOWNFILE_COMPLETE_SOURCES_LOW, v))
			f.shared.complete_sources_low = v;
		if (sf->AssignIfExist(EC_TAG_KNOWNFILE_COMPLETE_SOURCES_HIGH, v))
			f.shared.complete_sources_high = v;
	}
	{
		std::uint8_t pr = 0;
		if (sf->AssignIfExist(EC_TAG_KNOWNFILE_PRIO, pr)) {
			bool sh_auto = false;
			f.shared.priority = PriorityName(pr, sh_auto);
			f.shared.priority_auto = sh_auto;
		}
	}
	// Base CKnownFile detail tags (aich_hash, queued_count, path source).
	MergeKnownFileDetail(sf, f);
}

} // namespace

// --- Downloads (EC_TAG_PARTFILE)

namespace
{

// Apply the stateful RLE decode for the gap + part-status blobs on
// one partfile tag. Allocates `rle_state[ecid]` if absent; mutates
// it on each call (XOR-deltas against the prior decoded buffer).
// Output lands in `f.download.decoded_gaps` + `f.download
// .decoded_part_sources`. HTTP handlers read those without touching
// the decoder state.
void DecodeRleBlobsForPartFile(
	const CEC_PartFile_Tag *pf, FileSnapshot &f, std::map<std::uint32_t, PartFileEncoderData> &rle_state)
{
	const std::uint32_t ecid = pf->ID();
	PartFileEncoderData &enc = rle_state[ecid];

	if (const CECTag *gap_tag = pf->GetTagByName(EC_TAG_PARTFILE_GAP_STATUS)) {
		ArrayOfUInts64 gaps;
		enc.DecodeGaps(gap_tag, gaps);
		f.download.decoded_gaps.assign(gaps.begin(), gaps.end());
	}
	if (const CECTag *part_tag = pf->GetTagByName(EC_TAG_PARTFILE_PART_STATUS)) {
		ArrayOfUInts16 parts;
		enc.DecodeParts(part_tag, parts);
		f.download.decoded_part_sources.assign(parts.begin(), parts.end());
	}
}

// Clear the shared *session statistics* on a share-role-off transition
// while preserving the upload priority. The stats (xfer/requests/accepts
// counters) are per-share-session and must not survive; the upload
// priority is a persistent file attribute that amuled CValueMap-
// suppresses once sent, so wiping it here would strand a partfile that
// re-shares later with an empty `priority`.
void ClearSharedRoleKeepPriority(FileSnapshot &f)
{
	std::string prio = std::move(f.shared.priority);
	const bool prio_auto = f.shared.priority_auto;
	f.shared = FileSnapshot::SharedSide{};
	f.shared.priority = std::move(prio);
	f.shared.priority_auto = prio_auto;
}

} // namespace

void ApplyGetUpdateToDownloads(
	const CECPacket *resp, FileMap &cache, std::map<std::uint32_t, PartFileEncoderData> &rle_state)
{
	if (!resp)
		return;

	// Walk the response top level. Three tag-name dispatches:
	//  * EC_TAG_PARTFILE     → set is_downloading + merge download side
	//  * EC_TAG_FILE_REMOVED → clear download role; drop entry if it
	//                          had no shared role either
	//  * everything else     → handled by sibling Shared/Servers walkers
	for (CECPacket::const_iterator it = resp->begin(); it != resp->end(); ++it) {
		const CECTag *t = &*it;
		const ec_tagname_t name = t->GetTagName();

		if (name == EC_TAG_FILE_REMOVED) {
			const std::uint32_t ecid = static_cast<std::uint32_t>(t->GetInt());
			auto fit = cache.find(ecid);
			if (fit != cache.end()) {
				fit->second.is_downloading = false;
				// Reset the download sub-block so a future role-true
				// transition (or even a stale FindDownload lookup
				// after the role flag was checked) can't surface
				// stale stats from this dead downloading period.
				fit->second.download = FileSnapshot::DownloadSide{};
				if (!fit->second.is_shared)
					cache.erase(fit);
			}
			rle_state.erase(ecid);
			continue;
		}
		if (name != EC_TAG_PARTFILE)
			continue;

		const CEC_PartFile_Tag *pf = static_cast<const CEC_PartFile_Tag *>(t);
		const std::uint32_t ecid = pf->ID();

		auto map_it = cache.find(ecid);
		if (map_it == cache.end()) {
			// Brand-new ECID. INC_UPDATE ships HASH/NAME/SIZE on first
			// encounter (no two-pass needed) so the insert is fully
			// populated in one pass.
			FileSnapshot f;
			f.ecid = ecid;
			f.hash = TagHashLower(pf);
			f.is_downloading = true;
			MergePartFileTag(pf, f, /*is_new=*/true);
			DecodeRleBlobsForPartFile(pf, f, rle_state);
			cache.emplace(ecid, std::move(f));
		} else {
			map_it->second.is_downloading = true;
			MergePartFileTag(pf, map_it->second, /*is_new=*/false);
			DecodeRleBlobsForPartFile(pf, map_it->second, rle_state);
		}
	}
}

void ApplyGetUpdateToShared(const CECPacket *resp, FileMap &cache)
{
	if (!resp)
		return;

	// amuled's "shared files" surface is the union of completed
	// knownfiles (`theApp->sharedfiles` → EC_TAG_KNOWNFILE, always
	// shared) and partfiles with `IsShared()==true` (≥1 chunk complete
	// → EC_TAG_PARTFILE with `EC_TAG_PARTFILE_SHARED` child tag).
	// CEC_PartFile_Tag derives from CEC_SharedFile_Tag (same identity
	// + stat tag names) so we cast and pass through MergeSharedTag.
	//
	// EC_TAG_PARTFILE_SHARED is CValueMap-suppressed when unchanged:
	// present-and-true → set is_shared + merge; present-and-false →
	// clear is_shared (file stays in m_files if still downloading);
	// absent → preserve prior is_shared state.
	//
	// EC_TAG_FILE_REMOVED markers can target either a partfile or
	// knownfile ECID (unified server-side); we clear the shared role
	// + drop the entry if it had no downloading role either.
	for (CECPacket::const_iterator it = resp->begin(); it != resp->end(); ++it) {
		const CECTag *t = &*it;
		const ec_tagname_t name = t->GetTagName();

		if (name == EC_TAG_FILE_REMOVED) {
			const std::uint32_t ecid = static_cast<std::uint32_t>(t->GetInt());
			auto fit = cache.find(ecid);
			if (fit != cache.end()) {
				fit->second.is_shared = false;
				if (!fit->second.is_downloading)
					cache.erase(fit);
				else
					ClearSharedRoleKeepPriority(fit->second);
			}
			continue;
		}
		if (name != EC_TAG_KNOWNFILE && name != EC_TAG_PARTFILE)
			continue;

		const CEC_SharedFile_Tag *sf = static_cast<const CEC_SharedFile_Tag *>(t);
		const std::uint32_t ecid = sf->ID();

		if (name == EC_TAG_PARTFILE) {
			const CECTag *shared_flag = sf->GetTagByName(EC_TAG_PARTFILE_SHARED);
			if (shared_flag) {
				const bool is_shared = (shared_flag->GetInt() != 0);
				if (!is_shared) {
					// Partfile is_shared transitioned false (or
					// arrived for the first time unshared).
					// Reset the shared session stats; entry stays
					// in m_files because downloading role may still
					// hold it. If it doesn't, the downloads-walker
					// FILE_REMOVED will drop it. Upload priority is
					// preserved (persistent file attribute).
					auto fit = cache.find(ecid);
					if (fit != cache.end()) {
						fit->second.is_shared = false;
						ClearSharedRoleKeepPriority(fit->second);
					}
					continue;
				}
				// is_shared == true → fall through to the merge below.
			} else {
				// Flag suppressed (no change). Only meaningful for an
				// entry we already know was shared.
				const auto fit = cache.find(ecid);
				if (fit == cache.end() || !fit->second.is_shared)
					continue;
			}
		}

		auto map_it = cache.find(ecid);
		if (map_it == cache.end()) {
			// Brand-new ECID to the unified map (knownfile arriving
			// without a prior downloads-walker tick — its first
			// frame ships HASH unconditionally).
			FileSnapshot f;
			f.ecid = ecid;
			f.hash = TagHashLower(sf);
			f.is_shared = true;
			MergeSharedTag(sf, f);
			cache.emplace(ecid, std::move(f));
		} else {
			// Existing entry — flip is_shared on, merge fields.
			// If hash arrived (e.g. KNOWNFILE first frame) and we
			// don't already have one (rare path: prior partfile-
			// walker had hash suppressed), capture it now.
			if (map_it->second.hash.empty()) {
				const std::string h = TagHashLower(sf);
				if (!h.empty())
					map_it->second.hash = h;
			}
			map_it->second.is_shared = true;
			MergeSharedTag(sf, map_it->second);
		}
	}
}

// --- Clients (rides on the EC_TAG_CLIENT container inside the
// consolidated GET_UPDATE response).

void ApplyGetUpdateToClients(const CECPacket *resp,
	std::map<std::uint32_t, ClientSnapshot> &cache,
	const std::map<std::uint32_t, std::string> &file_hash_by_ecid)
{
	if (!resp)
		return;
	const CECTag *container = resp->GetTagByName(EC_TAG_CLIENT);
	if (!container)
		return;

	// Walk the per-client children. Every alive client in
	// theApp->clientlist surfaces here every tick (the outer
	// per-client tag is added unconditionally — only the children
	// are CValueMap-suppressed when unchanged). So we use the
	// "seen this tick = keep, absent = evict" pattern (same shape
	// as the servers walker above). There's no FILE_REMOVED
	// equivalent for clients on the server side.
	std::set<std::uint32_t> seen;
	for (CECTag::const_iterator it = container->begin(); it != container->end(); ++it) {
		const CECTag *t = &*it;
		if (t->GetTagName() != EC_TAG_CLIENT)
			continue;
		const CEC_UpDownClient_Tag *cli = static_cast<const CEC_UpDownClient_Tag *>(t);
		const std::uint32_t ecid = cli->ID();
		seen.insert(ecid);

		auto map_it = cache.find(ecid);
		if (map_it == cache.end()) {
			ClientSnapshot fresh;
			fresh.ecid = ecid;
			MergeClientTag(cli, fresh, /*is_new=*/true, file_hash_by_ecid);
			cache.emplace(ecid, std::move(fresh));
		} else {
			MergeClientTag(cli, map_it->second, /*is_new=*/false, file_hash_by_ecid);
		}
	}

	// Evict cache entries not seen this tick — they're gone from the
	// amuled side (peer disconnected, dropped from queue, banned out
	// of the visible set, etc.).
	for (auto it = cache.begin(); it != cache.end();) {
		if (seen.find(it->first) == seen.end()) {
			it = cache.erase(it);
		} else {
			++it;
		}
	}
}

// --- /kad (rides on STAT_REQ response) ---------------------------------

namespace
{

const char *KadBuddyStatusName(std::uint32_t status_code)
{
	// EC ships the `buddyState` enum (ClientList.h) value directly.
	// Using the enum names rather than literal 0/1/2 so a future
	// reorder of buddyState can't silently re-label the wire.
	switch (static_cast<buddyState>(status_code)) {
	case Disconnected:
		return "no_buddy";
	case Connecting:
		return "connecting";
	case Connected:
		return "connected";
	default:
		return "unknown";
	}
}

// Render a host-byte-order uint32 IP as dotted-quad. amuled emits the
// Kad address with network bytes already swapped (see
// `ExternalConn.cpp:761` — `wxUINT32_SWAP_ALWAYS`).
std::string IPv4ToDotted(std::uint32_t ip_host_order)
{
	char buf[24];
	std::snprintf(buf,
		sizeof(buf),
		"%u.%u.%u.%u",
		(ip_host_order) & 0xFF,
		(ip_host_order >> 8) & 0xFF,
		(ip_host_order >> 16) & 0xFF,
		(ip_host_order >> 24) & 0xFF);
	return std::string(buf);
}

} // namespace

void ParseKadFromPacket(const CECPacket *resp, KadSnapshot &out)
{
	if (!resp)
		return;

	const CEC_ConnState_Tag *conn =
		static_cast<const CEC_ConnState_Tag *>(resp->GetTagByName(EC_TAG_CONNSTATE));

	out.state = KadStateString(conn);
	if (conn) {
		out.firewalled = conn->IsKadFirewalled();
	}

	if (const CECTag *t = resp->GetTagByName(EC_TAG_STATS_KAD_USERS)) {
		out.users = static_cast<std::uint32_t>(t->GetInt());
	}
	if (const CECTag *t = resp->GetTagByName(EC_TAG_STATS_KAD_FILES)) {
		out.files = static_cast<std::uint32_t>(t->GetInt());
	}
	if (const CECTag *t = resp->GetTagByName(EC_TAG_STATS_KAD_NODES)) {
		out.nodes = static_cast<std::uint32_t>(t->GetInt());
	}

	// These ship only when Kad is connected (server gates them at
	// ExternalConn.cpp:755 `if (Kademlia::CKademlia::IsConnected())`).
	if (const CECTag *t = resp->GetTagByName(EC_TAG_STATS_KAD_FIREWALLED_UDP)) {
		out.firewalled_udp = (t->GetInt() != 0);
	}
	if (const CECTag *t = resp->GetTagByName(EC_TAG_STATS_KAD_INDEXED_SOURCES)) {
		out.indexed_sources = static_cast<std::uint32_t>(t->GetInt());
	}
	if (const CECTag *t = resp->GetTagByName(EC_TAG_STATS_KAD_INDEXED_KEYWORDS)) {
		out.indexed_keywords = static_cast<std::uint32_t>(t->GetInt());
	}
	if (const CECTag *t = resp->GetTagByName(EC_TAG_STATS_KAD_INDEXED_NOTES)) {
		out.indexed_notes = static_cast<std::uint32_t>(t->GetInt());
	}
	if (const CECTag *t = resp->GetTagByName(EC_TAG_STATS_KAD_INDEXED_LOAD)) {
		out.indexed_load = static_cast<std::uint32_t>(t->GetInt());
	}
	if (const CECTag *t = resp->GetTagByName(EC_TAG_STATS_KAD_IP_ADDRESS)) {
		out.ip = IPv4ToDotted(static_cast<std::uint32_t>(t->GetInt()));
	}
	if (const CECTag *t = resp->GetTagByName(EC_TAG_STATS_KAD_IN_LAN_MODE)) {
		out.in_lan_mode = (t->GetInt() != 0);
	}
	if (const CECTag *t = resp->GetTagByName(EC_TAG_STATS_BUDDY_STATUS)) {
		out.buddy_status = KadBuddyStatusName(static_cast<std::uint32_t>(t->GetInt()));
	}
	if (const CECTag *t = resp->GetTagByName(EC_TAG_STATS_BUDDY_IP)) {
		out.buddy_ip = IPv4ToDotted(static_cast<std::uint32_t>(t->GetInt()));
	}
	if (const CECTag *t = resp->GetTagByName(EC_TAG_STATS_BUDDY_PORT)) {
		out.buddy_port = static_cast<std::uint16_t>(t->GetInt());
	}
}

// --- /logs/amule (incremental, piggybacks on STAT_REQ) -----------------

void ParseAmuleLogFromPacket(const CECPacket *resp, std::vector<std::string> &out_new_lines)
{
	out_new_lines.clear();
	if (!resp)
		return;
	// `EC_TAG_STATS_LOGGER_MESSAGE` is a parent tag with child
	// `EC_TAG_STRING` entries, one per new log line drained from
	// the per-connection CLoggerAccess cursor on the server side
	// (`ExternalConn.cpp:700-715`). Absent when there's nothing
	// new since our last tick.
	const CECTag *logger = resp->GetTagByName(EC_TAG_STATS_LOGGER_MESSAGE);
	if (!logger)
		return;
	for (CECTag::const_iterator it = logger->begin(); it != logger->end(); ++it) {
		const CECTag *t = &*it;
		if (t->GetTagName() != EC_TAG_STRING)
			continue;
		out_new_lines.emplace_back(t->GetStringData().utf8_str());
	}
}

// --- /servers (rides on GET_UPDATE response) ---------------------------

namespace
{

const char *ServerPriorityName(std::uint32_t prio_code)
{
	// SRV_PR_* constants live in `Server.h`. Note the values aren't
	// monotone with priority (NORMAL=0, HIGH=1, LOW=2) — using the
	// named macros instead of literal 0/1/2 saves anyone reading
	// this from re-checking Server.h to remember the order.
	switch (prio_code) {
	case SRV_PR_NORMAL:
		return "normal";
	case SRV_PR_HIGH:
		return "high";
	case SRV_PR_LOW:
		return "low";
	default:
		return "normal";
	}
}

// Build (or merge into) a ServerSnapshot from one per-server tag.
// Identity-only tags (name/description/version/IPv4) are subject to
// CValueMap suppression at the server side, so for an existing entry
// we leave the cached value alone when the source string is empty.
void MergeServerTag(const CEC_Server_Tag *st, ServerSnapshot &s, bool is_new)
{
	s.ecid = st->ID();
	{
		wxString tmp;
		const std::string n = std::string(st->ServerName(&tmp).utf8_str());
		if (is_new || !n.empty())
			s.name = n;
	}
	{
		wxString tmp;
		const std::string d = std::string(st->ServerDesc(&tmp).utf8_str());
		if (is_new || !d.empty())
			s.description = d;
	}
	{
		wxString tmp;
		const std::string v = std::string(st->ServerVersion(&tmp).utf8_str());
		if (is_new || !v.empty())
			s.version = v;
	}
	// IP + port shipping shape varies by EC detail level:
	//  * FULL/WEB/UPDATE (webserver, amulecmd) pack them into the
	//    OUTER tag as IPv4 data (st->GetIPv4Data()).
	//  * INC_UPDATE / GET_UPDATE (amulegui, amuleapi) ship them as
	//    CHILD tags EC_TAG_SERVER_IP + EC_TAG_SERVER_PORT
	//    (ECSpecialCoreTags.cpp:112-113); the outer tag carries the
	//    ECID instead, so GetIPv4Data() returns all-zeros and
	//    /servers[].address silently degrades to "0.0.0.0:0".
	//
	// Try the child-tag shape first; fall back to GetIPv4Data() so
	// any future use of FULL detail still works.
	{
		std::uint32_t ip_he = 0;
		std::uint16_t port = 0;
		const bool have_ip = st->AssignIfExist(EC_TAG_SERVER_IP, ip_he);
		const bool have_port = st->AssignIfExist(EC_TAG_SERVER_PORT, port);
		if (have_ip || have_port) {
			if (have_ip)
				s.ip = ip_he;
			if (have_port)
				s.port = port;
			// Build "1.2.3.4:port" once we have both halves.
			if (s.ip != 0 && s.port != 0) {
				char buf[32];
				std::snprintf(buf,
					sizeof(buf),
					"%u.%u.%u.%u:%u",
					static_cast<unsigned>((s.ip) & 0xFFu),
					static_cast<unsigned>((s.ip >> 8) & 0xFFu),
					static_cast<unsigned>((s.ip >> 16) & 0xFFu),
					static_cast<unsigned>((s.ip >> 24) & 0xFFu),
					static_cast<unsigned>(s.port));
				s.address = buf;
			}
		}
		// The FULL-detail fallback that used to read st->GetIPv4Data()
		// here was removed: amuleapi's refresher only ever asks for
		// EC_DETAIL_INC_UPDATE (RefresherTick.cpp), so the child-tag
		// shape above is the only one we observe in production. The
		// fallback also triggered a libec Debug-build assertion on
		// non-IPv4 outer tags (RefresherTest fixtures pack the ECID
		// as a uint32 in the EC_TAG_SERVER slot), aborting the test
		// process before any assertion in our own code could run.
		// Resurrect the fallback alongside a public type predicate on
		// CECTag if a future detail-level shift makes it relevant.
	}
	{
		std::uint32_t v = 0;
		if (st->AssignIfExist(EC_TAG_SERVER_PING, v))
			s.ping_ms = v;
	}
	{
		std::uint32_t v = 0;
		if (st->AssignIfExist(EC_TAG_SERVER_FAILED, v))
			s.failed = v;
	}
	{
		std::uint32_t v = 0;
		if (st->AssignIfExist(EC_TAG_SERVER_USERS, v))
			s.users = v;
	}
	{
		std::uint32_t v = 0;
		if (st->AssignIfExist(EC_TAG_SERVER_USERS_MAX, v))
			s.max_users = v;
	}
	{
		std::uint32_t v = 0;
		if (st->AssignIfExist(EC_TAG_SERVER_FILES, v))
			s.files = v;
	}
	{
		std::uint32_t v = 0;
		if (st->AssignIfExist(EC_TAG_SERVER_PRIO, v)) {
			s.priority = ServerPriorityName(v);
		} else if (is_new) {
			s.priority = "normal";
		}
	}
	{
		bool v = false;
		if (st->AssignIfExist(EC_TAG_SERVER_STATIC, v))
			s.is_static = v;
	}
}

} // namespace

void ApplyGetUpdateToServers(const CECPacket *resp, std::map<std::uint32_t, ServerSnapshot> &cache)
{
	if (!resp)
		return;
	// Find the EC_TAG_SERVER container at top level. Unlike the
	// legacy `EC_OP_GET_SERVER_LIST` shape (one EC_TAG_SERVER per
	// server at the response root), GET_UPDATE wraps the per-server
	// tags in one CECEmptyTag container — same `EC_TAG_SERVER` name
	// for the container itself. We iterate INTO the container.
	const CECTag *container = resp->GetTagByName(EC_TAG_SERVER);
	if (!container)
		return;

	// The container always carries the FULL current server list (no
	// FILE_REMOVED markers for servers on the server side — see
	// ExternalConn.cpp:985-994), but individual per-server fields are
	// CValueMap-suppressed on unchanged values. Two consequences:
	//  1. Servers absent from the response are gone on amuled's
	//     side — we evict by "not seen this tick".
	//  2. For servers we already cache, identity tags may be absent
	//     this tick; MergeServerTag leaves cached values intact
	//     (the `if (is_new || !n.empty())` guard).
	std::set<std::uint32_t> seen;
	for (CECTag::const_iterator it = container->begin(); it != container->end(); ++it) {
		const CECTag *t = &*it;
		if (t->GetTagName() != EC_TAG_SERVER)
			continue;
		const CEC_Server_Tag *st = static_cast<const CEC_Server_Tag *>(t);
		const std::uint32_t ecid = st->ID();
		seen.insert(ecid);

		auto map_it = cache.find(ecid);
		if (map_it == cache.end()) {
			ServerSnapshot fresh;
			MergeServerTag(st, fresh, /*is_new=*/true);
			cache.emplace(ecid, std::move(fresh));
		} else {
			MergeServerTag(st, map_it->second, /*is_new=*/false);
		}
	}

	// Evict cache entries we didn't see this tick — they're gone on
	// the amuled side (operator removed them, or a fresh connection
	// is rebuilding the list from a different serverlist source).
	for (auto it = cache.begin(); it != cache.end();) {
		if (seen.find(it->first) == seen.end()) {
			it = cache.erase(it);
		} else {
			++it;
		}
	}
}

// --- /stats/tree -------------------------------------------------------

namespace
{

// EC value type -> stable lowercase API string. Mirrors the EC_VALUE_* enum
// (ECCodes.h); the numeric value is carried raw (seconds, bytes, bytes/s, …)
// so clients format and localize.
const char *ECStatValueTypeName(int type)
{
	switch (type) {
	case EC_VALUE_INTEGER:
		return "integer";
	case EC_VALUE_ISTRING:
		return "istring";
	case EC_VALUE_BYTES:
		return "bytes";
	case EC_VALUE_ISHORT:
		return "ishort";
	case EC_VALUE_TIME:
		return "time";
	case EC_VALUE_SPEED:
		return "speed";
	case EC_VALUE_STRING:
		return "string";
	case EC_VALUE_DOUBLE:
		return "double";
	default:
		return "integer";
	}
}

// Extract one EC_TAG_STAT_NODE_VALUE as a typed value, raw and untranslated
// (mirrors ECSpecialTags::FormatValue but without wxGetTranslation / unit
// formatting). Recurses one level for the optional nested "(total …)" value.
void ExtractStatsValue(const CECTag *v, StatsTreeValue &out)
{
	const CECTag *vt = v->GetTagByName(EC_TAG_STAT_VALUE_TYPE);
	const int type = vt != nullptr ? (int)vt->GetInt() : EC_VALUE_INTEGER;
	out.type = ECStatValueTypeName(type);
	switch (type) {
	case EC_VALUE_STRING:
		// The wire string is English (daemon uses wxTRANSLATE); the API must
		// relay it verbatim and never translate -- that is a client concern.
		out.kind = StatsTreeValue::Str;
		out.str = std::string(v->GetStringData().utf8_str());
		break;
	case EC_VALUE_DOUBLE:
		out.kind = StatsTreeValue::Dbl;
		out.dbl = v->GetDoubleData();
		break;
	default:
		out.kind = StatsTreeValue::Num;
		out.num = v->GetInt();
		break;
	}
	// Locale-independent sentinel token, when the daemon tagged this value
	// (e.g. "never"/"not_available"). Additive: the English string above is
	// left intact; enum_token stays empty and is dropped otherwise.
	const CECTag *et = v->GetTagByName(EC_TAG_STAT_VALUE_ENUM);
	if (et) {
		out.enum_token = std::string(et->GetStringData().utf8_str());
	}
	const CECTag *nested = v->GetTagByName(EC_TAG_STAT_NODE_VALUE);
	if (nested) {
		StatsTreeValue e;
		ExtractStatsValue(nested, e);
		out.extra.push_back(std::move(e));
	}
}

void ParseStatsTreeNode(const CECTag *node, StatsTreeNode &out)
{
	const CEC_StatTree_Node_Tag *n = static_cast<const CEC_StatTree_Node_Tag *>(node);
	// Untranslated English label template exactly as EC carries it (e.g.
	// "Uptime: %s"); NOT GetDisplayString(), which translates and locale-formats
	// in the amuleapi process and would make output depend on --locale.
	out.label = std::string(n->GetStringData().utf8_str());
	// Stable machine key, if the daemon set one. Legacy daemons omit the
	// tag; out.key stays empty and is dropped from the JSON.
	const CECTag *keyTag = n->GetTagByName(EC_TAG_STAT_NODE_KEY);
	if (keyTag) {
		out.key = std::string(keyTag->GetStringData().utf8_str());
	}
	// Raw machine value (client version / OS string) for data-labelled
	// nodes. Legacy daemons omit it; out.raw stays empty and is dropped.
	const CECTag *rawTag = n->GetTagByName(EC_TAG_STAT_NODE_RAW);
	if (rawTag) {
		out.raw = std::string(rawTag->GetStringData().utf8_str());
	}
	// Raw numeric ratio (download-per-upload), only present on the ratio node
	// and only when the daemon could compute it. Legacy daemons omit both.
	const CECTag *ratioTag = n->GetTagByName(EC_TAG_STAT_NODE_RATIO);
	if (ratioTag) {
		out.has_ratio_session = true;
		out.ratio_session = ratioTag->GetDoubleData();
	}
	const CECTag *ratioTotalTag = n->GetTagByName(EC_TAG_STAT_NODE_RATIO_TOTAL);
	if (ratioTotalTag) {
		out.has_ratio_total = true;
		out.ratio_total = ratioTotalTag->GetDoubleData();
	}
	for (CECTag::const_iterator it = n->begin(); it != n->end(); ++it) {
		if (it->GetTagName() == EC_TAG_STAT_NODE_VALUE) {
			StatsTreeValue v;
			ExtractStatsValue(&*it, v);
			out.values.push_back(std::move(v));
		} else if (it->GetTagName() == EC_TAG_STATTREE_NODE) {
			StatsTreeNode child;
			ParseStatsTreeNode(&*it, child);
			out.children.push_back(std::move(child));
		}
	}
}

} // namespace

void ParseStatsTreeFromPacket(const CECPacket *resp, StatsTreeNode &out)
{
	out.label.clear();
	out.values.clear();
	out.children.clear();
	if (!resp)
		return;
	// amuled emits a single root EC_TAG_STATTREE_NODE; its label is
	// always an unlabeled container, so we drop it and surface its
	// direct children at the top level. This matches what amuleweb's
	// `am_load_stats_tree.php` does and what the reference REST
	// branch's /stats/tree handler does.
	const CECTag *root = resp->GetTagByName(EC_TAG_STATTREE_NODE);
	if (!root)
		return;
	for (CECTag::const_iterator it = root->begin(); it != root->end(); ++it) {
		if (it->GetTagName() != EC_TAG_STATTREE_NODE)
			continue;
		StatsTreeNode child;
		ParseStatsTreeNode(&*it, child);
		out.children.push_back(std::move(child));
	}
}

// --- /stats/graphs/{graph} --------------------------------------------

namespace
{

// EC_TAG_STATSGRAPH_DATA is a binary blob of N interleaved uint32
// channels, each value pre-converted to network byte order via
// `ENDIAN_HTONL` on the amuled side (Statistics.cpp:621-624). We
// have to byte-swap back to host order before consumption — `ntohl`
// is a no-op on big-endian hosts and the canonical 4-byte swap on
// little-endian, which is what every modern target (x86_64, arm64)
// runs.
std::uint32_t BigEndianToHost32(const std::uint8_t *p)
{
	return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
	       (static_cast<std::uint32_t>(p[2]) << 8) | (static_cast<std::uint32_t>(p[3]));
}

void UnpackInterleavedUint32(const std::uint8_t *bytes,
	std::size_t byte_len,
	unsigned num_channels,
	std::vector<std::vector<std::uint32_t>> &out_channels)
{
	out_channels.assign(num_channels, std::vector<std::uint32_t>{});
	if (!bytes || byte_len == 0 || num_channels == 0)
		return;
	const std::size_t total_u32s = byte_len / sizeof(std::uint32_t);
	const std::size_t num_points = total_u32s / num_channels;
	for (unsigned c = 0; c < num_channels; ++c) {
		out_channels[c].reserve(num_points);
	}
	for (std::size_t p = 0; p < num_points; ++p) {
		for (unsigned c = 0; c < num_channels; ++c) {
			out_channels[c].push_back(
				BigEndianToHost32(bytes + (p * num_channels + c) * sizeof(std::uint32_t)));
		}
	}
}

} // namespace

void ParseGraphsFromPacket(const CECPacket *resp, StatsGraphs &out)
{
	out = StatsGraphs{};
	if (!resp)
		return;

	if (const CECTag *t = resp->GetTagByName(EC_TAG_STATSGRAPH_DATA)) {
		// 4 interleaved channels per amuled-side layout
		// (Statistics.cpp:621-624):
		//  ch0 = kBpsDownCur * 1024  (bytes per second)
		//  ch1 = kBpsUpCur   * 1024  (bytes per second)
		//  ch2 = cntConnections      (active client connections)
		//  ch3 = kadNodesCur         (Kad nodes currently routed)
		std::vector<std::vector<std::uint32_t>> channels;
		UnpackInterleavedUint32(static_cast<const std::uint8_t *>(t->GetTagData()),
			t->GetTagDataLen(),
			/*num_channels=*/4,
			channels);
		if (channels.size() >= 4) {
			out.download_bps = std::move(channels[0]);
			out.upload_bps = std::move(channels[1]);
			out.connections = std::move(channels[2]);
			out.kad_nodes = std::move(channels[3]);
		}
	}
	// EC_TAG_STATSGRAPH_DATA_CONN carries the upload-slot / download-
	// slot counts (2 interleaved channels) — useful for the dashboard
	// graph but not in StatsGraphs surface yet. Skipping until a
	// concrete client asks; the EC bytes still travel for free since
	// they're in the same response.
	if (const CECTag *t = resp->GetTagByName(EC_TAG_STATSGRAPH_SESSION_DL)) {
		out.session_download_bytes = static_cast<std::uint64_t>(t->GetInt());
	}
	if (const CECTag *t = resp->GetTagByName(EC_TAG_STATSGRAPH_SESSION_UL)) {
		out.session_upload_bytes = static_cast<std::uint64_t>(t->GetInt());
	}
	if (const CECTag *t = resp->GetTagByName(EC_TAG_STATSGRAPH_SESSION_KAD)) {
		out.session_kad_bytes = static_cast<std::uint64_t>(t->GetInt());
	}
}

// --- /search/results (full fetch per tick) -----------------------------

namespace
{
// CSearchFile::DownloadStatus (SearchFile.h) → wire string (issue #429).
// Values are ABI-stable (serialized over EC as EC_TAG_PARTFILE_STATUS).
const char *SearchStatusName(std::uint32_t code)
{
	switch (code) {
	case 0: // NEW
		return "new";
	case 1: // DOWNLOADED
		return "downloaded";
	case 2: // QUEUED
		return "queued";
	case 3: // CANCELED
		return "canceled";
	case 4: // QUEUEDCANCELED
		return "queued_canceled";
	default:
		return "new";
	}
}

// Locale-independent file-type token from the filename, mirroring the
// desktop's GetFiletypeByName (untranslated) lowercased — same tokens as
// the shared-detail `file_type`.
std::string SearchTypeToken(const std::string &name)
{
	const wxString desc =
		GetFiletypeByName(CPath(wxString::FromUTF8(name.c_str())), /*translated=*/false);
	std::string s(desc.utf8_str());
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return s;
}
} // namespace

void ApplySearchFull(const CECPacket *resp, std::map<std::uint32_t, SearchResult> &cache)
{
	cache.clear();
	if (!resp)
		return;
	for (CECPacket::const_iterator it = resp->begin(); it != resp->end(); ++it) {
		const CECTag *t = &*it;
		if (t->GetTagName() != EC_TAG_SEARCHFILE)
			continue;
		const CEC_SearchFile_Tag *sf = static_cast<const CEC_SearchFile_Tag *>(t);
		SearchResult r;
		r.ecid = sf->ID();
		{
			std::string h(sf->FileHashString().utf8_str());
			std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) {
				return std::tolower(c);
			});
			r.hash = std::move(h);
		}
		r.name = std::string(sf->FileName().utf8_str());
		r.size = sf->SizeFull();
		{
			std::uint32_t v = 0;
			if (sf->AssignIfExist(EC_TAG_PARTFILE_SOURCE_COUNT, v))
				r.source_count = v;
		}
		{
			std::uint32_t v = 0;
			if (sf->AssignIfExist(EC_TAG_PARTFILE_SOURCE_COUNT_XFER, v))
				r.complete_source_count = v;
		}
		r.already_have = sf->AlreadyHave();
		// Grouping (issue #431): a child hit carries its parent's ECID in
		// EC_TAG_SEARCH_PARENT. Recorded here; folded into the parent's
		// children[] in the second pass below.
		{
			std::uint32_t v = 0;
			if (sf->AssignIfExist(EC_TAG_SEARCH_PARENT, v)) {
				r.parent_ecid = v;
				r.has_parent = true;
			}
		}
		{
			std::uint8_t v = 0;
			if (sf->AssignIfExist(EC_TAG_KNOWNFILE_RATING, v))
				r.rating = v;
		}
		// Download status (issue #429): amuled packs the CSearchFile
		// status in EC_TAG_PARTFILE_STATUS on every search-result tag.
		{
			std::uint32_t v = 0;
			r.status = SearchStatusName(sf->AssignIfExist(EC_TAG_PARTFILE_STATUS, v) ? v : 0);
		}
		// File type, computed from the filename (no EC data needed).
		r.type = SearchTypeToken(r.name);
		// Media metadata (issue #430): present only when the hit carried
		// FT_MEDIA_* tags (known/probed locally). Any present tag marks
		// has_media so the API emits the `media` object.
		{
			std::uint32_t v = 0;
			if (sf->AssignIfExist(EC_TAG_KNOWNFILE_MEDIA_LENGTH, v)) {
				r.media.length_s = v;
				r.has_media = true;
			}
			if (sf->AssignIfExist(EC_TAG_KNOWNFILE_MEDIA_BITRATE, v)) {
				r.media.bitrate = v;
				r.has_media = true;
			}
		}
		if (const CECTag *x = sf->GetTagByName(EC_TAG_KNOWNFILE_MEDIA_CODEC)) {
			r.media.codec = std::string(x->GetStringData().utf8_str());
			r.has_media = true;
		}
		if (const CECTag *x = sf->GetTagByName(EC_TAG_KNOWNFILE_MEDIA_ARTIST)) {
			r.media.artist = std::string(x->GetStringData().utf8_str());
			r.has_media = true;
		}
		if (const CECTag *x = sf->GetTagByName(EC_TAG_KNOWNFILE_MEDIA_ALBUM)) {
			r.media.album = std::string(x->GetStringData().utf8_str());
			r.has_media = true;
		}
		if (const CECTag *x = sf->GetTagByName(EC_TAG_KNOWNFILE_MEDIA_TITLE)) {
			r.media.title = std::string(x->GetStringData().utf8_str());
			r.has_media = true;
		}
		cache.emplace(r.ecid, std::move(r));
	}

	// Second pass (issue #431): fold each child into its parent's
	// children[] and drop it from the top-level set, so the API serves
	// one parent row per hash+size with the alternative filenames nested.
	// A child whose parent isn't in the set (shouldn't happen — the core
	// emits the parent before its children) is left as a top-level row so
	// nothing is silently lost.
	std::vector<std::uint32_t> folded;
	for (auto &kv : cache) {
		SearchResult &child = kv.second;
		if (!child.has_parent)
			continue;
		auto pit = cache.find(child.parent_ecid);
		if (pit == cache.end())
			continue;
		SearchResult::Child c;
		c.ecid = child.ecid;
		c.name = child.name;
		c.hash = child.hash;
		c.source_count = child.source_count;
		c.complete_source_count = child.complete_source_count;
		pit->second.children.push_back(std::move(c));
		folded.push_back(kv.first);
	}
	for (std::uint32_t ecid : folded) {
		cache.erase(ecid);
	}
}

// --- Search-progress, daemon-supplied lifecycle path -------------------
//
// Reads EC_TAG_SEARCH_LIFECYCLE_STATE from the EC_OP_SEARCH_PROGRESS
// response — the unambiguous lifecycle tag landed alongside this PR.
// No sentinel decode, no `saw_in_progress` tracking, no defensive
// timeout: the daemon's flag is the source of truth. amuleapi pins a
// daemon version that carries the new tags, so this is the only path.
SearchProgressSnapshot AdvanceSearchProgress(
	const SearchProgressSnapshot &prev, std::uint32_t lifecycle_state, std::uint32_t pct_now)
{
	SearchProgressSnapshot next = prev;
	if (lifecycle_state == 2 /* SEARCH_LIFECYCLE_FINISHED */) {
		next.percent = 100;
		next.complete = true;
		next.active = false;
	} else if (lifecycle_state == 1 /* SEARCH_LIFECYCLE_RUNNING */) {
		next.complete = false;
		next.active = true;
		// Unified 0..100 the daemon already computed for this search kind
		// (global = real server-queue percent; Kad = cosmetic time-ramp;
		// local = instantaneous). No kind special-casing here anymore.
		next.percent = (pct_now > 100) ? 100 : pct_now;
	} else {
		// SEARCH_LIFECYCLE_IDLE — refresher shouldn't be calling us
		// in this state (active was true on entry), but stay defensive.
		next.complete = false;
		next.active = false;
		next.percent = 0;
	}
	return next;
}

// --- /preferences + /categories (one EC roundtrip) ---------------------

namespace
{

void ParseCategoryTag(const CECTag *cat_tag, CategorySnapshot &c)
{
	const CEC_Category_Tag *ct = static_cast<const CEC_Category_Tag *>(cat_tag);
	// Category index lives in the tag's int payload (set by
	// `CECTag(name, cat_index)` at construction — see
	// `ECSpecialCoreTags.cpp` category ctor).
	c.index = static_cast<std::uint32_t>(ct->GetInt());
	c.name = std::string(ct->Name().utf8_str());
	c.path = std::string(ct->Path().utf8_str());
	c.comment = std::string(ct->Comment().utf8_str());
	c.color = ct->Color();
	c.priority_code = ct->Prio();
	// Reuse the download-priority namer — categories use the same
	// PR_* code space.
	{
		bool _ignore = false;
		c.priority = PriorityName(c.priority_code, _ignore);
	}
}

void ParseGeneralPrefs(const CECTag *gen, PreferencesSnapshot &out)
{
	if (const CECTag *t = gen->GetTagByName(EC_TAG_USER_NICK)) {
		out.nickname = std::string(t->GetStringData().utf8_str());
	}
	if (const CECTag *t = gen->GetTagByName(EC_TAG_USER_HASH)) {
		out.user_hash = std::string(t->GetMD4Data().Encode().Lower().utf8_str());
	}
	if (const CECTag *t = gen->GetTagByName(EC_TAG_USER_HOST)) {
		out.host_name = std::string(t->GetStringData().utf8_str());
	}
	if (gen->GetTagByName(EC_TAG_GENERAL_CHECK_NEW_VERSION)) {
		out.check_new_version = true;
	}
	// Capability: 3.1+ daemons always send this bool (true when built with
	// ENABLE_VERSION_CHECK, false when compiled out). Absent means a pre-3.1
	// daemon that can't relay a result over EC anyway, so it stays false.
	if (const CECTag *t = gen->GetTagByName(EC_TAG_GENERAL_VERSION_CHECK_AVAILABLE)) {
		out.version_check_available = t->GetInt() != 0;
	}
}

void ParseConnectionPrefs(const CECTag *conn, PreferencesSnapshot &out)
{
	if (const CECTag *t = conn->GetTagByName(EC_TAG_CONN_UL_CAP)) {
		out.max_upload_cap_kbps = static_cast<std::uint32_t>(t->GetInt());
	}
	if (const CECTag *t = conn->GetTagByName(EC_TAG_CONN_DL_CAP)) {
		out.max_download_cap_kbps = static_cast<std::uint32_t>(t->GetInt());
	}
	if (const CECTag *t = conn->GetTagByName(EC_TAG_CONN_MAX_UL)) {
		out.max_upload_kbps = static_cast<std::uint32_t>(t->GetInt());
	}
	if (const CECTag *t = conn->GetTagByName(EC_TAG_CONN_MAX_DL)) {
		out.max_download_kbps = static_cast<std::uint32_t>(t->GetInt());
	}
	if (const CECTag *t = conn->GetTagByName(EC_TAG_CONN_SLOT_ALLOCATION)) {
		out.slot_allocation = static_cast<std::uint32_t>(t->GetInt());
	}
	if (const CECTag *t = conn->GetTagByName(EC_TAG_CONN_TCP_PORT)) {
		out.tcp_port = static_cast<std::uint16_t>(t->GetInt());
	}
	if (const CECTag *t = conn->GetTagByName(EC_TAG_CONN_UDP_PORT)) {
		out.udp_port = static_cast<std::uint16_t>(t->GetInt());
	}
	// The EmptyTag markers (presence = true, absence = false).
	out.udp_disabled = conn->GetTagByName(EC_TAG_CONN_UDP_DISABLE) != nullptr;
	out.autoconnect = conn->GetTagByName(EC_TAG_CONN_AUTOCONNECT) != nullptr;
	out.reconnect = conn->GetTagByName(EC_TAG_CONN_RECONNECT) != nullptr;
	out.network_ed2k = conn->GetTagByName(EC_TAG_NETWORK_ED2K) != nullptr;
	out.network_kad = conn->GetTagByName(EC_TAG_NETWORK_KADEMLIA) != nullptr;

	if (const CECTag *t = conn->GetTagByName(EC_TAG_CONN_MAX_FILE_SOURCES)) {
		out.max_sources_per_file = static_cast<std::uint32_t>(t->GetInt());
	}
	if (const CECTag *t = conn->GetTagByName(EC_TAG_CONN_MAX_CONN)) {
		out.max_connections = static_cast<std::uint32_t>(t->GetInt());
	}
}

// --- Extended EC-carried preference categories (issue #437) ----------
//
// Boolean encoding follows the core serializer (ECSpecialMuleTags.cpp):
// most bools are emitted as a bare CECEmptyTag only when true, so
// presence == true; a few (directories.share_hidden/auto_rescan/
// follow_symlinks/exclude_regex, security.can_see_shares) are emitted
// as a value tag every time, so they read GetInt() != 0.

void ParseDirectoriesPrefs(const CECTag *d, PreferencesSnapshot &out)
{
	if (const CECTag *t = d->GetTagByName(EC_TAG_DIRECTORIES_INCOMING)) {
		out.directories.incoming = std::string(t->GetStringData().utf8_str());
	}
	if (const CECTag *t = d->GetTagByName(EC_TAG_DIRECTORIES_TEMP)) {
		out.directories.temp = std::string(t->GetStringData().utf8_str());
	}
	if (const CECTag *shared = d->GetTagByName(EC_TAG_DIRECTORIES_SHARED)) {
		out.directories.shared.clear();
		for (const CECTag &child : *shared) {
			if (child.GetTagName() == EC_TAG_STRING)
				out.directories.shared.emplace_back(child.GetStringData().utf8_str());
		}
	}
	if (const CECTag *t = d->GetTagByName(EC_TAG_DIRECTORIES_SHARE_HIDDEN))
		out.directories.share_hidden = t->GetInt() != 0;
	if (const CECTag *t = d->GetTagByName(EC_TAG_DIRECTORIES_AUTO_RESCAN))
		out.directories.auto_rescan = t->GetInt() != 0;
	if (const CECTag *t = d->GetTagByName(EC_TAG_DIRECTORIES_FOLLOW_SYMLINKS))
		out.directories.follow_symlinks = t->GetInt() != 0;
	if (const CECTag *t = d->GetTagByName(EC_TAG_DIRECTORIES_EXCLUDE_PATTERNS)) {
		out.directories.exclude_patterns = std::string(t->GetStringData().utf8_str());
	}
	if (const CECTag *t = d->GetTagByName(EC_TAG_DIRECTORIES_EXCLUDE_REGEX))
		out.directories.exclude_regex = t->GetInt() != 0;
}

void ParseFilesPrefs(const CECTag *f, PreferencesSnapshot &out)
{
	out.files.ich_enabled = f->GetTagByName(EC_TAG_FILES_ICH_ENABLED) != nullptr;
	out.files.aich_trust = f->GetTagByName(EC_TAG_FILES_AICH_TRUST) != nullptr;
	out.files.new_paused = f->GetTagByName(EC_TAG_FILES_NEW_PAUSED) != nullptr;
	out.files.new_auto_dl_prio = f->GetTagByName(EC_TAG_FILES_NEW_AUTO_DL_PRIO) != nullptr;
	out.files.new_auto_ul_prio = f->GetTagByName(EC_TAG_FILES_NEW_AUTO_UL_PRIO) != nullptr;
	out.files.preview_prio = f->GetTagByName(EC_TAG_FILES_PREVIEW_PRIO) != nullptr;
	out.files.start_next_paused = f->GetTagByName(EC_TAG_FILES_START_NEXT_PAUSED) != nullptr;
	out.files.resume_same_cat = f->GetTagByName(EC_TAG_FILES_RESUME_SAME_CAT) != nullptr;
	out.files.save_sources = f->GetTagByName(EC_TAG_FILES_SAVE_SOURCES) != nullptr;
	out.files.extract_metadata = f->GetTagByName(EC_TAG_FILES_EXTRACT_METADATA) != nullptr;
	out.files.alloc_full_size = f->GetTagByName(EC_TAG_FILES_ALLOC_FULL_SIZE) != nullptr;
	out.files.check_free_space = f->GetTagByName(EC_TAG_FILES_CHECK_FREE_SPACE) != nullptr;
	if (const CECTag *t = f->GetTagByName(EC_TAG_FILES_MIN_FREE_SPACE)) {
		out.files.min_free_space_mb = static_cast<std::uint32_t>(t->GetInt());
	}
	out.files.create_normal = f->GetTagByName(EC_TAG_FILES_CREATE_NORMAL) != nullptr;
}

void ParseServersPrefs(const CECTag *s, PreferencesSnapshot &out)
{
	out.servers.remove_dead = s->GetTagByName(EC_TAG_SERVERS_REMOVE_DEAD) != nullptr;
	if (const CECTag *t = s->GetTagByName(EC_TAG_SERVERS_DEAD_SERVER_RETRIES)) {
		out.servers.dead_server_retries = static_cast<std::uint32_t>(t->GetInt());
	}
	out.servers.auto_update = s->GetTagByName(EC_TAG_SERVERS_AUTO_UPDATE) != nullptr;
	out.servers.add_from_server = s->GetTagByName(EC_TAG_SERVERS_ADD_FROM_SERVER) != nullptr;
	out.servers.add_from_client = s->GetTagByName(EC_TAG_SERVERS_ADD_FROM_CLIENT) != nullptr;
	out.servers.use_score_system = s->GetTagByName(EC_TAG_SERVERS_USE_SCORE_SYSTEM) != nullptr;
	out.servers.smart_id_check = s->GetTagByName(EC_TAG_SERVERS_SMART_ID_CHECK) != nullptr;
	out.servers.safe_server_connect = s->GetTagByName(EC_TAG_SERVERS_SAFE_SERVER_CONNECT) != nullptr;
	out.servers.autoconn_static_only = s->GetTagByName(EC_TAG_SERVERS_AUTOCONN_STATIC_ONLY) != nullptr;
	out.servers.manual_high_prio = s->GetTagByName(EC_TAG_SERVERS_MANUAL_HIGH_PRIO) != nullptr;
	if (const CECTag *t = s->GetTagByName(EC_TAG_SERVERS_UPDATE_URL)) {
		out.servers.update_url = std::string(t->GetStringData().utf8_str());
	}
}

void ParseSecurityPrefs(const CECTag *s, PreferencesSnapshot &out)
{
	if (const CECTag *t = s->GetTagByName(EC_TAG_SECURITY_CAN_SEE_SHARES))
		out.security.can_see_shares = t->GetInt() != 0;
	out.security.ipfilter_clients = s->GetTagByName(EC_TAG_IPFILTER_CLIENTS) != nullptr;
	out.security.ipfilter_servers = s->GetTagByName(EC_TAG_IPFILTER_SERVERS) != nullptr;
	out.security.ipfilter_auto_update = s->GetTagByName(EC_TAG_IPFILTER_AUTO_UPDATE) != nullptr;
	if (const CECTag *t = s->GetTagByName(EC_TAG_IPFILTER_UPDATE_URL)) {
		out.security.ipfilter_update_url = std::string(t->GetStringData().utf8_str());
	}
	if (const CECTag *t = s->GetTagByName(EC_TAG_IPFILTER_LEVEL)) {
		out.security.ipfilter_level = static_cast<std::uint32_t>(t->GetInt());
	}
	out.security.ipfilter_filter_lan = s->GetTagByName(EC_TAG_IPFILTER_FILTER_LAN) != nullptr;
	out.security.use_secident = s->GetTagByName(EC_TAG_SECURITY_USE_SECIDENT) != nullptr;
	out.security.obfuscation_supported =
		s->GetTagByName(EC_TAG_SECURITY_OBFUSCATION_SUPPORTED) != nullptr;
	out.security.obfuscation_requested =
		s->GetTagByName(EC_TAG_SECURITY_OBFUSCATION_REQUESTED) != nullptr;
	out.security.obfuscation_required = s->GetTagByName(EC_TAG_SECURITY_OBFUSCATION_REQUIRED) != nullptr;
}

void ParseMessageFilterPrefs(const CECTag *m, PreferencesSnapshot &out)
{
	out.message_filter.enabled = m->GetTagByName(EC_TAG_MSGFILTER_ENABLED) != nullptr;
	out.message_filter.all = m->GetTagByName(EC_TAG_MSGFILTER_ALL) != nullptr;
	out.message_filter.friends = m->GetTagByName(EC_TAG_MSGFILTER_FRIENDS) != nullptr;
	out.message_filter.secure = m->GetTagByName(EC_TAG_MSGFILTER_SECURE) != nullptr;
	out.message_filter.by_keyword = m->GetTagByName(EC_TAG_MSGFILTER_BY_KEYWORD) != nullptr;
	if (const CECTag *t = m->GetTagByName(EC_TAG_MSGFILTER_KEYWORDS)) {
		out.message_filter.keywords = std::string(t->GetStringData().utf8_str());
	}
}

void ParseRemoteControlsPrefs(const CECTag *rc, PreferencesSnapshot &out)
{
	out.remote_controls.webserver_enabled = rc->GetTagByName(EC_TAG_WEBSERVER_AUTORUN) != nullptr;
	if (const CECTag *t = rc->GetTagByName(EC_TAG_WEBSERVER_PORT)) {
		out.remote_controls.webserver_port = static_cast<std::uint32_t>(t->GetInt());
	}
	out.remote_controls.webserver_use_gzip = rc->GetTagByName(EC_TAG_WEBSERVER_USEGZIP) != nullptr;
	if (const CECTag *t = rc->GetTagByName(EC_TAG_WEBSERVER_REFRESH)) {
		out.remote_controls.webserver_refresh = static_cast<std::uint32_t>(t->GetInt());
	}
	if (const CECTag *t = rc->GetTagByName(EC_TAG_WEBSERVER_TEMPLATE)) {
		out.remote_controls.webserver_template = std::string(t->GetStringData().utf8_str());
	}
	out.remote_controls.webserver_guest_enabled = rc->GetTagByName(EC_TAG_WEBSERVER_GUEST) != nullptr;
	out.remote_controls.amuleapi_enabled = rc->GetTagByName(EC_TAG_AMULEAPI_AUTORUN) != nullptr;
	if (const CECTag *t = rc->GetTagByName(EC_TAG_AMULEAPI_PORT)) {
		out.remote_controls.amuleapi_port = static_cast<std::uint32_t>(t->GetInt());
	}
	if (const CECTag *t = rc->GetTagByName(EC_TAG_AMULEAPI_BIND)) {
		out.remote_controls.amuleapi_bind = std::string(t->GetStringData().utf8_str());
	}
	// Passwords (EC_TAG_PASSWD_HASH / EC_TAG_AMULEAPI_PASSWD) are
	// deliberately NOT read — write-only, never surfaced on GET.
}

void ParseOnlineSigPrefs(const CECTag *o, PreferencesSnapshot &out)
{
	out.online_signature.enabled = o->GetTagByName(EC_TAG_ONLINESIG_ENABLED) != nullptr;
}

void ParseCoreTweaksPrefs(const CECTag *c, PreferencesSnapshot &out)
{
	if (const CECTag *t = c->GetTagByName(EC_TAG_CORETW_MAX_CONN_PER_FIVE)) {
		out.core_tweaks.max_conn_per_five = static_cast<std::uint32_t>(t->GetInt());
	}
	out.core_tweaks.verbose = c->GetTagByName(EC_TAG_CORETW_VERBOSE) != nullptr;
	if (const CECTag *t = c->GetTagByName(EC_TAG_CORETW_FILEBUFFER)) {
		out.core_tweaks.filebuffer = static_cast<std::uint32_t>(t->GetInt());
	}
	if (const CECTag *t = c->GetTagByName(EC_TAG_CORETW_UL_QUEUE)) {
		out.core_tweaks.ul_queue = static_cast<std::uint32_t>(t->GetInt());
	}
	if (const CECTag *t = c->GetTagByName(EC_TAG_CORETW_SRV_KEEPALIVE_TIMEOUT)) {
		out.core_tweaks.srv_keepalive_timeout = static_cast<std::uint32_t>(t->GetInt());
	}
	if (const CECTag *t = c->GetTagByName(EC_TAG_CORETW_KAD_MAX_SEARCHES)) {
		out.core_tweaks.kad_max_searches = static_cast<std::uint32_t>(t->GetInt());
	}
	if (const CECTag *t = c->GetTagByName(EC_TAG_CORETW_KAD_REASK_MS)) {
		out.core_tweaks.kad_reask_ms = static_cast<std::uint32_t>(t->GetInt());
	}
	if (const CECTag *t = c->GetTagByName(EC_TAG_CORETW_SOURCE_REASK_MS)) {
		out.core_tweaks.source_reask_ms = static_cast<std::uint32_t>(t->GetInt());
	}
}

void ParseKademliaPrefs(const CECTag *k, PreferencesSnapshot &out)
{
	if (const CECTag *t = k->GetTagByName(EC_TAG_KADEMLIA_UPDATE_URL)) {
		out.kademlia.update_url = std::string(t->GetStringData().utf8_str());
	}
}

} // namespace

void ParsePreferencesFromPacket(
	const CECPacket *resp, PreferencesSnapshot &out_prefs, std::vector<CategorySnapshot> &out_cats)
{
	out_cats.clear();
	if (!resp)
		return;

	// Each prefs sub-section is one top-level CECEmptyTag with named
	// child fields. `EC_TAG_PREFS_CATEGORIES` wraps individual
	// `EC_TAG_CATEGORY` entries (one per index).
	if (const CECTag *gen = resp->GetTagByName(EC_TAG_PREFS_GENERAL)) {
		ParseGeneralPrefs(gen, out_prefs);
	}
	if (const CECTag *conn = resp->GetTagByName(EC_TAG_PREFS_CONNECTIONS)) {
		ParseConnectionPrefs(conn, out_prefs);
	}
	if (const CECTag *d = resp->GetTagByName(EC_TAG_PREFS_DIRECTORIES)) {
		ParseDirectoriesPrefs(d, out_prefs);
	}
	if (const CECTag *f = resp->GetTagByName(EC_TAG_PREFS_FILES)) {
		ParseFilesPrefs(f, out_prefs);
	}
	if (const CECTag *s = resp->GetTagByName(EC_TAG_PREFS_SERVERS)) {
		ParseServersPrefs(s, out_prefs);
	}
	if (const CECTag *s = resp->GetTagByName(EC_TAG_PREFS_SECURITY)) {
		ParseSecurityPrefs(s, out_prefs);
	}
	if (const CECTag *m = resp->GetTagByName(EC_TAG_PREFS_MESSAGEFILTER)) {
		ParseMessageFilterPrefs(m, out_prefs);
	}
	if (const CECTag *rc = resp->GetTagByName(EC_TAG_PREFS_REMOTECTRL)) {
		ParseRemoteControlsPrefs(rc, out_prefs);
	}
	if (const CECTag *o = resp->GetTagByName(EC_TAG_PREFS_ONLINESIG)) {
		ParseOnlineSigPrefs(o, out_prefs);
	}
	if (const CECTag *c = resp->GetTagByName(EC_TAG_PREFS_CORETWEAKS)) {
		ParseCoreTweaksPrefs(c, out_prefs);
	}
	if (const CECTag *k = resp->GetTagByName(EC_TAG_PREFS_KADEMLIA)) {
		ParseKademliaPrefs(k, out_prefs);
	}
	if (const CECTag *cats = resp->GetTagByName(EC_TAG_PREFS_CATEGORIES)) {
		for (CECTag::const_iterator it = cats->begin(); it != cats->end(); ++it) {
			const CECTag *cat = &*it;
			if (cat->GetTagName() != EC_TAG_CATEGORY)
				continue;
			CategorySnapshot c;
			ParseCategoryTag(cat, c);
			out_cats.push_back(std::move(c));
		}
	}
}

// RefresherTick + TwoPhaseRefresh live in RefresherTick.cpp so that
// this TU stays App-free and the unit tests can link the Apply*
// functions without pulling in wxApp / ExternalConnector.

} // namespace webapi
