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

#include <muleunit/test.h>

#include <set>

#include "PrefsSchema.h"
#include "Server.h" // SRV_PR_*
#include "PrefsSchema.h"
#include "Refresher.h"
#include "State.h"

#include "RLE.h" // PartFileEncoderData for the rle_state arg

#include <ec/cpp/ECPacket.h>
#include <ec/cpp/ECTag.h>
#include <ec/cpp/ECCodes.h>

#include "include/protocol/ed2k/ClientSoftware.h" // SO_* client-software enum

#include "kademlia/utils/UInt128.h" // CUInt128 (EC_TAG_KAD_ID payload)

#include <cstdint>
#include <map>

using namespace muleunit;
using namespace webapi;

DECLARE_SIMPLE(Refresher)
// Own suite: the preference-schema domain guards below are about
// PrefsSchema.cpp, which this target already compiles and links, not about the
// EC walkers the rest of the file covers.
DECLARE_SIMPLE(PrefsSchema)

// ----------------------------------------------------------------------
// EC_TAG_FILE_REMOVED — INC-protocol deletion marker. With GET_UPDATE
// + EC_DETAIL_INC_UPDATE the marker arrives in the consolidated response
// packet. Both ApplyGetUpdateToDownloads and ApplyGetUpdateToShared
// react to it (one will be a no-op for any given ECID since the
// server-side encoder map is unified across both surfaces, but the
// dispatch is per-walker).
// ----------------------------------------------------------------------

TEST(Refresher, FileRemovedErasesFromDownloads)
{
	// Pre-seed two downloads in the cache.
	FileMap cache;
	{
		FileSnapshot d;
		d.ecid = 42;
		d.hash = "aaaa0000aaaa0000aaaa0000aaaa0000";
		d.name = "doomed.iso";
		cache.emplace(42, d);
	}
	{
		FileSnapshot d;
		d.ecid = 99;
		d.hash = "bbbb1111bbbb1111bbbb1111bbbb1111";
		d.name = "survivor.iso";
		cache.emplace(99, d);
	}

	// Craft a GET_UPDATE response that contains a single
	// EC_TAG_FILE_REMOVED marker pointing at ECID 42.
	// The response packet's op code is what amuled emits per
	// ExternalConn.cpp:874 (EC_OP_SHARED_FILES); the walker doesn't
	// care, it iterates child tags.
	CECPacket resp(EC_OP_SHARED_FILES);
	resp.AddTag(CECTag(EC_TAG_FILE_REMOVED, static_cast<std::uint32_t>(42)));

	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	ApplyGetUpdateToDownloads(&resp, cache, rle_state);

	// Doomed download is gone.
	ASSERT_TRUE(cache.find(42) == cache.end());
	// Survivor is untouched — INC protocol uses explicit deletion
	// markers, never "absence implies removed".
	ASSERT_TRUE(cache.find(99) != cache.end());
	ASSERT_EQUALS(std::string("survivor.iso"), cache.find(99)->second.name);
}

TEST(Refresher, FileRemovedErasesFromShared)
{
	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	// Symmetric to FileRemovedErasesFromDownloads. The server-side
	// encoder map is unified across partfiles + sharedfiles, so a
	// FILE_REMOVED marker could target an ECID in either cache.
	// ApplyGetUpdateToShared evicts unconditionally; the eventual
	// cross-walker call in RefresherTick has both walkers fire on
	// the same response so the right cache loses the entry.
	FileMap cache;
	{
		FileSnapshot s;
		s.ecid = 33;
		s.hash = "1111aaaa1111aaaa1111aaaa1111aaaa";
		s.name = "shared-doomed.iso";
		cache.emplace(33, s);
	}

	CECPacket resp(EC_OP_SHARED_FILES);
	resp.AddTag(CECTag(EC_TAG_FILE_REMOVED, static_cast<std::uint32_t>(33)));

	ApplyGetUpdateToShared(&resp, cache, rle_state);

	ASSERT_TRUE(cache.find(33) == cache.end());
	ASSERT_TRUE(cache.empty());
}

TEST(Refresher, FileRemovedForUnknownEcidIsNoOp)
{
	// Cache contains a single known download.
	FileMap cache;
	{
		FileSnapshot d;
		d.ecid = 7;
		d.hash = "cccc2222cccc2222cccc2222cccc2222";
		d.name = "kept.iso";
		cache.emplace(7, d);
	}

	// Server emits a stale removal marker for ECID 9999 we've never
	// seen (race: server-side gen bumped between the two lookups).
	// Erasing a missing key must be a no-op.
	CECPacket resp(EC_OP_SHARED_FILES);
	resp.AddTag(CECTag(EC_TAG_FILE_REMOVED, static_cast<std::uint32_t>(9999)));

	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	ApplyGetUpdateToDownloads(&resp, cache, rle_state);

	ASSERT_EQUALS(static_cast<size_t>(1), cache.size());
	ASSERT_TRUE(cache.find(7) != cache.end());
	ASSERT_EQUALS(std::string("kept.iso"), cache.find(7)->second.name);
}

// ----------------------------------------------------------------------
// Empty response (no churn since the last tick) — INC protocol's
// silent-skip semantics. Downloads + shared caches stay intact.
// ----------------------------------------------------------------------

TEST(Refresher, EmptyResponseLeavesCachesIntact)
{
	FileMap downloads;
	{
		FileSnapshot d;
		d.ecid = 1;
		d.name = "alpha";
		downloads.emplace(1, d);
	}
	FileMap shared;
	{
		FileSnapshot s;
		s.ecid = 2;
		s.name = "beta";
		shared.emplace(2, s);
	}

	CECPacket resp(EC_OP_SHARED_FILES);
	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	ApplyGetUpdateToDownloads(&resp, downloads, rle_state);
	ApplyGetUpdateToShared(&resp, shared, rle_state);

	// INC protocol: empty response means "no changes since last tick".
	// Cache stays intact — no bulk-delete fallback needed.
	ASSERT_EQUALS(static_cast<size_t>(1), downloads.size());
	ASSERT_EQUALS(static_cast<size_t>(1), shared.size());
}

// ----------------------------------------------------------------------
// Mixed top-level dispatch — one GET_UPDATE response carries both
// EC_TAG_PARTFILE and EC_TAG_KNOWNFILE at the same level. The two
// walkers must each consume only their own tag type without
// cross-contaminating the other cache.
// ----------------------------------------------------------------------

TEST(Refresher, MixedTopLevelDispatchedByTagName)
{
	FileMap downloads;
	FileMap shared;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;

	CECPacket resp(EC_OP_SHARED_FILES);
	// One partfile (ECID 10) — should land in downloads only.
	resp.AddTag(CECTag(EC_TAG_PARTFILE, static_cast<std::uint32_t>(10)));
	// One sharedfile (ECID 20) — should land in shared only.
	resp.AddTag(CECTag(EC_TAG_KNOWNFILE, static_cast<std::uint32_t>(20)));
	// A FILE_REMOVED marker (ECID 99) — erases from both walkers'
	// caches; since neither was pre-seeded, it's a no-op for both.
	resp.AddTag(CECTag(EC_TAG_FILE_REMOVED, static_cast<std::uint32_t>(99)));

	ApplyGetUpdateToDownloads(&resp, downloads, rle_state);
	ApplyGetUpdateToShared(&resp, shared, rle_state);

	// Downloads walker captured ECID 10 only — NOT ECID 20 (that
	// belongs to shared) and NOT ECID 99 (that's the FILE_REMOVED).
	ASSERT_EQUALS(static_cast<size_t>(1), downloads.size());
	ASSERT_TRUE(downloads.find(10) != downloads.end());
	ASSERT_TRUE(downloads.find(20) == downloads.end());

	// Shared walker captured ECID 20 only.
	ASSERT_EQUALS(static_cast<size_t>(1), shared.size());
	ASSERT_TRUE(shared.find(20) != shared.end());
	ASSERT_TRUE(shared.find(10) == shared.end());
}

// ----------------------------------------------------------------------
// Shared partfile dispatch — amuled's /shared surface is the union
// of completed knownfiles AND partfiles with `IsShared()=true`
// (i.e. ≥1 chunk completed → uploadable). GET_UPDATE ships partfiles
// as EC_TAG_PARTFILE with a child `EC_TAG_PARTFILE_SHARED` bool.
// The shared walker has to consume both top-level tag types and gate
// partfile inclusion on the flag.
// ----------------------------------------------------------------------

TEST(Refresher, SharedPartfileWithFlagTrueLandsInShared)
{
	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	FileMap cache;
	CECPacket resp(EC_OP_SHARED_FILES);
	{
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(50));
		// IsShared==true: this partfile has ≥1 chunk and is currently
		// uploadable. The shared walker should pick it up.
		pf.AddTag(CECTag(EC_TAG_PARTFILE_SHARED, static_cast<std::uint8_t>(1)));
		resp.AddTag(pf);
	}

	// PARTFILE_HASH is CValueMap-suppressed on the partfile-to-shared
	// transition tick — supply identity via the downloads-cache fallback,
	// which is how the live code recovers it.
	std::map<std::uint32_t, std::pair<std::string, std::string>> fallback;
	fallback[50] = std::make_pair(
		std::string("aaaa3333aaaa3333aaaa3333aaaa3333"), std::string("shared-test.iso"));
	ApplyGetUpdateToShared(&resp, cache, rle_state);

	ASSERT_EQUALS(static_cast<size_t>(1), cache.size());
	ASSERT_TRUE(cache.find(50) != cache.end());
	ASSERT_EQUALS(static_cast<std::uint32_t>(50), cache.find(50)->second.ecid);
}

TEST(Refresher, UnsharedPartfileSkippedFromShared)
{
	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	// PARTFILE arrives with EC_TAG_PARTFILE_SHARED=false. The
	// shared walker must NOT insert it — the file is in the download
	// queue but has zero chunks completed, so no peer can request it.
	FileMap cache;
	CECPacket resp(EC_OP_SHARED_FILES);
	{
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(60));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_SHARED, static_cast<std::uint8_t>(0)));
		resp.AddTag(pf);
	}

	ApplyGetUpdateToShared(&resp, cache, rle_state);

	ASSERT_TRUE(cache.empty());
}

// ----------------------------------------------------------------------
// Shared availability bar — EC_TAG_PARTFILE_PART_STATUS on the
// EC_TAG_KNOWNFILE tag (issue #982). amuled RLE-encodes the per-part
// source counts as an XOR delta against the previously encoded state,
// so the decoder is stateful and every frame must be fed to it in
// order. These tests drive the real RLE_Data encoder to build the
// blobs, so they pin the wire format rather than a re-implementation
// of it.
// ----------------------------------------------------------------------

namespace
{

// Encode one availability vector the way CKnownFile_Encoder::Encode
// does and hang it off `parent` as EC_TAG_PARTFILE_PART_STATUS.
void AddEncodedPartStatus(CECTag &parent, RLE_Data &enc, const ArrayOfUInts16 &sources)
{
	int len = 0;
	bool changed = false;
	const uint8 *blob = enc.Encode(sources, len, changed);
	parent.AddTag(CECTag(EC_TAG_PARTFILE_PART_STATUS, len, blob));
	delete[] blob;
}

} // namespace

// ----------------------------------------------------------------------
// Chat sessions -- EC_OP_CHAT_SESSIONS (issue #971). The reply is the
// daemon's COMPLETE session set plus only the messages newer than the
// cursor the client sent, so the walker replaces the session vector
// wholesale while appending messages incrementally. A session missing
// from a reply is the ONLY signal a close produces.
// ----------------------------------------------------------------------

namespace
{

// Build one EC_TAG_CHAT_SESSION container the way the daemon does.
CECTag MakeChatSession(std::uint64_t gui_id, const char *name)
{
	CECTag t(EC_TAG_CHAT_SESSION, gui_id);
	t.AddTag(CECTag(EC_TAG_CHAT_PEER_NAME, wxString::FromAscii(name)));
	return t;
}

void AddChatMessage(CECTag &session, std::uint32_t id, bool outgoing, std::uint32_t ts, const char *text)
{
	CECTag m(EC_TAG_CHAT_MESSAGE, wxString::FromUTF8(text));
	m.AddTag(CECTag(EC_TAG_CHAT_MSG_ID, id));
	m.AddTag(CECTag(EC_TAG_CHAT_DIRECTION, static_cast<std::uint8_t>(outgoing ? 1 : 0)));
	m.AddTag(CECTag(EC_TAG_CHAT_TIMESTAMP, ts));
	session.AddTag(m);
}

// GUI_ID for 10.0.0.1:4662, LSB-first like the wire.
const std::uint64_t kPeerA = (static_cast<std::uint64_t>(0x0100000Au) << 16) | 4662u;
const std::uint64_t kPeerB = (static_cast<std::uint64_t>(0x0200000Au) << 16) | 4662u;

} // namespace

TEST(Refresher, ChatSessionDecodesIdentityAndMessages)
{
	std::vector<ChatSessionSnapshot> cache;
	std::uint32_t cursor = 0;
	std::vector<ChatSessionSnapshot> fresh;
	std::vector<std::uint64_t> closed;

	CECPacket resp(EC_OP_CHAT_SESSIONS);
	resp.AddTag(CECTag(EC_TAG_CHAT_MSG_ID, static_cast<std::uint32_t>(2)));
	{
		CECTag s = MakeChatSession(kPeerA, "alice");
		s.AddTag(CECTag(EC_TAG_CLIENT, static_cast<std::uint32_t>(77)));
		s.AddTag(CECTag(EC_TAG_FRIEND, static_cast<std::uint32_t>(12)));
		AddChatMessage(s, 1, false, 1000, "hi");
		AddChatMessage(s, 2, true, 1001, "hello back");
		resp.AddTag(s);
	}

	ApplyChatSessions(&resp, cache, cursor, fresh, closed);

	ASSERT_EQUALS(static_cast<size_t>(1), cache.size());
	ASSERT_EQUALS(kPeerA, cache[0].gui_id);
	// GUI_ID splits back into the REST conversation key.
	ASSERT_EQUALS(std::string("10.0.0.1"), cache[0].ip);
	ASSERT_EQUALS(static_cast<std::uint16_t>(4662), cache[0].port);
	ASSERT_EQUALS(std::string("10.0.0.1:4662"), cache[0].PeerKey());
	ASSERT_EQUALS(std::string("alice"), cache[0].name);
	ASSERT_EQUALS(static_cast<std::uint32_t>(77), cache[0].client_ecid);
	ASSERT_EQUALS(static_cast<std::uint32_t>(12), cache[0].friend_ecid);
	ASSERT_EQUALS(static_cast<size_t>(2), cache[0].messages.size());
	ASSERT_TRUE(!cache[0].messages[0].outgoing);
	ASSERT_TRUE(cache[0].messages[1].outgoing);
	ASSERT_EQUALS(std::string("hello back"), cache[0].messages[1].text);
	ASSERT_EQUALS(static_cast<std::uint32_t>(2), cursor);
	ASSERT_EQUALS(static_cast<size_t>(1), fresh.size());
	ASSERT_TRUE(closed.empty());
}

TEST(Refresher, ChatMessagesAccumulateAcrossTicks)
{
	// Later replies carry only what is past the cursor, so the walker must
	// carry the earlier messages over rather than replacing them -- otherwise
	// every tick would shrink the transcript to whatever just arrived.
	std::vector<ChatSessionSnapshot> cache;
	std::uint32_t cursor = 0;
	{
		std::vector<ChatSessionSnapshot> fresh;
		std::vector<std::uint64_t> closed;
		CECPacket resp(EC_OP_CHAT_SESSIONS);
		resp.AddTag(CECTag(EC_TAG_CHAT_MSG_ID, static_cast<std::uint32_t>(1)));
		CECTag s = MakeChatSession(kPeerA, "alice");
		AddChatMessage(s, 1, false, 1000, "first");
		resp.AddTag(s);
		ApplyChatSessions(&resp, cache, cursor, fresh, closed);
	}
	{
		std::vector<ChatSessionSnapshot> fresh;
		std::vector<std::uint64_t> closed;
		CECPacket resp(EC_OP_CHAT_SESSIONS);
		resp.AddTag(CECTag(EC_TAG_CHAT_MSG_ID, static_cast<std::uint32_t>(2)));
		CECTag s = MakeChatSession(kPeerA, "alice");
		AddChatMessage(s, 2, true, 1001, "second");
		resp.AddTag(s);
		ApplyChatSessions(&resp, cache, cursor, fresh, closed);
		// Only the genuinely new one is reported for SSE.
		ASSERT_EQUALS(static_cast<size_t>(1), fresh.size());
		ASSERT_EQUALS(static_cast<size_t>(1), fresh[0].messages.size());
		ASSERT_EQUALS(std::string("second"), fresh[0].messages[0].text);
	}
	ASSERT_EQUALS(static_cast<size_t>(1), cache.size());
	ASSERT_EQUALS(static_cast<size_t>(2), cache[0].messages.size());
	ASSERT_EQUALS(std::string("first"), cache[0].messages[0].text);
	ASSERT_EQUALS(std::string("second"), cache[0].messages[1].text);
	ASSERT_EQUALS(static_cast<std::uint32_t>(2), cursor);
}

TEST(Refresher, ChatSessionAbsentFromReplyIsReportedClosed)
{
	// Absence IS the close signal -- there is no expiry tag -- so a session
	// the previous tick held must be dropped AND reported, not merged
	// forward. Merging would resurrect closed conversations forever.
	std::vector<ChatSessionSnapshot> cache;
	std::uint32_t cursor = 0;
	{
		std::vector<ChatSessionSnapshot> fresh;
		std::vector<std::uint64_t> closed;
		CECPacket resp(EC_OP_CHAT_SESSIONS);
		CECTag a = MakeChatSession(kPeerA, "alice");
		AddChatMessage(a, 1, false, 1000, "hi");
		resp.AddTag(a);
		resp.AddTag(MakeChatSession(kPeerB, "bob"));
		ApplyChatSessions(&resp, cache, cursor, fresh, closed);
		ASSERT_EQUALS(static_cast<size_t>(2), cache.size());
	}
	{
		std::vector<ChatSessionSnapshot> fresh;
		std::vector<std::uint64_t> closed;
		CECPacket resp(EC_OP_CHAT_SESSIONS);
		resp.AddTag(MakeChatSession(kPeerB, "bob"));
		ApplyChatSessions(&resp, cache, cursor, fresh, closed);
		ASSERT_EQUALS(static_cast<size_t>(1), cache.size());
		ASSERT_EQUALS(kPeerB, cache[0].gui_id);
		ASSERT_EQUALS(static_cast<size_t>(1), closed.size());
		ASSERT_EQUALS(kPeerA, closed[0]);
	}
}

TEST(Refresher, ChatSessionWithNoNewMessagesIsStillListed)
{
	// A session with nothing past the cursor still encodes, with no message
	// children: that is how a client connecting late learns it exists at all.
	std::vector<ChatSessionSnapshot> cache;
	std::uint32_t cursor = 0;
	std::vector<ChatSessionSnapshot> fresh;
	std::vector<std::uint64_t> closed;

	CECPacket resp(EC_OP_CHAT_SESSIONS);
	resp.AddTag(CECTag(EC_TAG_CHAT_MSG_ID, static_cast<std::uint32_t>(9)));
	resp.AddTag(MakeChatSession(kPeerA, "alice"));

	ApplyChatSessions(&resp, cache, cursor, fresh, closed);

	ASSERT_EQUALS(static_cast<size_t>(1), cache.size());
	ASSERT_TRUE(cache[0].messages.empty());
	// Nothing to emit an SSE frame for.
	ASSERT_TRUE(fresh.empty());
	// The cursor still advances, so ids evicted on the daemon are not
	// requested forever.
	ASSERT_EQUALS(static_cast<std::uint32_t>(9), cursor);
}

TEST(Refresher, ChatSessionKeepsAKnownNameWhenTheReplyOmitsIt)
{
	// The daemon suppresses an unchanged name; letting that blank the cached
	// one would make an established conversation fall back to its ip:port
	// label mid-stream.
	std::vector<ChatSessionSnapshot> cache;
	std::uint32_t cursor = 0;
	{
		std::vector<ChatSessionSnapshot> fresh;
		std::vector<std::uint64_t> closed;
		CECPacket resp(EC_OP_CHAT_SESSIONS);
		resp.AddTag(MakeChatSession(kPeerA, "alice"));
		ApplyChatSessions(&resp, cache, cursor, fresh, closed);
	}
	{
		std::vector<ChatSessionSnapshot> fresh;
		std::vector<std::uint64_t> closed;
		CECPacket resp(EC_OP_CHAT_SESSIONS);
		resp.AddTag(CECTag(EC_TAG_CHAT_SESSION, kPeerA)); // no name child
		ApplyChatSessions(&resp, cache, cursor, fresh, closed);
	}
	ASSERT_EQUALS(std::string("alice"), cache[0].name);
	ASSERT_EQUALS(std::string("alice"), cache[0].DisplayName());
}

TEST(Refresher, ChatSessionWithoutANameFallsBackToAddress)
{
	std::vector<ChatSessionSnapshot> cache;
	std::uint32_t cursor = 0;
	std::vector<ChatSessionSnapshot> fresh;
	std::vector<std::uint64_t> closed;

	CECPacket resp(EC_OP_CHAT_SESSIONS);
	resp.AddTag(CECTag(EC_TAG_CHAT_SESSION, kPeerA));
	ApplyChatSessions(&resp, cache, cursor, fresh, closed);

	// The same string the desktop builds, and the one the SSE payload and
	// the REST list must agree on.
	ASSERT_EQUALS(std::string("IP: 10.0.0.1 Port: 4662"), cache[0].DisplayName());
}

TEST(Refresher, SharedKnownFileDecodesAvailability)
{
	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	FileMap cache;
	RLE_Data enc;

	ArrayOfUInts16 sources;
	sources.push_back(3);
	sources.push_back(0);
	sources.push_back(12);
	sources.push_back(1);

	CECPacket resp(EC_OP_SHARED_FILES);
	{
		CECTag kf(EC_TAG_KNOWNFILE, static_cast<std::uint32_t>(80));
		AddEncodedPartStatus(kf, enc, sources);
		resp.AddTag(kf);
	}

	ApplyGetUpdateToShared(&resp, cache, rle_state);

	auto it = cache.find(80);
	ASSERT_TRUE(it != cache.end());
	const std::vector<std::uint16_t> &got = it->second.shared.decoded_part_sources;
	ASSERT_EQUALS(static_cast<size_t>(4), got.size());
	ASSERT_EQUALS(static_cast<std::uint16_t>(3), got[0]);
	ASSERT_EQUALS(static_cast<std::uint16_t>(0), got[1]);
	ASSERT_EQUALS(static_cast<std::uint16_t>(12), got[2]);
	ASSERT_EQUALS(static_cast<std::uint16_t>(1), got[3]);
}

TEST(Refresher, SharedKnownFileAvailabilityTracksDifferentialFrames)
{
	// Second and later frames are XOR deltas against the previous
	// decoded buffer, so a decoder that is not carried across ticks
	// (or is fed a frame twice) paints garbage rather than merely
	// lagging. Drive two frames through one encoder/decoder pair.
	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	FileMap cache;
	RLE_Data enc;

	ArrayOfUInts16 first;
	first.push_back(1);
	first.push_back(1);
	first.push_back(1);
	{
		CECPacket resp(EC_OP_SHARED_FILES);
		CECTag kf(EC_TAG_KNOWNFILE, static_cast<std::uint32_t>(81));
		AddEncodedPartStatus(kf, enc, first);
		resp.AddTag(kf);
		ApplyGetUpdateToShared(&resp, cache, rle_state);
	}

	ArrayOfUInts16 second;
	second.push_back(1);
	second.push_back(7);
	second.push_back(0);
	{
		CECPacket resp(EC_OP_SHARED_FILES);
		CECTag kf(EC_TAG_KNOWNFILE, static_cast<std::uint32_t>(81));
		AddEncodedPartStatus(kf, enc, second);
		resp.AddTag(kf);
		ApplyGetUpdateToShared(&resp, cache, rle_state);
	}

	const std::vector<std::uint16_t> &got = cache.find(81)->second.shared.decoded_part_sources;
	ASSERT_EQUALS(static_cast<size_t>(3), got.size());
	ASSERT_EQUALS(static_cast<std::uint16_t>(1), got[0]);
	ASSERT_EQUALS(static_cast<std::uint16_t>(7), got[1]);
	ASSERT_EQUALS(static_cast<std::uint16_t>(0), got[2]);
}

TEST(Refresher, SharedWalkerLeavesPartfileAvailabilityToDownloadsWalker)
{
	// A shared partfile reaches this walker as EC_TAG_PARTFILE, and the
	// downloads walker has already consumed that same tag's PART_STATUS
	// on this very response. Decoding it a second time here would apply
	// the XOR delta twice and desync the decoder permanently, so the
	// shared walker must not touch the decoder state for a PARTFILE tag.
	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	FileMap cache;
	RLE_Data enc;

	ArrayOfUInts16 sources;
	sources.push_back(5);
	sources.push_back(5);

	CECPacket resp(EC_OP_SHARED_FILES);
	{
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(82));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_SHARED, static_cast<std::uint8_t>(1)));
		AddEncodedPartStatus(pf, enc, sources);
		resp.AddTag(pf);
	}

	ApplyGetUpdateToShared(&resp, cache, rle_state);

	auto it = cache.find(82);
	ASSERT_TRUE(it != cache.end());
	ASSERT_TRUE(it->second.shared.decoded_part_sources.empty());
	// No decoder state was created for this ECID: the downloads walker
	// owns it.
	ASSERT_TRUE(rle_state.find(82) == rle_state.end());
}

TEST(Refresher, SharedPartfileTransitionsOutClearsSharedRole)
{
	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	// Pre-seed a shared partfile in cache (was sharing on previous
	// ticks). Now the operator paused / stopped it: the next tick
	// emits EC_TAG_PARTFILE_SHARED=false. The walker must clear the
	// is_shared role (and reset the shared sub-block so /shared can't
	// surface stale upload stats). The entry itself stays in the
	// unified map — entity-level eviction is FILE_REMOVED's job.
	FileMap cache;
	{
		FileSnapshot s;
		s.ecid = 70;
		s.hash = "dddd4444dddd4444dddd4444dddd4444";
		s.name = "was-sharing.iso";
		s.is_shared = true;
		s.shared.uploaded_bytes_session = 99; // stale stat to verify the reset
		cache.emplace(70, s);
	}
	CECPacket resp(EC_OP_SHARED_FILES);
	{
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(70));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_SHARED, static_cast<std::uint8_t>(0)));
		resp.AddTag(pf);
	}

	ApplyGetUpdateToShared(&resp, cache, rle_state);

	ASSERT_TRUE(cache.find(70) != cache.end());
	ASSERT_TRUE(!cache.find(70)->second.is_shared);
	// Stale upload stats from the prior sharing period must be cleared
	// so /shared can never re-surface them.
	ASSERT_EQUALS(static_cast<std::uint64_t>(0), cache.find(70)->second.shared.uploaded_bytes_session);
}

TEST(Refresher, SuppressedSharedFlagPreservesCachedPartfile)
{
	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	// CValueMap suppresses the EC_TAG_PARTFILE_SHARED tag when the
	// value matches the previous frame. For a cached partfile that
	// was previously shared, the absence of the flag means "still
	// shared" — the walker must keep it and apply stat deltas.
	FileMap cache;
	{
		FileSnapshot s;
		s.ecid = 80;
		s.hash = "eeee5555eeee5555eeee5555eeee5555";
		s.name = "still-sharing.iso";
		cache.emplace(80, s);
	}
	CECPacket resp(EC_OP_SHARED_FILES);
	// PARTFILE with no EC_TAG_PARTFILE_SHARED child — flag suppressed.
	resp.AddTag(CECTag(EC_TAG_PARTFILE, static_cast<std::uint32_t>(80)));

	ApplyGetUpdateToShared(&resp, cache, rle_state);

	ASSERT_TRUE(cache.find(80) != cache.end());
	ASSERT_EQUALS(std::string("still-sharing.iso"), cache.find(80)->second.name);
}

TEST(Refresher, SuppressedSharedFlagSkipsUnknownPartfile)
{
	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	// Mirror of the previous test: a PARTFILE with the SHARED flag
	// suppressed AND no prior cache entry means "we have no signal
	// that this is shared." Don't insert blindly — wait for the next
	// tick that flips the state to emit the flag.
	FileMap cache;
	CECPacket resp(EC_OP_SHARED_FILES);
	resp.AddTag(CECTag(EC_TAG_PARTFILE, static_cast<std::uint32_t>(90)));

	ApplyGetUpdateToShared(&resp, cache, rle_state);

	ASSERT_TRUE(cache.empty());
}

// ----------------------------------------------------------------------
// New ECID arrives in one tick with identity baked in — the whole
// point of the GET_UPDATE consolidation. INC_UPDATE doesn't hit the
// EC_DETAIL_UPDATE early-return at ECSpecialCoreTags.cpp:244-246, so
// HASH / NAME / SIZE are shipped on first encounter; no second
// roundtrip needed.
// ----------------------------------------------------------------------

TEST(Refresher, NewPartfileInsertedInOneTick)
{
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;

	// Craft a partfile tag with just the ECID. The walker dispatches
	// on tag name, calls TagHashLower + MergePartFileTag — both of
	// which gracefully tolerate an absent child set. After the
	// walker runs, ECID 55 is in the cache with default-init fields.
	// (In production a real CEC_PartFile_Tag at INC_UPDATE always
	// carries the full identity child set; this test pins the bare-
	// minimum insertion path.)
	CECPacket resp(EC_OP_SHARED_FILES);
	resp.AddTag(CECTag(EC_TAG_PARTFILE, static_cast<std::uint32_t>(55)));

	ApplyGetUpdateToDownloads(&resp, cache, rle_state);

	// The new ECID landed — no needed.
	ASSERT_EQUALS(static_cast<size_t>(1), cache.size());
	ASSERT_TRUE(cache.find(55) != cache.end());
	ASSERT_EQUALS(static_cast<std::uint32_t>(55), cache.find(55)->second.ecid);
}

// ----------------------------------------------------------------------
// A partfile that is BOTH downloading and shared carries two independent
// priorities: the download priority (EC_TAG_PARTFILE_PRIO, surfaced on
// /downloads) and the upload priority (EC_TAG_KNOWNFILE_PRIO, surfaced on
// /shared). They live in separate sub-blocks; the shared-walker pass must
// not clobber the download value written by the downloads-walker pass.
// (Regression: a single top-level snapshot `priority` field let the two
// overwrite each other — /downloads reported the upload level.)
// ----------------------------------------------------------------------

TEST(Refresher, BothFilePrioritiesAreIndependent)
{
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;

	// Downloads pass: partfile ECID 77 at download priority PR_HIGH (=2,
	// Constants.h) → "high".
	{
		CECPacket resp(EC_OP_SHARED_FILES);
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(77));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_PRIO, static_cast<std::uint8_t>(2)));
		resp.AddTag(pf);
		ApplyGetUpdateToDownloads(&resp, cache, rle_state);
	}
	ASSERT_TRUE(cache.find(77) != cache.end());
	ASSERT_TRUE(cache.find(77)->second.is_downloading);
	ASSERT_EQUALS(std::string("high"), cache.find(77)->second.download.priority);

	// Shared pass: SAME ECID, shared flag on, upload priority PR_LOW (=0)
	// → "low". Must land in shared.priority and leave download.priority.
	{
		CECPacket resp(EC_OP_SHARED_FILES);
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(77));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_SHARED, static_cast<std::uint8_t>(1)));
		pf.AddTag(CECTag(EC_TAG_KNOWNFILE_PRIO, static_cast<std::uint8_t>(0)));
		resp.AddTag(pf);
		ApplyGetUpdateToShared(&resp, cache, rle_state);
	}

	const auto it = cache.find(77);
	ASSERT_TRUE(it != cache.end());
	ASSERT_TRUE(it->second.is_downloading);
	ASSERT_TRUE(it->second.is_shared);
	ASSERT_EQUALS(std::string("high"), it->second.download.priority); // not clobbered
	ASSERT_EQUALS(std::string("low"), it->second.shared.priority);
}

// ----------------------------------------------------------------------
// A partfile that starts downloading BEFORE it shares. Its upload
// priority (EC_TAG_KNOWNFILE_PRIO) is emitted on early ticks while it is
// still download-only; amuled then CValueMap-suppresses the unchanged
// tag. When the file later flips shared, the shared walker never sees
// the priority tag again. The downloads walker must therefore latch the
// upload priority from the partfile tag, and the share-off reset must
// preserve it, so /shared reports a real level instead of "".
// (Regression: amule-org/amule#384 follow-up — empty shared `priority`.)
// ----------------------------------------------------------------------

TEST(Refresher, SharedPriorityLatchedBeforeSharingSurvivesSuppression)
{
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;

	// Tick 1 — download-only. The partfile tag carries both priorities
	// (download PR_HIGH=2 → "high", upload PR_LOW=0 → "low") and an
	// explicit not-shared flag. Downloads walker runs first, then shared.
	{
		CECPacket resp(EC_OP_SHARED_FILES);
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(78));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_PRIO, static_cast<std::uint8_t>(2)));
		pf.AddTag(CECTag(EC_TAG_KNOWNFILE_PRIO, static_cast<std::uint8_t>(0)));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_SHARED, static_cast<std::uint8_t>(0)));
		resp.AddTag(pf);
		ApplyGetUpdateToDownloads(&resp, cache, rle_state);
		ApplyGetUpdateToShared(&resp, cache, rle_state);
	}
	{
		const auto it = cache.find(78);
		ASSERT_TRUE(it != cache.end());
		ASSERT_TRUE(!it->second.is_shared);
		// Upload priority was latched by the downloads walker and NOT
		// wiped by the share-off reset.
		ASSERT_EQUALS(std::string("low"), it->second.shared.priority);
	}

	// Tick 2 — file flips shared, but the unchanged upload priority is
	// now suppressed (no EC_TAG_KNOWNFILE_PRIO child). Without the latch
	// the shared walker would leave shared.priority empty.
	{
		CECPacket resp(EC_OP_SHARED_FILES);
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(78));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_SHARED, static_cast<std::uint8_t>(1)));
		resp.AddTag(pf);
		ApplyGetUpdateToShared(&resp, cache, rle_state);
	}

	const auto it = cache.find(78);
	ASSERT_TRUE(it != cache.end());
	ASSERT_TRUE(it->second.is_shared);
	ASSERT_EQUALS(std::string("high"), it->second.download.priority);
	ASSERT_EQUALS(std::string("low"), it->second.shared.priority); // not empty
}

// ----------------------------------------------------------------------
// Single-file detail decode (issue #417). The download-detail and
// shared-detail endpoints read these off the same snapshot the walkers
// build, so pin that the new tags land in the right sub-blocks.
// ----------------------------------------------------------------------

TEST(Refresher, DownloadDetailTagsDecodeIntoSnapshot)
{
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	CECPacket resp(EC_OP_SHARED_FILES);
	CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(101));
	pf.AddTag(CECTag(EC_TAG_PARTFILE_LAST_SEEN_COMP, static_cast<std::uint32_t>(1700000000)));
	pf.AddTag(CECTag(EC_TAG_PARTFILE_LAST_RECV, static_cast<std::uint32_t>(1700000123)));
	pf.AddTag(CECTag(EC_TAG_PARTFILE_DOWNLOAD_ACTIVE, static_cast<std::uint32_t>(3600)));
	pf.AddTag(CECTag(EC_TAG_PARTFILE_AVAILABLE_PARTS, static_cast<std::uint16_t>(12)));
	pf.AddTag(CECTag(EC_TAG_PARTFILE_HASHED_PART_COUNT, static_cast<std::uint16_t>(3)));
	pf.AddTag(CECTag(EC_TAG_PARTFILE_LOST_CORRUPTION, static_cast<std::uint64_t>(9728000)));
	pf.AddTag(CECTag(EC_TAG_PARTFILE_GAINED_COMPRESSION, static_cast<std::uint64_t>(4096)));
	pf.AddTag(CECTag(EC_TAG_PARTFILE_SAVED_ICH, static_cast<std::uint32_t>(7)));
	// Still sent by the daemon, deliberately not decoded any more: partmet_id
	// was dropped from the surface, and an undecoded tag must not disturb the
	// rest of the parse.
	pf.AddTag(CECTag(EC_TAG_PARTFILE_PARTMETID, static_cast<std::uint32_t>(42)));
	// Base CKnownFile tags carried on the partfile tag too.
	pf.AddTag(CECTag(EC_TAG_KNOWNFILE_ON_QUEUE, static_cast<std::uint32_t>(5)));
	pf.AddTag(CECTag(EC_TAG_KNOWNFILE_AICH_MASTERHASH, std::string("ABCDEF0123")));
	pf.AddTag(CECTag(EC_TAG_KNOWNFILE_FILENAME, std::string("042.part")));
	pf.AddTag(CECTag(EC_TAG_KNOWNFILE_PATH, std::string("/home/me/.aMule/Temp")));
	resp.AddTag(pf);

	ApplyGetUpdateToDownloads(&resp, cache, rle_state);

	const auto it = cache.find(101);
	ASSERT_TRUE(it != cache.end());
	const auto &d = it->second;
	ASSERT_EQUALS(static_cast<std::uint32_t>(1700000000), d.download.last_seen_complete_at);
	ASSERT_EQUALS(static_cast<std::uint32_t>(1700000123), d.download.last_received_at);
	ASSERT_EQUALS(static_cast<std::uint32_t>(3600), d.download.active_seconds);
	ASSERT_EQUALS(static_cast<std::uint16_t>(12), d.download.parts_available_count);
	ASSERT_EQUALS(static_cast<std::uint16_t>(3), d.download.hashed_part_count);
	ASSERT_EQUALS(static_cast<std::uint64_t>(9728000), d.download.lost_to_corruption_bytes);
	ASSERT_EQUALS(static_cast<std::uint64_t>(4096), d.download.gained_by_compression_bytes);
	ASSERT_EQUALS(static_cast<std::uint32_t>(7), d.download.ich_recovered_packet_count);
	ASSERT_EQUALS(static_cast<std::uint32_t>(5), d.queued_count);
	ASSERT_EQUALS(std::string("ABCDEF0123"), d.aich_hash);
	ASSERT_EQUALS(std::string("042.part"), d.part_met_basename);
	ASSERT_EQUALS(std::string("/home/me/.aMule/Temp"), d.on_disk_dir);
}

TEST(Refresher, SharedDetailTagsDecodeIntoSnapshot)
{
	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	FileMap cache;
	CECPacket resp(EC_OP_SHARED_FILES);
	CECTag kf(EC_TAG_KNOWNFILE, static_cast<std::uint32_t>(202));
	kf.AddTag(CECTag(EC_TAG_KNOWNFILE_COMPLETE_SOURCES, static_cast<std::uint16_t>(8)));
	kf.AddTag(CECTag(EC_TAG_KNOWNFILE_COMPLETE_SOURCES_LOW, static_cast<std::uint16_t>(5)));
	kf.AddTag(CECTag(EC_TAG_KNOWNFILE_COMPLETE_SOURCES_HIGH, static_cast<std::uint16_t>(11)));
	kf.AddTag(CECTag(EC_TAG_KNOWNFILE_ON_QUEUE, static_cast<std::uint32_t>(9)));
	kf.AddTag(CECTag(EC_TAG_KNOWNFILE_AICH_MASTERHASH, std::string("FEDCBA9876")));
	kf.AddTag(CECTag(EC_TAG_KNOWNFILE_FILENAME, std::string("/home/me/Incoming")));
	kf.AddTag(CECTag(EC_TAG_KNOWNFILE_PATH, std::string("/home/me/Incoming")));
	// Live upload activity (issue #466).
	kf.AddTag(CECTag(EC_TAG_KNOWNFILE_UPLOAD_SPEED, static_cast<std::uint32_t>(51200)));
	kf.AddTag(CECTag(EC_TAG_KNOWNFILE_UPLOADING_COUNT, static_cast<std::uint16_t>(3)));
	kf.AddTag(CECTag(EC_TAG_KNOWNFILE_LAST_UPLOAD, static_cast<std::uint32_t>(1700000500)));
	kf.AddTag(CECTag(EC_TAG_KNOWNFILE_SHARED_SINCE, static_cast<std::uint32_t>(1699000000)));
	// Verify Local Data / AICH rebuild progress over a complete share.
	kf.AddTag(CECTag(EC_TAG_KNOWNFILE_HASHED_PART_COUNT, static_cast<std::uint16_t>(4)));
	resp.AddTag(kf);

	ApplyGetUpdateToShared(&resp, cache, rle_state);

	const auto it = cache.find(202);
	ASSERT_TRUE(it != cache.end());
	const auto &s = it->second;
	ASSERT_EQUALS(static_cast<std::uint16_t>(5), s.shared.complete_sources_low);
	ASSERT_EQUALS(static_cast<std::uint16_t>(11), s.shared.complete_sources_high);
	ASSERT_EQUALS(static_cast<std::uint32_t>(9), s.queued_count);
	ASSERT_EQUALS(std::string("FEDCBA9876"), s.aich_hash);
	// Completed known file → the directory path arrives on its own tag
	// (the write layer reports it verbatim, with `incomplete` alongside).
	ASSERT_EQUALS(std::string("/home/me/Incoming"), s.on_disk_dir);
	// Upload activity (issue #466) decodes into the shared sub-block.
	ASSERT_EQUALS(static_cast<std::uint32_t>(51200), s.shared.upload_speed_bytes_per_second);
	ASSERT_EQUALS(static_cast<std::uint16_t>(3), s.shared.uploading_client_count);
	ASSERT_EQUALS(static_cast<std::uint32_t>(1700000500), s.shared.last_upload);
	ASSERT_EQUALS(static_cast<std::uint16_t>(4), s.shared.hashing_progress);
	ASSERT_EQUALS(static_cast<std::uint32_t>(1699000000), s.shared.shared_since);
}

// Comment/rating (issue #419): the user's own comment+rating land at the
// top level; the per-source EC_TAG_PARTFILE_COMMENTS container decodes
// into download.source_comments (4 index-grouped children per source,
// rating -1 = unrated).
TEST(Refresher, CommentRatingAndSourceCommentsDecode)
{
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	CECPacket resp(EC_OP_SHARED_FILES);
	CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(303));
	pf.AddTag(CECTag(EC_TAG_KNOWNFILE_COMMENT, std::string("my own note")));
	pf.AddTag(CECTag(EC_TAG_KNOWNFILE_RATING, static_cast<std::uint32_t>(4)));
	CECEmptyTag comments(EC_TAG_PARTFILE_COMMENTS);
	comments.AddTag(CECTag(EC_TAG_PARTFILE_COMMENTS, std::string("alice")));
	comments.AddTag(CECTag(EC_TAG_PARTFILE_COMMENTS, std::string("movie.mkv")));
	comments.AddTag(CECTag(EC_TAG_PARTFILE_COMMENTS, static_cast<std::uint64_t>(5)));
	comments.AddTag(CECTag(EC_TAG_PARTFILE_COMMENTS, std::string("great quality")));
	comments.AddTag(CECTag(EC_TAG_PARTFILE_COMMENTS, std::string("bob")));
	comments.AddTag(CECTag(EC_TAG_PARTFILE_COMMENTS, std::string("film.avi")));
	comments.AddTag(CECTag(EC_TAG_PARTFILE_COMMENTS, static_cast<std::uint64_t>(-1))); // unrated
	comments.AddTag(CECTag(EC_TAG_PARTFILE_COMMENTS, std::string("no rating here")));
	pf.AddTag(comments);
	resp.AddTag(pf);

	ApplyGetUpdateToDownloads(&resp, cache, rle_state);

	const auto it = cache.find(303);
	ASSERT_TRUE(it != cache.end());
	ASSERT_EQUALS(std::string("my own note"), it->second.comment);
	ASSERT_EQUALS(4, static_cast<int>(it->second.rating));
	ASSERT_EQUALS(static_cast<size_t>(2), it->second.download.source_comments.size());
	ASSERT_EQUALS(std::string("alice"), it->second.download.source_comments[0].username);
	ASSERT_EQUALS(std::string("movie.mkv"), it->second.download.source_comments[0].filename);
	ASSERT_EQUALS(5, static_cast<int>(it->second.download.source_comments[0].rating));
	ASSERT_EQUALS(-1, static_cast<int>(it->second.download.source_comments[1].rating));
	ASSERT_EQUALS(std::string("no rating here"), it->second.download.source_comments[1].comment);
}

// Source-reported filenames (issue #420) are delta-encoded by amuled and
// accumulated across ticks: tick 1 adds two names; tick 2 removes one
// (COUNTS=0) and updates the other's count (COUNTS-only child).
TEST(Refresher, SourceNamesDeltaAccumulate)
{
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;

	{
		CECPacket resp(EC_OP_SHARED_FILES);
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(404));
		CECEmptyTag names(EC_TAG_PARTFILE_SOURCE_NAMES);
		CECTag c1(EC_TAG_PARTFILE_SOURCE_NAMES, static_cast<std::uint32_t>(1));
		c1.AddTag(CECTag(EC_TAG_PARTFILE_SOURCE_NAMES, std::string("Movie.mkv")));
		c1.AddTag(CECTag(EC_TAG_PARTFILE_SOURCE_NAMES_COUNTS, static_cast<std::uint32_t>(7)));
		names.AddTag(c1);
		CECTag c2(EC_TAG_PARTFILE_SOURCE_NAMES, static_cast<std::uint32_t>(2));
		c2.AddTag(CECTag(EC_TAG_PARTFILE_SOURCE_NAMES, std::string("movie.avi")));
		c2.AddTag(CECTag(EC_TAG_PARTFILE_SOURCE_NAMES_COUNTS, static_cast<std::uint32_t>(2)));
		names.AddTag(c2);
		pf.AddTag(names);
		resp.AddTag(pf);
		ApplyGetUpdateToDownloads(&resp, cache, rle_state);
	}
	{
		const auto it = cache.find(404);
		ASSERT_TRUE(it != cache.end());
		ASSERT_EQUALS(static_cast<size_t>(2), it->second.download.source_names.size());
		ASSERT_EQUALS(std::string("Movie.mkv"), it->second.download.source_names[1].name);
		ASSERT_EQUALS(static_cast<std::uint32_t>(7), it->second.download.source_names[1].count);
	}

	// Tick 2: remove id 2 (count 0), bump id 1's count to 9 (no name).
	{
		CECPacket resp(EC_OP_SHARED_FILES);
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(404));
		CECEmptyTag names(EC_TAG_PARTFILE_SOURCE_NAMES);
		CECTag rem(EC_TAG_PARTFILE_SOURCE_NAMES, static_cast<std::uint32_t>(2));
		rem.AddTag(CECTag(EC_TAG_PARTFILE_SOURCE_NAMES_COUNTS, static_cast<std::uint32_t>(0)));
		names.AddTag(rem);
		CECTag upd(EC_TAG_PARTFILE_SOURCE_NAMES, static_cast<std::uint32_t>(1));
		upd.AddTag(CECTag(EC_TAG_PARTFILE_SOURCE_NAMES_COUNTS, static_cast<std::uint32_t>(9)));
		names.AddTag(upd);
		pf.AddTag(names);
		resp.AddTag(pf);
		ApplyGetUpdateToDownloads(&resp, cache, rle_state);
	}
	const auto it = cache.find(404);
	ASSERT_TRUE(it != cache.end());
	ASSERT_EQUALS(static_cast<size_t>(1), it->second.download.source_names.size());
	ASSERT_EQUALS(static_cast<std::uint32_t>(9), it->second.download.source_names[1].count);
	ASSERT_TRUE(it->second.download.source_names.find(2) == it->second.download.source_names.end());
}

// A4AF (issue #421): the auto flag decodes into download.a4af_auto and
// the EC_TAG_PARTFILE_A4AF_SOURCES container's EC_TAG_ECID children into
// the a4af_sources list (full replace when present).
TEST(Refresher, A4afAutoAndSourcesDecode)
{
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	CECPacket resp(EC_OP_SHARED_FILES);
	CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(505));
	pf.AddTag(CECTag(EC_TAG_PARTFILE_A4AFAUTO, true));
	CECEmptyTag a4af(EC_TAG_PARTFILE_A4AF_SOURCES);
	a4af.AddTag(CECTag(EC_TAG_ECID, static_cast<std::uint32_t>(1234)));
	a4af.AddTag(CECTag(EC_TAG_ECID, static_cast<std::uint32_t>(5678)));
	pf.AddTag(a4af);
	resp.AddTag(pf);

	ApplyGetUpdateToDownloads(&resp, cache, rle_state);

	const auto it = cache.find(505);
	ASSERT_TRUE(it != cache.end());
	ASSERT_TRUE(it->second.download.a4af_auto);
	ASSERT_EQUALS(static_cast<size_t>(2), it->second.download.a4af_sources.size());
	ASSERT_EQUALS(static_cast<std::uint32_t>(1234), it->second.download.a4af_sources[0]);
	ASSERT_EQUALS(static_cast<std::uint32_t>(5678), it->second.download.a4af_sources[1]);
}

// Media metadata (issue #418): the six FT_MEDIA_* EC tags decode into the
// `media` sub-struct and set `has_media`; a file with no media tags keeps
// has_media=false (so the API omits the `media` object).
TEST(Refresher, MediaMetadataDecode)
{
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	CECPacket resp(EC_OP_SHARED_FILES);
	CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(606));
	pf.AddTag(CECTag(EC_TAG_KNOWNFILE_MEDIA_LENGTH, static_cast<std::uint32_t>(5400)));
	pf.AddTag(CECTag(EC_TAG_KNOWNFILE_MEDIA_BITRATE, static_cast<std::uint32_t>(1500)));
	pf.AddTag(CECTag(EC_TAG_KNOWNFILE_MEDIA_CODEC, std::string("h264")));
	pf.AddTag(CECTag(EC_TAG_KNOWNFILE_MEDIA_ARTIST, std::string("Some Artist")));
	pf.AddTag(CECTag(EC_TAG_KNOWNFILE_MEDIA_ALBUM, std::string("Some Album")));
	pf.AddTag(CECTag(EC_TAG_KNOWNFILE_MEDIA_TITLE, std::string("Some Title")));
	resp.AddTag(pf);
	// A second file with no media tags stays has_media=false.
	resp.AddTag(CECTag(EC_TAG_PARTFILE, static_cast<std::uint32_t>(607)));

	ApplyGetUpdateToDownloads(&resp, cache, rle_state);

	const auto it = cache.find(606);
	ASSERT_TRUE(it != cache.end());
	ASSERT_TRUE(it->second.has_media);
	ASSERT_EQUALS(static_cast<std::uint32_t>(5400), it->second.media.duration_seconds);
	ASSERT_EQUALS(static_cast<std::uint32_t>(1500), it->second.media.bitrate_kilobits_per_second);
	ASSERT_EQUALS(std::string("h264"), it->second.media.codec);
	ASSERT_EQUALS(std::string("Some Artist"), it->second.media.artist);
	ASSERT_EQUALS(std::string("Some Album"), it->second.media.album);
	ASSERT_EQUALS(std::string("Some Title"), it->second.media.title);

	const auto it2 = cache.find(607);
	ASSERT_TRUE(it2 != cache.end());
	ASSERT_TRUE(!it2->second.has_media);
}

// The clear half of the same contract. amuled sends a zero / empty value for
// a field it previously sent a real one for -- a tag that is simply not
// offered reads as UNCHANGED to a CValueMap peer, so an omitted field would
// leave this snapshot serving the stale value, including one inherited
// unverified from a search result. That is exactly what the completion
// re-probe exists to correct, and it regressed once already.
TEST(Refresher, MediaMetadataClearRemovesFieldsAndRecomputesHasMedia)
{
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	{
		CECPacket resp(EC_OP_SHARED_FILES);
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(700));
		pf.AddTag(CECTag(EC_TAG_KNOWNFILE_MEDIA_LENGTH, static_cast<std::uint32_t>(180)));
		pf.AddTag(CECTag(EC_TAG_KNOWNFILE_MEDIA_BITRATE, static_cast<std::uint32_t>(1500)));
		pf.AddTag(CECTag(EC_TAG_KNOWNFILE_MEDIA_CODEC, std::string("h264")));
		pf.AddTag(CECTag(EC_TAG_KNOWNFILE_MEDIA_ARTIST, std::string("Peer Supplied")));
		resp.AddTag(pf);
		ApplyGetUpdateToDownloads(&resp, cache, rle_state);
	}
	ASSERT_TRUE(cache.find(700)->second.has_media);
	ASSERT_EQUALS(std::string("Peer Supplied"), cache.find(700)->second.media.artist);

	// The local probe found a codec and nothing else, so amuled clears the
	// three fields it could not determine and keeps the one it could.
	{
		CECPacket resp(EC_OP_SHARED_FILES);
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(700));
		pf.AddTag(CECTag(EC_TAG_KNOWNFILE_MEDIA_LENGTH, static_cast<std::uint32_t>(0)));
		pf.AddTag(CECTag(EC_TAG_KNOWNFILE_MEDIA_BITRATE, static_cast<std::uint32_t>(0)));
		pf.AddTag(CECTag(EC_TAG_KNOWNFILE_MEDIA_ARTIST, std::string()));
		resp.AddTag(pf);
		ApplyGetUpdateToDownloads(&resp, cache, rle_state);
	}
	const auto it = cache.find(700);
	ASSERT_TRUE(it != cache.end());
	ASSERT_EQUALS(static_cast<std::uint32_t>(0), it->second.media.duration_seconds);
	ASSERT_EQUALS(static_cast<std::uint32_t>(0), it->second.media.bitrate_kilobits_per_second);
	ASSERT_TRUE(it->second.media.artist.empty());
	// The codec was not mentioned this time, which means UNCHANGED -- not
	// cleared. Distinguishing those two is the whole point of the design.
	ASSERT_EQUALS(std::string("h264"), it->second.media.codec);
	// Still has media, because one field survives.
	ASSERT_TRUE(it->second.has_media);
}

TEST(Refresher, MediaMetadataClearingEveryFieldDropsHasMedia)
{
	// has_media is DERIVED, not latched: a file whose every field has been
	// cleared must stop reporting media, or the API keeps emitting an empty
	// `media` object for it.
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	{
		CECPacket resp(EC_OP_SHARED_FILES);
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(701));
		pf.AddTag(CECTag(EC_TAG_KNOWNFILE_MEDIA_LENGTH, static_cast<std::uint32_t>(180)));
		pf.AddTag(CECTag(EC_TAG_KNOWNFILE_MEDIA_CODEC, std::string("mp3")));
		resp.AddTag(pf);
		ApplyGetUpdateToDownloads(&resp, cache, rle_state);
	}
	ASSERT_TRUE(cache.find(701)->second.has_media);
	{
		CECPacket resp(EC_OP_SHARED_FILES);
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(701));
		pf.AddTag(CECTag(EC_TAG_KNOWNFILE_MEDIA_LENGTH, static_cast<std::uint32_t>(0)));
		pf.AddTag(CECTag(EC_TAG_KNOWNFILE_MEDIA_CODEC, std::string()));
		resp.AddTag(pf);
		ApplyGetUpdateToDownloads(&resp, cache, rle_state);
	}
	ASSERT_TRUE(!cache.find(701)->second.has_media);
}

// ----------------------------------------------------------------------
// /servers — GET_UPDATE wraps per-server tags in an EC_TAG_SERVER
// container at top level. Walker iterates INTO the container and
// merges per-ECID; cache entries not seen in the response get evicted
// because the server side has no FILE_REMOVED equivalent for servers
// (the container always carries the full current list).
// ----------------------------------------------------------------------

TEST(Refresher, ServersFromContainerMergesByEcid)
{
	std::map<std::uint32_t, ServerSnapshot> cache;
	// Pre-seed an entry that should disappear: a server the operator
	// removed from amuled between ticks (it won't show up in the new
	// response's SERVER container).
	{
		ServerSnapshot s;
		s.ecid = 9999;
		s.name = "removed";
		cache.emplace(9999, s);
	}

	// Build a SERVER container with one per-server child (ECID 42).
	CECPacket resp(EC_OP_SHARED_FILES);
	{
		CECTag container(EC_TAG_SERVER, static_cast<std::uint32_t>(0));
		// One per-server child tag inside the container — same
		// EC_TAG_SERVER name (the walker disambiguates by depth, not
		// by name). Minimum tags needed for the merge to populate
		// the snapshot.
		CECTag srv(EC_TAG_SERVER, static_cast<std::uint32_t>(42));
		srv.AddTag(CECTag(EC_TAG_SERVER_USERS, static_cast<std::uint32_t>(1234)));
		// #440 server host country ISO code resolved daemon-side.
		srv.AddTag(CECTag(EC_TAG_SERVER_COUNTRY, wxString::FromUTF8("de")));
		container.AddTag(srv);
		resp.AddTag(container);
	}

	ApplyGetUpdateToServers(&resp, cache);

	ASSERT_EQUALS(static_cast<size_t>(1), cache.size());
	ASSERT_TRUE(cache.find(42) != cache.end());
	ASSERT_TRUE(cache.find(9999) == cache.end()); // evicted
	ASSERT_EQUALS(static_cast<std::uint32_t>(1234), cache[42].users);
	ASSERT_EQUALS(std::string("de"), cache[42].country_code);
}

// The four fields issue #974 added: publishing limits + capability
// bitmasks. They ride the same GET_UPDATE response the rest of the
// server snapshot comes from, so the only thing to prove is that
// MergeServerTag reads them and that a tag CValueMap suppressed on an
// unchanged tick does not clobber the cached value.
TEST(Refresher, ServerLimitsAndFlagsReachTheSnapshot)
{
	std::map<std::uint32_t, ServerSnapshot> cache;

	CECPacket resp(EC_OP_SHARED_FILES);
	{
		CECTag container(EC_TAG_SERVER, static_cast<std::uint32_t>(0));
		CECTag srv(EC_TAG_SERVER, static_cast<std::uint32_t>(7));
		srv.AddTag(CECTag(EC_TAG_SERVER_FILES_SOFT, static_cast<std::uint32_t>(1000)));
		srv.AddTag(CECTag(EC_TAG_SERVER_FILES_HARD, static_cast<std::uint32_t>(5000)));
		srv.AddTag(CECTag(EC_TAG_SERVER_TCP_FLAGS,
			static_cast<std::uint32_t>(SRV_TCPFLG_COMPRESSION | SRV_TCPFLG_RELATEDSEARCH)));
		srv.AddTag(CECTag(EC_TAG_SERVER_UDP_FLAGS,
			static_cast<std::uint32_t>(SRV_UDPFLG_EXT_GETSOURCES | SRV_UDPFLG_LARGEFILES)));
		container.AddTag(srv);
		resp.AddTag(container);
	}

	ApplyGetUpdateToServers(&resp, cache);

	ASSERT_TRUE(cache.find(7) != cache.end());
	ASSERT_EQUALS(static_cast<std::uint32_t>(1000), cache[7].soft_file_limit);
	ASSERT_EQUALS(static_cast<std::uint32_t>(5000), cache[7].hard_file_limit);
	ASSERT_EQUALS(static_cast<std::uint32_t>(SRV_TCPFLG_COMPRESSION | SRV_TCPFLG_RELATEDSEARCH),
		cache[7].tcp_flags);
	ASSERT_EQUALS(static_cast<std::uint32_t>(SRV_UDPFLG_EXT_GETSOURCES | SRV_UDPFLG_LARGEFILES),
		cache[7].udp_flags);

	// Next tick: the server is still in the list but nothing it
	// announced changed, so CValueMap suppresses all four tags. The
	// cached values have to survive -- dropping to 0 here would read
	// as "the server stopped supporting everything".
	CECPacket quiet(EC_OP_SHARED_FILES);
	{
		CECTag container(EC_TAG_SERVER, static_cast<std::uint32_t>(0));
		CECTag srv(EC_TAG_SERVER, static_cast<std::uint32_t>(7));
		srv.AddTag(CECTag(EC_TAG_SERVER_USERS, static_cast<std::uint32_t>(42)));
		container.AddTag(srv);
		quiet.AddTag(container);
	}

	ApplyGetUpdateToServers(&quiet, cache);

	ASSERT_EQUALS(static_cast<std::uint32_t>(42), cache[7].users);
	ASSERT_EQUALS(static_cast<std::uint32_t>(1000), cache[7].soft_file_limit);
	ASSERT_EQUALS(static_cast<std::uint32_t>(5000), cache[7].hard_file_limit);
	ASSERT_EQUALS(static_cast<std::uint32_t>(SRV_TCPFLG_COMPRESSION | SRV_TCPFLG_RELATEDSEARCH),
		cache[7].tcp_flags);
	ASSERT_EQUALS(static_cast<std::uint32_t>(SRV_UDPFLG_EXT_GETSOURCES | SRV_UDPFLG_LARGEFILES),
		cache[7].udp_flags);
}

// A server old enough not to send the tags at all leaves the defaults
// in place: 0 / 0 / no bits, which the API documents as "not reported".
TEST(Refresher, ServerWithoutLimitOrFlagTagsKeepsZeroDefaults)
{
	std::map<std::uint32_t, ServerSnapshot> cache;

	CECPacket resp(EC_OP_SHARED_FILES);
	{
		CECTag container(EC_TAG_SERVER, static_cast<std::uint32_t>(0));
		CECTag srv(EC_TAG_SERVER, static_cast<std::uint32_t>(3));
		srv.AddTag(CECTag(EC_TAG_SERVER_USERS, static_cast<std::uint32_t>(9)));
		container.AddTag(srv);
		resp.AddTag(container);
	}

	ApplyGetUpdateToServers(&resp, cache);

	ASSERT_TRUE(cache.find(3) != cache.end());
	ASSERT_EQUALS(static_cast<std::uint32_t>(0), cache[3].soft_file_limit);
	ASSERT_EQUALS(static_cast<std::uint32_t>(0), cache[3].hard_file_limit);
	ASSERT_EQUALS(static_cast<std::uint32_t>(0), cache[3].tcp_flags);
	ASSERT_EQUALS(static_cast<std::uint32_t>(0), cache[3].udp_flags);
}

TEST(Refresher, ServersEmptyContainerEmptiesCache)
{
	std::map<std::uint32_t, ServerSnapshot> cache;
	cache.emplace(1, ServerSnapshot{});
	cache.emplace(2, ServerSnapshot{});

	// Empty SERVER container (operator removed every server). Every
	// pre-seeded entry is "not seen this tick" → evicted.
	CECPacket resp(EC_OP_SHARED_FILES);
	resp.AddTag(CECTag(EC_TAG_SERVER, static_cast<std::uint32_t>(0)));

	ApplyGetUpdateToServers(&resp, cache);

	ASSERT_TRUE(cache.empty());
}

TEST(Refresher, ServersNoContainerLeavesCacheAlone)
{
	// Defensive: if a response is missing the SERVER container
	// entirely (which production amuled never does — it always
	// emits the container even when empty), the walker leaves the
	// cache untouched. Better than wiping on an unexpected wire
	// shape.
	std::map<std::uint32_t, ServerSnapshot> cache;
	cache.emplace(7, ServerSnapshot{});

	CECPacket resp(EC_OP_SHARED_FILES);
	// No EC_TAG_SERVER container in the response.

	ApplyGetUpdateToServers(&resp, cache);

	ASSERT_EQUALS(static_cast<size_t>(1), cache.size());
	ASSERT_TRUE(cache.find(7) != cache.end());
}

// ----------------------------------------------------------------------
// Friends — same container shape as servers: full list every tick, per-field
// CValueMap suppression, eviction by "not seen".
// ----------------------------------------------------------------------

TEST(Refresher, FriendsFromContainerMergesByEcid)
{
	std::map<std::uint32_t, FriendSnapshot> cache;
	{
		// A friend removed on the daemon side between ticks.
		FriendSnapshot f;
		f.ecid = 9999;
		f.name = "removed";
		cache.emplace(9999, f);
	}

	CECPacket resp(EC_OP_SHARED_FILES);
	{
		CECTag container(EC_TAG_FRIEND, static_cast<std::uint32_t>(0));
		CECTag fr(EC_TAG_FRIEND, static_cast<std::uint32_t>(12));
		fr.AddTag(CECTag(EC_TAG_FRIEND_NAME, wxString::FromUTF8("alice")));
		fr.AddTag(CECTag(EC_TAG_FRIEND_IP, static_cast<std::uint32_t>(0x2A7100CB)));
		fr.AddTag(CECTag(EC_TAG_FRIEND_PORT, static_cast<std::uint16_t>(4662)));
		fr.AddTag(CECTag(EC_TAG_FRIEND_CLIENT, static_cast<std::uint32_t>(4382)));
		fr.AddTag(CECTag(EC_TAG_FRIEND_FRIENDSLOT, true));
		container.AddTag(fr);
		resp.AddTag(container);
	}

	ApplyGetUpdateToFriends(&resp, cache);

	ASSERT_EQUALS(static_cast<size_t>(1), cache.size());
	ASSERT_TRUE(cache.find(12) != cache.end());
	ASSERT_TRUE(cache.find(9999) == cache.end()); // evicted
	ASSERT_EQUALS(std::string("alice"), cache[12].name);
	ASSERT_EQUALS(std::string("203.0.113.42"), cache[12].ip);
	ASSERT_EQUALS(static_cast<std::uint16_t>(4662), cache[12].port);
	ASSERT_EQUALS(static_cast<std::uint32_t>(4382), cache[12].client_ecid);
	ASSERT_TRUE(cache[12].friend_slot);
}

TEST(Refresher, FriendsSuppressedFieldsKeepCachedValues)
{
	// CValueMap omits unchanged fields, so a tick carrying only the changed
	// one must not blank the rest. The friend goes offline (CLIENT -> 0) and
	// nothing else is sent.
	std::map<std::uint32_t, FriendSnapshot> cache;
	{
		FriendSnapshot f;
		f.ecid = 12;
		f.name = "alice";
		f.ip = "203.0.113.42";
		f.port = 4662;
		f.client_ecid = 4382;
		f.friend_slot = true;
		cache.emplace(12, f);
	}

	CECPacket resp(EC_OP_SHARED_FILES);
	{
		CECTag container(EC_TAG_FRIEND, static_cast<std::uint32_t>(0));
		CECTag fr(EC_TAG_FRIEND, static_cast<std::uint32_t>(12));
		fr.AddTag(CECTag(EC_TAG_FRIEND_CLIENT, static_cast<std::uint32_t>(0)));
		container.AddTag(fr);
		resp.AddTag(container);
	}

	ApplyGetUpdateToFriends(&resp, cache);

	ASSERT_EQUALS(static_cast<size_t>(1), cache.size());
	ASSERT_EQUALS(static_cast<std::uint32_t>(0), cache[12].client_ecid); // went offline
	ASSERT_EQUALS(std::string("alice"), cache[12].name);                 // preserved
	ASSERT_EQUALS(std::string("203.0.113.42"), cache[12].ip);            // preserved
	ASSERT_TRUE(cache[12].friend_slot);                                  // preserved
}

TEST(Refresher, FriendsWithoutSlotTagReadFalse)
{
	// An older daemon does not serialize EC_TAG_FRIEND_FRIENDSLOT at all.
	// The snapshot must degrade to false rather than carrying junk.
	std::map<std::uint32_t, FriendSnapshot> cache;

	CECPacket resp(EC_OP_SHARED_FILES);
	{
		CECTag container(EC_TAG_FRIEND, static_cast<std::uint32_t>(0));
		CECTag fr(EC_TAG_FRIEND, static_cast<std::uint32_t>(5));
		fr.AddTag(CECTag(EC_TAG_FRIEND_NAME, wxString::FromUTF8("bob")));
		container.AddTag(fr);
		resp.AddTag(container);
	}

	ApplyGetUpdateToFriends(&resp, cache);

	ASSERT_TRUE(cache.find(5) != cache.end());
	ASSERT_TRUE(!cache[5].friend_slot);
	ASSERT_EQUALS(std::string(""), cache[5].ip); // zero IP renders empty
}

TEST(Refresher, FriendsNoContainerLeavesCacheAlone)
{
	std::map<std::uint32_t, FriendSnapshot> cache;
	cache.emplace(7, FriendSnapshot{});

	CECPacket resp(EC_OP_SHARED_FILES);
	// No EC_TAG_FRIEND container at all.

	ApplyGetUpdateToFriends(&resp, cache);

	ASSERT_EQUALS(static_cast<size_t>(1), cache.size());
	ASSERT_TRUE(cache.find(7) != cache.end());
}

// ----------------------------------------------------------------------
// RLE state map — cleaned up alongside the cache when a partfile
// gets evicted via FILE_REMOVED. Without the cleanup, the decoder's
// internal buffer (~200 KB per partfile on TB-class files) would
// slowly leak.
// ----------------------------------------------------------------------

TEST(Refresher, RleStateErasedAlongsideFileRemoved)
{
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	{
		FileSnapshot d;
		d.ecid = 77;
		d.hash = "aaaa0000aaaa0000aaaa0000aaaa0000";
		d.name = "doomed.iso";
		cache.emplace(77, d);
		// Simulate a previous tick having allocated a decoder for ECID 77.
		rle_state.emplace(77, PartFileEncoderData{});
	}

	CECPacket resp(EC_OP_SHARED_FILES);
	resp.AddTag(CECTag(EC_TAG_FILE_REMOVED, static_cast<std::uint32_t>(77)));

	ApplyGetUpdateToDownloads(&resp, cache, rle_state);

	ASSERT_TRUE(cache.find(77) == cache.end());
	ASSERT_TRUE(rle_state.find(77) == rle_state.end());
}

TEST(Refresher, RleStatePreservedForKnownEntryAcrossTick)
{
	// A partfile already in cache should KEEP its RLE state across a
	// tick that brings no new info. The decoder relies on its buffer
	// surviving from the prior tick.
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;
	{
		FileSnapshot d;
		d.ecid = 5;
		d.hash = "bbbb1111bbbb1111bbbb1111bbbb1111";
		d.name = "stable.iso";
		cache.emplace(5, d);
		rle_state.emplace(5, PartFileEncoderData{});
	}

	// A no-op response (no PARTFILE tags, no FILE_REMOVED). Nothing
	// should churn.
	CECPacket resp(EC_OP_SHARED_FILES);
	ApplyGetUpdateToDownloads(&resp, cache, rle_state);

	ASSERT_TRUE(cache.find(5) != cache.end());
	ASSERT_TRUE(rle_state.find(5) != rle_state.end());
}

// ----------------------------------------------------------------------
// /stats/tree — recursive walk strips the root container and surfaces
// its children at the top level. Crafted as a hand-built CECTag tree.
// ----------------------------------------------------------------------

TEST(Refresher, StatusDecodeCompleteOverridesStopped)
{
	// A completed download in amuled sits in `m_completedDownloads`
	// with EC_TAG_PARTFILE_STOPPED set true. The decoder used to
	// short-circuit on `stopped` and report "paused" — masking the
	// PS_COMPLETE state from /downloads consumers (and breaking the
	// status=="completed" filter). PS_COMPLETE (and
	// PS_COMPLETING) must take priority over the stopped flag.
	//
	// PS_COMPLETE = 9 (Constants.h). Crafting a partfile tag with
	// PS_STATUS=9 + STOPPED=true exercises the merge path through
	// ApplyGetUpdateToDownloads.
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;

	CECPacket resp(EC_OP_SHARED_FILES);
	{
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(101));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_STATUS, static_cast<std::uint8_t>(9 /* PS_COMPLETE */)));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_STOPPED, true));
		resp.AddTag(pf);
	}

	ApplyGetUpdateToDownloads(&resp, cache, rle_state);

	ASSERT_TRUE(cache.find(101) != cache.end());
	ASSERT_EQUALS(std::string("completed"), cache.find(101)->second.download.status);
}

TEST(Refresher, StatusDecodeCompletingOverridesStopped)
{
	// Same shape, PS_COMPLETING (=8) takes priority over stopped too
	// — covers the in-flight finalization race where the cache is
	// being moved from m_filelist to m_completedDownloads.
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;

	CECPacket resp(EC_OP_SHARED_FILES);
	{
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(102));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_STATUS, static_cast<std::uint8_t>(8 /* PS_COMPLETING */)));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_STOPPED, true));
		resp.AddTag(pf);
	}

	ApplyGetUpdateToDownloads(&resp, cache, rle_state);
	ASSERT_TRUE(cache.find(102) != cache.end());
	ASSERT_EQUALS(std::string("completing"), cache.find(102)->second.download.status);
}

TEST(Refresher, StatusDecodeStoppedNonCompleteReportsStopped)
{
	// A download that's stopped but NOT yet completed (user hit Stop
	// mid-transfer) surfaces as the distinct wire status "stopped":
	// stop = pause + drop all sources + reset the Kad source search,
	// and clients need to tell it apart from a plain "paused" (which
	// keeps its sources). PS_COMPLETE / PS_COMPLETING still take
	// priority over the stopped flag — see the two tests above.
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;

	CECPacket resp(EC_OP_SHARED_FILES);
	{
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(103));
		// PS_READY = 0 (transferring). User stopped it.
		pf.AddTag(CECTag(EC_TAG_PARTFILE_STATUS, static_cast<std::uint8_t>(0)));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_STOPPED, true));
		resp.AddTag(pf);
	}

	ApplyGetUpdateToDownloads(&resp, cache, rle_state);
	ASSERT_TRUE(cache.find(103) != cache.end());
	ASSERT_EQUALS(std::string("stopped"), cache.find(103)->second.download.status);
}

TEST(Refresher, StatusDecodePausedNotStoppedReportsPaused)
{
	// The complement: a paused file that is NOT stopped (PS_PAUSED with
	// the stopped flag clear) keeps its sources and reports "paused",
	// distinct from the "stopped" state above. Pins that the "stopped"
	// wire status is gated on EC_TAG_PARTFILE_STOPPED, not on PS_PAUSED.
	FileMap cache;
	std::map<std::uint32_t, PartFileEncoderData> rle_state;

	CECPacket resp(EC_OP_SHARED_FILES);
	{
		CECTag pf(EC_TAG_PARTFILE, static_cast<std::uint32_t>(104));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_STATUS, static_cast<std::uint8_t>(7 /* PS_PAUSED */)));
		pf.AddTag(CECTag(EC_TAG_PARTFILE_STOPPED, false));
		resp.AddTag(pf);
	}

	ApplyGetUpdateToDownloads(&resp, cache, rle_state);
	ASSERT_TRUE(cache.find(104) != cache.end());
	ASSERT_EQUALS(std::string("paused"), cache.find(104)->second.download.status);
}

TEST(Refresher, ParseStatsTreeStripsRootAndRecursesChildren)
{
	// Build:
	//  root
	//  ├── Transfer
	//  │   └── Total bytes ...
	//  └── Connection
	CECPacket resp(EC_OP_STATSTREE);
	CECTag root(EC_TAG_STATTREE_NODE, wxString("root-container-label-discarded"));
	{
		CECTag transfer(EC_TAG_STATTREE_NODE, wxString("Transfer"));
		// Nodes may carry a stable machine key; Connection below omits it
		// to exercise the "no key" path.
		transfer.AddTag(CECTag(EC_TAG_STAT_NODE_KEY, wxString("transfer")));
		{
			CECTag total(EC_TAG_STATTREE_NODE, wxString("Total bytes transferred: 12.3 GiB"));
			// Raw numeric ratios ride along as distinctly-named double tags.
			total.AddTag(CECTag(EC_TAG_STAT_NODE_RATIO, static_cast<double>(2.5)));
			total.AddTag(CECTag(EC_TAG_STAT_NODE_RATIO_TOTAL, static_cast<double>(3.5)));
			transfer.AddTag(total);
		}
		root.AddTag(transfer);
	}
	{
		CECTag conn(EC_TAG_STATTREE_NODE, wxString("Connection"));
		root.AddTag(conn);
	}
	resp.AddTag(root);

	StatsTreeNode out;
	ParseStatsTreeFromPacket(&resp, out);

	// The root container itself is discarded; we expose its 2 children
	// (Transfer + Connection) as top-level nodes.
	ASSERT_TRUE(out.label.empty());
	ASSERT_EQUALS(static_cast<size_t>(2), out.children.size());
	// Transfer subtree.
	ASSERT_EQUALS(std::string("Transfer"), out.children[0].label);
	// Stable machine key is parsed when present...
	ASSERT_EQUALS(std::string("transfer"), out.children[0].key);
	ASSERT_EQUALS(static_cast<size_t>(1), out.children[0].children.size());
	ASSERT_EQUALS(std::string("Total bytes transferred: 12.3 GiB"), out.children[0].children[0].label);
	// ...and empty when the node omits the tag (Transfer's child + Connection).
	ASSERT_TRUE(out.children[0].children[0].key.empty());
	// Raw numeric ratios are parsed from the distinctly-named double tags.
	ASSERT_TRUE(out.children[0].children[0].has_ratio_session);
	ASSERT_TRUE(out.children[0].children[0].ratio_session > 2.49 &&
		    out.children[0].children[0].ratio_session < 2.51);
	ASSERT_TRUE(out.children[0].children[0].has_ratio_total);
	ASSERT_TRUE(out.children[0].children[0].ratio_total > 3.49 &&
		    out.children[0].children[0].ratio_total < 3.51);
	// Nodes without the ratio tags report neither.
	ASSERT_TRUE(!out.children[0].has_ratio_session);
	ASSERT_TRUE(!out.children[0].has_ratio_total);
	// Connection is a leaf at this depth.
	ASSERT_EQUALS(std::string("Connection"), out.children[1].label);
	ASSERT_TRUE(out.children[1].key.empty());
	ASSERT_EQUALS(static_cast<size_t>(0), out.children[1].children.size());
}

// ----------------------------------------------------------------------
// AdvanceSearchProgress — maps EC_TAG_SEARCH_LIFECYCLE_STATE +
// EC_TAG_SEARCH_LIFECYCLE_PERCENT into (percent, complete, active).
// Trusts the daemon's flags; the percent is the daemon's unified 0..100
// for every kind (global = real, Kad = cosmetic ramp), so amuleapi no
// longer masks it per-kind — it just passes it through and clamps.
// ----------------------------------------------------------------------

namespace
{

webapi::SearchProgressSnapshot MakeActive(const std::string &kind)
{
	webapi::SearchProgressSnapshot s;
	s.active = true;
	s.kind = kind;
	return s;
}

constexpr std::uint32_t LIFECYCLE_IDLE = 0;
constexpr std::uint32_t LIFECYCLE_RUNNING = 1;
constexpr std::uint32_t LIFECYCLE_FINISHED = 2;

} // namespace

// Search result status + type (issue #429): EC_TAG_PARTFILE_STATUS decodes
// to the lowercase status string, and `type` is derived from the filename.
// The union reply is not per-search: every result carries EC_TAG_SEARCH_ID the
// first time the daemon mentions it, and the applier attributes later diffed
// tags through the ECID -> search_id index. These fixtures therefore stamp one
// search id on every result tag and pre-create its slot, which is what the
// refresher does via MarkSearchStarted before any poll runs.
static constexpr std::uint32_t kSid = 4242;

TEST(Refresher, SearchResultStatusAndTypeDecode)
{
	std::map<std::uint32_t, SearchSlot> slots;
	std::map<std::uint32_t, std::uint32_t> owner;
	slots[kSid];
	std::map<std::uint32_t, SearchResult> &cache = slots[kSid].results;
	CECPacket resp(EC_OP_SEARCH_RESULTS);
	CECTag sf(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(70));
	sf.AddTag(CECTag(EC_TAG_SEARCH_ID, kSid));
	sf.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("cool.movie.mkv")));
	sf.AddTag(CECTag(EC_TAG_PARTFILE_SIZE_FULL, static_cast<std::uint64_t>(123)));
	sf.AddTag(CECTag(EC_TAG_PARTFILE_STATUS, static_cast<std::uint32_t>(2))); // QUEUED
	resp.AddTag(sf);
	// A second result with no status tag defaults to "new"; a .mp3 → audio.
	CECTag sf2(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(71));
	sf2.AddTag(CECTag(EC_TAG_SEARCH_ID, kSid));
	sf2.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("song.mp3")));
	sf2.AddTag(CECTag(EC_TAG_PARTFILE_SIZE_FULL, static_cast<std::uint64_t>(4)));
	resp.AddTag(sf2);

	ApplySearchUnion(&resp, slots, owner);

	const auto it = cache.find(70);
	ASSERT_TRUE(it != cache.end());
	ASSERT_EQUALS(std::string("queued"), it->second.status);
	// Normalised from GetFiletypeByName's UI label: singular, snake_case,
	// and "unknown" rather than "any". Same token set as the shared-detail
	// file_type, which shares this helper.
	ASSERT_EQUALS(std::string("video"), it->second.type);

	const auto it2 = cache.find(71);
	ASSERT_TRUE(it2 != cache.end());
	ASSERT_EQUALS(std::string("new"), it2->second.status);
	ASSERT_EQUALS(std::string("audio"), it2->second.type);
}

TEST(Refresher, SearchProgressRunningCarriesPercentForGlobal)
{
	using webapi::AdvanceSearchProgress;
	webapi::SearchProgressSnapshot s = MakeActive("global");
	s = AdvanceSearchProgress(s, LIFECYCLE_RUNNING, /*pct=*/42);
	ASSERT_TRUE(s.active);
	ASSERT_TRUE(!s.complete);
	ASSERT_EQUALS(static_cast<uint32_t>(42), s.percent);
}

TEST(Refresher, SearchProgressRunningPassesThroughKadRamp)
{
	using webapi::AdvanceSearchProgress;
	webapi::SearchProgressSnapshot s = MakeActive("kad");
	// The daemon synthesises a cosmetic time-ramp for Kad and ships it in
	// EC_TAG_SEARCH_LIFECYCLE_PERCENT, so amuleapi no longer masks Kad to
	// 0 — it passes the daemon value straight through.
	s = AdvanceSearchProgress(s, LIFECYCLE_RUNNING, /*pct=*/37);
	ASSERT_TRUE(s.active);
	ASSERT_EQUALS(static_cast<uint32_t>(37), s.percent);
}

TEST(Refresher, SearchProgressRunningClampsPercentAbove100)
{
	using webapi::AdvanceSearchProgress;
	webapi::SearchProgressSnapshot s = MakeActive("global");
	// The daemon's percent tag is 0..100, but stay defensive: any value
	// above 100 is clamped rather than surfaced raw to consumers.
	s = AdvanceSearchProgress(s, LIFECYCLE_RUNNING, /*pct=*/250);
	ASSERT_TRUE(s.active);
	ASSERT_EQUALS(static_cast<uint32_t>(100), s.percent);
}

TEST(Refresher, SearchProgressFinishedSetsComplete)
{
	using webapi::AdvanceSearchProgress;
	webapi::SearchProgressSnapshot s = MakeActive("global");
	s = AdvanceSearchProgress(s, LIFECYCLE_FINISHED, /*pct=*/0);
	ASSERT_TRUE(!s.active);
	ASSERT_TRUE(s.complete);
	ASSERT_EQUALS(static_cast<uint32_t>(100), s.percent);
}

TEST(Refresher, SearchProgressIdleZeroesOutGracefully)
{
	using webapi::AdvanceSearchProgress;
	webapi::SearchProgressSnapshot s = MakeActive("kad");
	// Refresher shouldn't call us with state=IDLE (it gates on active
	// being true on entry), but stay defensive: flip both flags off.
	s = AdvanceSearchProgress(s, LIFECYCLE_IDLE, /*pct=*/0);
	ASSERT_TRUE(!s.active);
	ASSERT_TRUE(!s.complete);
	ASSERT_EQUALS(static_cast<uint32_t>(0), s.percent);
}

// Search result media metadata (issue #430): the EC_TAG_KNOWNFILE_MEDIA_*
// tags (present only for hits known/probed locally) decode into the
// SearchResult media sub-struct and set has_media; a hit with none stays
// has_media=false so the API omits the `media` object.
TEST(Refresher, SearchResultMediaDecode)
{
	std::map<std::uint32_t, SearchSlot> slots;
	std::map<std::uint32_t, std::uint32_t> owner;
	slots[kSid];
	std::map<std::uint32_t, SearchResult> &cache = slots[kSid].results;
	CECPacket resp(EC_OP_SEARCH_RESULTS);
	CECTag sf(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(80));
	sf.AddTag(CECTag(EC_TAG_SEARCH_ID, kSid));
	sf.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("show.s01e01.mkv")));
	sf.AddTag(CECTag(EC_TAG_PARTFILE_SIZE_FULL, static_cast<std::uint64_t>(999)));
	sf.AddTag(CECTag(EC_TAG_KNOWNFILE_MEDIA_LENGTH, static_cast<std::uint32_t>(1320)));
	sf.AddTag(CECTag(EC_TAG_KNOWNFILE_MEDIA_BITRATE, static_cast<std::uint32_t>(2500)));
	sf.AddTag(CECTag(EC_TAG_KNOWNFILE_MEDIA_CODEC, std::string("h264")));
	resp.AddTag(sf);
	// A second hit with no media tags stays has_media=false.
	CECTag sf2(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(81));
	sf2.AddTag(CECTag(EC_TAG_SEARCH_ID, kSid));
	sf2.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("nomedia.bin")));
	sf2.AddTag(CECTag(EC_TAG_PARTFILE_SIZE_FULL, static_cast<std::uint64_t>(4)));
	resp.AddTag(sf2);

	ApplySearchUnion(&resp, slots, owner);

	const auto it = cache.find(80);
	ASSERT_TRUE(it != cache.end());
	ASSERT_TRUE(it->second.has_media);
	ASSERT_EQUALS(static_cast<std::uint32_t>(1320), it->second.media.duration_seconds);
	ASSERT_EQUALS(static_cast<std::uint32_t>(2500), it->second.media.bitrate_kilobits_per_second);
	ASSERT_EQUALS(std::string("h264"), it->second.media.codec);

	const auto it2 = cache.find(81);
	ASSERT_TRUE(it2 != cache.end());
	ASSERT_TRUE(!it2->second.has_media);
}

// --- #431: result grouping folds same-hash/diff-name children --------
//
// A parent plus two children (each carrying EC_TAG_SEARCH_PARENT) must
// collapse to a single top-level result with the two alternative names
// nested in children[]; the child ECIDs must not remain top-level.
TEST(Refresher, SearchResultGroupingFoldsChildren)
{
	std::map<std::uint32_t, SearchSlot> slots;
	std::map<std::uint32_t, std::uint32_t> owner;
	slots[kSid];
	std::map<std::uint32_t, SearchResult> &cache = slots[kSid].results;
	CECPacket resp(EC_OP_SEARCH_RESULTS);

	CECTag parent(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(100));
	parent.AddTag(CECTag(EC_TAG_SEARCH_ID, kSid));
	parent.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("best.mkv")));
	parent.AddTag(CECTag(EC_TAG_PARTFILE_SIZE_FULL, static_cast<std::uint64_t>(123)));
	parent.AddTag(CECTag(EC_TAG_PARTFILE_SOURCE_COUNT, static_cast<std::uint32_t>(30)));
	resp.AddTag(parent);

	CECTag c1(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(101));
	c1.AddTag(CECTag(EC_TAG_SEARCH_ID, kSid));
	c1.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("alt.name.mkv")));
	c1.AddTag(CECTag(EC_TAG_PARTFILE_SIZE_FULL, static_cast<std::uint64_t>(123)));
	c1.AddTag(CECTag(EC_TAG_PARTFILE_SOURCE_COUNT, static_cast<std::uint32_t>(10)));
	c1.AddTag(CECTag(EC_TAG_SEARCH_PARENT, static_cast<std::uint32_t>(100)));
	resp.AddTag(c1);

	CECTag c2(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(102));
	c2.AddTag(CECTag(EC_TAG_SEARCH_ID, kSid));
	c2.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("third.mkv")));
	c2.AddTag(CECTag(EC_TAG_PARTFILE_SIZE_FULL, static_cast<std::uint64_t>(123)));
	c2.AddTag(CECTag(EC_TAG_SEARCH_PARENT, static_cast<std::uint32_t>(100)));
	resp.AddTag(c2);

	ApplySearchUnion(&resp, slots, owner);

	ASSERT_EQUALS(static_cast<size_t>(1), cache.size());
	const auto it = cache.find(100);
	ASSERT_TRUE(it != cache.end());
	ASSERT_TRUE(cache.find(101) == cache.end());
	ASSERT_TRUE(cache.find(102) == cache.end());
	ASSERT_EQUALS(static_cast<size_t>(2), it->second.children.size());
	// map iterates ecid-ascending, so 101 folds before 102.
	ASSERT_EQUALS(std::string("alt.name.mkv"), it->second.children[0].name);
	ASSERT_EQUALS(static_cast<std::uint32_t>(101), it->second.children[0].ecid);
	ASSERT_EQUALS(static_cast<std::uint32_t>(10), it->second.children[0].source_count);
	ASSERT_EQUALS(std::string("third.mkv"), it->second.children[1].name);
}

// --- Browse results carry the folder they live in ---------------------
//
// EC_TAG_SEARCHFILE_DIRECTORY is attached by the core only to results
// filed from a peer's shared-file listing, which is what makes it the
// browse-only "Directories" column. It is per-RESULT, not per-search: two
// copies of one file in different folders of the same share group under a
// single parent and each must keep its own folder.
TEST(Refresher, SearchResultDirectoryDecodesAndRidesEachChild)
{
	std::map<std::uint32_t, SearchSlot> slots;
	std::map<std::uint32_t, std::uint32_t> owner;
	slots[kSid];
	std::map<std::uint32_t, SearchResult> &cache = slots[kSid].results;
	CECPacket resp(EC_OP_SEARCH_RESULTS);

	CECTag parent(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(200));
	parent.AddTag(CECTag(EC_TAG_SEARCH_ID, kSid));
	parent.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("shared.iso")));
	parent.AddTag(CECTag(EC_TAG_PARTFILE_SIZE_FULL, static_cast<std::uint64_t>(4096)));
	parent.AddTag(CECTag(EC_TAG_SEARCHFILE_DIRECTORY, std::string("Incoming/ISOs")));
	resp.AddTag(parent);

	CECTag child(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(201));
	child.AddTag(CECTag(EC_TAG_SEARCH_ID, kSid));
	child.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("shared-copy.iso")));
	child.AddTag(CECTag(EC_TAG_PARTFILE_SIZE_FULL, static_cast<std::uint64_t>(4096)));
	child.AddTag(CECTag(EC_TAG_SEARCH_PARENT, static_cast<std::uint32_t>(200)));
	child.AddTag(CECTag(EC_TAG_SEARCHFILE_DIRECTORY, std::string("Backup/ISOs")));
	resp.AddTag(child);

	ApplySearchUnion(&resp, slots, owner);

	const auto it = cache.find(200);
	ASSERT_TRUE(it != cache.end());
	ASSERT_EQUALS(std::string("Incoming/ISOs"), it->second.directory);
	ASSERT_EQUALS(static_cast<size_t>(1), it->second.children.size());
	// The child keeps ITS folder rather than inheriting the parent's.
	ASSERT_EQUALS(std::string("Backup/ISOs"), it->second.children[0].directory);
}

TEST(Refresher, SearchResultDirectoryEmptyOnOrdinaryHit)
{
	// A server/Kad hit never carries the tag, and must report an empty
	// string rather than anything that could read as a real folder.
	std::map<std::uint32_t, SearchSlot> slots;
	std::map<std::uint32_t, std::uint32_t> owner;
	slots[kSid];
	std::map<std::uint32_t, SearchResult> &cache = slots[kSid].results;
	CECPacket resp(EC_OP_SEARCH_RESULTS);
	CECTag hit(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(300));
	hit.AddTag(CECTag(EC_TAG_SEARCH_ID, kSid));
	hit.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("remote.iso")));
	resp.AddTag(hit);

	ApplySearchUnion(&resp, slots, owner);

	const auto it = cache.find(300);
	ASSERT_TRUE(it != cache.end());
	ASSERT_TRUE(it->second.directory.empty());
}

// --- #359: peer software_version must be locale-independent ----------
//
// The daemon formats the version string with gettext, so an unidentified
// client yields _("Unknown") -- "Desconocido" on a Spanish daemon. amuleapi
// is a separate process and can't reverse the translation, so the refresher
// keys off the numeric software code (locale-independent) and emits the
// lowercase "unknown" sentinel instead of the translated string.

// Build a one-client GET_UPDATE response: an EC_TAG_CLIENT container with a
// single child client carrying the software code and (optionally) a version
// string. Mirrors the amuled-side EC_TAG_CLIENT shape.
static void PutOneClient(CECPacket &resp,
	std::uint32_t ecid,
	std::uint32_t soft,
	const char *ver_str /* nullptr = omit the version tag */)
{
	CECTag container(EC_TAG_CLIENT, static_cast<std::uint32_t>(0));
	CECTag cli(EC_TAG_CLIENT, ecid);
	cli.AddTag(CECTag(EC_TAG_CLIENT_SOFTWARE, soft));
	if (ver_str)
		cli.AddTag(CECTag(EC_TAG_CLIENT_SOFT_VER_STR, wxString::FromUTF8(ver_str)));
	container.AddTag(cli);
	resp.AddTag(container);
}

// Per-part bitmaps (issue #984). Three wire conventions have to survive the
// decoder: an EMPTY tag means "holds every part", the buffer is LSB-first
// within each byte, and an absent tag means unchanged rather than zero.
static void PutClientWithPartStatus(CECPacket &resp,
	std::uint32_t ecid,
	const std::uint8_t *buf /* nullptr = empty tag */,
	std::size_t len)
{
	CECTag container(EC_TAG_CLIENT, static_cast<std::uint32_t>(0));
	CECTag cli(EC_TAG_CLIENT, ecid);
	if (buf) {
		cli.AddTag(CECTag(EC_TAG_CLIENT_PART_STATUS, len, buf));
	} else {
		cli.AddTag(CECEmptyTag(EC_TAG_CLIENT_PART_STATUS));
	}
	container.AddTag(cli);
	resp.AddTag(container);
}

TEST(Refresher, ClientEmptyPartStatusMeansFullSource)
{
	std::map<std::uint32_t, ClientSnapshot> cache;
	const FileMap no_files;
	CECPacket resp(EC_OP_SHARED_FILES);
	PutClientWithPartStatus(resp, 11, nullptr, 0);

	ApplyGetUpdateToClients(&resp, cache, no_files);

	ASSERT_TRUE(cache.find(11) != cache.end());
	ASSERT_TRUE(cache[11].has_part_status);
	// "all" rather than an empty bitmap: the true length is the file's part
	// count, which only the renderer knows.
	ASSERT_TRUE(cache[11].part_status_all);
	ASSERT_TRUE(cache[11].part_status.empty());
}

TEST(Refresher, ClientPartStatusIsDecodedLsbFirst)
{
	std::map<std::uint32_t, ClientSnapshot> cache;
	const FileMap no_files;
	CECPacket resp(EC_OP_SHARED_FILES);
	// 0x05 = bits 0 and 2 set, LSB-first (BitVector::s_posMask).
	const std::uint8_t buf[] = { 0x05 };
	PutClientWithPartStatus(resp, 12, buf, sizeof(buf));

	ApplyGetUpdateToClients(&resp, cache, no_files);

	ASSERT_TRUE(cache.find(12) != cache.end());
	ASSERT_TRUE(cache[12].has_part_status);
	ASSERT_TRUE(!cache[12].part_status_all);
	ASSERT_EQUALS(static_cast<size_t>(8), cache[12].part_status.size());
	ASSERT_TRUE(cache[12].part_status[0]);
	ASSERT_TRUE(!cache[12].part_status[1]);
	ASSERT_TRUE(cache[12].part_status[2]);
	ASSERT_TRUE(!cache[12].part_status[7]);
}

// Same helper plus a REQUEST_FILE ECID, for the swap tests below. The core
// always sends that tag when the request file changes (AddDiffTag), and sends
// it as a literal 0 when the peer stops requesting anything
// (ECSpecialCoreTags.cpp:443).
static void PutClientWithReqFileAndPartStatus(CECPacket &resp,
	std::uint32_t ecid,
	std::uint32_t req_file_ecid,
	const std::uint8_t *buf /* nullptr = no part-status tag at all */,
	std::size_t len)
{
	CECTag container(EC_TAG_CLIENT, static_cast<std::uint32_t>(0));
	CECTag cli(EC_TAG_CLIENT, ecid);
	cli.AddTag(CECTag(EC_TAG_CLIENT_REQUEST_FILE, req_file_ecid));
	if (buf) {
		cli.AddTag(CECTag(EC_TAG_CLIENT_PART_STATUS, len, buf));
	}
	container.AddTag(cli);
	resp.AddTag(container);
}

// ApplyGetUpdateToClients resolves a peer's ECID against the live file map,
// so the fixtures need FileMap entries rather than a bare ecid->hash map.
static FileMap FilesWithHashes(std::initializer_list<std::pair<std::uint32_t, const char *>> rows)
{
	FileMap m;
	for (const auto &row : rows) {
		FileSnapshot f;
		f.ecid = row.first;
		f.hash = row.second;
		m.emplace(row.first, std::move(f));
	}
	return m;
}

TEST(Refresher, ClientPartStatusIsDroppedWhenTheRequestFileChanges)
{
	// An A4AF swap. CUpDownClient::SetReqFile clears m_downPartStatus without
	// repopulating it, and the core then sends no PART_STATUS until the peer
	// answers for the new file -- so a bitmap kept across the change would
	// describe the OLD file and report the peer as holding parts of a file it
	// has never sent a status for.
	std::map<std::uint32_t, ClientSnapshot> cache;
	const FileMap files = FilesWithHashes(
		{ { 70, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" }, { 71, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" } });

	const std::uint8_t buf[] = { 0x05 };
	CECPacket first(EC_OP_SHARED_FILES);
	PutClientWithReqFileAndPartStatus(first, 20, 70, buf, sizeof(buf));
	ApplyGetUpdateToClients(&first, cache, files);
	ASSERT_TRUE(cache[20].has_part_status);
	ASSERT_EQUALS(std::string("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), cache[20].download_file_hash);

	// Second tick: same peer, different file, and no PART_STATUS with it.
	CECPacket second(EC_OP_SHARED_FILES);
	PutClientWithReqFileAndPartStatus(second, 20, 71, nullptr, 0);
	ApplyGetUpdateToClients(&second, cache, files);

	ASSERT_EQUALS(std::string("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"), cache[20].download_file_hash);
	ASSERT_TRUE(!cache[20].has_part_status);
	ASSERT_TRUE(!cache[20].part_status_all);
	ASSERT_TRUE(cache[20].part_status.empty());
}

TEST(Refresher, ClientPartStatusInTheSameTickSurvivesTheFileChange)
{
	// The invalidation must not eat a bitmap the same packet carries: the
	// hash is merged before the PART_STATUS decode, so a peer that swaps and
	// reports its new status in one tick keeps the new status.
	std::map<std::uint32_t, ClientSnapshot> cache;
	const FileMap files = FilesWithHashes(
		{ { 70, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" }, { 71, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" } });

	const std::uint8_t old_buf[] = { 0x05 };
	CECPacket first(EC_OP_SHARED_FILES);
	PutClientWithReqFileAndPartStatus(first, 21, 70, old_buf, sizeof(old_buf));
	ApplyGetUpdateToClients(&first, cache, files);

	const std::uint8_t new_buf[] = { 0x02 };
	CECPacket second(EC_OP_SHARED_FILES);
	PutClientWithReqFileAndPartStatus(second, 21, 71, new_buf, sizeof(new_buf));
	ApplyGetUpdateToClients(&second, cache, files);

	ASSERT_EQUALS(std::string("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"), cache[21].download_file_hash);
	ASSERT_TRUE(cache[21].has_part_status);
	ASSERT_TRUE(!cache[21].part_status[0]);
	ASSERT_TRUE(cache[21].part_status[1]);
}

TEST(Refresher, ClientRequestFileZeroClearsTheDownloadHash)
{
	// The core spells "no request file" as the tag carrying 0, not as an
	// absent tag. Reading that as "unchanged" left a finished peer listed as
	// a source of the file it had just completed, bitmap and all.
	std::map<std::uint32_t, ClientSnapshot> cache;
	const FileMap files = FilesWithHashes({ { 70, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" } });

	const std::uint8_t buf[] = { 0x05 };
	CECPacket first(EC_OP_SHARED_FILES);
	PutClientWithReqFileAndPartStatus(first, 22, 70, buf, sizeof(buf));
	ApplyGetUpdateToClients(&first, cache, files);
	ASSERT_EQUALS(std::string("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), cache[22].download_file_hash);

	CECPacket second(EC_OP_SHARED_FILES);
	PutClientWithReqFileAndPartStatus(second, 22, 0, nullptr, 0);
	ApplyGetUpdateToClients(&second, cache, files);

	ASSERT_TRUE(cache[22].download_file_hash.empty());
	ASSERT_TRUE(!cache[22].has_part_status);
	ASSERT_TRUE(cache[22].part_status.empty());
}

TEST(Refresher, ClientAbsentPartStatusLeavesCachedBitmap)
{
	// Tagmap convention: a tick that does not carry the tag says nothing
	// about it, so a previously decoded bitmap must survive.
	std::map<std::uint32_t, ClientSnapshot> cache;
	{
		ClientSnapshot c;
		c.ecid = 13;
		c.part_status = { true, false, true };
		c.has_part_status = true;
		cache.emplace(13, c);
	}
	const FileMap no_files;
	CECPacket resp(EC_OP_SHARED_FILES);
	PutOneClient(resp, 13, static_cast<std::uint32_t>(SO_AMULE), nullptr);

	ApplyGetUpdateToClients(&resp, cache, no_files);

	ASSERT_TRUE(cache[13].has_part_status);
	ASSERT_EQUALS(static_cast<size_t>(3), cache[13].part_status.size());
	ASSERT_TRUE(cache[13].part_status[0]);
	ASSERT_TRUE(cache[13].part_status[2]);
}

TEST(Refresher, ClientVersionUnknownYieldsLocaleIndependentSentinel)
{
	std::map<std::uint32_t, ClientSnapshot> cache;
	const FileMap no_files;
	CECPacket resp(EC_OP_SHARED_FILES);
	// Unidentified client + a translated version string, as a non-English
	// daemon would ship it. Must NOT leak into the API response.
	PutOneClient(resp, 7, static_cast<std::uint32_t>(SO_UNKNOWN), "Desconocido");
	ApplyGetUpdateToClients(&resp, cache, no_files);
	ASSERT_TRUE(cache.find(7) != cache.end());
	ASSERT_EQUALS(std::string("unknown"), cache[7].software_version);
}

TEST(Refresher, ClientVersionKnownStringPassesThrough)
{
	std::map<std::uint32_t, ClientSnapshot> cache;
	const FileMap no_files;
	CECPacket resp(EC_OP_SHARED_FILES);
	PutOneClient(resp, 8, static_cast<std::uint32_t>(SO_AMULE), "aMule 2.3.3");
	ApplyGetUpdateToClients(&resp, cache, no_files);
	ASSERT_TRUE(cache.find(8) != cache.end());
	ASSERT_EQUALS(std::string("aMule 2.3.3"), cache[8].software_version);
}

TEST(Refresher, ClientKnownButNoVersionStringFallsBackToSentinel)
{
	std::map<std::uint32_t, ClientSnapshot> cache;
	const FileMap no_files;
	CECPacket resp(EC_OP_SHARED_FILES);
	// Known software, but the daemon shipped no version string at all.
	PutOneClient(resp, 9, static_cast<std::uint32_t>(SO_EMULE), nullptr);
	ApplyGetUpdateToClients(&resp, cache, no_files);
	ASSERT_TRUE(cache.find(9) != cache.end());
	ASSERT_EQUALS(std::string("unknown"), cache[9].software_version);
}

// --- #422: detail-only client fields decode --------------------------
//
// The section-B fields ride the INC_UPDATE client tag and are captured
// into ClientSnapshot by MergeClientTag; the detail endpoint serializes
// them. Verify each decodes, HighID/LowID derives from the hybrid id,
// and EC_TAG_CLIENT_FROM maps to the stable origin token.
TEST(Refresher, ClientDetailFieldsDecode)
{
	std::map<std::uint32_t, ClientSnapshot> cache;
	const FileMap no_files;
	CECPacket resp(EC_OP_SHARED_FILES);
	CECTag container(EC_TAG_CLIENT, static_cast<std::uint32_t>(0));

	// A HighID peer carrying the full detail-only set.
	CECTag hi(EC_TAG_CLIENT, static_cast<std::uint32_t>(50));
	hi.AddTag(CECTag(EC_TAG_CLIENT_USER_ID, static_cast<std::uint32_t>(0x04030201)));
	// Host-order IPv4; FormatClientIpv4 renders LSB-first => 127.0.0.1.
	hi.AddTag(CECTag(EC_TAG_CLIENT_SERVER_IP, static_cast<std::uint32_t>(0x0100007F)));
	hi.AddTag(CECTag(EC_TAG_CLIENT_SERVER_PORT, static_cast<std::uint16_t>(4242)));
	hi.AddTag(CECTag(EC_TAG_CLIENT_SERVER_NAME, wxString::FromUTF8("test-server")));
	hi.AddTag(CECTag(EC_TAG_CLIENT_KAD_PORT, static_cast<std::uint16_t>(4672)));
	hi.AddTag(CECTag(EC_TAG_CLIENT_FROM, static_cast<std::uint64_t>(3))); // SF_KADEMLIA
	hi.AddTag(CECTag(EC_TAG_PARTFILE_NAME, wxString::FromUTF8("upload.iso")));
	hi.AddTag(CECTag(EC_TAG_CLIENT_AVAILABLE_PARTS, static_cast<std::uint32_t>(7)));
	hi.AddTag(CECTag(EC_TAG_CLIENT_MOD_VERSION, wxString::FromUTF8("mod-x")));
	hi.AddTag(CECTag(EC_TAG_CLIENT_DISABLE_VIEW_SHARED, true));
	// #423 friend status + DL/UP modifier.
	hi.AddTag(CECTag(EC_TAG_CLIENT_IS_FRIEND, true));
	hi.AddTag(CECTag(EC_TAG_CLIENT_SCORE_RATIO, static_cast<double>(2.5)));
	// #439 peer country ISO code resolved daemon-side.
	hi.AddTag(CECTag(EC_TAG_CLIENT_COUNTRY, wxString::FromUTF8("de")));
	container.AddTag(hi);

	// A LowID peer (hybrid id < 0x1000000) with no section-B tags.
	CECTag lo(EC_TAG_CLIENT, static_cast<std::uint32_t>(51));
	lo.AddTag(CECTag(EC_TAG_CLIENT_USER_ID, static_cast<std::uint32_t>(1234)));
	container.AddTag(lo);

	resp.AddTag(container);
	ApplyGetUpdateToClients(&resp, cache, no_files);

	const auto it = cache.find(50);
	ASSERT_TRUE(it != cache.end());
	const ClientSnapshot &cs = it->second;
	ASSERT_EQUALS(static_cast<std::uint32_t>(0x04030201), cs.ed2k_user_id);
	ASSERT_TRUE(cs.high_id);
	ASSERT_EQUALS(std::string("127.0.0.1"), cs.server_ip);
	ASSERT_EQUALS(static_cast<std::uint16_t>(4242), cs.server_port);
	ASSERT_EQUALS(std::string("test-server"), cs.server_name);
	ASSERT_EQUALS(std::string("de"), cs.country_code);
	ASSERT_EQUALS(static_cast<std::uint16_t>(4672), cs.kad_port);
	ASSERT_EQUALS(std::string("kad"), cs.source_origin);
	ASSERT_EQUALS(std::string("upload.iso"), cs.upload_file_name);
	ASSERT_TRUE(cs.has_parts_offered_count);
	ASSERT_EQUALS(static_cast<std::uint32_t>(7), cs.parts_offered_count);
	ASSERT_EQUALS(std::string("mod-x"), cs.client_mod_name);
	ASSERT_TRUE(cs.view_shared_disabled);
	ASSERT_TRUE(cs.is_friend);
	ASSERT_TRUE(cs.credit_ratio > 2.4 && cs.credit_ratio < 2.6);

	const auto it2 = cache.find(51);
	ASSERT_TRUE(it2 != cache.end());
	ASSERT_TRUE(!it2->second.high_id);
	ASSERT_TRUE(!it2->second.has_parts_offered_count);
	// #423 fields absent on the wire => defaults preserved.
	ASSERT_TRUE(!it2->second.is_friend);
	ASSERT_TRUE(it2->second.credit_ratio == 0.0);
}

// --- #437: extended EC preference categories decode ------------------
//
// Covers both boolean encodings the core serializer uses: value tags
// (share_hidden/exclude_patterns_use_regex -> GetInt()!=0) and bare presence
// tags (ich_enabled/use_secident/endgame_enabled -> tag present == true), the
// 3-state shared_files_visibility enum decoded from the wire int (#596, #655),
// plus ints, strings, and the directories.shared_paths string array.
TEST(Refresher, PreferencesExtendedCategoriesDecode)
{
	CECPacket resp(EC_OP_SET_PREFERENCES);

	CECEmptyTag dir(EC_TAG_PREFS_DIRECTORIES);
	dir.AddTag(CECTag(EC_TAG_DIRECTORIES_INCOMING, wxString::FromUTF8("/inc")));
	dir.AddTag(CECTag(EC_TAG_DIRECTORIES_TEMP, wxString::FromUTF8("/tmp")));
	CECTag shared(EC_TAG_DIRECTORIES_SHARED, static_cast<std::uint32_t>(2));
	shared.AddTag(CECTag(EC_TAG_STRING, wxString::FromUTF8("/a")));
	shared.AddTag(CECTag(EC_TAG_STRING, wxString::FromUTF8("/b")));
	dir.AddTag(shared);
	dir.AddTag(CECTag(EC_TAG_DIRECTORIES_SHARE_HIDDEN, true)); // value-encoded bool
	dir.AddTag(CECTag(EC_TAG_DIRECTORIES_EXCLUDE_REGEX, true));
	resp.AddTag(dir);

	CECEmptyTag files(EC_TAG_PREFS_FILES);
	files.AddTag(CECEmptyTag(EC_TAG_FILES_ICH_ENABLED)); // presence == true
	files.AddTag(CECEmptyTag(EC_TAG_FILES_ENDGAME));     // presence == true (#596)
	files.AddTag(CECTag(EC_TAG_FILES_MIN_FREE_SPACE, static_cast<std::uint32_t>(512)));
	resp.AddTag(files);

	CECEmptyTag srv(EC_TAG_PREFS_SERVERS);
	srv.AddTag(CECTag(EC_TAG_SERVERS_DEAD_SERVER_RETRIES, static_cast<std::uint16_t>(5)));
	srv.AddTag(CECTag(EC_TAG_SERVERS_UPDATE_URL, wxString::FromUTF8("http://srv")));
	resp.AddTag(srv);

	CECEmptyTag sec(EC_TAG_PREFS_SECURITY);
	sec.AddTag(CECTag(EC_TAG_SECURITY_CAN_SEE_SHARES, (uint8)2)); // 3-state: 2 = nobody (#596)
	sec.AddTag(CECTag(EC_TAG_IPFILTER_LEVEL, static_cast<std::uint32_t>(100)));
	sec.AddTag(CECEmptyTag(EC_TAG_SECURITY_USE_SECIDENT)); // presence == true
	resp.AddTag(sec);

	// Message-filter category: presence bools + keyword strings, incl. the
	// show-in-log / comment-filter fields wired over EC.
	CECEmptyTag mf(EC_TAG_PREFS_MESSAGEFILTER);
	mf.AddTag(CECEmptyTag(EC_TAG_MSGFILTER_ENABLED));         // presence == true
	mf.AddTag(CECEmptyTag(EC_TAG_MSGFILTER_SHOW_IN_LOG));     // presence == true
	mf.AddTag(CECEmptyTag(EC_TAG_MSGFILTER_FILTER_COMMENTS)); // presence == true
	mf.AddTag(CECTag(EC_TAG_MSGFILTER_KEYWORDS, wxString::FromUTF8("spam,ads")));
	mf.AddTag(CECTag(EC_TAG_MSGFILTER_COMMENT_KEYWORDS, wxString::FromUTF8("junk,scam")));
	resp.AddTag(mf);

	CECEmptyTag cw(EC_TAG_PREFS_CORETWEAKS);
	cw.AddTag(CECTag(EC_TAG_CORETW_MAX_CONN_PER_FIVE, static_cast<std::uint32_t>(200)));
	cw.AddTag(CECTag(EC_TAG_CORETW_KAD_REASK_MS, static_cast<std::uint32_t>(1800000)));
	resp.AddTag(cw);

	CECEmptyTag kad(EC_TAG_PREFS_KADEMLIA);
	kad.AddTag(CECTag(EC_TAG_KADEMLIA_UPDATE_URL, wxString::FromUTF8("http://nodes")));
	resp.AddTag(kad);

	CECEmptyTag ip2c(EC_TAG_PREFS_IP2COUNTRY);
	ip2c.AddTag(CECTag(EC_TAG_IP2COUNTRY_SUPPORTED, true));  // value-encoded bool
	ip2c.AddTag(CECTag(EC_TAG_IP2COUNTRY_ENABLED, true));    // value-encoded bool
	ip2c.AddTag(CECTag(EC_TAG_IP2COUNTRY_SOURCE, (uint8)1)); // MaxMind
	ip2c.AddTag(CECTag(EC_TAG_IP2COUNTRY_CUSTOM_URL, wxString::FromUTF8("http://geo")));
	ip2c.AddTag(CECTag(EC_TAG_IP2COUNTRY_MAXMIND_LICENSE, wxString::FromUTF8("LICKEY")));
	ip2c.AddTag(CECTag(EC_TAG_IP2COUNTRY_DB_LOADED, true)); // value-encoded bool
	ip2c.AddTag(CECTag(EC_TAG_IP2COUNTRY_LOADED_SOURCE, wxString::FromUTF8("maxmind")));
	resp.AddTag(ip2c);

	PreferencesSnapshot p;
	std::vector<CategorySnapshot> cats;
	ParsePreferencesFromPacket(&resp, p, cats);

	ASSERT_EQUALS(std::string("/inc"), p.directories.incoming_path);
	ASSERT_EQUALS(std::string("/tmp"), p.directories.temp_path);
	ASSERT_EQUALS(static_cast<size_t>(2), p.directories.shared_paths.size());
	ASSERT_EQUALS(std::string("/a"), p.directories.shared_paths[0]);
	ASSERT_TRUE(p.directories.share_hidden);
	ASSERT_TRUE(p.directories.exclude_patterns_use_regex);
	ASSERT_TRUE(!p.directories.rescan_on_startup); // absent -> false

	ASSERT_TRUE(p.files.ich_enabled);
	ASSERT_TRUE(!p.files.trust_unverified_aich_hashes); // absent presence tag -> false
	ASSERT_TRUE(p.files.endgame_mode_enabled);          // presence tag -> true (#596)
	ASSERT_EQUALS(static_cast<std::uint32_t>(512), p.files.min_free_space_mebibytes);

	ASSERT_EQUALS(static_cast<std::uint32_t>(5), p.servers.dead_server_retry_count);
	ASSERT_EQUALS(std::string("http://srv"), p.servers.update_url);

	// 3-state (#596): the middle/high value round-trips, not just 0/1, and
	// decodes to its enum string rather than the wire int (#655).
	ASSERT_EQUALS(std::string("nobody"), p.security.shared_files_visibility);
	ASSERT_EQUALS(static_cast<std::uint32_t>(100), p.security.ipfilter_min_access_level);
	ASSERT_TRUE(p.security.secure_identification_enabled);
	ASSERT_TRUE(!p.security.obfuscation_required); // absent -> false

	ASSERT_TRUE(p.message_filter.enabled);
	ASSERT_TRUE(p.message_filter.log_filtered_messages);
	ASSERT_TRUE(p.message_filter.filter_comments);
	ASSERT_TRUE(!p.message_filter.filter_all_messages); // absent -> false
	ASSERT_EQUALS(std::string("spam,ads"), p.message_filter.keywords);
	ASSERT_EQUALS(std::string("junk,scam"), p.message_filter.comment_keywords);

	ASSERT_EQUALS(static_cast<std::uint32_t>(200), p.advanced.max_new_connections_per_5_seconds);
	// EC carries milliseconds; the API speaks the minutes the core actually
	// stores, so the decode divides by 60000 (#1159 section 5).
	ASSERT_EQUALS(static_cast<std::uint32_t>(30), p.advanced.kad_source_reask_minutes);
	ASSERT_EQUALS(std::string("http://nodes"), p.kad.update_url);

	ASSERT_TRUE(p.geoip.supported);
	ASSERT_TRUE(p.geoip.enabled);
	ASSERT_EQUALS(std::string("maxmind"), p.geoip.source); // uint8 1 -> "maxmind"
	ASSERT_EQUALS(std::string("http://geo"), p.geoip.custom_update_url);
	ASSERT_EQUALS(std::string("LICKEY"), p.geoip.maxmind_license);
	ASSERT_TRUE(!p.geoip.auto_update); // absent -> false
	ASSERT_TRUE(p.geoip.db_loaded);
	ASSERT_EQUALS(std::string("maxmind"), p.geoip.loaded_source);
	ASSERT_TRUE(!p.geoip.download_in_progress); // absent -> false
}

// --- #655: enum strings and the nested remote_controls shape ----------
//
// The EC layer is unchanged by the field-naming pass: it still carries the
// proxy type as a wire int and both remote-control subsystems in one flat
// category. What changed is the API shape the snapshot feeds, so this pins
// the two translations that now happen at the webapi boundary.
TEST(Refresher, PreferencesEnumAndNestedRemoteControlsDecode)
{
	CECPacket resp(EC_OP_SET_PREFERENCES);

	CECEmptyTag conn(EC_TAG_PREFS_CONNECTIONS);
	conn.AddTag(CECTag(EC_TAG_PROXY_TYPE, (uint8)2)); // wire 2 == HTTP
	resp.AddTag(conn);

	CECEmptyTag rc(EC_TAG_PREFS_REMOTECTRL);
	rc.AddTag(CECEmptyTag(EC_TAG_WEBSERVER_AUTORUN)); // presence == true
	rc.AddTag(CECTag(EC_TAG_WEBSERVER_PORT, static_cast<std::uint32_t>(4711)));
	rc.AddTag(CECTag(EC_TAG_WEBSERVER_REFRESH, static_cast<std::uint32_t>(120)));
	rc.AddTag(CECTag(EC_TAG_WEBSERVER_TEMPLATE, wxString::FromUTF8("php-default")));
	rc.AddTag(CECTag(EC_TAG_AMULEAPI_PORT, static_cast<std::uint32_t>(4712)));
	rc.AddTag(CECTag(EC_TAG_AMULEAPI_BIND, wxString::FromUTF8("127.0.0.1")));
	resp.AddTag(rc);

	PreferencesSnapshot p;
	std::vector<CategorySnapshot> cats;
	ParsePreferencesFromPacket(&resp, p, cats);

	// Wire int -> enum string, and an unset field stays empty rather than
	// defaulting to the first enum member.
	ASSERT_EQUALS(std::string("http"), p.proxy_type);

	// One flat EC category fans out into the two nested API sub-objects.
	ASSERT_TRUE(p.remote_controls.webserver.enabled);
	ASSERT_EQUALS(static_cast<std::uint32_t>(4711), p.remote_controls.webserver.port);
	ASSERT_EQUALS(static_cast<std::uint32_t>(120), p.remote_controls.webserver.refresh_seconds);
	ASSERT_EQUALS(std::string("php-default"), p.remote_controls.webserver.template_name);
	ASSERT_TRUE(!p.remote_controls.webserver.guest_enabled); // absent -> false

	ASSERT_TRUE(!p.remote_controls.amuleapi.enabled); // absent -> false
	ASSERT_EQUALS(static_cast<std::uint32_t>(4712), p.remote_controls.amuleapi.port);
	ASSERT_EQUALS(std::string("127.0.0.1"), p.remote_controls.amuleapi.bind_address);
}

// Every 3-state / 4-state wire value maps to its documented enum string, and
// an out-of-range value is not invented into a valid one (#655).
TEST(Refresher, PreferencesEnumStringsCoverEveryWireValue)
{
	const char *const kProxy[] = { "socks5", "socks4", "http", "socks4a" };
	for (std::uint8_t wire = 0; wire < 4; ++wire) {
		CECPacket resp(EC_OP_SET_PREFERENCES);
		CECEmptyTag conn(EC_TAG_PREFS_CONNECTIONS);
		conn.AddTag(CECTag(EC_TAG_PROXY_TYPE, wire));
		resp.AddTag(conn);

		PreferencesSnapshot p;
		std::vector<CategorySnapshot> cats;
		ParsePreferencesFromPacket(&resp, p, cats);
		ASSERT_EQUALS(std::string(kProxy[wire]), p.proxy_type);
	}

	const char *const kVisibility[] = { "everybody", "friends", "nobody" };
	for (std::uint8_t wire = 0; wire < 3; ++wire) {
		CECPacket resp(EC_OP_SET_PREFERENCES);
		CECEmptyTag sec(EC_TAG_PREFS_SECURITY);
		sec.AddTag(CECTag(EC_TAG_SECURITY_CAN_SEE_SHARES, wire));
		resp.AddTag(sec);

		PreferencesSnapshot p;
		std::vector<CategorySnapshot> cats;
		ParsePreferencesFromPacket(&resp, p, cats);
		ASSERT_EQUALS(std::string(kVisibility[wire]), p.security.shared_files_visibility);
	}

	// Out of range: proxy_type clears, visibility falls back to the default.
	{
		CECPacket resp(EC_OP_SET_PREFERENCES);
		CECEmptyTag conn(EC_TAG_PREFS_CONNECTIONS);
		conn.AddTag(CECTag(EC_TAG_PROXY_TYPE, (uint8)9));
		resp.AddTag(conn);
		CECEmptyTag sec(EC_TAG_PREFS_SECURITY);
		sec.AddTag(CECTag(EC_TAG_SECURITY_CAN_SEE_SHARES, (uint8)9));
		resp.AddTag(sec);

		PreferencesSnapshot p;
		std::vector<CategorySnapshot> cats;
		ParsePreferencesFromPacket(&resp, p, cats);
		ASSERT_TRUE(p.proxy_type.empty());
		ASSERT_EQUALS(std::string("everybody"), p.security.shared_files_visibility);
	}
}

// --- #655: the preferences schema table is self-consistent -------------
//
// GET emit, PATCH apply and the EC decode are all driven off PrefSchema(),
// so a malformed row silently breaks all three at once. These pin the
// invariants the three walkers rely on.
TEST(Refresher, PrefsSchemaIsWellFormed)
{
	std::set<std::string> seen;
	std::size_t emitted = 0;

	for (std::size_t i = 0; i < PrefSchemaSize(); ++i) {
		const PrefField &f = PrefSchema()[i];
		const std::string id = std::string(f.category) + "." + f.key;

		// No duplicate field, or one walker would apply it twice.
		ASSERT_TRUE(seen.insert(id).second);

		// Every category must resolve to an EC group.
		ASSERT_TRUE(PrefGroupTagFor(f.category) != 0);

		// Rows that round-trip a value need a backing member; rows that
		// never do must not have one.
		const bool needs_member = f.access == PrefAccess::ReadWrite ||
					  f.access == PrefAccess::ReadOnly || f.access == PrefAccess::Bespoke;
		ASSERT_TRUE(needs_member == (f.member != nullptr));

		// Enum rows carry a non-empty, nullptr-terminated name table.
		if (f.type == PrefType::Enum) {
			ASSERT_TRUE(f.enum_names != nullptr);
			ASSERT_TRUE(f.enum_names[0] != nullptr);
		} else {
			ASSERT_TRUE(f.enum_names == nullptr);
		}

		// A capability gate must name a real bool in the same category.
		if (f.gated_by) {
			bool found = false;
			for (std::size_t g = 0; g < PrefSchemaSize(); ++g) {
				const PrefField &c = PrefSchema()[g];
				if (std::string(c.category) == f.category &&
					std::string(c.key) == f.gated_by) {
					ASSERT_TRUE(c.type == PrefType::Bool);
					found = true;
				}
			}
			ASSERT_TRUE(found);
		}

		if (f.access != PrefAccess::WriteOnly && f.access != PrefAccess::Rejected)
			++emitted;
	}

	// The documented payload is 119 fields. A row added or dropped without
	// updating docs/api/REFERENCE.md should trip this.
	ASSERT_EQUALS(static_cast<std::size_t>(119), emitted);
}

// The schema's irregularities are enumerated rather than merely counted: each
// is deliberate and documented, so a new one appearing is a mistake worth
// catching rather than a pattern to copy.
//
// Exactly two fields invert, and both do so because the EC tag they ride on is
// named for the negative case: EC_TAG_CONN_UDP_DISABLE and, since #655,
// EC_TAG_FILES_CREATE_NORMAL. In both the API states the positive fact and the
// schema undoes the EC negation on read and write alike.
TEST(Refresher, PrefsSchemaIrregularitiesStayContained)
{
	std::size_t inverted = 0, foreign_group = 0, bespoke = 0;
	for (std::size_t i = 0; i < PrefSchemaSize(); ++i) {
		const PrefField &f = PrefSchema()[i];
		if (f.invert) {
			++inverted;
			const std::string key(f.key);
			ASSERT_TRUE(key == "extended_udp_port_enabled" || key == "create_sparse_files");
		}
		if (f.read_group != 0) {
			++foreign_group;
			ASSERT_EQUALS(std::string("upnp_supported"), std::string(f.key));
		}
		if (f.access == PrefAccess::Bespoke) {
			++bespoke;
			ASSERT_EQUALS(std::string("guest_enabled"), std::string(f.key));
		}
	}
	ASSERT_EQUALS(static_cast<std::size_t>(2), inverted);
	ASSERT_EQUALS(static_cast<std::size_t>(1), foreign_group);
	ASSERT_EQUALS(static_cast<std::size_t>(1), bespoke);
}

// --- #692: ed2k server priority maps both ways -------------------------
//
// The SRV_PR_* wire values are not monotone (NORMAL=0, HIGH=1, LOW=2), so a
// name->code mapping that assumed a name's position in a list was its code --
// which is exactly what the generic enum helper on the preferences path does
// -- would be wrong for every value. Pin both directions and the round trip.
TEST(Refresher, ServerPriorityMapsBothWays)
{
	ASSERT_EQUALS(std::string("normal"), std::string(ServerPriorityName(SRV_PR_NORMAL)));
	ASSERT_EQUALS(std::string("high"), std::string(ServerPriorityName(SRV_PR_HIGH)));
	ASSERT_EQUALS(std::string("low"), std::string(ServerPriorityName(SRV_PR_LOW)));

	std::uint32_t code = 0xFFFFFFFFu;
	ASSERT_TRUE(ServerPriorityCode("normal", code));
	ASSERT_EQUALS(static_cast<std::uint32_t>(SRV_PR_NORMAL), code);
	ASSERT_TRUE(ServerPriorityCode("high", code));
	ASSERT_EQUALS(static_cast<std::uint32_t>(SRV_PR_HIGH), code);
	ASSERT_TRUE(ServerPriorityCode("low", code));
	ASSERT_EQUALS(static_cast<std::uint32_t>(SRV_PR_LOW), code);

	// name -> code -> name is the identity for every accepted name.
	for (const char *name : { "low", "normal", "high" }) {
		std::uint32_t c = 0;
		ASSERT_TRUE(ServerPriorityCode(name, c));
		ASSERT_EQUALS(std::string(name), std::string(ServerPriorityName(c)));
	}

	// An unknown name is rejected and leaves the out-param untouched, so a
	// caller that ignores the return value cannot silently write a priority.
	std::uint32_t untouched = 4242;
	ASSERT_TRUE(!ServerPriorityCode("urgent", untouched));
	ASSERT_EQUALS(static_cast<std::uint32_t>(4242), untouched);
	ASSERT_TRUE(!ServerPriorityCode("", untouched));
	ASSERT_TRUE(!ServerPriorityCode("HIGH", untouched)); // case-sensitive by design
	ASSERT_EQUALS(static_cast<std::uint32_t>(4242), untouched);
}

// --- union EC_OP_SEARCH_PROGRESS -------------------------------------------
//
// Absence from a union reply is how amuleapi learns a search expired, so the
// parser's return value is load-bearing: a reply it could not parse must be
// rejected outright rather than handed back as an empty union, which the
// caller would read as "every tracked search is gone" and retire in one pass.

// --- The incremental contract -------------------------------------------
//
// Under EC_DETAIL_INC_UPDATE the daemon sends only what moved, so an absent
// field means "unchanged". These pin that per field, because the failure mode
// is silent: a guard forgotten on one field reverts it to its default on the
// first quiet poll and nothing else notices.

TEST(Refresher, SearchUnionSecondPollKeepsFieldsTheDiffOmits)
{
	std::map<std::uint32_t, SearchSlot> slots;
	std::map<std::uint32_t, std::uint32_t> owner;
	slots[kSid];

	CECPacket first(EC_OP_SEARCH_RESULTS);
	CECTag sf(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(70));
	sf.AddTag(CECTag(EC_TAG_SEARCH_ID, kSid));
	sf.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("cool.movie.mkv")));
	sf.AddTag(CECTag(EC_TAG_PARTFILE_SIZE_FULL, static_cast<std::uint64_t>(123)));
	sf.AddTag(CECTag(EC_TAG_PARTFILE_STATUS, static_cast<std::uint32_t>(2))); // QUEUED
	sf.AddTag(CECTag(EC_TAG_PARTFILE_SOURCE_COUNT, static_cast<std::uint32_t>(9)));
	first.AddTag(sf);
	ApplySearchUnion(&first, slots, owner);

	// A diff that moves only the source count. Everything else is absent,
	// which is the daemon saying "unchanged", not "cleared".
	CECPacket second(EC_OP_SEARCH_RESULTS);
	CECTag d(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(70));
	d.AddTag(CECTag(EC_TAG_PARTFILE_SOURCE_COUNT, static_cast<std::uint32_t>(11)));
	second.AddTag(d);
	ApplySearchUnion(&second, slots, owner);

	const auto it = slots[kSid].results.find(70);
	ASSERT_TRUE(it != slots[kSid].results.end());
	ASSERT_EQUALS(static_cast<std::uint32_t>(11), it->second.source_count);
	// The fields the diff said nothing about.
	ASSERT_EQUALS(std::string("cool.movie.mkv"), it->second.name);
	ASSERT_EQUALS(static_cast<std::uint64_t>(123), it->second.size);
	ASSERT_EQUALS(std::string("queued"), it->second.status);
	ASSERT_TRUE(it->second.already_downloaded);
	ASSERT_EQUALS(std::string("video"), it->second.type);
}

TEST(Refresher, SearchUnionAppliesAStatusChangeOnAFinishedSearch)
{
	// The case #1187 section 2 is about: a hit gets downloaded after its
	// search finished. Nothing here knows or cares that the search is over --
	// which is the point, since the union polls it either way.
	std::map<std::uint32_t, SearchSlot> slots;
	std::map<std::uint32_t, std::uint32_t> owner;
	slots[kSid];

	CECPacket first(EC_OP_SEARCH_RESULTS);
	CECTag sf(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(70));
	sf.AddTag(CECTag(EC_TAG_SEARCH_ID, kSid));
	sf.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("iso.img")));
	sf.AddTag(CECTag(EC_TAG_PARTFILE_STATUS, static_cast<std::uint32_t>(0))); // NEW
	first.AddTag(sf);
	ApplySearchUnion(&first, slots, owner);
	ASSERT_TRUE(!slots[kSid].results.find(70)->second.already_downloaded);

	CECPacket second(EC_OP_SEARCH_RESULTS);
	CECTag d(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(70));
	d.AddTag(CECTag(EC_TAG_PARTFILE_STATUS, static_cast<std::uint32_t>(1))); // DOWNLOADED
	second.AddTag(d);
	ApplySearchUnion(&second, slots, owner);

	const auto &r = slots[kSid].results.find(70)->second;
	ASSERT_EQUALS(std::string("downloaded"), r.status);
	ASSERT_TRUE(r.already_downloaded);
}

TEST(Refresher, SearchUnionQuietPollRemovesNothing)
{
	// Absence stopped meaning deletion: a poll that mentions a search not at
	// all must leave its results alone. Sweeping here would empty every
	// result set on the first tick where nothing changed.
	std::map<std::uint32_t, SearchSlot> slots;
	std::map<std::uint32_t, std::uint32_t> owner;
	slots[kSid];

	CECPacket first(EC_OP_SEARCH_RESULTS);
	CECTag sf(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(70));
	sf.AddTag(CECTag(EC_TAG_SEARCH_ID, kSid));
	sf.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("still.here.bin")));
	first.AddTag(sf);
	ApplySearchUnion(&first, slots, owner);
	ASSERT_EQUALS(static_cast<std::size_t>(1), slots[kSid].results.size());

	const CECPacket quiet(EC_OP_SEARCH_RESULTS);
	ApplySearchUnion(&quiet, slots, owner);
	ASSERT_EQUALS(static_cast<std::size_t>(1), slots[kSid].results.size());
	ASSERT_EQUALS(std::string("still.here.bin"), slots[kSid].results.find(70)->second.name);
}

TEST(Refresher, SearchUnionRemovesOnlyOnTheExplicitTombstone)
{
	std::map<std::uint32_t, SearchSlot> slots;
	std::map<std::uint32_t, std::uint32_t> owner;
	slots[kSid];

	CECPacket first(EC_OP_SEARCH_RESULTS);
	for (std::uint32_t ecid : { 70u, 71u }) {
		CECTag sf(EC_TAG_SEARCHFILE, ecid);
		sf.AddTag(CECTag(EC_TAG_SEARCH_ID, kSid));
		sf.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("f.bin")));
		first.AddTag(sf);
	}
	ApplySearchUnion(&first, slots, owner);
	ASSERT_EQUALS(static_cast<std::size_t>(2), slots[kSid].results.size());

	CECPacket second(EC_OP_SEARCH_RESULTS);
	second.AddTag(CECTag(EC_TAG_FILE_REMOVED, static_cast<std::uint32_t>(71)));
	ApplySearchUnion(&second, slots, owner);
	ASSERT_EQUALS(static_cast<std::size_t>(1), slots[kSid].results.size());
	ASSERT_TRUE(slots[kSid].results.find(70) != slots[kSid].results.end());
	// The index loses it with the map, or a later reuse of the ECID would be
	// attributed to a search that no longer holds it.
	ASSERT_TRUE(owner.find(71) == owner.end());
	ASSERT_TRUE(owner.find(70) != owner.end());
}

TEST(Refresher, SearchUnionAttributesDiffedTagsAcrossTwoSearches)
{
	// The index earns its keep here: the second poll carries no search id on
	// either result, so both can only be placed by remembering who owns them.
	const std::uint32_t sid_a = 1;
	const std::uint32_t sid_b = 2 | 0x80000000u; // Kad ids carry the mask
	std::map<std::uint32_t, SearchSlot> slots;
	std::map<std::uint32_t, std::uint32_t> owner;
	slots[sid_a];
	slots[sid_b];

	CECPacket first(EC_OP_SEARCH_RESULTS);
	CECTag a(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(10));
	a.AddTag(CECTag(EC_TAG_SEARCH_ID, sid_a));
	a.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("a.bin")));
	first.AddTag(a);
	CECTag b(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(20));
	b.AddTag(CECTag(EC_TAG_SEARCH_ID, sid_b));
	b.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("b.bin")));
	first.AddTag(b);
	ApplySearchUnion(&first, slots, owner);

	CECPacket second(EC_OP_SEARCH_RESULTS);
	CECTag da(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(10));
	da.AddTag(CECTag(EC_TAG_PARTFILE_SOURCE_COUNT, static_cast<std::uint32_t>(5)));
	second.AddTag(da);
	CECTag db(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(20));
	db.AddTag(CECTag(EC_TAG_PARTFILE_SOURCE_COUNT, static_cast<std::uint32_t>(7)));
	second.AddTag(db);
	ApplySearchUnion(&second, slots, owner);

	ASSERT_EQUALS(static_cast<std::uint32_t>(5), slots[sid_a].results.find(10)->second.source_count);
	ASSERT_EQUALS(static_cast<std::uint32_t>(7), slots[sid_b].results.find(20)->second.source_count);
	// And neither leaked into the other.
	ASSERT_EQUALS(static_cast<std::size_t>(1), slots[sid_a].results.size());
	ASSERT_EQUALS(static_cast<std::size_t>(1), slots[sid_b].results.size());
}

TEST(Refresher, SearchUnionDiffedChildStaysFoldedIntoItsParent)
{
	// A grouped child is addressable by its own ECID on the wire, so it gets
	// diffed tags of its own -- which is why `raw` keeps children and the
	// folded view is rebuilt from it rather than merged into.
	std::map<std::uint32_t, SearchSlot> slots;
	std::map<std::uint32_t, std::uint32_t> owner;
	slots[kSid];

	CECPacket first(EC_OP_SEARCH_RESULTS);
	CECTag parent(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(100));
	parent.AddTag(CECTag(EC_TAG_SEARCH_ID, kSid));
	parent.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("movie.mkv")));
	first.AddTag(parent);
	CECTag child(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(101));
	child.AddTag(CECTag(EC_TAG_SEARCH_ID, kSid));
	child.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("movie.alt.mkv")));
	child.AddTag(CECTag(EC_TAG_SEARCH_PARENT, static_cast<std::uint32_t>(100)));
	first.AddTag(child);
	ApplySearchUnion(&first, slots, owner);
	ASSERT_EQUALS(static_cast<std::size_t>(1), slots[kSid].results.size());
	ASSERT_EQUALS(static_cast<std::size_t>(1), slots[kSid].results.find(100)->second.children.size());

	// The child's source count moves; it must still be nested, not promoted.
	CECPacket second(EC_OP_SEARCH_RESULTS);
	CECTag dc(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(101));
	dc.AddTag(CECTag(EC_TAG_PARTFILE_SOURCE_COUNT, static_cast<std::uint32_t>(3)));
	second.AddTag(dc);
	ApplySearchUnion(&second, slots, owner);

	ASSERT_EQUALS(static_cast<std::size_t>(1), slots[kSid].results.size());
	const auto &kids = slots[kSid].results.find(100)->second.children;
	ASSERT_EQUALS(static_cast<std::size_t>(1), kids.size());
	ASSERT_EQUALS(static_cast<std::uint32_t>(3), kids[0].source_count);
	ASSERT_EQUALS(std::string("movie.alt.mkv"), kids[0].name);
}

TEST(Refresher, SearchUnionSeesTheKadLookupEnd)
{
	// The daemon now always sends EC_TAG_PARTFILE_KAD_COMMENT_SEARCHING, and
	// sends it through the valuemap, so the 1 -> 0 transition is itself a
	// child tag and the result carrying it is not elided as unchanged. It
	// used to be emitted only while the lookup ran or had notes, which made
	// "finished, found nothing" a tag that simply stopped appearing -- and an
	// incremental client cannot tell that from silence.
	std::map<std::uint32_t, SearchSlot> slots;
	std::map<std::uint32_t, std::uint32_t> owner;
	slots[kSid];

	CECPacket running(EC_OP_SEARCH_RESULTS);
	CECTag sf(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(70));
	sf.AddTag(CECTag(EC_TAG_SEARCH_ID, kSid));
	sf.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("noted.bin")));
	sf.AddTag(CECTag(EC_TAG_PARTFILE_KAD_COMMENT_SEARCHING, static_cast<std::uint64_t>(1)));
	running.AddTag(sf);
	ApplySearchUnion(&running, slots, owner);
	ASSERT_TRUE(slots[kSid].results.find(70)->second.kad_comment_searching);

	CECPacket done(EC_OP_SEARCH_RESULTS);
	CECTag d(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(70));
	d.AddTag(CECTag(EC_TAG_PARTFILE_KAD_COMMENT_SEARCHING, static_cast<std::uint64_t>(0)));
	done.AddTag(d);
	ApplySearchUnion(&done, slots, owner);

	const auto &r = slots[kSid].results.find(70)->second;
	ASSERT_TRUE(!r.kad_comment_searching);
	// ...and the poll that carried it touched nothing else.
	ASSERT_EQUALS(std::string("noted.bin"), r.name);
}

TEST(Refresher, SearchUnionCommentsAbsenceMeansNoNotes)
{
	// The comments container is the one field where absence is a value: it is
	// built only when there are notes and added without the valuemap, so it is
	// never diffed away. A poll that omits it is saying "no notes", not
	// "unchanged" -- otherwise a note set could never shrink to empty.
	std::map<std::uint32_t, SearchSlot> slots;
	std::map<std::uint32_t, std::uint32_t> owner;
	slots[kSid];

	CECPacket withNotes(EC_OP_SEARCH_RESULTS);
	CECTag sf(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(70));
	sf.AddTag(CECTag(EC_TAG_SEARCH_ID, kSid));
	sf.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("noted.bin")));
	CECEmptyTag cont(EC_TAG_PARTFILE_COMMENTS);
	cont.AddTag(CECTag(EC_TAG_PARTFILE_COMMENTS, std::string("someone")));
	cont.AddTag(CECTag(EC_TAG_PARTFILE_COMMENTS, std::string("noted.bin")));
	cont.AddTag(CECTag(EC_TAG_PARTFILE_COMMENTS, static_cast<std::uint64_t>(4)));
	cont.AddTag(CECTag(EC_TAG_PARTFILE_COMMENTS, std::string("good file")));
	sf.AddTag(cont);
	withNotes.AddTag(sf);
	ApplySearchUnion(&withNotes, slots, owner);
	ASSERT_EQUALS(static_cast<std::size_t>(1), slots[kSid].results.find(70)->second.comments.size());

	CECPacket without(EC_OP_SEARCH_RESULTS);
	CECTag d(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(70));
	d.AddTag(CECTag(EC_TAG_PARTFILE_SOURCE_COUNT, static_cast<std::uint32_t>(2)));
	without.AddTag(d);
	ApplySearchUnion(&without, slots, owner);
	ASSERT_EQUALS(static_cast<std::size_t>(0), slots[kSid].results.find(70)->second.comments.size());
}

TEST(Refresher, SearchUnionIgnoresResultsForAnUnknownSearch)
{
	// Slot creation belongs to MarkSearchStarted / discovery, which also set
	// the lifecycle state GET /search reports. Auto-creating one here would
	// produce a search with results and no state behind it.
	std::map<std::uint32_t, SearchSlot> slots;
	std::map<std::uint32_t, std::uint32_t> owner;

	CECPacket resp(EC_OP_SEARCH_RESULTS);
	CECTag sf(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(70));
	sf.AddTag(CECTag(EC_TAG_SEARCH_ID, static_cast<std::uint32_t>(999)));
	sf.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("orphan.bin")));
	resp.AddTag(sf);
	ApplySearchUnion(&resp, slots, owner);

	ASSERT_TRUE(slots.empty());
	// And it is not left in the index pointing at a slot that never existed.
	ASSERT_TRUE(owner.find(70) == owner.end());
}

TEST(Refresher, SearchUnionLeavesADetachedSlotAlone)
{
	// The daemon evicted the search from its ring. That makes it emit an
	// EC_TAG_FILE_REMOVED for every result it had sent us -- but the tick has
	// already detached the slot, precisely so those tombstones do not erase
	// the results the retirement path means to keep for late reads.
	std::map<std::uint32_t, SearchSlot> slots;
	std::map<std::uint32_t, std::uint32_t> owner;
	SearchSlot &slot = slots[kSid];
	SearchResult keep;
	keep.name = "kept.bin";
	keep.status = "new";
	slot.raw[70] = keep;
	slot.results[70] = keep;
	slot.detached = true;
	owner[70] = kSid;

	CECPacket resp(EC_OP_SEARCH_RESULTS);
	resp.AddTag(CECTag(EC_TAG_FILE_REMOVED, static_cast<std::uint32_t>(70)));
	ApplySearchUnion(&resp, slots, owner);

	ASSERT_EQUALS(static_cast<size_t>(1), slots[kSid].raw.size());
	ASSERT_EQUALS(static_cast<size_t>(1), slots[kSid].results.size());
	ASSERT_EQUALS(std::string("kept.bin"), slots[kSid].results[70].name);
	// The index entry stays too: nothing will re-establish it, and dropping
	// it would strand the result it points at.
	ASSERT_TRUE(owner.find(70) != owner.end());
}

TEST(Refresher, SearchUnionDoesNotUpdateADetachedSlot)
{
	// Same freeze in the other direction. A diffed tag arriving for a search
	// the daemon no longer holds is the tail of an eviction, not news.
	std::map<std::uint32_t, SearchSlot> slots;
	std::map<std::uint32_t, std::uint32_t> owner;
	SearchSlot &slot = slots[kSid];
	SearchResult keep;
	keep.name = "kept.bin";
	keep.source_count = 3;
	slot.raw[70] = keep;
	slot.results[70] = keep;
	slot.detached = true;
	owner[70] = kSid;

	CECPacket resp(EC_OP_SEARCH_RESULTS);
	CECTag sf(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(70));
	sf.AddTag(CECTag(EC_TAG_SEARCH_ID, kSid));
	sf.AddTag(CECTag(EC_TAG_PARTFILE_SOURCE_COUNT, static_cast<uint32>(99)));
	resp.AddTag(sf);
	ApplySearchUnion(&resp, slots, owner);

	ASSERT_EQUALS(static_cast<std::uint32_t>(3), slots[kSid].results[70].source_count);
}

TEST(Refresher, SearchUnionDefaultSidAdoptsAnIdLessReply)
{
	// The per-search EC_DETAIL_FULL fetch -- the resync path the differential
	// union has no opcode for. Its reply carries no EC_TAG_SEARCH_ID at all,
	// so every tag in it has to be attributed to the search that was asked
	// for, and the ECID index built from that.
	std::map<std::uint32_t, SearchSlot> slots;
	std::map<std::uint32_t, std::uint32_t> owner;
	slots[kSid];

	CECPacket resp(EC_OP_SEARCH_RESULTS);
	CECTag sf(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(71));
	sf.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("seeded.bin")));
	resp.AddTag(sf);
	ApplySearchUnion(&resp, slots, owner, kSid);

	ASSERT_EQUALS(static_cast<size_t>(1), slots[kSid].results.size());
	ASSERT_EQUALS(std::string("seeded.bin"), slots[kSid].results[71].name);
	ASSERT_EQUALS(kSid, owner[71]);
}

TEST(Refresher, SearchUnionLeavesNoIndexEntryForADetachedSlot)
{
	// The index is what eviction walks to clean up after a slot, and it walks
	// it via the slot's own results. An entry written for a result that was
	// then dropped on the detached guard is in neither place, so nothing ever
	// removes it -- and a reused ECID would resolve through it to a search
	// that is gone.
	std::map<std::uint32_t, SearchSlot> slots;
	std::map<std::uint32_t, std::uint32_t> owner;
	SearchSlot &slot = slots[kSid];
	slot.detached = true;

	CECPacket resp(EC_OP_SEARCH_RESULTS);
	CECTag sf(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(72));
	sf.AddTag(CECTag(EC_TAG_SEARCH_ID, kSid));
	sf.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("late.bin")));
	resp.AddTag(sf);
	ApplySearchUnion(&resp, slots, owner);

	// Not merged, and no index entry left pointing at the slot that refused it.
	ASSERT_TRUE(slots[kSid].raw.empty());
	ASSERT_TRUE(owner.find(72) == owner.end());
}

TEST(Refresher, SearchUnionStillIndexesAResultItAccepts)
{
	// The guard above must not cost the normal path its index entry: that is
	// what attributes every later diffed tag, which carries no search id.
	std::map<std::uint32_t, SearchSlot> slots;
	std::map<std::uint32_t, std::uint32_t> owner;
	slots[kSid];

	CECPacket resp(EC_OP_SEARCH_RESULTS);
	CECTag sf(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(73));
	sf.AddTag(CECTag(EC_TAG_SEARCH_ID, kSid));
	sf.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("kept.bin")));
	resp.AddTag(sf);
	ApplySearchUnion(&resp, slots, owner);

	ASSERT_EQUALS(kSid, owner[73]);
	ASSERT_EQUALS(std::string("kept.bin"), slots[kSid].results[73].name);
}

TEST(Refresher, AMergingFullReplyLeavesTheResyncFlagSet)
{
	// The flag says "a union reply was lost, so this slot may hold rows the
	// daemon has already dropped". Only a replace answers that: a merge
	// re-reads what the daemon still has and cannot remove what it does not.
	// Clearing it on a merge strands those rows for the life of the slot --
	// the tombstones went out in the reply that was lost, and the daemon
	// never mentions them again. The HTTP refresh path (RefreshSearchIfStale)
	// takes exactly this mode, and it serves the same non-active slots the
	// flag is set on.
	std::map<std::uint32_t, SearchSlot> slots;
	std::map<std::uint32_t, std::uint32_t> owner;
	SearchSlot &slot = slots[kSid];
	slot.needs_resync = true;
	slot.raw[80].ecid = 80;
	slot.raw[80].name = "dropped-by-the-daemon.bin";
	owner[80] = kSid;

	// A FULL reply that no longer carries ECID 80. No tombstone: a FULL reply
	// has none to carry.
	CECPacket resp(EC_OP_SEARCH_RESULTS);
	CECTag sf(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(81));
	sf.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("still-here.bin")));
	resp.AddTag(sf);
	ApplySearchFullReply(&resp, slots, owner, kSid, /*replace=*/false);

	// The stale row survives the merge, which is precisely why the obligation
	// must survive with it.
	ASSERT_TRUE(slots[kSid].raw.find(80) != slots[kSid].raw.end());
	ASSERT_TRUE(slots[kSid].needs_resync);
}

TEST(Refresher, AReplacingFullReplyDropsStaleRowsAndClearsTheFlag)
{
	// The re-seed the tick issues. Swapping the rows wholesale is the only
	// way to express a deletion from a reply that carries no tombstones, and
	// having done it the slot is authoritative again.
	std::map<std::uint32_t, SearchSlot> slots;
	std::map<std::uint32_t, std::uint32_t> owner;
	SearchSlot &slot = slots[kSid];
	slot.needs_resync = true;
	slot.raw[80].ecid = 80;
	slot.raw[80].name = "dropped-by-the-daemon.bin";
	slot.results = slot.raw;
	owner[80] = kSid;

	CECPacket resp(EC_OP_SEARCH_RESULTS);
	CECTag sf(EC_TAG_SEARCHFILE, static_cast<std::uint32_t>(81));
	sf.AddTag(CECTag(EC_TAG_PARTFILE_NAME, std::string("still-here.bin")));
	resp.AddTag(sf);
	ApplySearchFullReply(&resp, slots, owner, kSid, /*replace=*/true);

	ASSERT_TRUE(slots[kSid].raw.find(80) == slots[kSid].raw.end());
	ASSERT_TRUE(slots[kSid].results.find(80) == slots[kSid].results.end());
	// The index entry goes with the row, or a later result reusing the ECID
	// resolves through it.
	ASSERT_TRUE(owner.find(80) == owner.end());
	ASSERT_EQUALS(std::string("still-here.bin"), slots[kSid].results[81].name);
	ASSERT_TRUE(!slots[kSid].needs_resync);
}

TEST(Refresher, AnEmptyReplacingReplyClearsTheFoldedView)
{
	// The daemon reporting nothing left is a real answer, and the one an
	// empty-reply merge cannot express: ApplySearchUnion refolds only slots
	// it touched, so without the unconditional refold the stale fold would
	// outlive the rows it was built from.
	std::map<std::uint32_t, SearchSlot> slots;
	std::map<std::uint32_t, std::uint32_t> owner;
	SearchSlot &slot = slots[kSid];
	slot.needs_resync = true;
	slot.raw[80].ecid = 80;
	slot.raw[80].name = "gone.bin";
	slot.results = slot.raw;
	owner[80] = kSid;

	CECPacket resp(EC_OP_SEARCH_RESULTS);
	ApplySearchFullReply(&resp, slots, owner, kSid, /*replace=*/true);

	ASSERT_TRUE(slots[kSid].raw.empty());
	ASSERT_TRUE(slots[kSid].results.empty());
	ASSERT_TRUE(owner.empty());
	ASSERT_TRUE(!slots[kSid].needs_resync);
}

TEST(Refresher, SearchProgressUnionParsesEveryEntry)
{
	CECPacket resp(EC_OP_SEARCH_PROGRESS);
	for (std::uint32_t sid = 1; sid <= 3; ++sid) {
		CECTag entry(EC_TAG_SEARCH_ID, sid);
		entry.AddTag(CECTag(EC_TAG_SEARCH_LIFECYCLE_PERCENT, static_cast<uint8>(sid * 10)));
		entry.AddTag(CECTag(EC_TAG_SEARCH_LIFECYCLE_STATE, static_cast<uint8>(1)));
		resp.AddTag(entry);
	}

	std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> out;
	ASSERT_TRUE(ParseSearchProgressUnion(&resp, out));
	ASSERT_EQUALS(static_cast<size_t>(3), out.size());
	ASSERT_EQUALS(static_cast<std::uint32_t>(20), out[2].first);
	ASSERT_EQUALS(static_cast<std::uint32_t>(1), out[2].second);
}

TEST(Refresher, SearchProgressUnionOmitsExpiredSearch)
{
	// Two tracked searches, one reported: the missing id must simply not be in
	// the map, which is what tells the caller to retire that search.
	CECPacket resp(EC_OP_SEARCH_PROGRESS);
	CECTag entry(EC_TAG_SEARCH_ID, static_cast<std::uint32_t>(7));
	entry.AddTag(CECTag(EC_TAG_SEARCH_LIFECYCLE_PERCENT, static_cast<uint8>(100)));
	entry.AddTag(CECTag(EC_TAG_SEARCH_LIFECYCLE_STATE, static_cast<uint8>(2)));
	resp.AddTag(entry);

	std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> out;
	ASSERT_TRUE(ParseSearchProgressUnion(&resp, out));
	ASSERT_TRUE(out.find(7) != out.end());
	ASSERT_TRUE(out.find(8) == out.end());
}

TEST(Refresher, SearchProgressUnionAcceptsEmptyReply)
{
	// Right opcode, no children: the daemon legitimately holds none of the
	// searches asked about. Accepted, so the caller retires all of them. The
	// discriminator is the opcode, never the child count.
	CECPacket resp(EC_OP_SEARCH_PROGRESS);
	std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> out;
	ASSERT_TRUE(ParseSearchProgressUnion(&resp, out));
	ASSERT_TRUE(out.empty());
}

TEST(Refresher, SearchProgressUnionRejectsFailureReply)
{
	// EC_OP_FAILED must NOT be read as an empty union: that would retire every
	// tracked search at once. Rejecting it sends the caller back to per-id
	// polling, which expires only on an explicit EC_TAG_SEARCH_EXPIRED.
	CECPacket resp(EC_OP_FAILED);
	std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> out;
	ASSERT_TRUE(!ParseSearchProgressUnion(&resp, out));
	ASSERT_TRUE(out.empty());

	ASSERT_TRUE(!ParseSearchProgressUnion(NULL, out));
}

// ----------------------------------------------------------------------
// ParseKadFromPacket — /kad's node_id rides EC_TAG_KAD_ID, a UINT128
// sub-tag of EC_TAG_CONNSTATE that amuled sends only while Kad is
// running. The API contract is 32 LOWERCASE hex chars (the desktop
// panel renders the same value uppercase), and an empty string rather
// than an all-zero id when the sub-tag is absent.
// ----------------------------------------------------------------------

namespace
{

// CEC_ConnState_Tag is core-side (it builds itself from theApp), so
// tests synthesise the wire shape instead: a plain EC_TAG_CONNSTATE
// carrying the flag word, which is exactly what ParseKadFromPacket
// downcasts. Bit 0x04 = connected to Kad, 0x10 = Kad running.
CECTag MakeConnState(std::uint32_t flags)
{
	return CECTag(EC_TAG_CONNSTATE, flags);
}

} // namespace

TEST(Refresher, KadNodeIdDecodesAsLowercaseHex)
{
	static const uint8_t id[16] = {
		0x8f, 0x3a, 0x1c, 0x07, 0xd9, 0x4b, 0x2e, 0x5a, 0x60, 0x18, 0xbb, 0x4c, 0x7f, 0x20, 0x9d, 0x3e
	};

	CECPacket resp(EC_OP_STATS);
	CECTag conn = MakeConnState(0x04 | 0x10);
	conn.AddTag(CECTag(EC_TAG_KAD_ID, CUInt128(id)));
	resp.AddTag(conn);

	KadSnapshot out;
	ParseKadFromPacket(&resp, out);

	ASSERT_EQUALS(std::string("connected"), out.state);
	ASSERT_EQUALS(std::string("8f3a1c07d94b2e5a6018bb4c7f209d3e"), out.node_id);
}

TEST(Refresher, KadNodeIdEmptyWhenTagAbsent)
{
	// Kad not running: amuled omits EC_TAG_KAD_ID entirely. An all-zero
	// id would read as a real identity, so the field must stay empty --
	// and that is exactly the case `state == "disabled"` describes.
	CECPacket resp(EC_OP_STATS);
	resp.AddTag(MakeConnState(0));

	KadSnapshot out;
	ParseKadFromPacket(&resp, out);

	ASSERT_EQUALS(std::string("disabled"), out.state);
	ASSERT_TRUE(out.node_id.empty());
}

TEST(Refresher, KadPublicIpIsOursAndBuddyIpIsSeparate)
{
	// The two addresses in this payload belong to different parties, and
	// they land in differently-named fields -- which is the whole reason
	// ours is `public_ip` and not a bare `ip`. Encoded the way
	// IPv4ToDotted reads them, least-significant byte first: 0x057100CB
	// is 203.0.113.5 and 0x097100CB is 203.0.113.9.
	CECPacket resp(EC_OP_STATS);
	resp.AddTag(MakeConnState(0x04 | 0x10));
	resp.AddTag(CECTag(EC_TAG_STATS_KAD_IP_ADDRESS, static_cast<std::uint32_t>(0x057100CBu)));
	resp.AddTag(CECTag(EC_TAG_STATS_BUDDY_IP, static_cast<std::uint32_t>(0x097100CBu)));

	KadSnapshot out;
	ParseKadFromPacket(&resp, out);

	ASSERT_EQUALS(std::string("203.0.113.5"), out.public_ip);
	ASSERT_EQUALS(std::string("203.0.113.9"), out.buddy_ip);
}

TEST(Refresher, KadBuddyStatusIsNoBuddyWhenTagAbsent)
{
	// amuled ships EC_TAG_STATS_BUDDY_STATUS only while Kad is
	// connected. Absent, the field must still hold one of the values
	// /kad documents -- an empty string is a fourth value outside the
	// enum. The core and amulegui both read the absence as
	// `Disconnected`, so "no_buddy" is the amule-faithful answer.
	CECPacket resp(EC_OP_STATS);
	resp.AddTag(MakeConnState(0));

	KadSnapshot out;
	ParseKadFromPacket(&resp, out);

	ASSERT_EQUALS(std::string("disabled"), out.state);
	ASSERT_EQUALS(std::string("no_buddy"), out.buddy_status);
}

// ----------------------------------------------------------------------
// ParseStatusFromPacket — our own eD2k identity and the four stats
// counters /status reports. Everything below decodes from tags amuled
// already sends in the EC_DETAIL_FULL STAT_REQ response; these tests
// exercise the decode itself, which is where the two sentinels live
// (0xffffffff for "connect in flight" and the free-space -1 that
// arrives on the wire as 0xFFFFFFFFFFFFFFFF).
//
// MakeConnState above supplies the EC_TAG_CONNSTATE flag word; bit
// 0x01 = connected to ed2k, 0x02 = connecting.
// ----------------------------------------------------------------------

TEST(Refresher, StatusHighIdLandsIdAndDottedQuad)
{
	// A HighID *is* our public IPv4 packed LSB-first, so public_ip has to
	// fall out of the id rather than being a second field to trust: 210.2.150.73
	// is 210 | 2<<8 | 150<<16 | 73<<24 == 1234567890, and that identity is
	// exactly what the /status docs promise.
	CECPacket resp(EC_OP_STATS);
	CECTag conn = MakeConnState(0x01);
	conn.AddTag(CECTag(EC_TAG_ED2K_ID, static_cast<std::uint32_t>(1234567890u)));
	resp.AddTag(conn);

	StatusSnapshot out;
	ParseStatusFromPacket(&resp, out);

	ASSERT_EQUALS(std::string("connected"), out.ed2k_state);
	ASSERT_TRUE(out.ed2k_high_id);
	ASSERT_EQUALS(static_cast<std::uint32_t>(1234567890u), out.ed2k_user_id);
	ASSERT_EQUALS(std::string("210.2.150.73"), out.ed2k_public_ip);
}

TEST(Refresher, StatusLowIdKeepsIdButHasNoPublicAddress)
{
	// A LowID is a small number the server picked for a firewalled client
	// and encodes no address at all -- rendering it as a dotted quad would
	// invent one (here it would read "42.0.0.0").
	CECPacket resp(EC_OP_STATS);
	CECTag conn = MakeConnState(0x01);
	conn.AddTag(CECTag(EC_TAG_ED2K_ID, static_cast<std::uint32_t>(42u)));
	resp.AddTag(conn);

	StatusSnapshot out;
	ParseStatusFromPacket(&resp, out);

	ASSERT_EQUALS(std::string("connected"), out.ed2k_state);
	ASSERT_TRUE(!out.ed2k_high_id);
	ASSERT_EQUALS(static_cast<std::uint32_t>(42u), out.ed2k_user_id);
	ASSERT_TRUE(out.ed2k_public_ip.empty());
}

TEST(Refresher, StatusConnectingSentinelNeverReachesTheSnapshot)
{
	// amuled sends 0xffffffff while a connect is in flight
	// (ECSpecialCoreTags.cpp). Surfaced verbatim it would read as a
	// HighID of 255.255.255.255; normalised, the id stays 0.
	CECPacket resp(EC_OP_STATS);
	CECTag conn = MakeConnState(0x01);
	conn.AddTag(CECTag(EC_TAG_ED2K_ID, static_cast<std::uint32_t>(0xffffffffu)));
	resp.AddTag(conn);

	StatusSnapshot out;
	ParseStatusFromPacket(&resp, out);

	ASSERT_EQUALS(static_cast<std::uint32_t>(0u), out.ed2k_user_id);
	ASSERT_TRUE(out.ed2k_public_ip.empty());
}

TEST(Refresher, StatusDisconnectedIsNotReportedAsLowId)
{
	// The bug the high_id rename fixed: with no EC_TAG_ED2K_ID the id reads
	// 0, and HasLowID() is "id < 16777216", so the old negative spelling
	// reported low_id: true for a daemon that simply has no id yet -- a
	// firewall diagnosis out of thin air. Positive sense reads correctly.
	CECPacket resp(EC_OP_STATS);
	resp.AddTag(MakeConnState(0));

	StatusSnapshot out;
	ParseStatusFromPacket(&resp, out);

	ASSERT_EQUALS(std::string("disconnected"), out.ed2k_state);
	ASSERT_TRUE(!out.ed2k_high_id);
	ASSERT_EQUALS(static_cast<std::uint32_t>(0u), out.ed2k_user_id);
	ASSERT_TRUE(out.ed2k_public_ip.empty());
}

TEST(Refresher, StatusOverheadAndFreeSpaceTagsDecode)
{
	CECPacket resp(EC_OP_STATS);
	resp.AddTag(MakeConnState(0x01));
	resp.AddTag(CECTag(EC_TAG_STATS_DOWN_OVERHEAD, static_cast<std::uint32_t>(8700u)));
	resp.AddTag(CECTag(EC_TAG_STATS_UP_OVERHEAD, static_cast<std::uint32_t>(1100u)));
	resp.AddTag(CECTag(EC_TAG_STATS_TEMP_FREE_SPACE, static_cast<std::uint64_t>(48318382080ull)));
	resp.AddTag(CECTag(EC_TAG_STATS_INCOMING_FREE_SPACE, static_cast<std::uint64_t>(24159191040ull)));

	StatusSnapshot out;
	ParseStatusFromPacket(&resp, out);

	ASSERT_EQUALS(static_cast<std::uint64_t>(8700), out.download_overhead_bytes_per_second);
	ASSERT_EQUALS(static_cast<std::uint64_t>(1100), out.upload_overhead_bytes_per_second);
	ASSERT_EQUALS(static_cast<std::int64_t>(48318382080LL), out.temp_free_bytes);
	ASSERT_EQUALS(static_cast<std::int64_t>(24159191040LL), out.incoming_free_bytes);
}

TEST(Refresher, StatusFreeSpaceUnknownSentinelReadsBackAsMinusOne)
{
	// amuled's FREE_SPACE_UNKNOWN is a signed -1 and its EC serializer
	// casts it straight to uint64, so the wire carries
	// 0xFFFFFFFFFFFFFFFF. Read unsigned that becomes 18446744073709551615
	// -- "17 exabytes free", the exact opposite of the truth. The decode
	// has to land -1 so the handlers can emit null.
	CECPacket resp(EC_OP_STATS);
	resp.AddTag(MakeConnState(0x01));
	resp.AddTag(CECTag(EC_TAG_STATS_TEMP_FREE_SPACE, static_cast<std::uint64_t>(0xFFFFFFFFFFFFFFFFull)));
	resp.AddTag(
		CECTag(EC_TAG_STATS_INCOMING_FREE_SPACE, static_cast<std::uint64_t>(0xFFFFFFFFFFFFFFFFull)));

	StatusSnapshot out;
	ParseStatusFromPacket(&resp, out);

	ASSERT_EQUALS(static_cast<std::int64_t>(-1), out.temp_free_bytes);
	ASSERT_EQUALS(static_cast<std::int64_t>(-1), out.incoming_free_bytes);
}

TEST(Refresher, StatusWithoutStatsTagsKeepsZeroOverheadAndUnknownDisk)
{
	// An amuled old enough not to send these tags must still produce a
	// serviceable snapshot: overhead 0 (the documented "daemon reports
	// nothing"), disk -1 so /status answers null rather than 0.
	CECPacket resp(EC_OP_STATS);
	resp.AddTag(MakeConnState(0x01));

	StatusSnapshot out;
	ParseStatusFromPacket(&resp, out);

	ASSERT_EQUALS(static_cast<std::uint64_t>(0), out.download_overhead_bytes_per_second);
	ASSERT_EQUALS(static_cast<std::uint64_t>(0), out.upload_overhead_bytes_per_second);
	ASSERT_EQUALS(static_cast<std::int64_t>(-1), out.temp_free_bytes);
	ASSERT_EQUALS(static_cast<std::int64_t>(-1), out.incoming_free_bytes);
}

// ----------------------------------------------------------------------
// Preference schema domains (#1174).
//
// The schema's bounds and the core's storage limits are two copies of one
// fact, and nothing used to fail when they drifted apart: eight fields
// declared a range wider than what CPreferences could hold, so a PATCH inside
// the declared range was accepted, answered 200, and the value was changed by
// integer division, by a uint8/uint16 wrap, or by a clamp that did not run
// until the next daemon start.
//
// These assert the properties that are checkable from the schema alone. They
// deliberately do not restate each field's numbers -- a test that hardcodes
// the same constants as the table only asserts the table equals a copy of
// itself. What they catch is the SHAPE of the defect.
// ----------------------------------------------------------------------

TEST(PrefsSchema, NumericRowsHaveACoherentDomain)
{
	const webapi::PrefField *schema = webapi::PrefSchema();
	for (std::size_t i = 0; i < webapi::PrefSchemaSize(); ++i) {
		const webapi::PrefField &f = schema[i];
		if (f.type != webapi::PrefType::Uint16 && f.type != webapi::PrefType::Uint32)
			continue;
		const std::string where = std::string(f.category) + "." + f.key;
		ASSERT_TRUE_M(f.min <= f.max, (where + ": min above max"));
		// A Uint16 row whose ceiling does not fit in a uint16 is the
		// max_new_connections_per_5s defect exactly: the declared range says
		// one thing and the wire type says another, and the excess wraps.
		if (f.type == webapi::PrefType::Uint16) {
			ASSERT_TRUE_M(f.max <= 65535u, (where + ": Uint16 row declares a max above 65535"));
		}
		if (f.step) {
			// Both endpoints have to be reachable, or the range advertises
			// values the step forbids.
			ASSERT_TRUE_M((f.max % f.step) == 0, (where + ": max is not a multiple of step"));
			ASSERT_TRUE_M((f.min % f.step) == 0, (where + ": min is not a multiple of step"));
		}
	}
}

TEST(PrefsSchema, AnUnboundedNumericRowIsADeliberateChoice)
{
	// The forcing function. A new numeric row that leaves its ceiling at the
	// full uint32 range fails here until someone puts it on this list, which
	// is the review conversation the eight fields in #1174 never had. Both
	// entries below were resolved to genuine uint32 members in the core during
	// that audit.
	static const char *const kKnownUnbounded[] = {
		"files.min_free_space_mebibytes",
		"remote_controls.webserver.refresh_seconds",
	};
	const webapi::PrefField *schema = webapi::PrefSchema();
	for (std::size_t i = 0; i < webapi::PrefSchemaSize(); ++i) {
		const webapi::PrefField &f = schema[i];
		if (f.type != webapi::PrefType::Uint16 && f.type != webapi::PrefType::Uint32)
			continue;
		if (f.max != 0xFFFFFFFFu)
			continue;
		const std::string where = std::string(f.category) + "." + f.key;
		bool listed = false;
		for (const char *known : kKnownUnbounded) {
			if (where == known) {
				listed = true;
				break;
			}
		}
		ASSERT_TRUE_M(listed,
			(where + ": numeric row is unbounded; give it a real max, or add it to "
				 "kKnownUnbounded once you have checked the core's storage width"));
	}
}
