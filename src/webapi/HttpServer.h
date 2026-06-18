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

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>


// Boost.Beast-based HTTP/1.1 server. Runs in its own std::thread —
// boost::asio::io_context::run() is blocking and Beast's async
// chain stays inside that thread until Stop() is called.
//
// Phase 2 deliberately doesn't share state with the wxApp thread:
// the /api/v0/version handler reads a compile-time string. Phase 3
// introduces the wxQueueEvent-based EC bridge so handlers can fan
// out EC requests onto the wxApp thread; the bridge lives in a
// separate file (Api.cpp) and HttpServer stays transport-only.

namespace boost { namespace asio { class io_context; } }

class CHttpServer {
public:
	// The dispatch callback runs on the HTTP server's I/O thread.
	// Anything stateful it touches must be either thread-safe or
	// trampolined onto the wxApp thread (Phase 3+).
	struct Request {
		std::string method;            // "GET", "POST", ...
		std::string target;            // raw URI: "/api/v0/version?x=1"
		std::map<std::string, std::string> headers;
		std::string body;
		// Client IP as observed by the accept socket. amuleapi rate-
		// limits by this string verbatim; the `X-Forwarded-For` honor
		// toggle is a deferred Phase 9 follow-up (PLAN §12 Q6
		// implementation notes).
		std::string remote_addr;
	};

	struct Response {
		unsigned    status = 200;
		std::string content_type = "application/json";
		std::map<std::string, std::string> headers;
		std::string body;
	};

	using Handler = std::function<Response(const Request &)>;

	// Phase 8: long-lived streaming responses (SSE). The streaming
	// handler is given a `Writer` it can use to push chunks at will;
	// the connection stays open until the writer signals close or
	// the peer disconnects.
	class Writer {
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
	using StreamingHandler = std::function<void(
		const Request &, Writer &writer,
		unsigned &http_status, std::string &content_type,
		std::map<std::string, std::string> &response_headers)>;

	// Optional resolver: tells the HTTP server whether an incoming
	// request should be dispatched to the streaming handler (true) or
	// the normal Handler (false). Phase 8's wiring matches "GET
	// /api/v0/events"; other endpoints stay request/response.
	using StreamingResolver = std::function<bool(const Request &)>;

	// Bind + listen on `bind_address`:`port`. Returns false (and
	// populates LastError) on bind failure — the most common reason
	// is the port being in use by another amuleapi instance or a
	// stale TIME_WAIT socket.
	bool Start(const std::string &bind_address,
	           unsigned           port,
	           Handler            handler,
	           StreamingResolver  streaming_resolver = nullptr,
	           StreamingHandler   streaming_handler  = nullptr);

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
	std::string           m_lastError;
};


#endif // WEBAPI_HTTPSERVER_H
