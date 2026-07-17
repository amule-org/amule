# amuleapi — REST API + SSE daemon. Standalone executable that
# connects to amuled via the existing EC protocol (same wire as
# amulecmd / amuleweb) and serves /api/v0/* over HTTP via Boost.Beast.

add_executable (amuleapi
	${CMAKE_SOURCE_DIR}/src/ExternalConnector.cpp
	${CMAKE_SOURCE_DIR}/src/OtherFunctions.cpp
	${CMAKE_SOURCE_DIR}/src/RLE.cpp
	${CMAKE_SOURCE_DIR}/src/NetworkFunctions.cpp
	${CMAKE_SOURCE_DIR}/src/LoggerConsole.cpp
	AmuleApiConfig.cpp
	Api.cpp
	App.cpp
	Auth.cpp
	EventBus.cpp
	EventDiff.cpp
	HttpServer.cpp
	LogTee.cpp
	Refresher.cpp
	RefresherTick.cpp
	StaticFs.cpp
	State.cpp
)

if (WIN32)
	target_sources (amuleapi
		PRIVATE ${CMAKE_BINARY_DIR}/version.rc
	)
endif()

# Console / daemon — never builds wx GUI bits.
target_compile_definitions (amuleapi
	PRIVATE wxUSE_GUI=0
)

target_include_directories (amuleapi
	PRIVATE ${CMAKE_BINARY_DIR}
	PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
	PRIVATE ${SRC_DIR}
	PRIVATE ${Boost_INCLUDE_DIR}
)

target_link_libraries (amuleapi
	PRIVATE ec
	PRIVATE mulecommon
	PRIVATE mulesocket
	PRIVATE webcommon
	PRIVATE wxWidgets::NET
	PRIVATE ${Boost_LIBRARIES}
	# zlib powers the on-the-wire gzip encoder in HttpServer (Content-
	# Encoding: gzip on regular responses + per-event Z_SYNC_FLUSH on
	# SSE). NEED_ZLIB is already set by cmake/options.cmake for
	# BUILD_AMULEAPI, so zlib.cmake has already run at project scope
	# and ZLIB::ZLIB is available.
	PRIVATE ZLIB::ZLIB
)

# Same gettext shim as amuleweb — _() in the CLI text needs libintl on
# macOS / MinGW. Glibc bakes the symbols into libc, so the guard is a
# noop there.
if (ENABLE_NLS AND Intl_FOUND)
	target_include_directories (amuleapi PRIVATE ${Intl_INCLUDE_DIRS})
	target_link_libraries (amuleapi PRIVATE ${Intl_LIBRARIES})
endif()

if (HAVE_BFD)
	target_link_libraries (amuleapi
		PRIVATE ${LIBBFD}
	)
endif()

# ExternalConnector.cpp pulls in libreadline when HAVE_LIBREADLINE is
# defined at configure time (it's part of the readline-augmented TextShell
# implementation amulecmd uses for tab completion). amuleapi never
# enters TextShell, but the linker still resolves the references because
# they live in unconditionally-emitted symbols inside the same TU.
if (HAVE_LIBREADLINE)
	target_link_libraries (amuleapi
		PRIVATE ${READLINE_LIBRARIES}
	)
endif()

if (APPLE)
	# CoreServices / ApplicationServices — same set amuleweb links, used
	# by wxStandardPaths::GetUserDataDir() on a .app bundle to look up
	# Application Support/ correctly.
	target_link_libraries (amuleapi
		PRIVATE "-framework CoreServices"
		PRIVATE "-framework ApplicationServices"
	)
endif()

install (TARGETS amuleapi
	RUNTIME DESTINATION bin
)

# Bundled static-frontend tree. Empty StaticRoot in amuleapi.conf keeps
# the daemon API-only; setting it to ${CMAKE_INSTALL_DATADIR}/amule/
# amuleapi-static makes the daemon serve this tree (placeholder
# index.html out of the box — replace with a real frontend bundle in
# place to start serving one).
install (DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/static/
	DESTINATION ${CMAKE_INSTALL_DATADIR}/amule/amuleapi-static
)
