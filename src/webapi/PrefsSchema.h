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
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA
//

#ifndef WEBAPI_PREFSSCHEMA_H
#define WEBAPI_PREFSSCHEMA_H

#include <cstddef>
#include <cstdint>

#include "ECCodes.h" // ec_tagname_t
#include "State.h"

namespace webapi
{

// One declarative description of every field on /api/v0/preferences, used by
// all three code paths that touch them: the EC decode (Refresher), the GET
// emitter and the PATCH applier (Api). Before this table each field was
// spelled out three times in three different idioms, which is how a rename
// could land in two places and be missed in the third.
//
// Adding a preference is one row. Renaming one is one token. Changing a
// field's polarity is flipping `invert` -- it is data here, not code.
//
// The table deliberately does NOT describe the EC protocol, only how the API
// maps onto it: tag numbers, presence-vs-value encodings and the one
// inverted tag are properties of EC and are recorded, not chosen, here.

enum class PrefType
{
	Bool,        // JSON bool
	Uint16,      // JSON number, u16 member
	Uint32,      // JSON number, u32 member
	String,      // JSON string
	StringArray, // JSON array of strings, packed as EC_TAG_STRING children
	Enum,        // JSON string drawn from `enum_names`, EC carries the index
	Md4Hex,      // read-only hex rendering of an EC MD4 hash (general.user_hash)
};

// How EC encodes a boolean *on the read path*. The core serializer emits most
// bools as a bare CECEmptyTag only when true (presence == true) but a handful
// as a value tag every time. Writes are always value tags: amuleapi sends
// SET_PREFERENCES at EC_DETAIL_FULL, where amuled's ApplyBoolean reads
// GetInt() != 0. Only `Bool` rows consult this.
enum class PrefEnc
{
	Presence,
	Value,
};

enum class PrefAccess
{
	ReadWrite, // emitted on GET, applied on PATCH
	ReadOnly,  // emitted on GET, silently ignored on PATCH (capabilities, live status)
	WriteOnly, // never emitted, applied on PATCH (passwords, triggers)
	Rejected,  // never emitted, 400 if sent -- the field belongs to another endpoint
	Bespoke,   // emitted on GET, PATCH handled by dedicated code (see below)
};

// `Bespoke` exists for exactly one field. remote_controls.webserver.guest_enabled
// and its guest_password share a single EC tag (EC_TAG_WEBSERVER_GUEST carries
// the enable bool as its value and the password hash as a child), so there is no
// 1:1 field-to-tag mapping for the table to express. Its GET emission is still
// table-driven; only the PATCH packing is hand-written.

struct PrefField
{
	const char *category; // dotted path, e.g. "files" or "remote_controls.webserver"
	const char *key;      // JSON key within that category
	ec_tagname_t tag;
	PrefType type;
	PrefEnc enc;
	bool invert; // API value is the negation of the EC value (read and write)
	PrefAccess access;
	std::uint32_t max; // inclusive upper bound for Uint16 / Uint32
	// Enum only: nullptr-terminated, in wire order -- a name's index is the
	// integer the daemon's Apply() casts back to its own enum.
	const char *const *enum_names;
	// Capability key in the same category that must not be false, else 409.
	const char *gated_by;
	// Address of the backing member. The row's PrefType fixes the cast; the
	// PREF_* macros static_assert the two agree, so a mismatch is a build error.
	void *(*member)(PreferencesSnapshot &);
	// EC group this field's tag actually lives in, when that is not the group
	// its JSON category maps to. 0 means "the category's own group". Exactly
	// one field needs it: connection.upnp_available is a daemon capability the
	// core serializes into [General], but the API surfaces it next to the other
	// UPnP settings under `connection`.
	ec_tagname_t read_group;
	// Divisor between the EC value and the API value, when the two use
	// different units. 0 means "same unit", which is every row but three.
	//
	// The core stores these three as whole minutes and its accessors
	// multiply by 60000 on the way out (Preferences.h:
	// `s_sourceReaskMins * 60000`), so EC carries milliseconds that are
	// always a multiple of 60000. Exposing that verbatim meant a client
	// writing 90000 read back 60000 and one writing 30000 read back 0 --
	// accepted, reported as success, changed underneath. The API therefore
	// speaks the unit the daemon can actually hold, and converts here.
	std::uint32_t ec_scale;
	// Inclusive lower bound for Uint16 / Uint32, checked with `max` against the
	// value the caller sent. 0 for a row with no floor, which is most of them.
	//
	// `max` alone is not a domain. A core member narrower than the declared
	// ceiling wraps, and a setter that divides truncates, so a value inside
	// [0, max] can still be rewritten on the way in -- and three of these
	// fields are clamped by CPreferences::LoadAllItems() at the NEXT daemon
	// start, which no amount of read-back checking after the PATCH can see.
	// The bound belongs here, declaratively, where it cannot race a snapshot.
	std::uint32_t min;
	// Granularity the core can actually store, when its setter divides. A value
	// that is not a multiple is a 400 naming the step, rather than a silent
	// truncation: SetFileBufferSize() is `val / 15000` into a uint8, so 20000
	// becomes 15000 and 14999 becomes 0. 0 means the row is not quantised.
	//
	// Rejecting rather than renaming the field to its stored unit is deliberate
	// and is the opposite of what #1159 chose for the three `_minutes` rows.
	// Those quantised to a unit a user already thinks in, so the rename cost
	// nothing; nobody thinks in 15000-byte blocks, and `file_buffer_blocks`
	// would push an implementation detail into the API's vocabulary.
	std::uint32_t step;
};

// EC group tag each category packs into. Two categories intentionally share
// one group (remote_controls.webserver / .amuleapi both live in
// EC_TAG_PREFS_REMOTECTRL): the JSON nesting is an API shape, not an EC one.
struct PrefCategory
{
	const char *name;
	ec_tagname_t group_tag;
};

const PrefField *PrefSchema();
std::size_t PrefSchemaSize();
const PrefCategory *PrefCategories();
std::size_t PrefCategoryCount();

// Convenience for callers that walk a single category.
ec_tagname_t PrefGroupTagFor(const char *category);

} // namespace webapi

#endif // WEBAPI_PREFSSCHEMA_H
