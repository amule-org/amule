//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//

// Whether a part index amuled reported about a peer may be relayed as an
// index into a given file's part bitmap.
//
// Both predicates are pure and their inputs are all reachable from a crafted
// EC frame, but the values that make them interesting -- the 0xffff sentinel,
// the boundary at part_count, a file with more than 65535 parts, a source
// whose state flips to "downloading" -- are not values a live peer hands out
// on request. So they live here, in a header that pulls in nothing but <string>
// and <cstdint>: no wx, no Boost.Beast, no EC. Same rationale as
// SharedContent.h and CompleteSourcesThrottle.h, and the same reason
// PartIndexTest can drive every branch without a daemon.
//
// The serializing half (which key gets written, and as what) stays in Api.cpp.

#ifndef AMULE_WEBAPI_PARTINDEX_H
#define AMULE_WEBAPI_PARTINDEX_H

#include <cstdint>
#include <string>

namespace webapi
{

//! The core answers 0xffff for "no block pending" (DownloadClient.cpp:
//! `GetNextRequestedPart`), and the desktop substitutes it for the downloading
//! part on a source that is not transferring (GenericClientListCtrl.cpp). It is
//! a sentinel, not a part number, and it has to be tested for by name rather
//! than left to fall out of a bounds check -- see UsablePartIndex.
constexpr std::uint16_t kNoPartPendingSentinel = 0xffffu;

//! The one `download_state` value that means bytes are moving from the peer to
//! us right now (Refresher.cpp: ClientDownloadStateName, mapping
//! DS_DOWNLOADING). Two unrelated places test for it -- the
//! `?activity=downloading` filter in Api.cpp and UsableLastDownloadingPart
//! below -- and they have to agree, so the spelling lives here once.
constexpr const char *kDownloadStateDownloading = "downloading";

//! True when a reported part index can actually address a chunk of a file with
//! `part_count` chunks. Three things make it unusable: the peer never reported
//! one (`present` false), kNoPartPendingSentinel, and any index left over from
//! a peer whose request file is not this one. Relayed raw, either of the last
//! two would draw a stripe on a chunk nothing is happening to.
//!
//! The sentinel is tested by name rather than left to fall out of the bounds
//! check, which only filters it while `part_count <= 0xffff`. Above that -- a
//! file larger than 65535 * kPartSizeBytes, about 637 GB -- 65535 is a real
//! chunk, so the bound alone would pass the sentinel through as an index on
//! exactly the files a source bar is most useful on. Part 65535 of such a file
//! is then unreportable, which is what the desktop already accepts.
inline bool UsablePartIndex(bool present, std::uint16_t part, std::uint64_t part_count)
{
	return present && part != kNoPartPendingSentinel && static_cast<std::uint64_t>(part) < part_count;
}

//! The same test for `last_downloading_part`, plus the download-state guard the
//! bounds check cannot supply: the core initialises m_lastDownloadingPart to 0
//! (BaseClient.cpp: CUpDownClient::Init) and ECSpecialCoreTags.cpp ships it with
//! AddTag rather than AddDiffTag, so it arrives on every frame whether or not
//! the peer is transferring. A connected-but-queued source therefore reports a
//! perfectly in-range 0, indistinguishable from one actually feeding chunk 0 --
//! and since most sources in a list are queued rather than transferring, a
//! renderer would mark chunk 0 as "downloading now" on nearly every row. The
//! desktop guards it the same way (GenericClientListCtrl.cpp: lastDownloadingPart
//! is forced to 0xffff unless GetDownloadState() == DS_DOWNLOADING); doing it in
//! the serializer instead means every API client gets it right once rather than
//! each rediscovering the rule.
//!
//! Takes the state string and the two scalars rather than a ClientSnapshot, so
//! this header needs neither State.h nor anything State.h reaches.
//!
//! Deliberately NOT applied to next_requested_part, exactly as the desktop does
//! not apply it either: 0xffff is that field's own "no block pending" answer, so
//! an idle peer already falls out through UsablePartIndex.
inline bool UsableLastDownloadingPart(
	const std::string &download_state, bool present, std::uint16_t part, std::uint64_t part_count)
{
	return download_state == kDownloadStateDownloading && UsablePartIndex(present, part, part_count);
}

} // namespace webapi

#endif // AMULE_WEBAPI_PARTINDEX_H
