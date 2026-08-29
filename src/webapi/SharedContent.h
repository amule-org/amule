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

// Pure helpers behind GET /api/v0/shared/{hash}/content — the endpoint
// that hands back the bytes of a completed shared file. Everything here
// decides something an attacker can influence: which path on disk we are
// willing to open, how much of it we send, and what we echo back into a
// response header. All four are the kind of decision that wants a test
// per branch rather than a read-through, so they live in their own TU
// that links without wx, Boost.Beast or EC — same rationale as StaticFs
// (see StaticFs.h:12-16), and the same reason SharedContentTest can link
// them standalone.
//
// The transport half (chunked/ranged writing, 206 assembly, the handler
// itself) stays in Api.cpp / HttpServer.cpp, which is where the Beast
// types already are.

#ifndef AMULE_WEBAPI_SHAREDCONTENT_H
#define AMULE_WEBAPI_SHAREDCONTENT_H

#include <cstdint>
#include <string>
#include <vector>

namespace webapi
{

// Join a shared file's on-disk directory with its basename.
//
// This exists because no single field carries the full path: the API's
// `path` on a shared file is the *directory* (it comes straight from
// EC_TAG_KNOWNFILE_PATH) and `name` is the bare filename. Every caller
// that wants to open the file has to put the two together, and doing it
// inline is exactly where a separator ends up doubled or a `..` ends up
// honoured.
//
// `name` must be a bare basename. A name carrying a separator, or `.` /
// `..`, is either corrupt local state or a hostile filename that reached
// us over ed2k, and there is no benign reading of either — both are
// rejected rather than sanitised, because sanitising invents a path the
// caller never asked for. An embedded NUL is rejected for the same
// reason it always is: std::string keeps it, the C path APIs do not, so
// the string we validate would not be the path we open.
//
// Returns false without touching `out` on any rejection.
bool JoinSharedPath(const std::string &dir, const std::string &name, std::string &out);

// Resolve a shared file to a canonical path that provably sits inside at
// least one configured share root, or fail.
//
// `roots` is a vector because aMule's share is a *list* of directories,
// not one root — the user adds each shared directory separately, so
// containment is "inside any one of them", not "inside the one".
//
// Any failure — bad join, missing file, canonical path outside every
// root — returns false, and the caller is expected to answer with the
// same opaque 404 for all of them, so the on-disk layout stays
// non-enumerable from outside (same discipline as StaticFs.h:28-33).
bool ResolveSharedContentPath(const std::vector<std::string> &roots,
	const std::string &dir,
	const std::string &name,
	std::string &fs_out);

// Outcome of parsing a Range request header.
enum class RangeResult
{
	kAbsent,        // No Range header at all — serve 200 with the whole file.
	kIgnore,        // Present but unsupported/malformed — serve 200 with the whole file.
	kUnsatisfiable, // Syntactically fine, but no byte of it exists — 416.
	kOk             // Serve 206 for [first_out, last_out].
};

// Parse a *single* byte range out of a Range header value.
//
// `last_out` is INCLUSIVE, matching the wire format, so the byte count
// is (last - first + 1).
//
// RFC 7233 §3.1 permits a server to ignore a Range header it does not
// wish to honour, and that permission is doing real work here: any range
// set with more than one range returns kIgnore and gets a plain 200.
// That is the deliberate neutralisation of the CVE-2011-3192 shape (the
// "Apache Killer"), where a few hundred overlapping ranges in one header
// make the server materialise a multipart body far larger than the file.
// We never assemble multipart/byteranges at all, so there is nothing to
// amplify. It is not an error either — an error would tempt a client
// into retrying the same header.
RangeResult ParseSingleByteRange(const std::string &header_value,
	std::uint64_t file_size,
	std::uint64_t &first_out,
	std::uint64_t &last_out);

// Build a complete Content-Disposition header *value* per RFC 6266.
//
// These filenames arrive from strangers on the ed2k network, which makes
// this the one place in the endpoint where remote input is written into
// a response header. A name containing CR or LF would end the header and
// let the sender append arbitrary headers — or a whole second response —
// so the quoted form is reduced to a conservative printable-ASCII subset
// and the filename* form percent-encodes everything outside RFC 6266's
// attr-char set. Neither form can carry a byte that terminates a header.
//
// Always `attachment`, never `inline`: both the bytes and the name are
// attacker-chosen, amuleapi serves the Web UI from this same origin, and
// there is currently no Content-Security-Policy anywhere in src/webapi.
// `inline` would therefore let a shared .html file script the Web UI's
// origin. `attachment` costs a download prompt and closes that off.
std::string BuildContentDisposition(const std::string &filename);

// "mtime-size" hex ETag, quoted — the same shape BuildStaticEtag
// produces (src/webapi/Api.cpp:501-507), so the dispatcher's
// ETag/If-None-Match/304 machinery behaves identically for this route.
//
// The reason to compute one at all is that CApiDispatcher::Dispatch
// otherwise MD5s the entire response body to derive a validator
// (State.cpp:530-533 + Api.cpp:854). For a multi-GB shared file that is
// not a slow path, it is an unusable one. A handler-set ETag makes
// Dispatch step aside (Api.cpp:825-828), so we pay a stat() instead of a
// full-file hash.
std::string BuildContentEtag(std::uint64_t mtime, std::uint64_t size);

// Does an If-Range precondition permit the accompanying Range to be
// honoured? `etag` is the quoted wire form BuildContentEtag returns.
//
// RFC 9110 §13.1.5, and the reason it is not optional: a client resuming
// an interrupted download sends the validator it already holds next to
// `Range: bytes=N-`, precisely so that a representation which changed
// underneath it answers 200 with the whole new file instead of a window
// of it. A server that honours the Range regardless returns a 206 of the
// NEW bytes, and the client — reading that 206 as confirmation its
// validator held — appends them to its copy of the OLD representation.
// The result is a file spliced from two versions that passes every
// length check the client can make. Immutability makes that rare here,
// not impossible, and silent rarely-wrong is the worst failure shape
// there is.
//
// STRONG comparison, which is the one thing this cannot borrow from
// webcommon::IfNoneMatchHits: that function accepts the weak `W/"..."`
// form, correctly, because If-None-Match asks whether two
// representations are equivalent. A byte range needs them
// byte-identical, so a weak validator must never match here. Reusing it
// would be the bug.
//
// An absent (empty) header returns true: no precondition, nothing to
// fail. A non-matching one returns false, and the caller is expected to
// drop the Range and serve the whole representation — §13.1.5 says to
// ignore the Range, not to reject the request.
bool IfRangeAllowsRange(const std::string &if_range, const std::string &etag);

} // namespace webapi

#endif // AMULE_WEBAPI_SHAREDCONTENT_H
