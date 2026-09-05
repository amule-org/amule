//								-*- C++ -*-
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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

#ifndef __KAD_AICH_HASH_LIST_H__
#define __KAD_AICH_HASH_LIST_H__

#include <array>
#include <cstddef>
#include <vector>

#include "../../Types.h"

////////////////////////////////////////
namespace Kademlia
{
////////////////////////////////////////

// Size of an AICH root hash in bytes.  Mirrors HASHSIZE in SHAHashSet.h and is
// restated here on purpose: keeping this translation unit free of the AICH
// hash-set machinery is what makes the codec unit-testable on its own.
const size_t KAD_AICH_HASH_SIZE = 20;

typedef std::array<uint8_t, KAD_AICH_HASH_SIZE> CKadAICHHash;

// The AICH root hashes reported for one indexed keyword entry, each with the
// number of publishers that reported it.
//
// Kad protocol version 0x09 added AICH hashes to keyword storage.  A publisher
// at 0x09 or above sends its file's AICH root hash in TAG_KADAICHHASHPUB
// ("\x36", BSOB of KAD_AICH_HASH_SIZE bytes) inside KADEMLIA2_PUBLISH_KEY_REQ;
// the indexing node accumulates those hashes per entry and reports them back in
// TAG_KADAICHHASHRESULT ("\x37", BSOB) on KADEMLIA2_SEARCH_RES.  The publisher
// count is the interesting part: an honest file has exactly one AICH hash, so
// several competing hashes -- or one hash with a single publisher against a
// popular file -- is the signal a searcher wants.
//
// Slots are never removed once created, because publishers hold their hash by
// index; DropReferenceAt() only decrements the popularity counter, and
// BuildCompactionMap() renumbers when the entry is written to disk.
class CKadAICHHashList
{
public:
	// The index value meaning "this publisher reported no AICH hash".  It is
	// the same 0xFFFF sentinel that travels in the on-disk keyword index.
	// Never bound to a reference: see the copy at the fill constructor in
	// BuildCompactionMap(), which keeps this free of an out-of-line definition.
	static constexpr uint16_t INVALID_INDEX = 0xFFFF;

	// Kad BSOB tags carry a uint8 length.  eMule holds the encoded
	// TAG_KADAICHHASHRESULT payload to 250 bytes rather than the full 255,
	// which caps the carried hashes at MAX_RESULT_HASHES:
	//   1 + (1 + 20) * 11 = 232 bytes, whereas 12 hashes would need 253.
	// The cap costs nothing in practice -- a file with more than a handful
	// of competing AICH hashes is unusable anyway.
	static const size_t MAX_RESULT_TAG_SIZE = 250;
	static const uint8_t MAX_RESULT_HASHES = 11;

	struct SResultHash
	{
		uint8_t m_popularity;
		CKadAICHHash m_hash;
	};

	// Adds one reference to `hash`, creating a slot for it if needed, and
	// returns its index.  Popularity saturates at 255 because it travels the
	// wire as a uint8.
	uint16_t AddReference(const CKadAICHHash &hash);

	// Drops one reference from the slot at `index`.  Out-of-range indexes
	// (including INVALID_INDEX) and already-unreferenced slots are ignored.
	void DropReferenceAt(uint16_t index);

	uint16_t GetSlotCount() const { return (uint16_t)m_hashes.size(); }
	uint16_t GetReferencedCount() const;
	bool IsEmpty() const { return GetReferencedCount() == 0; }

	// Both are only valid for `index` < GetSlotCount(); an out-of-range
	// index yields 0 / a zeroed hash rather than undefined behaviour.
	uint8_t GetPopularityAt(uint16_t index) const;
	const CKadAICHHash &GetHashAt(uint16_t index) const;

	// Maps each current slot index onto the index it will occupy once
	// unreferenced slots are dropped, or INVALID_INDEX if it is dropped.
	// Used when writing the keyword index to disk, so the stored publisher
	// indexes stay consistent with the stored hashes.
	std::vector<uint16_t> BuildCompactionMap() const;

	// Encodes the referenced hashes as a TAG_KADAICHHASHRESULT payload:
	//   <Count 1>{<Publishers 1><AICH Hash KAD_AICH_HASH_SIZE>} Count
	// Returns an empty buffer when nothing is referenced, so callers can
	// skip the tag entirely.
	std::vector<uint8_t> EncodeResultTag() const;

	// Decodes a TAG_KADAICHHASHRESULT payload.  Returns false and leaves
	// `out` empty on any malformed input: a short buffer, a hash count above
	// MAX_RESULT_HASHES, or a truncated final hash.  Entries claiming zero
	// publishers are dropped, and trailing bytes past the announced count
	// are ignored.
	static bool DecodeResultTag(const uint8_t *data, size_t length, std::vector<SResultHash> &out);

	// The hash the most publishers agreed on, or NULL for an empty list.
	// Ties resolve to the first entry, which is the order the sender chose.
	static const SResultHash *GetMostPopular(const std::vector<SResultHash> &hashes);

	// Whether a peer advertising `peerKadVersion` handles AICH hashes on
	// keyword storage.  Both directions consult this: TAG_KADAICHHASHPUB is
	// only sent to a peer that passes, and TAG_KADAICHHASHRESULT is only
	// honoured from a sender that passes.  A version of 0 means the peer is
	// unknown to us, which fails -- an unidentified sender gets no more
	// credit than a pre-0x09 one.
	static bool PeerSupportsAICHKeywordStorage(uint8_t peerKadVersion);

private:
	std::vector<CKadAICHHash> m_hashes;
	std::vector<uint8_t> m_popularity;
};

} // namespace Kademlia

#endif // __KAD_AICH_HASH_LIST_H__
