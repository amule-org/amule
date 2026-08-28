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

// A Boost.Beast message Body that serialises a bounded byte window
// [first, last] (inclusive, RFC 9110 range semantics) out of a file on disk,
// using a fixed-size read buffer.
//
// WHY THIS EXISTS INSTEAD OF beast::http::file_body
// -------------------------------------------------
// Stock `basic_file_body` can start a response at an arbitrary offset but it
// cannot stop before EOF. Its `value_type::seek(offset, ec)` is implemented as
//
//     file_.seek(offset, ec);
//     file_size_ = file_.size(ec);
//     file_size_ -= file_.pos(ec);      // i.e. "everything left in the file"
//
// so the cached body size — the number `Body::size()` reports and therefore the
// `Content-Length` `prepare_payload()` stamps — is always `size - first`. A
// `Range: bytes=0-1023` over a 4 GiB file would advertise and then actually
// write 4 GiB. There is no hook to bound the END of the window, which is the
// half of a byte range that matters here, so the writer has to be our own.
//
// WHY IT MATTERS FOR MEMORY
// -------------------------
// The alternative — the `string_body` path every other amuleapi response uses —
// materialises the whole body in RAM. amuleapi runs a 16-thread handler pool
// (kHandlerPoolThreads), so serving shared content out of a buffered body means
// up to 16 x filesize resident, with filesize routinely in the gigabytes for
// ed2k content. This body reads through one `kRangeFileReadChunkBytes` buffer owned by
// the writer, so the resident cost of a response is O(1) in the file size and
// the whole feature stops being a memory hazard.
//
// SCOPE
// -----
// Serialisation only. amuleapi never PARSES a file body — nothing uploads into
// a file through this transport — so the `reader` half of the Body concept is
// deliberately absent. `http::response_serializer` only ever instantiates
// `writer`, so the type still satisfies everything the write path asks for; a
// parser instantiated over this Body would fail to compile, which is the
// correct outcome rather than a silently wrong one.

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
#include <string>
#include <utility>

// Size of the writer's read buffer, and therefore the largest single chunk
// handed to the socket. 64 KiB is picked to be comfortably larger than a
// typical 16-64 KiB socket send buffer — so the syscall count is bounded by the
// socket, not by us — while still being small enough that the buffer lives
// inside the per-session serializer without mattering: at the file-response
// concurrency cap the transport enforces, the total is a few hundred KiB. The
// point of the whole type is that this number, not the file size, is what the
// process pays.
constexpr std::size_t kRangeFileReadChunkBytes = 64 * 1024;

// Boost.Beast Body concept implementation. `File` is the Beast file abstraction
// to use; the default `boost::beast::file` resolves to the native POSIX or
// Win32 implementation, matching what `beast::http::file_body` picks.
template <class File = boost::beast::file> struct BasicRangeFileBody
{
	static_assert(boost::beast::is_file<File>::value, "File type requirements not met");

	using file_type = File;

	class value_type;
	class writer;
	// No `reader`: see the SCOPE note at the top of this file.

	// Called from message::payload_size, which is what prepare_payload() uses
	// to stamp Content-Length. Returns the size of the WINDOW, not of the
	// file — the entire reason this type exists.
	static std::uint64_t size(const value_type &body) { return body.size(); }
};

// The `message::body` member for a message declared with this Body. Owns the
// open file handle, so the descriptor's lifetime is exactly the response's.
template <class File> class BasicRangeFileBody<File>::value_type
{
	friend class writer;
	friend struct BasicRangeFileBody;

	File m_file;
	// Size of the file as observed at open() time. Cached because the window
	// validation below has to compare against something stable: the file can
	// grow or shrink under us while we serialise, and answering a Range with
	// a Content-Length derived from a moving size is how a response ends up
	// contradicting its own framing.
	std::uint64_t m_file_size = 0;
	// Inclusive window. Only meaningful once SetWindow() has succeeded;
	// m_have_window keeps a caller that forgot from silently serialising
	// byte 0 alone.
	std::uint64_t m_first = 0;
	std::uint64_t m_last = 0;
	bool m_have_window = false;
	// Kept for diagnostics only — the open handle is what is actually read.
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

	// Set the inclusive byte window to serialise. Rejects anything that does
	// not describe at least one real byte of the file, rather than clamping:
	// a caller that computed a window from a stale size has already put a
	// `Content-Range` on the response describing the window it asked for, and
	// quietly shipping a different one would make the response lie about
	// itself. Failing here lets the transport answer 500 while the headers
	// are still ours to change.
	void SetWindow(std::uint64_t first, std::uint64_t last, boost::beast::error_code &ec)
	{
		if (!m_file.is_open() || first > last || first >= m_file_size || last >= m_file_size) {
			BOOST_BEAST_ASSIGN_EC(ec, boost::beast::http::error::bad_field);
			m_have_window = false;
			return;
		}
		m_first = first;
		m_last = last;
		m_have_window = true;
		ec = {};
	}

	// Number of bytes this body will put on the wire — the inclusive window
	// length. Zero before SetWindow(), which makes a forgotten call show up
	// as an empty response rather than as an unbounded one.
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
	char m_buf[kRangeFileReadChunkBytes];

public:
	using const_buffers_type = boost::asio::const_buffer;

	// The message is taken by non-const reference because reading advances
	// the file cursor: logically non-const even though it is bitwise const.
	// Same trade stock file_body makes — one thread may serialise a given
	// body at a time, which is exactly how the transport uses it (one
	// response, one session, one strand).
	template <bool isRequest, class Fields>
	writer(boost::beast::http::header<isRequest, Fields> &h, value_type &b)
	: m_body(b)
	, m_remain(b.size())
	{
		boost::ignore_unused(h);
		BOOST_ASSERT(m_body.m_file.is_open());
	}

	// Runs before any body byte is produced. This is where the file cursor
	// is placed at the start of the window.
	//
	// NOTE FOR HEAD: `serializer::next` calls init() even in split mode, i.e.
	// even when only `async_write_header` will ever run. That is fine and
	// intended — a seek moves a cursor, it reads nothing — and it is the
	// reason a HEAD on a file response costs one open + one lseek and not a
	// single byte of I/O, while still reporting the Content-Length a GET
	// would have produced (that number comes from Body::size, not from here).
	void init(boost::beast::error_code &ec)
	{
		m_body.m_file.seek(m_body.m_first, ec);
		if (ec) {
			return;
		}
		ec = {};
	}

	// Called repeatedly until it returns boost::none. Reads at most whatever
	// is left of the window, so the last chunk is naturally partial and we
	// can never over-read past `last` even though the file continues.
	boost::optional<std::pair<const_buffers_type, bool>> get(boost::beast::error_code &ec)
	{
		const std::size_t amount =
			m_remain > sizeof(m_buf) ? sizeof(m_buf) : static_cast<std::size_t>(m_remain);

		// Window fully served. (Also the degenerate zero-length case, which
		// SetWindow refuses to create but a defaulted value_type still has.)
		if (amount == 0) {
			ec = {};
			return boost::none;
		}

		const std::size_t nread = m_body.m_file.read(m_buf, amount, ec);
		if (ec) {
			return boost::none;
		}
		// A short read is legal and handled by simply returning fewer bytes;
		// a ZERO-length read is not, because we asked for bytes the cached
		// size said were there. That means the file was truncated under us
		// mid-response, and there is no honest way to finish a body whose
		// Content-Length is already on the wire — so surface it as an error
		// and let the transport tear the connection down, which is the only
		// signal a client can still read at that point.
		if (nread == 0) {
			BOOST_BEAST_ASSIGN_EC(ec, boost::beast::http::error::short_read);
			return boost::none;
		}
		BOOST_ASSERT(nread <= m_remain);

		m_remain -= nread;
		ec = {};
		return { { const_buffers_type{ m_buf, nread }, m_remain > 0 } };
	}
};

// The type the transport actually uses.
using RangeFileBody = BasicRangeFileBody<boost::beast::file>;

#endif // WEBAPI_RANGEFILEBODY_H
