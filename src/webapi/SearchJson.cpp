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

#include "SearchJson.h"

#include "State.h"

#include <JsonWriter.h>

#include <wx/string.h>

namespace webapi
{

void WriteSearchResultFields(CJsonWriter &w, const SearchResult &r)
{
	w.Key("hash");
	w.ValueString(wxString::FromUTF8(r.hash.c_str()));
	w.Key("name");
	w.ValueString(wxString::FromUTF8(r.name.c_str()));
	w.Key("size");
	w.ValueInt(static_cast<int64_t>(r.size));
	w.Key("sources");
	w.BeginObject();
	w.Key("total");
	w.ValueInt(static_cast<int64_t>(r.source_count));
	w.Key("complete");
	w.ValueInt(static_cast<int64_t>(r.complete_source_count));
	w.EndObject();
	w.Key("already_have");
	w.ValueBool(r.already_have);
	w.Key("rating");
	w.ValueInt(static_cast<int64_t>(r.rating));
	w.Key("status");
	w.ValueString(wxString::FromUTF8(r.status.c_str()));
	w.Key("type");
	w.ValueString(wxString::FromUTF8(r.type.c_str()));
	// Browse-only (the folder inside the peer's share). Always present so
	// clients need no presence check; empty on every server/Kad hit, which
	// never carries the tag.
	w.Key("directory");
	w.ValueString(wxString::FromUTF8(r.directory.c_str()));
	// Media metadata (issue #430) -- same shape as the file-detail `media`
	// object, and null when the hit carries no media tags. null rather than
	// omitted so the key is always present: this is the one place the
	// unknown-value rule reaches an object instead of a scalar, so a client
	// tests `media === null` before reaching into it.
	if (r.has_media) {
		w.Key("media");
		w.BeginObject();
		w.Key("length_s");
		w.ValueInt(static_cast<int64_t>(r.media.length_s));
		w.Key("bitrate");
		w.ValueInt(static_cast<int64_t>(r.media.bitrate));
		w.Key("codec");
		w.ValueString(wxString::FromUTF8(r.media.codec.c_str()));
		w.Key("artist");
		w.ValueString(wxString::FromUTF8(r.media.artist.c_str()));
		w.Key("album");
		w.ValueString(wxString::FromUTF8(r.media.album.c_str()));
		w.Key("title");
		w.ValueString(wxString::FromUTF8(r.media.title.c_str()));
		w.EndObject();
	} else {
		w.Key("media");
		w.ValueNull();
	}
	// Result grouping (issue #431): the same-hash/same-size hit's
	// alternative filenames. Always emitted (empty array when the hit was
	// seen under a single name) so clients can render the expandable tree
	// without a presence check. Each child shares the parent's `hash`; the
	// distinct `ecid` selects it for download-under-that-name (see
	// POST /search/results/{hash}/download).
	w.Key("children");
	w.BeginArray();
	for (const auto &c : r.children) {
		w.BeginObject();
		w.Key("ecid");
		w.ValueInt(static_cast<int64_t>(c.ecid));
		w.Key("name");
		w.ValueString(wxString::FromUTF8(c.name.c_str()));
		w.Key("hash");
		w.ValueString(wxString::FromUTF8(c.hash.c_str()));
		w.Key("sources");
		w.BeginObject();
		w.Key("total");
		w.ValueInt(static_cast<int64_t>(c.source_count));
		w.Key("complete");
		w.ValueInt(static_cast<int64_t>(c.complete_source_count));
		w.EndObject();
		w.Key("directory");
		w.ValueString(wxString::FromUTF8(c.directory.c_str()));
		w.EndObject();
	}
	w.EndArray();
	// On-demand Kad community ratings/comments (issue #434). `kad_comment_search_running`
	// is true while a lookup started via POST /search/results/{hash}/comments is
	// in flight; `comments` carries the Kad notes retrieved so far (empty until
	// then). Both are always present so clients need no presence check.
	w.Key("kad_comment_search_running");
	w.ValueBool(r.kad_comment_searching);
	w.Key("comments");
	w.BeginArray();
	for (const auto &c : r.comments) {
		w.BeginObject();
		w.Key("username");
		w.ValueString(wxString::FromUTF8(c.username.c_str()));
		w.Key("filename");
		w.ValueString(wxString::FromUTF8(c.filename.c_str()));
		w.Key("rating");
		w.ValueInt(static_cast<int64_t>(c.rating));
		w.Key("comment");
		w.ValueString(wxString::FromUTF8(c.comment.c_str()));
		w.EndObject();
	}
	w.EndArray();
}

} // namespace webapi
