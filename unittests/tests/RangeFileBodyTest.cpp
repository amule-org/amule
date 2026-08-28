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

// Exercises RangeFileBody end to end: build a response over a scratch file,
// run it through a real Beast response_serializer, and compare the bytes that
// come off the "wire" with the slice of the file they are supposed to be.
//
// Everything here is about the two things the type exists to get right: the
// window must END where it was told to (stock file_body cannot do that, see
// RangeFileBody.h), and the Content-Length prepare_payload() stamps must equal
// the number of body bytes actually written.

#include <muleunit/test.h>

#include "RangeFileBody.h"

#include <boost/beast/core/error.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/http/write.hpp>

#include <cstdio>
#include <string>

using namespace muleunit;

namespace beast = boost::beast;
namespace http = boost::beast::http;

DECLARE_SIMPLE(RangeFileBody)

namespace
{

// Minimal SyncWriteStream: collects everything the serializer emits into a
// string. Using the real http::write (rather than driving serializer::next by
// hand) means the test goes through the same buffer/consume protocol the
// transport's async_write does.
struct CollectingStream
{
	std::string out;

	template <class ConstBufferSequence>
	std::size_t write_some(const ConstBufferSequence &buffers, boost::system::error_code &ec)
	{
		std::size_t n = 0;
		for (auto it = boost::asio::buffer_sequence_begin(buffers);
			it != boost::asio::buffer_sequence_end(buffers);
			++it) {
			const boost::asio::const_buffer b = *it;
			out.append(static_cast<const char *>(b.data()), b.size());
			n += b.size();
		}
		ec = {};
		return n;
	}

	template <class ConstBufferSequence> std::size_t write_some(const ConstBufferSequence &buffers)
	{
		boost::system::error_code ec;
		return write_some(buffers, ec);
	}
};

// Deterministic, non-repeating-at-64-KiB content so an off-by-one buffer
// boundary shows up as wrong BYTES, not just a wrong length.
std::string MakeContent(std::size_t n)
{
	std::string s;
	s.reserve(n);
	for (std::size_t i = 0; i < n; ++i) {
		s.push_back(static_cast<char>((i * 31 + (i >> 8) * 7) & 0xFF));
	}
	return s;
}

const char *const kScratchPath = "RangeFileBodyTest.scratch";

void WriteScratch(const std::string &content)
{
	std::FILE *f = std::fopen(kScratchPath, "wb");
	ASSERT_TRUE(f != nullptr);
	if (!content.empty()) {
		std::fwrite(content.data(), 1, content.size(), f);
	}
	std::fclose(f);
}

// Result of serialising one window: the advertised length and the body bytes.
struct Serialised
{
	bool ok = false;
	std::string content_length; // verbatim header value
	std::string body;
};

Serialised SerialiseWindow(std::uint64_t first, std::uint64_t last, bool header_only)
{
	Serialised r;

	http::response<RangeFileBody> res;
	res.version(11);
	res.result(http::status::partial_content);
	beast::error_code ec;
	res.body().Open(kScratchPath, ec);
	if (ec) {
		return r;
	}
	res.body().SetWindow(first, last, ec);
	if (ec) {
		return r;
	}
	res.prepare_payload();

	CollectingStream stream;
	http::response_serializer<RangeFileBody> sr{ res };
	if (header_only) {
		sr.split(true);
		http::write_header(stream, sr, ec);
	} else {
		http::write(stream, sr, ec);
	}
	if (ec && ec != http::error::end_of_stream) {
		return r;
	}

	const std::size_t sep = stream.out.find("\r\n\r\n");
	if (sep == std::string::npos) {
		return r;
	}
	const std::string head = stream.out.substr(0, sep);
	r.body = stream.out.substr(sep + 4);

	const std::size_t cl = head.find("Content-Length: ");
	if (cl != std::string::npos) {
		const std::size_t eol = head.find("\r\n", cl);
		r.content_length = head.substr(cl + 16, eol - (cl + 16));
	}
	r.ok = true;
	return r;
}

// One window, checked against the expected slice of `content`.
void CheckWindow(const std::string &content, std::uint64_t first, std::uint64_t last)
{
	const Serialised s = SerialiseWindow(first, last, false);
	ASSERT_TRUE(s.ok);
	const std::string expected =
		content.substr(static_cast<std::size_t>(first), static_cast<std::size_t>(last - first + 1));
	// Advertised length and delivered length must agree — a mismatch here is
	// exactly the bug stock file_body would have shipped.
	ASSERT_EQUALS(std::to_string(expected.size()), s.content_length);
	ASSERT_EQUALS(expected.size(), s.body.size());
	ASSERT_TRUE(s.body == expected);
}

} // namespace

// ----------------------------------------------------------------------
// Windows smaller than the read buffer.
// ----------------------------------------------------------------------

TEST(RangeFileBody, WholeSmallFile)
{
	const std::string content = MakeContent(300);
	WriteScratch(content);
	CheckWindow(content, 0, content.size() - 1);
}

TEST(RangeFileBody, InteriorWindow)
{
	const std::string content = MakeContent(300);
	WriteScratch(content);
	CheckWindow(content, 100, 199);
}

TEST(RangeFileBody, SingleByteInTheMiddle)
{
	const std::string content = MakeContent(300);
	WriteScratch(content);
	CheckWindow(content, 150, 150);
}

TEST(RangeFileBody, FirstByte)
{
	const std::string content = MakeContent(300);
	WriteScratch(content);
	CheckWindow(content, 0, 0);
}

TEST(RangeFileBody, LastByte)
{
	const std::string content = MakeContent(300);
	WriteScratch(content);
	CheckWindow(content, content.size() - 1, content.size() - 1);
}

// ----------------------------------------------------------------------
// Windows that force multiple passes through the 64 KiB buffer. This is where
// a wrong `remain_` bound or a mishandled partial last chunk shows up.
// ----------------------------------------------------------------------

TEST(RangeFileBody, WholeFileLargerThanTheReadBuffer)
{
	const std::string content = MakeContent(1024 * 1024);
	WriteScratch(content);
	CheckWindow(content, 0, content.size() - 1);
}

TEST(RangeFileBody, WindowStraddlingBufferBoundaries)
{
	const std::string content = MakeContent(1024 * 1024);
	WriteScratch(content);
	// Starts and ends off any 64 KiB multiple, and spans several buffers.
	CheckWindow(content, kRangeFileReadChunkBytes - 3, 3 * kRangeFileReadChunkBytes + 7);
}

TEST(RangeFileBody, WindowEndingExactlyOnABufferBoundary)
{
	const std::string content = MakeContent(1024 * 1024);
	WriteScratch(content);
	// last is the final byte of the second buffer: the writer must then stop
	// rather than issue a zero-length read and report short_read.
	CheckWindow(content, 0, 2 * kRangeFileReadChunkBytes - 1);
}

TEST(RangeFileBody, WindowStopsWellBeforeEndOfFile)
{
	// The whole point: a bounded window over a much larger file. Stock
	// file_body would have written every remaining byte.
	const std::string content = MakeContent(1024 * 1024);
	WriteScratch(content);
	CheckWindow(content, 4096, 8191);
}

// ----------------------------------------------------------------------
// Framing.
// ----------------------------------------------------------------------

TEST(RangeFileBody, HeaderOnlyReportsTheGetLengthAndNoBody)
{
	// HEAD must describe what a GET would return. The transport serialises
	// the header alone; Content-Length still comes from Body::size, so it has
	// to be the window length while zero body bytes reach the wire.
	const std::string content = MakeContent(1024 * 1024);
	WriteScratch(content);
	const Serialised s = SerialiseWindow(1000, 500999, true);
	ASSERT_TRUE(s.ok);
	ASSERT_EQUALS(std::string("500000"), s.content_length);
	ASSERT_EQUALS(static_cast<size_t>(0), s.body.size());
}

TEST(RangeFileBody, SizeIsTheWindowNotTheRemainderOfTheFile)
{
	const std::string content = MakeContent(1024 * 1024);
	WriteScratch(content);

	http::response<RangeFileBody> res;
	beast::error_code ec;
	res.body().Open(kScratchPath, ec);
	ASSERT_TRUE(!ec);
	ASSERT_EQUALS(static_cast<size_t>(1024 * 1024), static_cast<size_t>(res.body().FileSize()));
	res.body().SetWindow(10, 19, ec);
	ASSERT_TRUE(!ec);
	ASSERT_EQUALS(static_cast<size_t>(10), static_cast<size_t>(res.body().size()));
	ASSERT_EQUALS(static_cast<size_t>(10), static_cast<size_t>(RangeFileBody::size(res.body())));
}

// ----------------------------------------------------------------------
// Window validation. The transport turns these failures into a 500 while the
// headers are still unwritten, so they must be reported, never clamped.
// ----------------------------------------------------------------------

TEST(RangeFileBody, RejectsWindowsOutsideTheFile)
{
	const std::string content = MakeContent(300);
	WriteScratch(content);

	http::response<RangeFileBody> res;
	beast::error_code ec;
	res.body().Open(kScratchPath, ec);
	ASSERT_TRUE(!ec);

	// last past EOF.
	res.body().SetWindow(0, 300, ec);
	ASSERT_TRUE(static_cast<bool>(ec));
	// first past EOF.
	ec = {};
	res.body().SetWindow(300, 400, ec);
	ASSERT_TRUE(static_cast<bool>(ec));
	// Inverted window.
	ec = {};
	res.body().SetWindow(10, 9, ec);
	ASSERT_TRUE(static_cast<bool>(ec));
	// A rejected window leaves nothing to serialise, rather than a stale one.
	ASSERT_EQUALS(static_cast<size_t>(0), static_cast<size_t>(res.body().size()));
}

TEST(RangeFileBody, OpenFailsForAMissingPath)
{
	http::response<RangeFileBody> res;
	beast::error_code ec;
	res.body().Open("RangeFileBodyTest.no-such-file", ec);
	ASSERT_TRUE(static_cast<bool>(ec));
	ASSERT_TRUE(!res.body().IsOpen());
}

TEST(RangeFileBody, EmptyFileHasNoValidWindow)
{
	// Zero-length files have no inclusive window at all; they belong on the
	// buffered `body` path, and the transport relies on this rejection to
	// notice a handler that routed one here.
	WriteScratch(std::string());
	http::response<RangeFileBody> res;
	beast::error_code ec;
	res.body().Open(kScratchPath, ec);
	ASSERT_TRUE(!ec);
	ASSERT_EQUALS(static_cast<size_t>(0), static_cast<size_t>(res.body().FileSize()));
	res.body().SetWindow(0, 0, ec);
	ASSERT_TRUE(static_cast<bool>(ec));

	std::remove(kScratchPath);
}
