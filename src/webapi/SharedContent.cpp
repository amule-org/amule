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

#include "SharedContent.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits.h>
#include <sstream>
#include <sys/stat.h>

#ifdef _WIN32
#include <stdlib.h>
#endif

namespace webapi
{

namespace
{

#ifdef _WIN32
const char kPathSep = '\\';
#else
const char kPathSep = '/';
#endif

// Name handed out when the remote filename yields nothing we are willing
// to echo. Deliberately boring: the alternative is an empty filename,
// which sends the client back to guessing a name from the URL — and the
// URL's last segment is the literal string "content".
const char *const kFallbackName = "download";

bool ContainsNul(const std::string &s)
{
	return s.find('\0') != std::string::npos;
}

// Parse an unsigned decimal with an explicit overflow guard.
//
// Not strtoull(): it signals overflow through errno plus a saturated
// ULLONG_MAX, which callers forget to check and which cannot be told
// apart from a genuine ULLONG_MAX. For a range bound, saturating is
// worse than wrapping — "bytes=<30 digits>-" would silently become a
// perfectly satisfiable read near EOF. It has to be a rejection.
//
// Requires at least one digit and rejects every non-digit byte, so
// signs, whitespace and hex prefixes fail here instead of being
// half-consumed.
bool ParseU64(const std::string &s, std::uint64_t &out)
{
	if (s.empty())
		return false;
	std::uint64_t v = 0;
	for (const char c : s) {
		if (c < '0' || c > '9')
			return false;
		const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
		// Checked *before* the multiply, so nothing ever wraps.
		if (v > (UINT64_MAX - digit) / 10)
			return false;
		v = v * 10 + digit;
	}
	out = v;
	return true;
}

// Strip the optional whitespace HTTP allows around a field value.
std::string TrimOws(const std::string &s)
{
	std::size_t b = 0;
	std::size_t e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t'))
		++b;
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t'))
		--e;
	return s.substr(b, e - b);
}

// Canonicalise `candidate` (an ABSOLUTE path) and accept it only if it
// names a regular file.
//
// Split from the containment test below, and deliberately: the candidate
// is the same path whichever root it is being compared against, but
// realpath() is a full path walk that stat()s every component. Recomputing
// it per root made an N-root share pay up to 2N of those walks on every
// range request, and the category paths just raised N. Resolving once and
// comparing the result against each root decides exactly the same thing —
// the boundary rule below is a pure string comparison over two paths that
// are already canonical.
//
// The regular-file test rides along here rather than after containment
// because it is a property of the candidate alone. Running it before the
// roots are walked cannot change the verdict: a non-regular file is
// refused whichever root it sits under, and the refusal is the same
// opaque false either way.
bool CanonicaliseRegularFile(const std::string &candidate, std::string &fs_out)
{
	if (candidate.empty())
		return false;

	char fs_real[PATH_MAX];
#ifdef _WIN32
	// _fullpath() is lexical-only (no reparse-point resolution). Same
	// trade-off StaticFs documents: on Windows symlinks require
	// elevation, so lexical containment covers the operator-misconfig
	// case this check targets.
	if (!_fullpath(fs_real, candidate.c_str(), PATH_MAX))
		return false;
#else
	if (!realpath(candidate.c_str(), fs_real))
		return false;
#endif

	// A directory (or a device, or a fifo) is not content. Checked here
	// rather than left to the caller's open() so that "it exists but is
	// not servable" produces the same opaque failure as everything else.
	struct stat st
	{
	};
	if (::stat(fs_real, &st) != 0)
		return false;
	if (!S_ISREG(st.st_mode))
		return false;

	fs_out.assign(fs_real);
	return true;
}

// Does the already-canonical `fs_real` land inside `root`?
//
// StaticFs::ResolveWithinRoot cannot be reused here: it joins root and
// the caller's path (`root_real + "/" + rel`), which is exactly right
// for a URL path but produces "/srv/share//srv/share/f" for an absolute
// candidate. The shared-file case has no relative form to offer — the
// directory arrives from EC_TAG_KNOWNFILE_PATH already absolute, and
// making it relative would mean a lexical prefix strip, i.e. deciding
// containment before checking it.
//
// So this is a sibling, not a weakening: same realpath()-based
// canonicalisation, and byte-for-byte the same boundary rule (the
// character at root_len must be a separator or the terminator, which is
// what stops "/srv/share-evil" from passing as "/srv/share").
bool CanonicalWithinRoot(const std::string &root, const std::string &fs_real)
{
	if (root.empty() || fs_real.empty())
		return false;

	char root_real[PATH_MAX];
#ifdef _WIN32
	if (!_fullpath(root_real, root.c_str(), PATH_MAX))
		return false;
#else
	if (!realpath(root.c_str(), root_real))
		return false;
#endif

	// _fullpath() preserves a trailing separator from its input, which
	// then breaks the prefix comparison; POSIX realpath() strips them.
	// Normalise so the boundary check is platform-agnostic.
	std::size_t root_len = std::strlen(root_real);
	while (root_len > 1 && (root_real[root_len - 1] == '/' || root_real[root_len - 1] == '\\')) {
		root_real[--root_len] = '\0';
	}
	if (fs_real.compare(0, root_len, root_real, root_len) != 0)
		return false;
	// Reads the NUL terminator when the paths are equal in length, which
	// is the "the candidate IS the root" case and exactly as intended.
	const char sep = fs_real.c_str()[root_len];
	if (sep != '/' && sep != '\\' && sep != '\0')
		return false;

	return true;
}

// RFC 5987 attr-char, the set RFC 6266's filename* may carry unencoded.
// Everything else — including every byte of a UTF-8 sequence, and every
// byte that could terminate a header — becomes %XX.
bool IsAttrChar(unsigned char c)
{
	if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
		return true;
	switch (c) {
	case '!':
	case '#':
	case '$':
	case '&':
	case '+':
	case '-':
	case '.':
	case '^':
	case '_':
	case '`':
	case '|':
	case '~':
		return true;
	default:
		return false;
	}
}

// Reduce a remote filename to something safe to sit between quotes in a
// header. Two different treatments on purpose:
//
//  - Control characters and DEL are DROPPED. They carry no meaning in a
//    filename, and dropping them is what lets an all-control name
//    collapse to empty and trigger the fallback below.
//  - Structural characters (quote, backslash, both path separators) are
//    REPLACED with '_', so the name keeps its shape and length. A quote
//    would close filename="..." early; a separator is honoured as a path
//    by some clients.
//  - Non-ASCII is replaced too: the quoted form has no encoding, so a
//    raw UTF-8 byte there is at the mercy of the client's guess. The
//    real name travels in filename*.
std::string SanitiseQuotedName(const std::string &filename)
{
	std::string out;
	out.reserve(filename.size());
	for (const char raw : filename) {
		const unsigned char c = static_cast<unsigned char>(raw);
		if (c < 0x20 || c == 0x7F)
			continue;
		if (c >= 0x80 || c == '"' || c == '\\' || c == '/') {
			out.push_back('_');
			continue;
		}
		out.push_back(raw);
	}
	return out;
}

std::string PercentEncodeAttrChars(const std::string &filename)
{
	static const char kHex[] = "0123456789ABCDEF";
	std::string out;
	out.reserve(filename.size());
	for (const char raw : filename) {
		const unsigned char c = static_cast<unsigned char>(raw);
		if (IsAttrChar(c)) {
			out.push_back(raw);
			continue;
		}
		out.push_back('%');
		out.push_back(kHex[c >> 4]);
		out.push_back(kHex[c & 0x0F]);
	}
	return out;
}

} // namespace

bool JoinSharedPath(const std::string &dir, const std::string &name, std::string &out)
{
	if (dir.empty() || name.empty())
		return false;
	// An embedded NUL makes the string we validate a different thing
	// from the path we would open, so nothing after this point can be
	// trusted about it.
	if (ContainsNul(dir) || ContainsNul(name))
		return false;
	// `name` is a basename by contract. Rejected rather than sanitised:
	// stripping the directory part would invent a path the caller never
	// asked about, and there is no benign source for a separator here.
	if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
		return false;
	if (name == "." || name == "..")
		return false;

	std::string joined = dir;
	const char tail = joined[joined.size() - 1];
	if (tail != '/' && tail != '\\')
		joined.push_back(kPathSep);
	joined += name;

	out.swap(joined);
	return true;
}

bool ResolveSharedContentPath(const std::vector<std::string> &roots,
	const std::string &dir,
	const std::string &name,
	std::string &fs_out)
{
	std::string candidate;
	if (!JoinSharedPath(dir, name, candidate))
		return false;

	// Resolved ONCE, before the roots are walked, rather than inside the
	// loop: this is the walk, and the roots only ever compare against its
	// result.
	std::string fs_real;
	if (!CanonicaliseRegularFile(candidate, fs_real))
		return false;

	// Inside ANY root is enough — aMule's share is a list of separately
	// added directories, so there is no single tree to be inside of. An
	// empty list therefore shares nothing, which is the correct reading
	// of "the user has configured no shares".
	for (const std::string &root : roots) {
		if (CanonicalWithinRoot(root, fs_real)) {
			fs_out.swap(fs_real);
			return true;
		}
	}
	return false;
}

RangeResult ParseSingleByteRange(const std::string &header_value,
	std::uint64_t file_size,
	std::uint64_t &first_out,
	std::uint64_t &last_out)
{
	const std::string value = TrimOws(header_value);
	if (value.empty())
		return RangeResult::kAbsent;

	const std::size_t eq = value.find('=');
	if (eq == std::string::npos)
		return RangeResult::kIgnore;

	// bytes-unit is a token and tokens are case-insensitive. Anything
	// else is a unit we do not implement; ignoring it serves the whole
	// file, which is always a correct answer.
	std::string unit = TrimOws(value.substr(0, eq));
	for (char &c : unit) {
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	}
	if (unit != "bytes")
		return RangeResult::kIgnore;

	const std::string set = TrimOws(value.substr(eq + 1));
	if (set.empty())
		return RangeResult::kIgnore;

	// See the header: any multi-range set is ignored outright, which is
	// what makes the CVE-2011-3192 amplification shape a no-op here.
	if (set.find(',') != std::string::npos)
		return RangeResult::kIgnore;

	const std::size_t dash = set.find('-');
	if (dash == std::string::npos)
		return RangeResult::kIgnore;
	// A second dash means this was never one byte-range-spec.
	if (set.find('-', dash + 1) != std::string::npos)
		return RangeResult::kIgnore;

	const std::string first_str = set.substr(0, dash);
	const std::string last_str = set.substr(dash + 1);

	// Deliberate whitespace policy: RFC 7233's byte-range-spec contains
	// no OWS at all — the only OWS the grammar allows is around the
	// commas of the `#rule`, and we reject every comma anyway. So the
	// tolerance stops at the field value's own edges (already trimmed
	// above, and stripped by HTTP field parsing regardless) and at the
	// "=" separating the unit. Inside the spec, ParseU64's digits-only
	// rule rejects any space, which is what makes "bytes=0 - 9"
	// unparseable rather than quietly read as "bytes=0-9".

	if (first_str.empty()) {
		// suffix-byte-range-spec: "-N" means the last N bytes.
		std::uint64_t suffix = 0;
		if (!ParseU64(last_str, suffix))
			return RangeResult::kIgnore;
		if (file_size == 0 || suffix == 0)
			return RangeResult::kUnsatisfiable;
		if (suffix >= file_size) {
			// Asking for more trailing bytes than exist is satisfied
			// by the whole file, not refused (RFC 7233 §2.1).
			first_out = 0;
		} else {
			first_out = file_size - suffix;
		}
		last_out = file_size - 1;
		return RangeResult::kOk;
	}

	std::uint64_t first = 0;
	if (!ParseU64(first_str, first))
		return RangeResult::kIgnore;

	if (last_str.empty()) {
		// "N-": to the end of the file.
		if (file_size == 0 || first >= file_size)
			return RangeResult::kUnsatisfiable;
		first_out = first;
		last_out = file_size - 1;
		return RangeResult::kOk;
	}

	std::uint64_t last = 0;
	if (!ParseU64(last_str, last))
		return RangeResult::kIgnore;
	// first > last is well-formed but meaningless, which RFC 7233 §2.1
	// makes an invalid byte-range-set rather than an unsatisfiable one —
	// so it is a 200, not a 416. A 416 would invite the client to retry
	// the same header after re-reading Content-Range.
	if (first > last)
		return RangeResult::kIgnore;
	if (file_size == 0 || first >= file_size)
		return RangeResult::kUnsatisfiable;

	first_out = first;
	last_out = (last >= file_size) ? (file_size - 1) : last;
	return RangeResult::kOk;
}

std::string BuildContentDisposition(const std::string &filename)
{
	std::string quoted = SanitiseQuotedName(filename);
	// If nothing printable survived, both forms name the file rather
	// than leaving the client to invent something from the URL. Using
	// the fallback for filename* too keeps the two halves agreeing —
	// a client that prefers the extended form should not end up with a
	// different name than one that does not.
	const bool usable = !quoted.empty();
	if (!usable)
		quoted = kFallbackName;
	const std::string extended = usable ? PercentEncodeAttrChars(filename) : std::string(kFallbackName);

	// `attachment` unconditionally — see the header for why `inline` is
	// not an option on this route.
	return "attachment; filename=\"" + quoted + "\"; filename*=UTF-8''" + extended;
}

std::string BuildContentEtag(std::uint64_t mtime, std::uint64_t size)
{
	// Same "mtime-size" hex shape BuildStaticEtag emits (Api.cpp:501-507),
	// strong-form quoted per RFC 7232.
	std::ostringstream oss;
	oss << '"' << std::hex << mtime << '-' << size << '"';
	return oss.str();
}

bool IfRangeAllowsRange(const std::string &if_range, const std::string &etag)
{
	const std::string value = TrimOws(if_range);
	// No precondition to fail. The Range stands on its own, which is also
	// the ordinary case: If-Range only ever appears on a resume.
	if (value.empty())
		return true;

	// RFC 9110 §13.1.5 says a valid entity-tag is told apart from a valid
	// HTTP-date by looking at the first two characters for a DQUOTE, and
	// that only the STRONG form may match. Both rejections land here: a
	// weak `W/"..."` starts with 'W', and every HTTP-date starts with a
	// day name, so neither reaches the comparison below.
	//
	// The date form is not implemented, and the fall-through is the safe
	// direction rather than a shortcut. A date validator has one-second
	// resolution, so a representation replaced twice inside the same
	// second compares EQUAL to the copy the client holds — precisely the
	// race this header exists to close. Treating a date as "does not
	// match" costs a full transfer the client may not have needed;
	// honouring it can cost a silently spliced file.
	if (value[0] != '"')
		return false;

	// Opaque octet-for-octet equality, quotes included. No list to walk
	// and no `*` to special-case: unlike If-None-Match, If-Range carries
	// exactly one validator by grammar.
	return value == etag;
}

} // namespace webapi
