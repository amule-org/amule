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

#ifndef WEBAPI_RANGEFILEBODY_H
#define WEBAPI_RANGEFILEBODY_H

// A Boost.Beast message Body that serialises a bounded byte window [first, last] (inclusive, RFC
// 9110 range semantics) out of a file on disk, using a fixed-size read buffer.
//
// Why this instead of beast::http::file_body: stock `basic_file_body` can start a response at an
// arbitrary offset but cannot stop before EOF. Its `value_type::seek(offset, ec)` is
//
//     file_.seek(offset, ec);
//     file_size_ = file_.size(ec);
//     file_size_ -= file_.pos(ec);      // i.e. "everything left in the file"
//
// so the cached body size -- the number `Body::size()` reports, and therefore the `Content-Length`
// `prepare_payload()` stamps -- is always `size - first`. A `Range: bytes=0-1023` over a 4 GiB file
// would advertise and then actually write 4 GiB. There is no hook to bound the END of the window,
// which is the half of a byte range that matters here, so the writer has to be our own.
//
// Why it matters for memory: the alternative, the `string_body` path every other amuleapi response
// uses, materialises the whole body in RAM. amuleapi runs a 16-thread handler pool
// (kHandlerPoolThreads), so serving shared content out of a buffered body means up to 16 x filesize
// resident, with filesize routinely in the gigabytes for ed2k content. This body reads through one
// `kRangeFileReadChunkBytes` buffer owned by the writer, so the resident cost of a response is O(1)
// in the file size. That buffer is heap-allocated on first use rather than held inline in the
// writer, because the writer is stored by value inside the serializer and the serializer sits in a
// `boost::optional` member of every Session -- see writer::m_buf.
//
// Scope: serialisation only. amuleapi never PARSES a file body -- nothing uploads into a file
// through this transport -- so the `reader` half of the Body concept is deliberately absent.
// `http::response_serializer` only ever instantiates `writer`, so the type still satisfies
// everything the write path asks for; a parser instantiated over this Body would fail to compile,
// which is the correct outcome rather than a silently wrong one.

// BOOST FLOOR: everything used here must exist in Boost 1.70 (MIN_BOOST_VERSION), which is close to
// what ubuntu:22.04's libboost-dev ships and therefore what the AppImage actually compiles amuleapi
// against -- unlike the Flatpak, it does not build the pinned 1.87 from source, and packaging.yml
// only fires on push to master, so a too-new API breaks AFTER merge with nothing on the PR to catch
// it. Two such traps are documented at their use sites below.

#include <boost/asio/buffer.hpp>
#include <boost/assert.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/file.hpp>
#include <boost/beast/core/file_base.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/core/ignore_unused.hpp>
#include <boost/optional.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

// Size of the writer's read buffer, and therefore the largest single chunk handed to the socket. 64
// KiB is comfortably larger than a typical 16-64 KiB socket send buffer -- so the syscall count is
// bounded by the socket, not by us -- while still small enough that a handful can be in flight at
// the file-response concurrency cap. It is heap-allocated by the writer on first use, so a
// connection that never streams a file never allocates it.
constexpr std::size_t kRangeFileReadChunkBytes = 64 * 1024;

// Boost.Beast Body concept implementation. `File` is the Beast file abstraction
// to use; the default resolves to the native POSIX or Win32 implementation.
template <class File = boost::beast::file> struct BasicRangeFileBody
{
	static_assert(boost::beast::is_file<File>::value, "File type requirements not met");

	using file_type = File;

	class value_type;
	class writer;
	// No `reader`: see the SCOPE note at the top of this file.

	// Called from message::payload_size, which is what prepare_payload() uses to
	// stamp Content-Length. Returns the size of the WINDOW, not of the file.
	static std::uint64_t size(const value_type &body) { return body.size(); }
};

// The `message::body` member for a message declared with this Body. Owns the
// open file handle, so the descriptor's lifetime is exactly the response's.
template <class File> class BasicRangeFileBody<File>::value_type
{
	friend class writer;
	friend struct BasicRangeFileBody;

	File m_file;
	// Size of the file as observed at open() time. Cached because the window validation below
	// needs something stable to compare against: answering a Range with a Content-Length
	// derived from a moving size is how a response ends up contradicting its own framing.
	std::uint64_t m_file_size = 0;
	// Inclusive window. Only meaningful once SetWindow() has succeeded;
	// m_have_window keeps a caller that forgot from serialising byte 0 alone.
	std::uint64_t m_first = 0;
	std::uint64_t m_last = 0;
	bool m_have_window = false;
	// Kept for diagnostics only -- the open handle is what is actually read.
	std::string m_path;

public:
	value_type() = default;
	~value_type() = default;
	value_type(value_type &&other) = default;
	value_type &operator=(value_type &&other) = default;

	// Open `path` for reading and cache its size. On success the window is
	// still unset; call SetWindow() before serialising.
	void Open(const char *path, boost::beast::error_code &ec)
	{
		m_file.open(path, boost::beast::file_mode::read, ec);
		if (ec) {
			return;
		}
		m_file_size = m_file.size(ec);
		if (ec) {
			Close();
			return;
		}
		m_path = path;
		m_have_window = false;
	}

	void Close()
	{
		boost::beast::error_code ignored;
		m_file.close(ignored);
		m_have_window = false;
	}

	bool IsOpen() const { return m_file.is_open(); }

	// Size of the file as seen at Open() time. The caller needs this to turn
	// a `Range` header into a concrete window and to fill `Content-Range`.
	std::uint64_t FileSize() const { return m_file_size; }

	const std::string &Path() const { return m_path; }

	// Set the inclusive byte window to serialise. Rejects anything that does not describe at
	// least one real byte of the file, rather than clamping: the caller has already put a
	// `Content-Range` on the response describing the window it asked for. Failing here leaves
	// the headers still ours to change.
	void SetWindow(std::uint64_t first, std::uint64_t last, boost::beast::error_code &ec)
	{
		if (!m_file.is_open() || first > last || first >= m_file_size || last >= m_file_size) {
			// Plain assignment rather than BOOST_BEAST_ASSIGN_EC: that macro is a
			// private Beast detail header and does not exist in 1.70 -- see the BOOST
			// FLOOR note at the top. Only the diagnostic location is lost.
			ec = boost::beast::http::error::bad_field;
			m_have_window = false;
			return;
		}
		m_first = first;
		m_last = last;
		m_have_window = true;
		ec = {};
	}

	// Number of bytes this body will put on the wire -- the inclusive window length. Zero
	// before SetWindow(), so a forgotten call shows up as an empty response rather than an
	// unbounded one.
	std::uint64_t size() const { return m_have_window ? (m_last - m_first + 1) : 0; }

	std::uint64_t First() const { return m_first; }
	std::uint64_t Last() const { return m_last; }

	File &file() { return m_file; }
};

// Serialisation half of the Body concept: hands the serializer one buffer at a
// time until the window is exhausted.
template <class File> class BasicRangeFileBody<File>::writer
{
	value_type &m_body;
	// Bytes of the window not yet handed to the serializer.
	std::uint64_t m_remain;
	// Heap, not `char m_buf[kRangeFileReadChunkBytes]`. `response_serializer` stores its
	// `writer wr_` BY VALUE, and the transport parks that serializer in a `boost::optional`
	// member of every Session, which reserves its storage inline whether or not it is ever
	// engaged -- so an inline array charged 64 KiB to every connection, an hours-long SSE
	// stream included. Allocated lazily in get(), which a HEAD never reaches.
	std::unique_ptr<char[]> m_buf;

public:
	using const_buffers_type = boost::asio::const_buffer;

	// The message is taken by non-const reference because reading advances the file cursor:
	// logically non-const even though it is bitwise const. Same trade stock file_body makes --
	// one thread may serialise a given body.
	template <bool isRequest, class Fields>
	writer(boost::beast::http::header<isRequest, Fields> &h, value_type &b)
	: m_body(b)
	, m_remain(b.size())
	{
		boost::ignore_unused(h);
		BOOST_ASSERT(m_body.m_file.is_open());
	}

	// Runs before any body byte is produced. This is where the file cursor is placed at the
	// start of the window.
	//
	// NOTE FOR HEAD: `serializer::next` calls init() even in split mode. That is intended -- a
	// seek moves a cursor, it reads nothing -- and it is why a HEAD costs one open plus one
	// lseek and not a single byte of I/O.
	void init(boost::beast::error_code &ec)
	{
		m_body.m_file.seek(m_body.m_first, ec);
		if (ec) {
			return;
		}
		ec = {};
	}

	// Called repeatedly until it returns boost::none. Reads at most whatever is
	// left of the window, so we can never over-read past `last`.
	boost::optional<std::pair<const_buffers_type, bool>> get(boost::beast::error_code &ec)
	{
		const std::size_t amount = m_remain > kRangeFileReadChunkBytes
						   ? kRangeFileReadChunkBytes
						   : static_cast<std::size_t>(m_remain);

		// Window fully served, or the degenerate zero-length case. Tested before
		// the allocation below, so the no-bytes case stays allocation-free.
		if (amount == 0) {
			ec = {};
			return boost::none;
		}

		if (!m_buf) {
			// Throwing new, not nothrow: a failed 64 KiB allocation is a process
			// already past saving, and the bad_alloc propagates out of the async op
			// into the same connection teardown the read-error branch produces.
			m_buf.reset(new char[kRangeFileReadChunkBytes]);
		}

		const std::size_t nread = m_body.m_file.read(m_buf.get(), amount, ec);
		if (ec) {
			return boost::none;
		}
		// A short read is legal and handled by returning fewer bytes; a ZERO-length read is
		// not, because we asked for bytes the cached size said were there. That means the
		// file was truncated under us mid-response, and there is no honest way to finish a
		// body whose Content-Length is already on the wire. `partial_message` and not the
		// more descriptive `short_read`: that enumerator arrived in Boost 1.73 -- see the
		// BOOST FLOOR note at the top.
		if (nread == 0) {
			ec = boost::beast::http::error::partial_message;
			return boost::none;
		}
		BOOST_ASSERT(nread <= m_remain);

		m_remain -= nread;
		ec = {};
		return { { const_buffers_type{ m_buf.get(), nread }, m_remain > 0 } };
	}
};

// The type the transport actually uses.
using RangeFileBody = BasicRangeFileBody<boost::beast::file>;

#endif // WEBAPI_RANGEFILEBODY_H
