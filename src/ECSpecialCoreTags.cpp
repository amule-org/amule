//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2011 Angel Vidal ( kry@amule.org )
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

#include <ec/cpp/ECTag.h>         // Needed for CECTag
#include <ec/cpp/ECSpecialTags.h> // Needed for special EC tag creator classes
#include <tags/FileTags.h>        // Needed for FT_MEDIA_* metadata tag names

// Since there are only constructors defined here,
// removing everything from non-local builds.

#include "amule.h"
#ifdef ENABLE_IP2COUNTRY
#include "IP2Country.h" // For CIP2Country (country tag serialisation, #439/#440)
#endif
#include "Server.h"        // Needed for CServer
#include "PartFile.h"      // Needed for CPartFile
#include "ServerConnect.h" // Needed for CServerConnect
#include "updownclient.h"  // Needed for CUpDownClient
#include "UploadQueue.h"   // Needed for CUploadQueue
#include "SharedFileList.h"
#include "SearchList.h"
#include "Friend.h"

#include "kademlia/kademlia/Kademlia.h"

// used for webserver, amulecmd
CEC_Server_Tag::CEC_Server_Tag(const CServer *server, EC_DETAIL_LEVEL detail_level)
: CECTag(EC_TAG_SERVER, EC_IPv4_t(server->GetIP(), server->GetPort()))
{
	wxString tmpStr;
	uint32 tmpInt;
	uint8 tmpShort;

	switch (detail_level) {
	case EC_DETAIL_INC_UPDATE:
		// should not get here
		wxFAIL;
		break;
	case EC_DETAIL_UPDATE:
		if ((tmpInt = server->GetPing()) != 0) {
			AddTag(CECTag(EC_TAG_SERVER_PING, tmpInt));
		}
		if ((tmpShort = (uint8)server->GetFailedCount()) != 0) {
			AddTag(CECTag(EC_TAG_SERVER_FAILED, tmpShort));
		}
		break;
	case EC_DETAIL_WEB:
	case EC_DETAIL_FULL:
		if ((tmpInt = server->GetPing()) != 0) {
			AddTag(CECTag(EC_TAG_SERVER_PING, tmpInt));
		}
		if ((tmpShort = (uint8)server->GetPreferences()) != SRV_PR_NORMAL) {
			AddTag(CECTag(EC_TAG_SERVER_PRIO, tmpShort));
		}
		if ((tmpShort = (uint8)server->GetFailedCount()) != 0) {
			AddTag(CECTag(EC_TAG_SERVER_FAILED, tmpShort));
		}
		if ((tmpShort = (server->IsStaticMember() ? 1 : 0)) != 0) {
			AddTag(CECTag(EC_TAG_SERVER_STATIC, tmpShort));
		}
		if (!(tmpStr = server->GetVersion()).IsEmpty()) {
			AddTag(CECTag(EC_TAG_SERVER_VERSION, tmpStr));
		}
		if (!(tmpStr = server->GetDescription()).IsEmpty()) {
			AddTag(CECTag(EC_TAG_SERVER_DESC, tmpStr));
		}
		if ((tmpInt = server->GetUsers()) != 0) {
			AddTag(CECTag(EC_TAG_SERVER_USERS, tmpInt));
		}
		if ((tmpInt = server->GetMaxUsers()) != 0) {
			AddTag(CECTag(EC_TAG_SERVER_USERS_MAX, tmpInt));
		}
		if ((tmpInt = server->GetFiles()) != 0) {
			AddTag(CECTag(EC_TAG_SERVER_FILES, tmpInt));
		}
		// Per-user publishing limits (issue #840). Sent from both server-tag
		// builders: this detail-level one carries the initial list, the valuemap
		// one below carries updates, and a tag added to only one of them leaves
		// the remote GUI's column permanently blank.
		if ((tmpInt = server->GetSoftFiles()) != 0) {
			AddTag(CECTag(EC_TAG_SERVER_FILES_SOFT, tmpInt));
		}
		if ((tmpInt = server->GetHardFiles()) != 0) {
			AddTag(CECTag(EC_TAG_SERVER_FILES_HARD, tmpInt));
		}
		// Wire capability flags. Diagnostics, and hidden by default in release
		// builds, but the remote GUI offers the same columns as the monolithic
		// app so it needs the same data behind them.
		if ((tmpInt = server->GetTCPFlags()) != 0) {
			AddTag(CECTag(EC_TAG_SERVER_TCP_FLAGS, tmpInt));
		}
		if ((tmpInt = server->GetUDPFlags()) != 0) {
			AddTag(CECTag(EC_TAG_SERVER_UDP_FLAGS, tmpInt));
		}
	/* fall through */
	case EC_DETAIL_CMD:
		if (!(tmpStr = server->GetListName()).IsEmpty()) {
			AddTag(CECTag(EC_TAG_SERVER_NAME, tmpStr));
		}
#ifdef ENABLE_IP2COUNTRY
		// Server host country ISO code (#440). WEB / FULL fall through to here,
		// so this covers the webserver, amulecmd and remote-detail paths.
		if (theApp->GetIP2Country() && theApp->GetIP2Country()->IsEnabled()) {
			AddTag(CECTag(EC_TAG_SERVER_COUNTRY,
				theApp->GetIP2Country()->GetCountryCode(server->GetFullIP())));
		}
#endif
	}
}

// used for amulegui (EC_DETAIL_INC_UPDATE)
CEC_Server_Tag::CEC_Server_Tag(const CServer *server, CValueMap *valuemap)
: CECTag(EC_TAG_SERVER, server->ECID())
{
	AddTag(EC_TAG_SERVER_NAME, server->GetListName(), valuemap);
	AddTag(EC_TAG_SERVER_DESC, server->GetDescription(), valuemap);
	AddTag(EC_TAG_SERVER_VERSION, server->GetVersion(), valuemap);
	AddTag(EC_TAG_SERVER_IP, server->GetIP(), valuemap);
	AddTag(EC_TAG_SERVER_PORT, server->GetPort(), valuemap);
	AddTag(EC_TAG_SERVER_PING, server->GetPing(), valuemap);
	AddTag(EC_TAG_SERVER_PRIO, server->GetPreferences(), valuemap);
	AddTag(EC_TAG_SERVER_FAILED, server->GetFailedCount(), valuemap);
	AddTag(EC_TAG_SERVER_STATIC, server->IsStaticMember(), valuemap);
	AddTag(EC_TAG_SERVER_USERS, server->GetUsers(), valuemap);
	AddTag(EC_TAG_SERVER_USERS_MAX, server->GetMaxUsers(), valuemap);
	AddTag(EC_TAG_SERVER_FILES, server->GetFiles(), valuemap);
	// Per-user publishing limits the server advertises (issue #840). Zero here
	// means it has not told us yet, and the GUI renders that as blank.
	AddTag(EC_TAG_SERVER_FILES_SOFT, server->GetSoftFiles(), valuemap);
	AddTag(EC_TAG_SERVER_FILES_HARD, server->GetHardFiles(), valuemap);
	AddTag(EC_TAG_SERVER_TCP_FLAGS, server->GetTCPFlags(), valuemap);
	AddTag(EC_TAG_SERVER_UDP_FLAGS, server->GetUDPFlags(), valuemap);
#ifdef ENABLE_IP2COUNTRY
	// Server host country ISO code for the remote GUI (#440).
	if (theApp->GetIP2Country() && theApp->GetIP2Country()->IsEnabled()) {
		AddTag(EC_TAG_SERVER_COUNTRY,
			theApp->GetIP2Country()->GetCountryCode(server->GetFullIP()),
			valuemap);
	}
#endif
}

CEC_ConnState_Tag::CEC_ConnState_Tag(EC_DETAIL_LEVEL detail_level)
: CECTag(EC_TAG_CONNSTATE,
	  (uint8)((theApp->IsConnectedED2K() ? 0x01 : 0x00) |
		  (theApp->serverconnect->IsConnecting() ? 0x02 : 0x00) |
		  (theApp->IsConnectedKad() ? 0x04 : 0x00) |
		  (Kademlia::CKademlia::IsFirewalled() ? 0x08 : 0x00) |
		  (Kademlia::CKademlia::IsRunning() ? 0x10 : 0x00)))
{
	if (theApp->IsConnectedED2K()) {
		if (theApp->serverconnect->GetCurrentServer()) {
			if (detail_level == EC_DETAIL_INC_UPDATE) {
				// Send no full server tag, just the ECID of the connected server
				AddTag(CECTag(
					EC_TAG_SERVER, theApp->serverconnect->GetCurrentServer()->ECID()));
			} else {
				AddTag(CEC_Server_Tag(
					theApp->serverconnect->GetCurrentServer(), detail_level));
			}
		}
		AddTag(CECTag(EC_TAG_ED2K_ID, theApp->GetED2KID()));
		// GetTicks() (a possibly-64-bit time_t) truncated to uint32 --
		// fine until 2106, matching EC_TAG_ED2K_ID's own uint32 id
		// space this tag already sits next to. Only sent while actually
		// connected, so amulegui/amuleapi never have to distinguish
		// "never connected" from "connected at the unix epoch"
		// (amule-org/amule#174).
		if (theApp->GetED2KConnectedSince().IsValid()) {
			AddTag(CECTag(EC_TAG_ED2K_CONNECTED_SINCE,
				(uint32)theApp->GetED2KConnectedSince().GetTicks()));
		}
	} else if (theApp->serverconnect->IsConnecting()) {
		AddTag(CECTag(EC_TAG_ED2K_ID, 0xffffffff));
	}

	AddTag(CECTag(EC_TAG_CLIENT_ID, theApp->GetID()));

	if (Kademlia::CKademlia::IsRunning()) {
		AddTag(CECTag(EC_TAG_KAD_ID, Kademlia::CKademlia::GetKadID()));
	}
	if (theApp->IsConnectedKad() && theApp->GetKadConnectedSince().IsValid()) {
		AddTag(CECTag(EC_TAG_KAD_CONNECTED_SINCE, (uint32)theApp->GetKadConnectedSince().GetTicks()));
	}
}

CEC_PartFile_Tag::CEC_PartFile_Tag(const CPartFile *file, EC_DETAIL_LEVEL detail_level, CValueMap *valuemap)
: CEC_SharedFile_Tag(file, detail_level, valuemap, EC_TAG_PARTFILE)
{
	AddTag(EC_TAG_PARTFILE_STATUS, file->GetStatus(), valuemap);
	AddTag(EC_TAG_PARTFILE_STOPPED, file->IsStopped(), valuemap);

	AddTag(EC_TAG_PARTFILE_SOURCE_COUNT, file->GetSourceCount(), valuemap);
	AddTag(EC_TAG_PARTFILE_SOURCE_COUNT_NOT_CURRENT, file->GetNotCurrentSourcesCount(), valuemap);
	AddTag(EC_TAG_PARTFILE_SOURCE_COUNT_XFER, file->GetTransferingSrcCount(), valuemap);
	AddTag(EC_TAG_PARTFILE_SOURCE_COUNT_A4AF, file->GetSrcA4AFCount(), valuemap);

	if ((file->GetTransferingSrcCount() > 0) || (detail_level != EC_DETAIL_UPDATE) || valuemap) {

		AddTag(EC_TAG_PARTFILE_SIZE_XFER, file->GetTransferred(), valuemap);
		AddTag(EC_TAG_PARTFILE_SIZE_DONE, file->GetCompletedSize(), valuemap);
		AddTag(EC_TAG_PARTFILE_SPEED, (uint64_t)(file->GetKBpsDown() * 1024), valuemap);
	}

	AddTag(EC_TAG_PARTFILE_PRIO,
		(file->IsAutoDownPriority() ? file->GetDownPriority() + 10 : file->GetDownPriority()),
		valuemap);

	AddTag(EC_TAG_PARTFILE_CAT, file->GetCategory(), valuemap);
	AddTag(EC_TAG_PARTFILE_LAST_SEEN_COMP, file->lastseencomplete, valuemap);
	AddTag(EC_TAG_PARTFILE_LAST_RECV, file->GetLastChangeDatetime(), valuemap);
	AddTag(EC_TAG_PARTFILE_DOWNLOAD_ACTIVE, file->GetDlActiveTime(), valuemap);
	AddTag(EC_TAG_PARTFILE_AVAILABLE_PARTS, file->GetAvailablePartCount(), valuemap);
	AddTag(EC_TAG_PARTFILE_HASHED_PART_COUNT, file->GetHashingProgress(), valuemap);

	AddTag(EC_TAG_PARTFILE_LOST_CORRUPTION, file->GetLostDueToCorruption(), valuemap);
	AddTag(EC_TAG_PARTFILE_GAINED_COMPRESSION, file->GetGainDueToCompression(), valuemap);
	AddTag(EC_TAG_PARTFILE_SAVED_ICH, file->TotalPacketsSavedDueToICH(), valuemap);
	AddTag(EC_TAG_PARTFILE_A4AFAUTO, file->IsA4AFAuto(), valuemap);

	// Community ratings/comments + the Kad-notes running flag are serialized by
	// the CEC_SharedFile_Tag base constructor (shared with plain shared files).

	if (detail_level == EC_DETAIL_UPDATE) {
		return;
	}

	AddTag(EC_TAG_PARTFILE_PARTMETID, file->GetPartMetNumber(), valuemap);

	// A4AF sources
	CECEmptyTag a4afTag(EC_TAG_PARTFILE_A4AF_SOURCES);
	const CKnownFile::SourceSet &a4afSources = file->GetA4AFList();
	for (CKnownFile::SourceSet::const_iterator it = a4afSources.begin(); it != a4afSources.end(); ++it) {
		a4afTag.AddTag(CECTag(EC_TAG_ECID, it->ECID()));
	}
	AddTag(a4afTag, valuemap);
}

namespace
{

// Emit the FT_MEDIA_* tags a file actually carries, one by one.
//
// NOT gated on GetMetaDataVer(): that predicate answers "has this file been
// probed", and using it to mean "all six fields are present" sends
// MEDIA_LENGTH=0 and MEDIA_BITRATE=0 for a file that probed to a codec and no
// duration -- a raw elementary stream, a truncated capture. Downstream that
// renders as Length 0:00 / Bitrate 0 kbps and reports has_media alongside
// length_s: 0, where the honest answer is N/A. A displayed zero is a claim;
// absence is not.
//
// Shared by the shared-file and search-result tag builders, which had
// diverged: the search one already emitted per field, the shared one gated
// all six on the aggregate.
void AddMediaTagsPresent(CECTag &target, const CAbstractFile *file, CValueMap *valuemap)
{
	// Present -> send the value. Absent, but we sent one before -> send an
	// explicit zero / empty so the remote drops it. Absent and never sent ->
	// send nothing, so a field that never had a value is reported as absent
	// rather than as a zero.
	//
	// The middle case is not optional. A tag that is simply not offered reads
	// as UNCHANGED to a CValueMap peer, and both receivers are add-only, so
	// omitting a CLEARED field would leave amulegui and the web UI serving the
	// stale value -- including an unverified one inherited from a search
	// result, which is exactly what the completion re-probe exists to correct.
	const auto emitInt = [&](ec_tagname_t ecId, uint32 value) {
		if (value) {
			target.AddTag(CECTag(ecId, value), valuemap);
		} else if (valuemap && valuemap->HasTag(ecId)) {
			target.AddTag(CECTag(ecId, static_cast<uint32>(0)), valuemap);
		}
	};
	emitInt(EC_TAG_KNOWNFILE_MEDIA_LENGTH, file->GetIntTagValue(FT_MEDIA_LENGTH));
	emitInt(EC_TAG_KNOWNFILE_MEDIA_BITRATE, file->GetIntTagValue(FT_MEDIA_BITRATE));
	static const struct
	{
		uint8 ftId;
		ec_tagname_t ecId;
	} kStrTags[] = { { FT_MEDIA_CODEC, EC_TAG_KNOWNFILE_MEDIA_CODEC },
		{ FT_MEDIA_ARTIST, EC_TAG_KNOWNFILE_MEDIA_ARTIST },
		{ FT_MEDIA_ALBUM, EC_TAG_KNOWNFILE_MEDIA_ALBUM },
		{ FT_MEDIA_TITLE, EC_TAG_KNOWNFILE_MEDIA_TITLE } };
	for (const auto &entry : kStrTags) {
		const wxString &value = file->GetStrTagValue(entry.ftId);
		if (!value.IsEmpty()) {
			target.AddTag(CECTag(entry.ecId, value), valuemap);
		} else if (valuemap && valuemap->HasTag(entry.ecId)) {
			target.AddTag(CECTag(entry.ecId, wxString()), valuemap);
		}
	}
}

} // namespace

CEC_SharedFile_Tag::CEC_SharedFile_Tag(
	const CKnownFile *file, EC_DETAIL_LEVEL detail_level, CValueMap *valuemap, ec_tagname_t name)
: CECTag(name, file->ECID())
{
	AddTag(EC_TAG_KNOWNFILE_REQ_COUNT, file->statistic.GetRequests(), valuemap);
	AddTag(EC_TAG_KNOWNFILE_REQ_COUNT_ALL, file->statistic.GetAllTimeRequests(), valuemap);

	AddTag(EC_TAG_KNOWNFILE_ACCEPT_COUNT, file->statistic.GetAccepts(), valuemap);
	AddTag(EC_TAG_KNOWNFILE_ACCEPT_COUNT_ALL, file->statistic.GetAllTimeAccepts(), valuemap);

	AddTag(EC_TAG_KNOWNFILE_XFERRED, file->statistic.GetTransferred(), valuemap);
	AddTag(EC_TAG_KNOWNFILE_XFERRED_ALL, file->statistic.GetAllTimeTransferred(), valuemap);
	AddTag(EC_TAG_KNOWNFILE_AICH_MASTERHASH, file->GetAICHMasterHash(), valuemap);

	AddTag(EC_TAG_KNOWNFILE_PRIO,
		(uint8)(file->IsAutoUpPriority() ? file->GetUpPriority() + 10 : file->GetUpPriority()),
		valuemap);

	AddTag(EC_TAG_KNOWNFILE_COMPLETE_SOURCES_LOW, file->m_nCompleteSourcesCountLo, valuemap);
	AddTag(EC_TAG_KNOWNFILE_COMPLETE_SOURCES_HIGH, file->m_nCompleteSourcesCountHi, valuemap);
	AddTag(EC_TAG_KNOWNFILE_COMPLETE_SOURCES, file->m_nCompleteSourcesCount, valuemap);

	if (name == EC_TAG_KNOWNFILE) {
		AddTag(EC_TAG_KNOWNFILE_HASHED_PART_COUNT, file->GetHashingProgress(), valuemap);
	}
	AddTag(EC_TAG_KNOWNFILE_ON_QUEUE, file->GetQueuedCount(), valuemap);

	// Live upload activity (issue #466). Emitted before the UPDATE
	// early-return so they refresh every tick like the download-side
	// speed/source counts. The speed + uploading count are computed from
	// m_ClientUploadList (core-only); amulegui receives them over EC.
#ifndef CLIENT_GUI
	AddTag(EC_TAG_KNOWNFILE_UPLOAD_SPEED, file->GetUploadDatarate(), valuemap);
	AddTag(EC_TAG_KNOWNFILE_UPLOADING_COUNT, file->GetTransferringClientCount(), valuemap);
#endif
	AddTag(EC_TAG_KNOWNFILE_LAST_UPLOAD, (uint32)file->GetLastUpload(), valuemap);

	// Community ratings/comments + the on-demand Kad-notes running flag, shared
	// by downloads AND shared files via the virtual GetRatingAndComments (a
	// partfile prepends its connected-source comments; a plain shared file
	// carries just its Kad notes). Emitted before the UPDATE early-return so the
	// flag's start -> finish and notes streaming in are visible on every poll;
	// the valuemap suppresses unchanged values, so idle files cost nothing after
	// the first send.
	CECEmptyTag sc(EC_TAG_PARTFILE_COMMENTS);
	FileRatingList list;
	file->GetRatingAndComments(list);
	for (FileRatingList::const_iterator it = list.begin(); it != list.end(); ++it) {
		// Tag children are evaluated by index, not by name.
		sc.AddTag(CECTag(EC_TAG_PARTFILE_COMMENTS, it->UserName));
		sc.AddTag(CECTag(EC_TAG_PARTFILE_COMMENTS, it->FileName));
		sc.AddTag(CECTag(EC_TAG_PARTFILE_COMMENTS, (uint64)it->Rating));
		sc.AddTag(CECTag(EC_TAG_PARTFILE_COMMENTS, it->Comment));
	}
	AddTag(sc, valuemap);
	AddTag(EC_TAG_PARTFILE_KAD_COMMENT_SEARCHING, file->IsKadCommentSearchRunning(), valuemap);

	if (detail_level == EC_DETAIL_UPDATE) {
		return;
	}

	AddTag(EC_TAG_PARTFILE_NAME, file->GetFileName().GetPrintable(), valuemap);
	AddTag(EC_TAG_PARTFILE_HASH, file->GetFileHash(), valuemap);
	// Partfile branch used to go through CFormat + CPath::RemoveExt every
	// call; the basename is now cached on CPartFile (lifetime-stable).
	AddTag(EC_TAG_KNOWNFILE_FILENAME,
		file->IsPartFile() ? static_cast<const CPartFile *>(file)->GetCachedPartMetBasename()
				   : file->GetFilePath().GetPrintable(),
		valuemap);
	// The on-disk directory, always — the Temp dir for a partfile, the
	// destination dir once completed. Unlike _FILENAME (which doubles as
	// the ".part" basename for partfiles), this never changes meaning
	// across the completed transition, so the REST API can expose an
	// unambiguous `path` on /downloads and /shared (issue #417).
	AddTag(EC_TAG_KNOWNFILE_PATH, file->GetFilePath().GetPrintable(), valuemap);

	// When the file was completed / first shared (issue #466). Static once
	// set, so it rides in the full-detail section rather than every tick.
	AddTag(EC_TAG_KNOWNFILE_SHARED_SINCE, (uint32)file->GetDateShared(), valuemap);

	AddTag(EC_TAG_PARTFILE_SIZE_FULL, file->GetFileSize(), valuemap);

	// Cached path: ed2k:// link construction is the single hottest item on
	// the EC dispatch chain for big shared-file libraries (#713 profile).
	// The cache holds the rare-changing base form; the dynamic source
	// suffix is appended live.
	AddTag(EC_TAG_PARTFILE_ED2K_LINK,
		file->GetED2kLinkForEC(theApp->IsConnectedED2K() && !theApp->serverconnect->IsLowID()),
		valuemap);

	AddTag(EC_TAG_KNOWNFILE_COMMENT, file->GetFileComment(), valuemap);
	AddTag(EC_TAG_KNOWNFILE_RATING, file->GetFileRating(), valuemap);

	// Audio/video media metadata (issue #418). Emitted per FIELD -- see
	// AddMediaTagsPresent for why the aggregate GetMetaDataVer() gate it used
	// to sit behind was wrong. Emitted by this shared base ctor, so both
	// /shared and /downloads (partfiles) carry it with no per-role code.
	AddMediaTagsPresent(*this, file, valuemap);
}

CEC_UpDownClient_Tag::CEC_UpDownClient_Tag(
	const CUpDownClient *client, EC_DETAIL_LEVEL detail_level, CValueMap *valuemap)
: CECTag(EC_TAG_CLIENT, client->ECID())
{
	// General
	AddDiffTag(this, EC_TAG_CLIENT_NAME, client->GetUserName(), valuemap);
	AddDiffTag(this, EC_TAG_CLIENT_HASH, client->GetUserHash(), valuemap);
	AddDiffTag(this, EC_TAG_CLIENT_USER_ID, client->GetUserIDHybrid(), valuemap);
	AddDiffTag(this, EC_TAG_CLIENT_SCORE, client->GetScore(), valuemap);
	AddDiffTag(this, EC_TAG_CLIENT_SOFTWARE, client->GetClientSoft(), valuemap);
	AddDiffTag(this, EC_TAG_CLIENT_SOFT_VER_STR, client->GetSoftVerStr(), valuemap);
	AddDiffTag(this, EC_TAG_CLIENT_USER_IP, client->GetIP(), valuemap);
	AddDiffTag(this, EC_TAG_CLIENT_USER_PORT, client->GetUserPort(), valuemap);
#ifdef ENABLE_IP2COUNTRY
	// Peer country ISO code resolved core-side (#439). Emitted whenever GeoIP
	// is enabled + supported — even empty for an IP that doesn't resolve — so
	// a frontend can treat tag-present as authoritative (possibly "unknown")
	// and tag-absent as "no daemon GeoIP".
	if (theApp->GetIP2Country() && theApp->GetIP2Country()->IsEnabled()) {
		// Numeric-IP overload: memoised, and it skips formatting the IP into
		// a string on the hit path. This runs for every peer on every EC
		// poll, and a peer's country cannot change while its IP does not.
		// It returns a const reference, which AddDiffTag takes without a copy.
		AddDiffTag(this,
			EC_TAG_CLIENT_COUNTRY,
			theApp->GetIP2Country()->GetCountryCode(client->GetFullIPNumeric()),
			valuemap);
	}
#endif
	AddDiffTag(this, EC_TAG_CLIENT_FROM, (uint64)client->GetSourceFrom(), valuemap);
	AddDiffTag(this, EC_TAG_CLIENT_SERVER_IP, client->GetServerIP(), valuemap);
	AddDiffTag(this, EC_TAG_CLIENT_SERVER_PORT, client->GetServerPort(), valuemap);
	AddDiffTag(this, EC_TAG_CLIENT_SERVER_NAME, client->GetServerName(), valuemap);

	// Transfers to Client
	AddDiffTag(this, EC_TAG_CLIENT_UP_SPEED, client->GetUploadDatarate(), valuemap);
	if (client->GetDownloadState() == DS_DOWNLOADING || valuemap) {
		AddDiffTag(this, EC_TAG_CLIENT_DOWN_SPEED, (double)(client->GetKBpsDown()), valuemap);
	}
	AddDiffTag(this, EC_TAG_CLIENT_UPLOAD_SESSION, client->GetSessionUp(), valuemap);
	AddDiffTag(this, EC_TAG_PARTFILE_SIZE_XFER, client->GetTransferredDown(), valuemap);
	AddDiffTag(this, EC_TAG_CLIENT_UPLOAD_TOTAL, client->GetUploadedTotal(), valuemap);
	AddDiffTag(this, EC_TAG_CLIENT_DOWNLOAD_TOTAL, client->GetDownloadedTotal(), valuemap);

	AddDiffTag(this, EC_TAG_CLIENT_UPLOAD_STATE, client->GetUploadState(), valuemap);
	AddDiffTag(this, EC_TAG_CLIENT_DOWNLOAD_STATE, client->GetDownloadState(), valuemap);
	AddDiffTag(this, EC_TAG_CLIENT_IDENT_STATE, (uint64)client->GetCurrentIdentState(), valuemap);
	AddDiffTag(this, EC_TAG_CLIENT_EXT_PROTOCOL, client->ExtProtocolAvailable(), valuemap);
	// These are not needed atm. Keep them for now, maybe columns get reintroduced in client view.
	// AddTag(CECTag(EC_TAG_CLIENT_WAIT_TIME, client->GetWaitTime()), valuemap);
	// AddTag(CECTag(EC_TAG_CLIENT_XFER_TIME, client->GetUpStartTimeDelay()), valuemap);
	// AddTag(CECTag(EC_TAG_CLIENT_QUEUE_TIME, (uint64)(::GetTickCount() - client->GetWaitStartTime())),
	// valuemap); AddTag(CECTag(EC_TAG_CLIENT_LAST_TIME, (uint64)(::GetTickCount() -
	// client->GetLastUpRequest())), valuemap);
	AddDiffTag(this, EC_TAG_CLIENT_WAITING_POSITION, client->GetUploadQueueWaitingPosition(), valuemap);
	AddDiffTag(this,
		EC_TAG_CLIENT_REMOTE_QUEUE_RANK,
		client->IsRemoteQueueFull() ? (uint16)0xffff : client->GetRemoteQueueRank(),
		valuemap);
	AddDiffTag(this, EC_TAG_CLIENT_OLD_REMOTE_QUEUE_RANK, client->GetOldRemoteQueueRank(), valuemap);
	AddDiffTag(this, EC_TAG_CLIENT_OBFUSCATION_STATUS, client->GetObfuscationStatus(), valuemap);
	AddDiffTag(this, EC_TAG_CLIENT_KAD_PORT, client->GetKadPort(), valuemap);
	AddDiffTag(this, EC_TAG_CLIENT_FRIEND_SLOT, client->GetFriendSlot(), valuemap);

	if (detail_level == EC_DETAIL_UPDATE) {
		return;
	}
	const CKnownFile *file = client->GetUploadFile();
	if (file) {
		AddDiffTag(this, EC_TAG_PARTFILE_NAME, file->GetFileName().GetPrintable(), valuemap);
		AddTag(CECTag(EC_TAG_CLIENT_UPLOAD_FILE, file->ECID()), valuemap);
	} else {
		AddTag(CECIntTag(EC_TAG_CLIENT_UPLOAD_FILE, 0), valuemap);
	}
	const CPartFile *pfile = client->GetRequestFile();
	AddDiffTag(this, EC_TAG_CLIENT_REQUEST_FILE, pfile ? pfile->ECID() : 0, valuemap);
	AddDiffTag(this, EC_TAG_CLIENT_REMOTE_FILENAME, client->GetClientFilename(), valuemap);

	if (detail_level != EC_DETAIL_INC_UPDATE) {
		return;
	}
	// Friend status + DL/UP modifier (issue #423). IsFriend() is the
	// friends-list membership (distinct from the FRIEND_SLOT reserved
	// upload slot above); GetScoreRatio() is the GUI "DL/UP modifier".
	AddDiffTag(this, EC_TAG_CLIENT_IS_FRIEND, client->IsFriend(), valuemap);
	AddDiffTag(this, EC_TAG_CLIENT_SCORE_RATIO, (double)client->GetScoreRatio(), valuemap);
	AddDiffTag(this, EC_TAG_CLIENT_DISABLE_VIEW_SHARED, client->HasDisabledSharedFiles(), valuemap);
	AddDiffTag(this, EC_TAG_CLIENT_VERSION, client->GetVersion(), valuemap);
	AddDiffTag(this, EC_TAG_CLIENT_MOD_VERSION, client->GetClientModString(), valuemap);
	AddDiffTag(this, EC_TAG_CLIENT_OS_INFO, client->GetClientOSInfo(), valuemap);
	AddDiffTag(this, EC_TAG_CLIENT_AVAILABLE_PARTS, client->GetAvailablePartCount(), valuemap);
	if (pfile) {
		const BitVector &partStatus = client->GetPartStatus();
		if (partStatus.size() == pfile->GetPartCount()) {
			if (partStatus.AllTrue()) {
				// send just an empty tag for a full source
				AddTag(CECEmptyTag(EC_TAG_CLIENT_PART_STATUS), valuemap);
			} else {
				AddTag(CECTag(EC_TAG_CLIENT_PART_STATUS,
					       partStatus.SizeBuffer(),
					       partStatus.GetBuffer()),
					valuemap);
			}
		}
		AddDiffTag(this, EC_TAG_CLIENT_NEXT_REQUESTED_PART, client->GetNextRequestedPart(), valuemap);
		AddTag(CECTag(EC_TAG_CLIENT_LAST_DOWNLOADING_PART, client->GetLastDownloadingPart()),
			valuemap);
	}
	if (file) {
		const BitVector &upPartStatus = client->GetUpPartStatus();
		if (upPartStatus.size() == file->GetPartCount()) {
			AddTag(CECTag(EC_TAG_CLIENT_UPLOAD_PART_STATUS,
				       upPartStatus.SizeBuffer(),
				       upPartStatus.GetBuffer()),
				valuemap);
		}
	}
}

//
// Search reply
//
CEC_SearchFile_Tag::CEC_SearchFile_Tag(
	const CSearchFile *file, EC_DETAIL_LEVEL detail_level, CValueMap *valuemap, uint32 searchID)
: CECTag(EC_TAG_SEARCHFILE, file->ECID())
{
	// Multi-search union poll (amulegui): the owning search's ID so the client
	// routes this result to the right tab. Emitted unconditionally (no
	// valuemap, before the UPDATE early-return) so a result object (re)created
	// on any poll can always be attributed. 0 => omitted (legacy / non-union).
	if (searchID) {
		AddTag(CECTag(EC_TAG_SEARCH_ID, searchID));
	}
	AddTag(CECTag(EC_TAG_PARTFILE_SOURCE_COUNT, file->GetSourceCount()), valuemap);
	AddTag(CECTag(EC_TAG_PARTFILE_SOURCE_COUNT_XFER, file->GetCompleteSourceCount()), valuemap);
	AddTag(CECTag(EC_TAG_PARTFILE_STATUS, (uint32)file->GetDownloadStatus()), valuemap);

	// On-demand Kad community ratings/comments for this result, reusing the
	// partfile tags (CSearchFile borrows EC_TAG_PARTFILE_* above). Emitted before
	// the UPDATE early-return so the flag's start -> finish and the notes
	// streaming in are visible on every poll.
	//
	// Deliberately WITHOUT the valuemap: unlike a download, a search result has
	// no EC change-generation and its reply builder
	// (Get_EC_Response_Search_Results) never resets the per-connection valuemap
	// when amulegui (re)creates the result object. Diffing here would send the
	// comments container exactly once per ECID for the connection's lifetime, so
	// a result object created before the notes arrived — the common case, since
	// the user opens the dialog on an existing result — would be deduped out and
	// the comments dialog would stay empty. Sending them raw keeps delivery
	// reliable; the block is gated on the built list so idle results cost nothing
	// and an empty container never clears the remote list.
	FileRatingList list;
	file->GetRatingAndComments(list);
	// Always emitted, and through the valuemap -- the same shape the download
	// side has always used for this tag (CEC_PartFile_Tag above).
	//
	// It used to be emitted only while the lookup was running or had notes to
	// show, so "finished, found nothing" was signalled by the tag going away.
	// That cannot survive the incremental union: a result whose only change is
	// a tag no longer being built produces a tag with no children, which
	// Get_EC_Response_Search_Results_Union drops as unchanged. The transition
	// was therefore invisible to every incremental client -- amulegui latched
	// the flag on, and so did amuleapi -- and only a full re-read cleared it.
	//
	// Through the valuemap the 1 -> 0 transition is itself a child tag, so the
	// result is not elided and every client sees the lookup end. A steady
	// state still costs nothing: unchanged, it is diffed away as before.
	AddTag(EC_TAG_PARTFILE_KAD_COMMENT_SEARCHING,
		(uint64)(file->IsKadCommentSearchRunning() ? 1 : 0),
		valuemap);
	// The container itself stays gated on there being notes, and stays off the
	// valuemap: an empty one must never be sent, because a client reads its
	// absence as "no notes" rather than as "unchanged".
	//
	// One consequence worth naming, since it looks like a bug from the other
	// end: a result that HAS notes is never elided by the multi-search union.
	// The union skips a result whose tag came out childless, and this
	// container is a child that off-valuemap means it is re-emitted on every
	// poll, unchanged or not. That is the correct trade -- the alternative
	// silently drops notes -- and it is bounded by how few results carry any.
	if (!list.empty()) {
		CECEmptyTag sc(EC_TAG_PARTFILE_COMMENTS);
		for (FileRatingList::const_iterator it = list.begin(); it != list.end(); ++it) {
			// Tag children are evaluated by index, not by name.
			sc.AddTag(CECTag(EC_TAG_PARTFILE_COMMENTS, it->UserName));
			sc.AddTag(CECTag(EC_TAG_PARTFILE_COMMENTS, it->FileName));
			sc.AddTag(CECTag(EC_TAG_PARTFILE_COMMENTS, (uint64)it->Rating));
			sc.AddTag(CECTag(EC_TAG_PARTFILE_COMMENTS, it->Comment));
		}
		AddTag(sc);
	}

	if (detail_level == EC_DETAIL_UPDATE) {
		return;
	}

	AddTag(CECTag(EC_TAG_PARTFILE_NAME, file->GetFileName().GetPrintable()), valuemap);
	AddTag(CECTag(EC_TAG_PARTFILE_SIZE_FULL, file->GetFileSize()), valuemap);
	AddTag(EC_TAG_PARTFILE_HASH, file->GetFileHash(), valuemap);
	if (file->GetParent()) {
		AddTag(EC_TAG_SEARCH_PARENT, file->GetParent()->ECID(), valuemap);
	}
	// Browse ("View Files") source info: the peer this listing came from and the
	// shared folder the file lives in. Set only on results filed from a peer's
	// shared-file list (CSearchList::ProcessSharedFileList), so ordinary
	// server/Kad hits — which never set these — emit nothing here.
	if (file->GetClientID()) {
		AddTag(CECTag(EC_TAG_SEARCHFILE_CLIENT_ID, file->GetClientID()), valuemap);
		AddTag(CECTag(EC_TAG_SEARCHFILE_CLIENT_PORT, file->GetClientPort()), valuemap);
	}
	if (!file->GetDirectory().IsEmpty()) {
		AddTag(CECTag(EC_TAG_SEARCHFILE_DIRECTORY, file->GetDirectory()), valuemap);
	}
	if (file->HasRating()) {
		AddTag(CECTag(EC_TAG_KNOWNFILE_RATING, (uint8)file->UserRating()), valuemap);
	}
	// Media metadata (issue #430). A hit carries FT_MEDIA_* tags only when
	// the file is known/probed locally; emit each EC_TAG_KNOWNFILE_MEDIA_*
	// (defined by issue #418) only when its value is present, so results
	// without media cost nothing.
	AddMediaTagsPresent(*this, file, valuemap);
}

//
// Friend
//
CEC_Friend_Tag::CEC_Friend_Tag(const CFriend *Friend, CValueMap *valuemap)
: CECTag(EC_TAG_FRIEND, Friend->ECID())
{
	AddTag(EC_TAG_FRIEND_NAME, Friend->GetName(), valuemap);
	AddTag(EC_TAG_FRIEND_HASH, Friend->GetUserHash(), valuemap);
	AddTag(EC_TAG_FRIEND_IP, Friend->GetIP(), valuemap);
	AddTag(EC_TAG_FRIEND_PORT, Friend->GetPort(), valuemap);
	const CClientRef &linkedClient = Friend->GetLinkedClient();
	AddTag(EC_TAG_FRIEND_CLIENT, linkedClient.IsLinked() ? linkedClient.ECID() : 0, valuemap);
	// The slot is settable over EC (EC_TAG_FRIEND_FRIENDSLOT on the request
	// side) but was never reported back, so every EC client read it as false
	// -- amulegui's "Establish Friend Slot" check mark could not follow the
	// state it had just set. Same tag id in both directions.
	AddTag(EC_TAG_FRIEND_FRIENDSLOT, Friend->HasFriendSlot(), valuemap);
}

// File_checked_for_headers
