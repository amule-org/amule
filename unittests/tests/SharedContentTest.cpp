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

#include <muleunit/test.h>

#include "SharedContent.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace muleunit;
using namespace webapi;

namespace
{

// Per-test scratch dir, same shape as StaticFsTest's helper: POSIX uses
// mkdtemp(), Windows builds a unique name under %TEMP%. Created empty;
// the test populates it and must call RemoveAll() itself, because
// muleunit's DECLARE_SIMPLE has no TearDown hook.
std::string MakeScratchRoot(const char *tag)
{
#ifdef _WIN32
	char tmp[MAX_PATH];
	DWORD n = GetTempPathA(MAX_PATH, tmp);
	std::string base(tmp, n);
	std::string dir =
		base + "amule-sharedcontent-" + tag + "-" + std::to_string(static_cast<long>(_getpid()));
	_mkdir(dir.c_str());
	return dir;
#else
	std::string tpl = "/tmp/amule-sharedcontent-";
	tpl += tag;
	tpl += "-XXXXXX";
	std::vector<char> buf(tpl.begin(), tpl.end());
	buf.push_back('\0');
	if (!mkdtemp(buf.data()))
		return std::string();
	return std::string(buf.data());
#endif
}

bool MkSubdir(const std::string &path)
{
#ifdef _WIN32
	return _mkdir(path.c_str()) == 0;
#else
	return mkdir(path.c_str(), 0755) == 0;
#endif
}

bool WriteFile(const std::string &path, const std::string &body)
{
	std::ofstream f(path.c_str(), std::ios::binary);
	if (!f.is_open())
		return false;
	f << body;
	return f.good();
}

void RemoveAll(const std::string &path)
{
	// Recursive rm via system(); test-scratch only, never on real data.
	// Quote the path to survive spaces / odd chars in $TMPDIR.
#ifdef _WIN32
	std::string cmd = "rmdir /S /Q \"" + path + "\" >NUL 2>&1";
#else
	std::string cmd = "rm -rf \"" + path + "\" 2>/dev/null";
#endif
	(void)std::system(cmd.c_str());
}

bool EndsWith(const std::string &s, const std::string &suffix)
{
	return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The platform separator JoinSharedPath is expected to insert. Kept as a
// helper so the join assertions read the same on both platforms.
#ifdef _WIN32
const char kSep = '\\';
#else
const char kSep = '/';
#endif

// 30 digits: comfortably past 2^64 (20 digits), so it can only be
// accepted by an implementation that wraps or saturates.
const char *const kOverflowDigits = "123456789012345678901234567890";

} // namespace

DECLARE_SIMPLE(SharedContent)

// ----------------------------------------------------------------------
// JoinSharedPath — the API's `path` is the directory and `name` is the
// basename, so this is the only place the two ever meet.
// ----------------------------------------------------------------------

TEST(SharedContent, JoinProducesDirSeparatorName)
{
	std::string out;
	ASSERT_TRUE(JoinSharedPath("/srv/share", "movie.avi", out));
	ASSERT_EQUALS(std::string("/srv/share") + kSep + "movie.avi", out);
}

TEST(SharedContent, JoinDoesNotDoubleTrailingSeparator)
{
	// A share root configured with a trailing slash is the common case
	// (that is how most path-picking UIs hand it over), so a doubled
	// separator here would be the normal path, not the exotic one.
	std::string out;
	ASSERT_TRUE(JoinSharedPath("/srv/share/", "movie.avi", out));
	ASSERT_EQUALS(std::string("/srv/share/movie.avi"), out);

	std::string out_bs;
	ASSERT_TRUE(JoinSharedPath("C:\\share\\", "movie.avi", out_bs));
	ASSERT_EQUALS(std::string("C:\\share\\movie.avi"), out_bs);
}

TEST(SharedContent, JoinRejectsEmptyDir)
{
	std::string out = "untouched";
	ASSERT_TRUE(!JoinSharedPath("", "movie.avi", out));
}

TEST(SharedContent, JoinRejectsEmptyName)
{
	std::string out;
	ASSERT_TRUE(!JoinSharedPath("/srv/share", "", out));
}

TEST(SharedContent, JoinRejectsNameWithForwardSlash)
{
	// `name` must be a bare basename. Anything with a separator is
	// either corrupt local state or a hostile ed2k filename; a
	// traversal attempt is only the loudest instance of that.
	std::string out;
	ASSERT_TRUE(!JoinSharedPath("/srv/share", "sub/movie.avi", out));
	ASSERT_TRUE(!JoinSharedPath("/srv/share", "../../etc/passwd", out));
}

TEST(SharedContent, JoinRejectsNameWithBackslash)
{
	std::string out;
	ASSERT_TRUE(!JoinSharedPath("/srv/share", "sub\\movie.avi", out));
	ASSERT_TRUE(!JoinSharedPath("/srv/share", "..\\..\\windows\\win.ini", out));
}

TEST(SharedContent, JoinRejectsDotAndDotDot)
{
	std::string out;
	ASSERT_TRUE(!JoinSharedPath("/srv/share", ".", out));
	ASSERT_TRUE(!JoinSharedPath("/srv/share", "..", out));
}

TEST(SharedContent, JoinRejectsEmbeddedNul)
{
	// std::string keeps the NUL, the C path APIs stop at it — so
	// without this the string we validated is not the path we open.
	const std::string dir_nul = std::string("/srv/share\0/evil", 16);
	const std::string name_nul = std::string("ok.bin\0.exe", 11);

	std::string out;
	ASSERT_TRUE(!JoinSharedPath(dir_nul, "movie.avi", out));
	ASSERT_TRUE(!JoinSharedPath("/srv/share", name_nul, out));
}

// ----------------------------------------------------------------------
// ResolveSharedContentPath — containment against a LIST of share roots.
// Every rejection is opaque; the caller maps them all to one 404.
// ----------------------------------------------------------------------

TEST(SharedContent, ResolveAcceptsFileInsideOneOfSeveralRoots)
{
	const std::string parent = MakeScratchRoot("multi-root");
	ASSERT_TRUE(!parent.empty());
	const std::string root_a = parent + "/a";
	const std::string root_b = parent + "/b";
	ASSERT_TRUE(MkSubdir(root_a));
	ASSERT_TRUE(MkSubdir(root_b));
	ASSERT_TRUE(WriteFile(root_b + "/movie.avi", "x"));

	std::vector<std::string> roots;
	roots.push_back(root_a);
	roots.push_back(root_b);

	std::string out;
	ASSERT_TRUE(ResolveSharedContentPath(roots, root_b, "movie.avi", out));
	// $TMPDIR is a symlink on macOS, so the canonical result is not
	// textually the path we built. The basename is the stable part.
	ASSERT_TRUE(EndsWith(out, "/movie.avi"));

	RemoveAll(parent);
}

TEST(SharedContent, ResolveRejectsFileOutsideEveryRoot)
{
	const std::string parent = MakeScratchRoot("outside-roots");
	ASSERT_TRUE(!parent.empty());
	const std::string root = parent + "/share";
	const std::string elsewhere = parent + "/elsewhere";
	ASSERT_TRUE(MkSubdir(root));
	ASSERT_TRUE(MkSubdir(elsewhere));
	ASSERT_TRUE(WriteFile(elsewhere + "/secret.txt", "pwn"));

	std::vector<std::string> roots;
	roots.push_back(root);

	std::string out;
	ASSERT_TRUE(!ResolveSharedContentPath(roots, elsewhere, "secret.txt", out));

	RemoveAll(parent);
}

TEST(SharedContent, ResolveRejectsWhenRootListIsEmpty)
{
	// No configured shares means nothing is servable — not "anything
	// is servable".
	const std::string root = MakeScratchRoot("empty-roots");
	ASSERT_TRUE(!root.empty());
	ASSERT_TRUE(WriteFile(root + "/movie.avi", "x"));

	std::vector<std::string> roots;
	std::string out;
	ASSERT_TRUE(!ResolveSharedContentPath(roots, root, "movie.avi", out));

	RemoveAll(root);
}

TEST(SharedContent, ResolveRejectsPrefixNeighbourRoot)
{
	// `/srv/share-evil` shares a textual prefix with `/srv/share` but is
	// a different directory. A plain strncmp containment check accepts
	// it; the boundary rule (the char at root_len must be a separator or
	// the terminator) is what rejects it.
	const std::string parent = MakeScratchRoot("prefix-neighbour");
	ASSERT_TRUE(!parent.empty());
	const std::string root = parent + "/share";
	const std::string neighbour = parent + "/share-evil";
	ASSERT_TRUE(MkSubdir(root));
	ASSERT_TRUE(MkSubdir(neighbour));
	ASSERT_TRUE(WriteFile(neighbour + "/secret.txt", "pwn"));

	std::vector<std::string> roots;
	roots.push_back(root);

	std::string out;
	ASSERT_TRUE(!ResolveSharedContentPath(roots, neighbour, "secret.txt", out));

	RemoveAll(parent);
}

TEST(SharedContent, ResolveRejectsMissingFile)
{
	const std::string root = MakeScratchRoot("missing-file");
	ASSERT_TRUE(!root.empty());

	std::vector<std::string> roots;
	roots.push_back(root);

	std::string out;
	ASSERT_TRUE(!ResolveSharedContentPath(roots, root, "not-there.avi", out));

	RemoveAll(root);
}

TEST(SharedContent, ResolveRejectsBadJoin)
{
	// A rejected join must not fall through to a resolve attempt.
	const std::string root = MakeScratchRoot("bad-join");
	ASSERT_TRUE(!root.empty());

	std::vector<std::string> roots;
	roots.push_back(root);

	std::string out;
	ASSERT_TRUE(!ResolveSharedContentPath(roots, root, "../escape.txt", out));
	ASSERT_TRUE(!ResolveSharedContentPath(roots, root, "", out));

	RemoveAll(root);
}

#ifndef _WIN32
TEST(SharedContent, ResolveRejectsSymlinkEscapingShareRoot)
{
	// The arbitrary-file-read case: a symlink planted *inside* a share
	// root pointing outside it. Every lexical check passes — the name is
	// a clean basename and the directory really is a configured root —
	// so only canonicalisation catches it. Anyone who can write into a
	// shared directory can plant this.
	const std::string parent = MakeScratchRoot("symlink-escape");
	ASSERT_TRUE(!parent.empty());
	const std::string root = parent + "/share";
	const std::string outside = parent + "/outside";
	ASSERT_TRUE(MkSubdir(root));
	ASSERT_TRUE(MkSubdir(outside));
	ASSERT_TRUE(WriteFile(outside + "/secret.txt", "pwn"));
	ASSERT_TRUE(symlink((outside + "/secret.txt").c_str(), (root + "/leak.txt").c_str()) == 0);

	std::vector<std::string> roots;
	roots.push_back(root);

	std::string out;
	ASSERT_TRUE(!ResolveSharedContentPath(roots, root, "leak.txt", out));

	RemoveAll(parent);
}

TEST(SharedContent, ResolveAcceptsSymlinkStayingInsideShareRoot)
{
	// Sanity counterpart: not every symlink is an attack, and rejecting
	// all of them would break ordinary shared trees.
	const std::string root = MakeScratchRoot("symlink-inside");
	ASSERT_TRUE(!root.empty());
	ASSERT_TRUE(WriteFile(root + "/real.avi", "ok"));
	ASSERT_TRUE(symlink((root + "/real.avi").c_str(), (root + "/alias.avi").c_str()) == 0);

	std::vector<std::string> roots;
	roots.push_back(root);

	std::string out;
	ASSERT_TRUE(ResolveSharedContentPath(roots, root, "alias.avi", out));
	ASSERT_TRUE(EndsWith(out, "/real.avi"));

	RemoveAll(root);
}
#endif // !_WIN32

// ----------------------------------------------------------------------
// ParseSingleByteRange — RFC 7233 §2.1/§3.1.
// ----------------------------------------------------------------------

TEST(SharedContent, RangeAbsentWhenHeaderEmpty)
{
	std::uint64_t f = 42, l = 42;
	ASSERT_TRUE(ParseSingleByteRange("", 1000, f, l) == RangeResult::kAbsent);
	ASSERT_TRUE(ParseSingleByteRange("   ", 1000, f, l) == RangeResult::kAbsent);
}

TEST(SharedContent, RangeFirstLastIsInclusive)
{
	std::uint64_t f = 0, l = 0;
	ASSERT_TRUE(ParseSingleByteRange("bytes=0-499", 1000, f, l) == RangeResult::kOk);
	ASSERT_EQUALS(static_cast<std::uint64_t>(0), f);
	ASSERT_EQUALS(static_cast<std::uint64_t>(499), l);
}

TEST(SharedContent, RangeSingleByte)
{
	std::uint64_t f = 9, l = 9;
	ASSERT_TRUE(ParseSingleByteRange("bytes=0-0", 1000, f, l) == RangeResult::kOk);
	ASSERT_EQUALS(static_cast<std::uint64_t>(0), f);
	ASSERT_EQUALS(static_cast<std::uint64_t>(0), l);
}

TEST(SharedContent, RangeOpenEndedRunsToEof)
{
	std::uint64_t f = 0, l = 0;
	ASSERT_TRUE(ParseSingleByteRange("bytes=500-", 1000, f, l) == RangeResult::kOk);
	ASSERT_EQUALS(static_cast<std::uint64_t>(500), f);
	ASSERT_EQUALS(static_cast<std::uint64_t>(999), l);
}

TEST(SharedContent, RangeLastIsClampedToEof)
{
	std::uint64_t f = 0, l = 0;
	ASSERT_TRUE(ParseSingleByteRange("bytes=990-1000000", 1000, f, l) == RangeResult::kOk);
	ASSERT_EQUALS(static_cast<std::uint64_t>(990), f);
	ASSERT_EQUALS(static_cast<std::uint64_t>(999), l);
}

TEST(SharedContent, RangeSuffixTakesLastBytes)
{
	std::uint64_t f = 0, l = 0;
	ASSERT_TRUE(ParseSingleByteRange("bytes=-500", 1000, f, l) == RangeResult::kOk);
	ASSERT_EQUALS(static_cast<std::uint64_t>(500), f);
	ASSERT_EQUALS(static_cast<std::uint64_t>(999), l);
}

TEST(SharedContent, RangeSuffixLargerThanFileIsWholeFile)
{
	std::uint64_t f = 7, l = 7;
	ASSERT_TRUE(ParseSingleByteRange("bytes=-5000", 1000, f, l) == RangeResult::kOk);
	ASSERT_EQUALS(static_cast<std::uint64_t>(0), f);
	ASSERT_EQUALS(static_cast<std::uint64_t>(999), l);
}

TEST(SharedContent, RangeZeroLengthSuffixIsUnsatisfiable)
{
	// "bytes=-0" asks for the last zero bytes. RFC 7233 §2.1 makes a
	// zero-length suffix unsatisfiable rather than an empty 206.
	std::uint64_t f = 0, l = 0;
	ASSERT_TRUE(ParseSingleByteRange("bytes=-0", 1000, f, l) == RangeResult::kUnsatisfiable);
}

TEST(SharedContent, RangeFirstPastEofIsUnsatisfiable)
{
	std::uint64_t f = 0, l = 0;
	ASSERT_TRUE(ParseSingleByteRange("bytes=999999-", 1000, f, l) == RangeResult::kUnsatisfiable);
	// The exact boundary: offset 1000 of a 1000-byte file is one past
	// the last valid byte.
	ASSERT_TRUE(ParseSingleByteRange("bytes=1000-", 1000, f, l) == RangeResult::kUnsatisfiable);
	ASSERT_TRUE(ParseSingleByteRange("bytes=1000-1005", 1000, f, l) == RangeResult::kUnsatisfiable);
	// ...and 999 is still inside it.
	ASSERT_TRUE(ParseSingleByteRange("bytes=999-", 1000, f, l) == RangeResult::kOk);
}

TEST(SharedContent, RangeOnEmptyFileIsAlwaysUnsatisfiable)
{
	std::uint64_t f = 0, l = 0;
	ASSERT_TRUE(ParseSingleByteRange("bytes=0-", 0, f, l) == RangeResult::kUnsatisfiable);
	ASSERT_TRUE(ParseSingleByteRange("bytes=0-0", 0, f, l) == RangeResult::kUnsatisfiable);
	ASSERT_TRUE(ParseSingleByteRange("bytes=-1", 0, f, l) == RangeResult::kUnsatisfiable);
}

TEST(SharedContent, RangeInvertedBoundsIsIgnored)
{
	// first > last is syntactically well-formed but semantically
	// invalid, so RFC 7233 §2.1 says the whole byte-range-set is
	// invalid — which we answer by ignoring the header, not by 416.
	std::uint64_t f = 0, l = 0;
	ASSERT_TRUE(ParseSingleByteRange("bytes=500-499", 1000, f, l) == RangeResult::kIgnore);
}

TEST(SharedContent, MultiRangeIsIgnoredNotServedAsMultipart)
{
	// CVE-2011-3192 ("Apache Killer"): a short header carrying many
	// overlapping ranges makes a multipart/byteranges responder
	// materialise far more bytes than the file holds. We never assemble
	// multipart at all — a comma in the set means a plain 200.
	std::uint64_t f = 0, l = 0;
	ASSERT_TRUE(ParseSingleByteRange("bytes=0-1,2-3", 1000, f, l) == RangeResult::kIgnore);
	ASSERT_TRUE(ParseSingleByteRange("bytes=0-1, 2-3", 1000, f, l) == RangeResult::kIgnore);
	ASSERT_TRUE(ParseSingleByteRange("bytes=0-,-1", 1000, f, l) == RangeResult::kIgnore);

	// The historical attack shape itself: hundreds of overlapping
	// ranges over a small file.
	std::string killer = "bytes=0-";
	for (int i = 0; i < 400; ++i)
		killer += ",5-" + std::to_string(1000 + i);
	ASSERT_TRUE(ParseSingleByteRange(killer, 1000, f, l) == RangeResult::kIgnore);
}

TEST(SharedContent, RangeUnitIsCaseInsensitiveButMustBeBytes)
{
	std::uint64_t f = 0, l = 0;
	ASSERT_TRUE(ParseSingleByteRange("BYTES=0-9", 1000, f, l) == RangeResult::kOk);
	ASSERT_TRUE(ParseSingleByteRange("Bytes=0-9", 1000, f, l) == RangeResult::kOk);
	// Any other unit is one we do not implement, so we serve the whole
	// file rather than guessing what it meant.
	ASSERT_TRUE(ParseSingleByteRange("items=0-1", 1000, f, l) == RangeResult::kIgnore);
	ASSERT_TRUE(ParseSingleByteRange("chunks=0-1", 1000, f, l) == RangeResult::kIgnore);
	ASSERT_TRUE(ParseSingleByteRange("0-9", 1000, f, l) == RangeResult::kIgnore);
}

TEST(SharedContent, RangeMalformedIsIgnored)
{
	std::uint64_t f = 0, l = 0;
	ASSERT_TRUE(ParseSingleByteRange("bytes=abc-def", 1000, f, l) == RangeResult::kIgnore);
	ASSERT_TRUE(ParseSingleByteRange("bytes=", 1000, f, l) == RangeResult::kIgnore);
	ASSERT_TRUE(ParseSingleByteRange("bytes=-", 1000, f, l) == RangeResult::kIgnore);
	ASSERT_TRUE(ParseSingleByteRange("bytes=0", 1000, f, l) == RangeResult::kIgnore);
	ASSERT_TRUE(ParseSingleByteRange("bytes=+0-9", 1000, f, l) == RangeResult::kIgnore);
	ASSERT_TRUE(ParseSingleByteRange("bytes=0x10-0x20", 1000, f, l) == RangeResult::kIgnore);
	ASSERT_TRUE(ParseSingleByteRange("bytes=0-9junk", 1000, f, l) == RangeResult::kIgnore);
}

TEST(SharedContent, RangeToleratesOnlyTheWhitespaceTheGrammarAllows)
{
	// RFC 7233's byte-range-spec has no OWS inside it — OWS is only
	// permitted around the list commas of the `#rule`, and we reject
	// every comma anyway. So leading/trailing field whitespace (which
	// HTTP field parsing strips regardless) is tolerated, and a space
	// around the dash or inside a number is not.
	std::uint64_t f = 0, l = 0;
	ASSERT_TRUE(ParseSingleByteRange("  bytes=0-9  ", 1000, f, l) == RangeResult::kOk);
	ASSERT_TRUE(ParseSingleByteRange("bytes = 0-9", 1000, f, l) == RangeResult::kOk);
	ASSERT_TRUE(ParseSingleByteRange("bytes=0 - 9", 1000, f, l) == RangeResult::kIgnore);
	ASSERT_TRUE(ParseSingleByteRange("bytes=0- 9", 1000, f, l) == RangeResult::kIgnore);
	ASSERT_TRUE(ParseSingleByteRange("bytes=1 0-9", 1000, f, l) == RangeResult::kIgnore);
}

TEST(SharedContent, RangeOverflowingDigitsAreRejectedNotWrapped)
{
	// A 30-digit bound cannot fit in uint64. Wrapping it would turn an
	// absurd request into a plausible in-range one; saturating it would
	// turn it into "the rest of the file". Both are wrong — it is
	// unparseable, so we ignore the header.
	std::uint64_t f = 0, l = 0;
	const std::string big(kOverflowDigits);
	ASSERT_TRUE(ParseSingleByteRange("bytes=" + big + "-", 1000, f, l) == RangeResult::kIgnore);
	ASSERT_TRUE(ParseSingleByteRange("bytes=0-" + big, 1000, f, l) == RangeResult::kIgnore);
	ASSERT_TRUE(ParseSingleByteRange("bytes=-" + big, 1000, f, l) == RangeResult::kIgnore);
	ASSERT_TRUE(ParseSingleByteRange("bytes=" + big + "-" + big, 1000, f, l) == RangeResult::kIgnore);

	// One past UINT64_MAX (which is 18446744073709551615) must fail
	// too, and the value itself must still parse as a plain number.
	ASSERT_TRUE(ParseSingleByteRange("bytes=18446744073709551616-", 1000, f, l) == RangeResult::kIgnore);
	ASSERT_TRUE(ParseSingleByteRange("bytes=18446744073709551615-", 1000, f, l) ==
		    RangeResult::kUnsatisfiable);
}

// ----------------------------------------------------------------------
// BuildContentDisposition — RFC 6266. The one place a remote-chosen
// string is written into a response header.
// ----------------------------------------------------------------------

TEST(SharedContent, DispositionPlainAsciiNameUsesBothForms)
{
	ASSERT_EQUALS(std::string("attachment; filename=\"movie.avi\"; filename*=UTF-8''movie.avi"),
		BuildContentDisposition("movie.avi"));
}

TEST(SharedContent, DispositionIsAlwaysAttachmentNeverInline)
{
	const std::string d = BuildContentDisposition("page.html");
	ASSERT_TRUE(d.compare(0, 10, "attachment") == 0);
	ASSERT_TRUE(d.find("inline") == std::string::npos);
}

TEST(SharedContent, DispositionStripsCrLfSoHeadersCannotBeInjected)
{
	// Regression test for response-header injection. The filename comes
	// off the ed2k network; a CRLF in it would end the header and let
	// the sender append their own — or a whole second response.
	const std::string evil = "a\r\nSet-Cookie: pwned=1\r\n\r\n<html>.bin";
	const std::string d = BuildContentDisposition(evil);
	ASSERT_TRUE(d.find('\r') == std::string::npos);
	ASSERT_TRUE(d.find('\n') == std::string::npos);
	// The bare newlines must not survive into the encoded form either;
	// if they were percent-encoded they would appear as %0D/%0A, which
	// is inert, but nothing raw may remain.
	ASSERT_TRUE(d.find("Set-Cookie: pwned=1\r") == std::string::npos);

	// A lone LF and a lone CR are just as dangerous with a lenient
	// client-side parser.
	const std::string lf = BuildContentDisposition("a\nb.bin");
	ASSERT_TRUE(lf.find('\n') == std::string::npos);
	const std::string cr = BuildContentDisposition("a\rb.bin");
	ASSERT_TRUE(cr.find('\r') == std::string::npos);
}

TEST(SharedContent, DispositionNeutralisesQuoteAndBackslash)
{
	// An unescaped quote would close filename="..." early and let the
	// rest of the name be read as further header parameters.
	const std::string d = BuildContentDisposition("a\"b\\c.bin");
	ASSERT_EQUALS(std::string("attachment; filename=\"a_b_c.bin\"; filename*=UTF-8''a%22b%5Cc.bin"), d);
}

TEST(SharedContent, DispositionPercentEncodesUtf8InTheExtendedForm)
{
	// Accented Latin: the quoted form cannot carry non-ASCII, so it
	// degrades; filename* carries the real name percent-encoded.
	ASSERT_EQUALS(std::string("attachment; filename=\"caf__.bin\"; filename*=UTF-8''caf%C3%A9.bin"),
		BuildContentDisposition("caf\xC3\xA9.bin"));

	// CJK: every byte is outside attr-char, so the whole stem encodes.
	ASSERT_EQUALS(
		std::string("attachment; filename=\"______.txt\"; filename*=UTF-8''%E6%97%A5%E6%9C%AC.txt"),
		BuildContentDisposition("\xE6\x97\xA5\xE6\x9C\xAC.txt"));
}

TEST(SharedContent, DispositionFallsBackToPlaceholderWhenNothingSurvives)
{
	// An all-control name leaves nothing printable. Emitting an empty
	// filename="" would let the client pick its own name from the URL,
	// so we name it ourselves.
	const std::string ctrl("\x01\x02\x1F\x7F", 4);
	ASSERT_EQUALS(std::string("attachment; filename=\"download\"; filename*=UTF-8''download"),
		BuildContentDisposition(ctrl));
	ASSERT_EQUALS(std::string("attachment; filename=\"download\"; filename*=UTF-8''download"),
		BuildContentDisposition(""));
}

TEST(SharedContent, DispositionRejectsPathSeparatorsInTheQuotedForm)
{
	// Some clients still honour a path in filename=. Flatten it.
	const std::string d = BuildContentDisposition("../../etc/passwd");
	ASSERT_TRUE(d.find("filename=\"") != std::string::npos);
	ASSERT_TRUE(d.find("/etc/") == std::string::npos);
	ASSERT_TRUE(d.find("attachment; filename=\".._.._etc_passwd\"") == 0);
}

TEST(SharedContent, DispositionSemicolonCannotSplitParameters)
{
	// A semicolon inside the quoted string is legal, but only because
	// the quotes hold; the extended form must still encode it so a
	// lenient parser cannot read it as a new parameter.
	const std::string d = BuildContentDisposition("a;b.bin");
	ASSERT_EQUALS(std::string("attachment; filename=\"a;b.bin\"; filename*=UTF-8''a%3Bb.bin"), d);
}

// ----------------------------------------------------------------------
// BuildContentEtag — mirrors BuildStaticEtag (Api.cpp:501-507) so the
// dispatcher's 304 machinery sees one validator shape for both routes.
// ----------------------------------------------------------------------

TEST(SharedContent, EtagHasTheSameShapeAsTheStaticOne)
{
	// Quoted strong validator, lowercase hex mtime, '-', lowercase hex
	// size. 0x68b0... is an ordinary epoch second.
	ASSERT_EQUALS(std::string("\"68b00000-400\""), BuildContentEtag(0x68b00000ull, 1024ull));
	ASSERT_EQUALS(std::string("\"0-0\""), BuildContentEtag(0, 0));
}

TEST(SharedContent, EtagIsStableForTheSameInputs)
{
	ASSERT_EQUALS(BuildContentEtag(1700000000ull, 4294967296ull),
		BuildContentEtag(1700000000ull, 4294967296ull));
}

TEST(SharedContent, EtagChangesWithMtimeAndWithSize)
{
	const std::string base = BuildContentEtag(1700000000ull, 1024ull);
	ASSERT_TRUE(base != BuildContentEtag(1700000001ull, 1024ull));
	ASSERT_TRUE(base != BuildContentEtag(1700000000ull, 1025ull));
	// A truncation that keeps the size but moves the mtime, and a
	// rewrite that keeps the mtime but moves the size, must both
	// invalidate — that is the whole point of the pair.
	ASSERT_TRUE(BuildContentEtag(1, 2) != BuildContentEtag(2, 1));
}

TEST(SharedContent, EtagHandlesLargeFileSizes)
{
	// Multi-GB shared files are the normal case for this endpoint, so
	// the size half must not be truncated to 32 bits.
	ASSERT_EQUALS(std::string("\"0-200000000\""), BuildContentEtag(0, 8589934592ull));
}
