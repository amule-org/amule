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

#ifndef WEBAPI_HTTPSERVER_H
#define WEBAPI_HTTPSERVER_H

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include <boost/optional.hpp>

// Boost.Beast-based HTTP/1.1 server. Runs in its own std::thread —
// boost::asio::io_context::run() is blocking and Beast's async
// chain stays inside that thread until Stop() is called.
//
// Deliberately doesn't share state with the wxApp thread. Handlers
// that need EC use the wxQueueEvent-based bridge in Api.cpp to fan
// out onto the wxApp thread; HttpServer stays transport-only.

namespace boost
{
namespace asio
{
class io_context;
}
} // namespace boost

// Does this Accept-Encoding value ask for gzip? Token search, so
// "gzip, deflate, br" and "gzip;q=1.0" both match and a longer name
// containing the letters does not.
//
// Shared rather than reimplemented: the dispatcher has to answer the same
// question when it describes what a GET would return, and a second copy of
// this parser is how the header-merge logic ended up in three places.
bool AcceptsGzip(const std::string &accept_encoding);

// Will a response with this body be gzipped on the way out?
//
// Shared because two places have to agree: the transport, which compresses and
// stamps the coding onto the validator, and the 304 paths, which must echo the
// SAME validator the equivalent 200 would have sent. They cannot each carry
// their own copy of the rule -- when they disagreed, a client that cached the
// gzip form was handed the identity ETag on revalidation and could never match
// its stored response again.
//
// `already_encoded` is true when a handler set Content-Encoding itself.
bool WillCompressBody(
	bool accepts_gzip, std::size_t body_size, const std::string &content_type, bool already_encoded);

// Add one token to a comma-separated header without dropping what is already
// there and without duplicating it. Comparison is token-by-token, so a short
// name cannot match inside a longer one.
//
// Declared here because every writer that merges a header has to share one
// definition: the logic previously existed in three copies, two consecutive
// rounds of fixes each reached a different two of them, and the copy nobody
// touched kept the old overwrite behaviour.
void AppendHeaderToken(std::map<std::string, std::string> &headers, const char *name, const char *token);

class CHttpServer
{
public:
	// The dispatch callback runs on the HTTP server's I/O thread.
	// Anything stateful it touches must be either thread-safe or
	// trampolined onto the wxApp thread.
	struct Request
	{
		std::string method; // "GET", "POST", ...
		std::string target; // raw URI: "/api/v0/version?x=1"
		std::map<std::string, std::string> headers;
		std::string body;
		// Client IP as observed by the accept socket. amuleapi rate-
		// limits by this string verbatim; the `X-Forwarded-For` honor
		// toggle is a deferred follow-up (		// implementation notes).
		std::string remote_addr;
	};

	struct Response
	{
		unsigned status = 200;
		std::string content_type = "application/json";
		std::map<std::string, std::string> headers;
		std::string body;

		// Serve the body straight off disk instead of out of `body`.
		//
		// Exists because the buffered `body` above is a memory hazard for
		// shared content: the handler pool is 16 threads wide, so a
		// multi-GB file answered through a std::string is up to 16 x
		// filesize resident. When `file` is set the transport streams the
		// window through a fixed 64 KiB buffer (see RangeFileBody.h) and
		// `body` is ignored entirely.
		struct FileSource
		{
			// Absolute path. The HANDLER owns resolution and the
			// containment check -- by the time the transport gets here
			// it opens the path blindly, and its only failure answer is
			// a 500, because a rejection at this depth may already be
			// too late to say anything more specific. Anything that
			// should have been a 403 or a 404 has to have been decided
			// upstream.
			std::string fs_path;
			// Inclusive window, RFC 9110 byte-range semantics: `last`
			// is the index of the last byte SENT, so a whole file of N
			// bytes is [0, N-1]. Both must lie inside the file; the
			// transport answers 500 rather than silently clamping,
			// since the handler has usually already described the
			// window in a `Content-Range` header. A zero-length file
			// has no valid window at all and belongs on the `body`
			// path.
			std::uint64_t first = 0;
			std::uint64_t last = 0;
		};
		// Setting this opts the response out of two things it would
		// otherwise get for free:
		//  * gzip. The transport never deflates a file response --
		//    compressing a range would break the byte accounting the
		//    Content-Range describes, and shared content is generally
		//    already entropy-coded anyway.
		//  * the dispatcher's whole-body ETag, which is computed by
		//    hashing `body`. There is no body here to hash, so a
		//    validator for a file response has to be built by the
		//    handler out of something cheap (size + mtime), and the
		//    dispatcher's gzip-coding suffix must not be applied to it.
		boost::optional<FileSource> file;
	};

	using Handler = std::function<Response(const Request &)>;

	// long-lived streaming responses (SSE). The streaming
	// handler is given a `Writer` it can use to push chunks at will;
	// the connection stays open until the writer signals close or
	// the peer disconnects.
	class Writer
	{
	public:
		// Write a chunk of bytes to the connection. Returns false if
		// the connection has been torn down (peer disconnect or
		// shutdown) — caller should stop pushing and let the session
		// die. Thread-safe: implementations post the write to the
		// io_context strand, so a caller on any thread is safe.
		virtual bool Write(const std::string &chunk) = 0;
		// True if the peer is still connected. Cheap to poll; useful
		// for the per-stream heartbeat timer to bail when the client
		// has hung up between pushes.
		virtual bool Alive() const = 0;
		virtual ~Writer() = default;
	};

	// StreamingHandler returns the response head (status, content_type,
	// initial headers) AND keeps writing chunks via the Writer until it
	// returns. The session's lifetime extends as long as the handler
	// hasn't returned AND the connection is alive — typical impls run
	// an event loop inside the handler and exit when the connection
	// closes (which Writer::Alive surfaces).
	using StreamingHandler = std::function<void(const Request &,
		Writer &writer,
		unsigned &http_status,
		std::string &content_type,
		std::map<std::string, std::string> &response_headers)>;

	// Optional resolver: tells the HTTP server whether an incoming
	// request should be dispatched to the streaming handler (true) or
	// the normal Handler (false). The current wiring matches "GET
	// /api/v0/events"; other endpoints stay request/response.
	using StreamingResolver = std::function<bool(const Request &)>;

	// Optional preflight: runs synchronously on the I/O thread BEFORE
	// the per-session worker thread is spawned and BEFORE the 32-slot
	// concurrency budget is claimed. Returns boost::none to admit the
	// connection; returns a populated Response to reject it (the
	// response is written verbatim and the connection closes). Used
	// to push the SSE auth check off the worker thread so an
	// unauthenticated peer can't tie up a slot for the read-timeout
	// window. Empty preflight (default) preserves the pre-existing
	// "auth inside the handler" behaviour.
	using StreamingPreflight = std::function<boost::optional<Response>(const Request &)>;

	// Stamps the CORS bundle on a response the transport built itself --
	// the 408 / 413 / 431 replies, which are written before any request
	// reaches a handler. Without it those are the only replies on the
	// surface a cross-origin browser client cannot read: it sees an opaque
	// fetch failure instead of the typed envelope. Takes the raw Origin
	// header, since at that point there is no parsed Request to hand over.
	using CorsStamper = std::function<void(
		std::map<std::string, std::string> &headers, const std::string &origin_header)>;

	// Bind + listen on `bind_address`:`port`. Returns false (and
	// populates LastError) on bind failure — the most common reason
	// is the port being in use by another amuleapi instance or a
	// stale TIME_WAIT socket.
	bool Start(const std::string &bind_address,
		unsigned port,
		Handler handler,
		StreamingResolver streaming_resolver = nullptr,
		StreamingHandler streaming_handler = nullptr,
		StreamingPreflight streaming_preflight = nullptr,
		CorsStamper cors_stamper = nullptr);

	// Stops the io_context, joins the thread. Safe to call from any
	// thread; Start() must have succeeded.
	void Stop();

	const std::string &LastError() const { return m_lastError; }

	// PIMPL — `Impl` holds the boost::asio io_context + the std::thread.
	// Constructor / destructor are declared but defined out-of-line in
	// HttpServer.cpp so callers don't need Boost.Asio's headers on the
	// include path and `std::is_destructible<CHttpServer>` doesn't probe
	// the incomplete `Impl` from foreign translation units.
	CHttpServer();
	~CHttpServer();

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
	std::string m_lastError;
};

#endif // WEBAPI_HTTPSERVER_H
