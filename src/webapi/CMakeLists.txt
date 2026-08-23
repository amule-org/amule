# amuleapi — REST API + SSE daemon. Standalone executable that
# connects to amuled via the existing EC protocol (same wire as
# amulecmd / amuleweb) and serves /api/v0/* over HTTP via Boost.Beast.

add_executable (amuleapi
	${CMAKE_SOURCE_DIR}/src/ExternalConnector.cpp
	# CUInt128 backs the EC_TAG_KAD_ID tag Refresher.cpp reads for
	# /kad's `node_id`. libec's ECUInt128.cpp only gets pulled out of
	# the archive once something references a UINT128 tag, and it then
	# needs the class itself -- which lives here, not in libec. Same
	# reason amulegui compiles this TU (src/CMakeLists.txt).
	${CMAKE_SOURCE_DIR}/src/kademlia/utils/UInt128.cpp
	${CMAKE_SOURCE_DIR}/src/OtherFunctions.cpp
	${CMAKE_SOURCE_DIR}/src/RLE.cpp
	${CMAKE_SOURCE_DIR}/src/NetworkFunctions.cpp
	${CMAKE_SOURCE_DIR}/src/LoggerConsole.cpp
	# Same embedded icon table the desktop GUI links (build-generated
	# by src/icons/embed_icons.py, or the checked-in fallback when the
	# host has no Python3 — see src/CMakeLists.txt for the resolution).
	# GET /flags/{code}.png serves the flag_<cc> entries straight out
	# of .rodata, which is what keeps the country artwork a single
	# committed copy instead of a duplicate under static/. Plain C byte
	# arrays, so it compiles fine under wxUSE_GUI=0; the top-level
	# icons amuleapi never asks for are pages that simply never fault
	# in.
	${AMULE_ICON_DATA_C}
	AmuleApiConfig.cpp
	Api.cpp
	App.cpp
	Auth.cpp
	EventBus.cpp
	EventDiff.cpp
	HttpServer.cpp
	LogTee.cpp
	PrefsSchema.cpp
	Refresher.cpp
	RefresherTick.cpp
	SearchJson.cpp
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
	# icon_data.c #includes "icon_data.h" unqualified, and Api.cpp does
	# the same to reach amule_find_icon() — same reason muleappgui puts
	# src/icons on its include path.
	PRIVATE ${SRC_DIR}/icons
	PRIVATE ${Boost_INCLUDE_DIR}
)

# ${AMULE_ICON_DATA_C} is generated into the build tree when Python3 was
# found at configure time; without it the variable already points at the
# checked-in source copy and no ordering dependency is needed. Mirrors
# what muleappgui does in src/CMakeLists.txt.
if (Python3_FOUND)
	add_dependencies (amuleapi generate_icon_data)
endif()

target_link_libraries (amuleapi
	PRIVATE ec
	PRIVATE mulecommon
	PRIVATE mulesocket
	PRIVATE webcommon
	PRIVATE picojson_header
	PRIVATE wxWidgets::NET
	PRIVATE ${Boost_LIBRARIES}
	# zlib powers the on-the-wire gzip encoder in HttpServer (Content-
	# Encoding: gzip on regular responses + per-event Z_SYNC_FLUSH on
	# SSE). NEED_ZLIB is already set by cmake/options.cmake for
	# BUILD_AMULEAPI, so zlib.cmake has already run at project scope
	# and ZLIB::ZLIB is available.
	PRIVATE ZLIB::ZLIB
)

# CEventBus's std::atomic<uint64_t> event counter needs libatomic on
# 32-bit targets without a hardware 8-byte CAS (PPC32, ARMv5/v6,
# MIPS32). muleappcore links it for the same reason, but amuleapi does
# not depend on muleappcore, so it needs its own copy. LIBATOMIC is set
# at the top of the root CMakeLists.txt — empty on 64-bit, "atomic" on
# 32-bit.
if (LIBATOMIC)
	target_link_libraries (amuleapi PRIVATE ${LIBATOMIC})
endif()

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
