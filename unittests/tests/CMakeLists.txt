add_executable (CTagTest
	CTagTest.cpp
	${CMAKE_SOURCE_DIR}/src/SafeFile.cpp
	${CMAKE_SOURCE_DIR}/src/MemFile.cpp
	${CMAKE_SOURCE_DIR}/src/Tag.cpp
	${CMAKE_SOURCE_DIR}/src/libs/common/Format.cpp
	${CMAKE_SOURCE_DIR}/src/libs/common/strerror_r.c
)

add_test (NAME CTagTest
	COMMAND CTagTest
)

target_include_directories (CTagTest
	PRIVATE ${CMAKE_BINARY_DIR}
	PRIVATE ${CMAKE_SOURCE_DIR}/src
	PRIVATE ${CMAKE_SOURCE_DIR}/src/include
)

target_link_libraries (CTagTest
	muleunit
)

# CValueMap is the incremental-update filter behind every EC tag that carries a
# value map. Its failure mode is silence -- a field wrongly judged unchanged
# just stops updating in the GUI -- so the contract is pinned here rather than
# left to a clean daemon run. Needs no app, no daemon and no database: the EC
# tag types and their generated codes are the whole dependency.
# How a peer's version is spelled. Each client project writes its own
# differently -- eMule letters the update (0.70b), eMule Plus omits zero
# components, lPhant counts its major one higher -- and the rendering used to
# exist twice: once in the handshake, once in the client-history rows, which
# re-derived it from the stored numeric and dropped every convention. The same
# peer could appear in one window under two versions depending only on whether
# it was online. Needs no app and no daemon.
add_executable (ClientVersionStringTest
	ClientVersionStringTest.cpp
	${CMAKE_SOURCE_DIR}/src/ClientVersionString.cpp
)

add_test (NAME ClientVersionStringTest
	COMMAND ClientVersionStringTest
)

target_include_directories (ClientVersionStringTest
	PRIVATE ${CMAKE_BINARY_DIR}
	PRIVATE ${CMAKE_SOURCE_DIR}/src
	PRIVATE ${CMAKE_SOURCE_DIR}/src/include
	PRIVATE ${CMAKE_SOURCE_DIR}/src/libs
)

target_link_libraries (ClientVersionStringTest
	muleunit
	mulecommon
)

add_executable (ECIdDiffTest
	ECIdDiffTest.cpp
	# See the note on CValueMapTest below: libec.a references DoECLogLine /
	# theLogger unconditionally in Debug, so the no-op definitions have to be
	# linked in or only the Debug matrix entries fail.
	${CMAKE_SOURCE_DIR}/src/LoggerConsole.cpp
)

add_test (NAME ECIdDiffTest
	COMMAND ECIdDiffTest
)

target_include_directories (ECIdDiffTest
	PRIVATE ${CMAKE_BINARY_DIR}
	PRIVATE ${CMAKE_SOURCE_DIR}/src
	PRIVATE ${CMAKE_SOURCE_DIR}/src/include
	PRIVATE ${CMAKE_SOURCE_DIR}/src/libs
	PRIVATE ${CMAKE_BINARY_DIR}/src/libs
	PRIVATE ${CMAKE_BINARY_DIR}/src/libs/ec/cpp
)

target_link_libraries (ECIdDiffTest
	muleunit
	ec
	mulecommon
)

# amulecmd's `progress` output. The daemon answers EC_OP_SEARCH_PROGRESS in
# three shapes and the client does not choose which: advertising the union
# capability alongside multi-search is what decides it. Reading one shape as
# another is invisible -- a top-level tag that is not there reads as zero, so
# `progress` reported 0 % for every search whatever it was doing (#1126).
# Needs no app and no daemon: the EC tag types are the whole dependency.
add_executable (SearchProgressReportTest
	SearchProgressReportTest.cpp
	${CMAKE_SOURCE_DIR}/src/SearchProgressReport.cpp
	# As for ECIdDiffTest: libec.a references DoECLogLine / theLogger
	# unconditionally in Debug, so the no-op definitions have to be linked in.
	${CMAKE_SOURCE_DIR}/src/LoggerConsole.cpp
)

add_test (NAME SearchProgressReportTest
	COMMAND SearchProgressReportTest
)

target_include_directories (SearchProgressReportTest
	PRIVATE ${CMAKE_BINARY_DIR}
	PRIVATE ${CMAKE_SOURCE_DIR}/src
	PRIVATE ${CMAKE_SOURCE_DIR}/src/include
	PRIVATE ${CMAKE_SOURCE_DIR}/src/libs
	PRIVATE ${CMAKE_BINARY_DIR}/src/libs
	PRIVATE ${CMAKE_BINARY_DIR}/src/libs/ec/cpp
)

target_link_libraries (SearchProgressReportTest
	muleunit
	ec
	mulecommon
)

add_executable (ECSocketFlagsTest
	ECSocketFlagsTest.cpp
	# As for ECIdDiffTest: libec.a references DoECLogLine / theLogger
	# unconditionally in Debug, so the no-op definitions have to be linked in.
	${CMAKE_SOURCE_DIR}/src/LoggerConsole.cpp
)

add_test (NAME ECSocketFlagsTest
	COMMAND ECSocketFlagsTest
)

target_include_directories (ECSocketFlagsTest
	PRIVATE ${CMAKE_BINARY_DIR}
	PRIVATE ${CMAKE_SOURCE_DIR}/src
	PRIVATE ${CMAKE_SOURCE_DIR}/src/include
	PRIVATE ${CMAKE_SOURCE_DIR}/src/libs
	PRIVATE ${CMAKE_BINARY_DIR}/src/libs
	PRIVATE ${CMAKE_BINARY_DIR}/src/libs/ec/cpp
)

target_link_libraries (ECSocketFlagsTest
	muleunit
	ec
	mulecommon
)

add_executable (CValueMapTest
	CValueMapTest.cpp
	# LoggerConsole.cpp provides the no-op DoECLogLine + theLogger symbols that
	# libec.a's Debug build references unconditionally (ECPacket / ECSocket /
	# ECTag debug-print paths). Release preprocesses those calls to
	# `do {} while(0)` via the ECLog.h define, so the symbols are unused there
	# and only the Debug matrix entries fail to link. Same reason RefresherTest
	# pulls this TU in.
	${CMAKE_SOURCE_DIR}/src/LoggerConsole.cpp
)

add_test (NAME CValueMapTest
	COMMAND CValueMapTest
)

target_include_directories (CValueMapTest
	PRIVATE ${CMAKE_BINARY_DIR}
	PRIVATE ${CMAKE_SOURCE_DIR}/src
	PRIVATE ${CMAKE_SOURCE_DIR}/src/include
	PRIVATE ${CMAKE_SOURCE_DIR}/src/libs
	PRIVATE ${CMAKE_BINARY_DIR}/src/libs
	PRIVATE ${CMAKE_BINARY_DIR}/src/libs/ec/cpp
)

target_link_libraries (CValueMapTest
	muleunit
	ec
	mulecommon
)

add_executable (CUInt128Test
	CUInt128Test.cpp
	${CMAKE_SOURCE_DIR}/src/kademlia/utils/UInt128.cpp
	${CMAKE_SOURCE_DIR}/src/libs/common/Format.cpp
	${CMAKE_SOURCE_DIR}/src/libs/common/strerror_r.c
)

add_test (NAME CUInt128Test
	COMMAND CUInt128Test
)

target_include_directories (CUInt128Test
	PRIVATE ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries (CUInt128Test
	muleunit
)

add_executable (FileDataIOTest
	FileDataIOTest.cpp
	${CMAKE_SOURCE_DIR}/src/SafeFile.cpp
	${CMAKE_SOURCE_DIR}/src/CFile.cpp
	${CMAKE_SOURCE_DIR}/src/MemFile.cpp
	${CMAKE_SOURCE_DIR}/src/kademlia/utils/UInt128.cpp
	${CMAKE_SOURCE_DIR}/src/Tag.cpp
	${CMAKE_SOURCE_DIR}/src/libs/common/Path.cpp
	${CMAKE_SOURCE_DIR}/src/libs/common/Format.cpp
	${CMAKE_SOURCE_DIR}/src/libs/common/strerror_r.c
)

add_test (NAME FileDataIOTest
	COMMAND FileDataIOTest
)

target_include_directories (FileDataIOTest
	PRIVATE ${CMAKE_BINARY_DIR}
	PRIVATE ${CMAKE_SOURCE_DIR}/src
	PRIVATE ${CMAKE_SOURCE_DIR}/src/include
)

target_link_libraries (FileDataIOTest
	muleunit
)

add_executable (CloneFileTest
	CloneFileTest.cpp
	${CMAKE_SOURCE_DIR}/src/SafeFile.cpp
	${CMAKE_SOURCE_DIR}/src/CFile.cpp
	${CMAKE_SOURCE_DIR}/src/MemFile.cpp
	${CMAKE_SOURCE_DIR}/src/kademlia/utils/UInt128.cpp
	${CMAKE_SOURCE_DIR}/src/Tag.cpp
	${CMAKE_SOURCE_DIR}/src/libs/common/Path.cpp
	${CMAKE_SOURCE_DIR}/src/libs/common/Format.cpp
	${CMAKE_SOURCE_DIR}/src/libs/common/strerror_r.c
)

add_test (NAME CloneFileTest
	COMMAND CloneFileTest
)

target_include_directories (CloneFileTest
	PRIVATE ${CMAKE_BINARY_DIR}
	PRIVATE ${CMAKE_SOURCE_DIR}/src
	PRIVATE ${CMAKE_SOURCE_DIR}/src/include
)

target_link_libraries (CloneFileTest
	muleunit
)

add_executable (FormatTest
	FormatTest.cpp
	${CMAKE_SOURCE_DIR}/src/libs/common/Format.cpp
	${CMAKE_SOURCE_DIR}/src/libs/common/strerror_r.c
)

add_test (NAME FormatTest
	COMMAND FormatTest
)

target_include_directories (FormatTest
	PRIVATE ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries (FormatTest
	muleunit
)

add_executable (NetworkFunctionsTest
	NetworkFunctionsTest.cpp
	${CMAKE_SOURCE_DIR}/src/NetworkFunctions.cpp
	${CMAKE_SOURCE_DIR}/src/LibSocket.cpp
	${CMAKE_SOURCE_DIR}/src/libs/common/Format.cpp
	${CMAKE_SOURCE_DIR}/src/libs/common/strerror_r.c
)

add_test (NAME NetworkFunctionsTest
	COMMAND NetworkFunctionsTest
)

target_compile_definitions (NetworkFunctionsTest
	PRIVATE "EC_REMOTE"
)

target_include_directories (NetworkFunctionsTest
	PRIVATE ${CMAKE_SOURCE_DIR}/src
	PRIVATE ${CMAKE_SOURCE_DIR}/src/include
)

target_include_directories (NetworkFunctionsTest
	PRIVATE ${Boost_INCLUDE_DIR}
)

target_link_libraries (NetworkFunctionsTest
	${Boost_LIBRARIES}
)

target_link_libraries (NetworkFunctionsTest
	muleunit
	wxWidgets::NET
)

if (WIN32)
	# This test compiles LibSocket.cpp directly (not via the mulesocket
	# library), so it needs iphlpapi for GetAdaptersAddresses() itself.
	target_link_libraries (NetworkFunctionsTest iphlpapi)
endif()

add_executable (PathTest
	PathTest.cpp
	${CMAKE_SOURCE_DIR}/src/libs/common/Format.cpp
	${CMAKE_SOURCE_DIR}/src/libs/common/strerror_r.c
	${CMAKE_SOURCE_DIR}/src/libs/common/Path.cpp
)

add_test (NAME PathTest
	COMMAND PathTest
)

target_link_libraries (PathTest
	muleunit
)

add_executable (RangeMapTest
	RangeMapTest.cpp
	${CMAKE_SOURCE_DIR}/src/libs/common/Format.cpp
	${CMAKE_SOURCE_DIR}/src/libs/common/strerror_r.c
)

add_test (NAME RangeMapTest
	COMMAND RangeMapTest
)

target_include_directories (RangeMapTest
	PRIVATE ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries (RangeMapTest
	muleunit
)

add_executable (StringFunctionsTest
	StringFunctionsTest.cpp
	${CMAKE_SOURCE_DIR}/src/libs/common/Format.cpp
	${CMAKE_SOURCE_DIR}/src/libs/common/strerror_r.c
	${CMAKE_SOURCE_DIR}/src/libs/common/Path.cpp
)

add_test (NAME StringFunctionsTest
	COMMAND StringFunctionsTest
)

target_link_libraries (StringFunctionsTest
	muleunit
)

add_executable (PhpArrayTest
	PhpArrayTest.cpp
	${CMAKE_SOURCE_DIR}/src/webserver/src/php_syntree.cpp
	${CMAKE_SOURCE_DIR}/src/libs/common/Format.cpp
	${CMAKE_SOURCE_DIR}/src/libs/common/strerror_r.c
)

add_test (NAME PhpArrayTest
	COMMAND PhpArrayTest
)

target_compile_definitions (PhpArrayTest
	PRIVATE PHP_STANDALONE_EN
)

target_include_directories (PhpArrayTest
	PRIVATE ${CMAKE_SOURCE_DIR}/src/webserver/src
)

target_link_libraries (PhpArrayTest
	muleunit
)

add_executable (CMuleCollectionTest
	CMuleCollectionTest.cpp
	${CMAKE_SOURCE_DIR}/src/MuleCollection.cpp
	# muleunit's MuleDebug.cpp needs CFormat for its backtrace path, which
	# is compiled in on glibc; without these the link fails on Linux while
	# passing on macOS.
	${CMAKE_SOURCE_DIR}/src/libs/common/Format.cpp
	${CMAKE_SOURCE_DIR}/src/libs/common/strerror_r.c
)

add_test (NAME CMuleCollectionTest
	COMMAND CMuleCollectionTest
)

target_include_directories (CMuleCollectionTest
	PRIVATE ${CMAKE_SOURCE_DIR}/src
	PRIVATE ${CMAKE_SOURCE_DIR}/src/include
)

target_link_libraries (CMuleCollectionTest
	muleunit
)

add_executable (MagnetURITest
	MagnetURITest.cpp
	${CMAKE_SOURCE_DIR}/src/MagnetURI.cpp
	# See the CMuleCollectionTest comment above -- same Linux-backtrace need.
	${CMAKE_SOURCE_DIR}/src/libs/common/Format.cpp
	${CMAKE_SOURCE_DIR}/src/libs/common/strerror_r.c
)

add_test (NAME MagnetURITest
	COMMAND MagnetURITest
)

target_include_directories (MagnetURITest
	PRIVATE ${CMAKE_SOURCE_DIR}/src
	PRIVATE ${CMAKE_SOURCE_DIR}/src/include
)

target_link_libraries (MagnetURITest
	muleunit
)

add_executable (SearchHistoryTest
	SearchHistoryTest.cpp
	${CMAKE_SOURCE_DIR}/src/SearchHistory.cpp
	# See the CMuleCollectionTest comment above -- same Linux-backtrace need.
	${CMAKE_SOURCE_DIR}/src/libs/common/Format.cpp
	${CMAKE_SOURCE_DIR}/src/libs/common/strerror_r.c
)

add_test (NAME SearchHistoryTest
	COMMAND SearchHistoryTest
)

target_include_directories (SearchHistoryTest
	PRIVATE ${CMAKE_SOURCE_DIR}/src
	PRIVATE ${CMAKE_SOURCE_DIR}/src/include
)

target_link_libraries (SearchHistoryTest
	muleunit
)

add_executable (TextFileTest
	TextFileTest.cpp
	${CMAKE_SOURCE_DIR}/src/libs/common/Format.cpp
	${CMAKE_SOURCE_DIR}/src/libs/common/strerror_r.c
	${CMAKE_SOURCE_DIR}/src/libs/common/Path.cpp
	${CMAKE_SOURCE_DIR}/src/libs/common/TextFile.cpp
)

add_test (NAME TextFileTest
	COMMAND TextFileTest
)

target_compile_definitions (TextFileTest
	PRIVATE "SRCDIR=${CMAKE_CURRENT_SOURCE_DIR}"
)

target_include_directories (TextFileTest
	PRIVATE ${CMAKE_SOURCE_DIR}/src
	PRIVATE ${CMAKE_SOURCE_DIR}/src/include
)

target_link_libraries (TextFileTest
	muleunit
)

add_executable (OtherFunctionsTest
	OtherFunctionsTest.cpp
	${CMAKE_SOURCE_DIR}/src/OtherFunctions.cpp
)

add_test (NAME OtherFunctionsTest
	COMMAND OtherFunctionsTest
)

target_include_directories (OtherFunctionsTest
	PRIVATE ${CMAKE_BINARY_DIR}
	PRIVATE ${CMAKE_SOURCE_DIR}/src
	PRIVATE ${CMAKE_SOURCE_DIR}/src/include
)

target_link_libraries (OtherFunctionsTest
	muleunit
	mulecommon
	CRYPTOPP::CRYPTOPP
)


# libwebcommon — JWT / JSON writer / route patterns. `webcommon` PUBLIC-links
# wxWidgets::BASE and CRYPTOPP::CRYPTOPP and exposes its own headers through
# INTERFACE_INCLUDE_DIRECTORIES, so each test target only needs to depend on
# `muleunit` and `webcommon`.

add_executable (CredentialsTest
	CredentialsTest.cpp
	# muleunit's MuleDebug.cpp needs CFormat for its backtrace path, which
	# is compiled in on glibc; without these the link fails on Linux while
	# passing on macOS.
	${CMAKE_SOURCE_DIR}/src/libs/common/Format.cpp
	${CMAKE_SOURCE_DIR}/src/libs/common/strerror_r.c
)

add_test (NAME CredentialsTest COMMAND CredentialsTest)

target_link_libraries (CredentialsTest
	muleunit
	webcommon
)

add_executable (JwtTest
	JwtTest.cpp
)
add_test (NAME JwtTest COMMAND JwtTest)
target_link_libraries (JwtTest
	muleunit
	webcommon
	mulecommon
)

add_executable (ECCryptTest
	ECCryptTest.cpp
	${CMAKE_SOURCE_DIR}/src/libs/ec/cpp/ECCrypt.cpp
)
add_test (NAME ECCryptTest COMMAND ECCryptTest)
target_include_directories (ECCryptTest
	PRIVATE ${CMAKE_BINARY_DIR}
	PRIVATE ${CMAKE_SOURCE_DIR}/src
	PRIVATE ${CMAKE_SOURCE_DIR}/src/libs/ec/cpp
)
target_link_libraries (ECCryptTest
	muleunit
	mulecommon
	CRYPTOPP::CRYPTOPP
)

add_executable (SharedFilesReloadLatchTest
	SharedFilesReloadLatchTest.cpp
)
add_test (NAME SharedFilesReloadLatchTest COMMAND SharedFilesReloadLatchTest)
target_include_directories (SharedFilesReloadLatchTest
	PRIVATE ${CMAKE_BINARY_DIR}
	PRIVATE ${CMAKE_SOURCE_DIR}/src
	PRIVATE ${CMAKE_SOURCE_DIR}/src/include
)
target_link_libraries (SharedFilesReloadLatchTest
	muleunit
	mulecommon
)

add_executable (ChatSessionStoreTest
	ChatSessionStoreTest.cpp
	${CMAKE_SOURCE_DIR}/src/ChatSessionStore.cpp
)
add_test (NAME ChatSessionStoreTest COMMAND ChatSessionStoreTest)
target_include_directories (ChatSessionStoreTest
	PRIVATE ${CMAKE_BINARY_DIR}
	PRIVATE ${CMAKE_SOURCE_DIR}/src
	PRIVATE ${CMAKE_SOURCE_DIR}/src/include
)
target_link_libraries (ChatSessionStoreTest
	muleunit
	mulecommon
)

add_executable (MediaProbeParseTest
	MediaProbeParseTest.cpp
	${CMAKE_SOURCE_DIR}/src/MediaProbe.cpp
)
add_test (NAME MediaProbeParseTest COMMAND MediaProbeParseTest)
target_include_directories (MediaProbeParseTest
	PRIVATE ${CMAKE_BINARY_DIR}
	PRIVATE ${CMAKE_SOURCE_DIR}/src
	PRIVATE ${CMAKE_SOURCE_DIR}/src/include
)
target_link_libraries (MediaProbeParseTest
	muleunit
	mulecommon
)

add_executable (JsonWriterTest
	JsonWriterTest.cpp
)
add_test (NAME JsonWriterTest COMMAND JsonWriterTest)
target_link_libraries (JsonWriterTest
	muleunit
	webcommon
	mulecommon
)

add_executable (PathPatternsTest
	PathPatternsTest.cpp
)
add_test (NAME PathPatternsTest COMMAND PathPatternsTest)
target_link_libraries (PathPatternsTest
	muleunit
	webcommon
	mulecommon
)

add_executable (EtagTest
	EtagTest.cpp
)
add_test (NAME EtagTest COMMAND EtagTest)
target_link_libraries (EtagTest
	muleunit
	webcommon
	mulecommon
)

add_executable (StaticFsTest
	StaticFsTest.cpp
	${CMAKE_SOURCE_DIR}/src/webapi/StaticFs.cpp
)
target_include_directories (StaticFsTest
	PRIVATE ${CMAKE_SOURCE_DIR}/src/webapi
)
add_test (NAME StaticFsTest COMMAND StaticFsTest)
target_link_libraries (StaticFsTest
	muleunit
	mulecommon
)

# amuleapi shared-file content endpoint: path containment against the
# share-root list, Range parsing, and the Content-Disposition builder.
# SharedContent.cpp is plain std/POSIX (no wx, Beast or EC), same as
# StaticFs.cpp, so it links standalone here.
add_executable (SharedContentTest
	SharedContentTest.cpp
	${CMAKE_SOURCE_DIR}/src/webapi/SharedContent.cpp
)
target_include_directories (SharedContentTest
	PRIVATE ${CMAKE_SOURCE_DIR}/src/webapi
)
add_test (NAME SharedContentTest COMMAND SharedContentTest)
target_link_libraries (SharedContentTest
	muleunit
	mulecommon
)

add_executable (EventBusTest
	EventBusTest.cpp
	${CMAKE_SOURCE_DIR}/src/webapi/EventBus.cpp
)
target_include_directories (EventBusTest
	PRIVATE ${CMAKE_SOURCE_DIR}/src/webapi
)
add_test (NAME EventBusTest COMMAND EventBusTest)
target_link_libraries (EventBusTest
	muleunit
	mulecommon
)

# CEventBus's std::atomic<uint64_t> event counter needs libatomic on
# 32-bit targets without a hardware 8-byte CAS (PPC32, ARMv5/v6,
# MIPS32); same reason amuleapi links it. Empty on 64-bit.
if (LIBATOMIC)
	target_link_libraries (EventBusTest ${LIBATOMIC})
endif()

# amuleapi log-file tee: exercises CRotatingLog's append + two-file size cap.
# LogTee.cpp is plain POSIX/std (no wx), so only muleunit + mulecommon are
# needed for the harness.
add_executable (LogTeeTest
	LogTeeTest.cpp
	${CMAKE_SOURCE_DIR}/src/webapi/LogTee.cpp
)
target_include_directories (LogTeeTest
	PRIVATE ${CMAKE_SOURCE_DIR}/src/webapi
)
add_test (NAME LogTeeTest COMMAND LogTeeTest)
target_link_libraries (LogTeeTest
	muleunit
	mulecommon
)

# Tests for EmitDiffsAndUpdate's log_appended path and cold-start
# gating. Pulls EventBus + State along with EventDiff itself — no EC,
# and the only wx dependency is the shared search-result writer below.
add_executable (EventDiffTest
	EventDiffTest.cpp
	${CMAKE_SOURCE_DIR}/src/webapi/EventBus.cpp
	${CMAKE_SOURCE_DIR}/src/webapi/EventDiff.cpp
	# search_result_added is documented as carrying exactly a results-list
	# entry, so EventDiff emits it through the same writer the REST handler
	# uses instead of a second hand-rolled copy. That writer speaks
	# CJsonWriter, which is what brings webcommon (and wx base) in here.
	${CMAKE_SOURCE_DIR}/src/webapi/SearchJson.cpp
	${CMAKE_SOURCE_DIR}/src/webapi/State.cpp
)
target_include_directories (EventDiffTest
	PRIVATE ${CMAKE_SOURCE_DIR}/src/webapi
	# ServerFlagNames.h reads the SRV_*FLG_* masks straight from the eD2k
	# protocol headers rather than restating them.
	PRIVATE ${CMAKE_SOURCE_DIR}/src/include
)
add_test (NAME EventDiffTest COMMAND EventDiffTest)
target_link_libraries (EventDiffTest
	muleunit
	mulecommon
	webcommon
	wxWidgets::BASE
)

# Pulls EventBus.cpp, so it needs libatomic on 32-bit targets too.
if (LIBATOMIC)
	target_link_libraries (EventDiffTest ${LIBATOMIC})
endif()

# amuleapi-specific: tests CAmuleApiConfig's file-IO + mode-bit
# enforcement. AmuleApiConfig.cpp is statically linked into the test
# binary (rather than a webapi-side static lib) — it's a single TU and
# the amuleapi exe is the only other consumer, so an extra library
# target for one TU would just add noise.
add_executable (AmuleApiConfigTest
	AmuleApiConfigTest.cpp
	${CMAKE_SOURCE_DIR}/src/webapi/AmuleApiConfig.cpp
)
add_test (NAME AmuleApiConfigTest COMMAND AmuleApiConfigTest)
target_include_directories (AmuleApiConfigTest
	PRIVATE ${CMAKE_SOURCE_DIR}/src/webapi
)
target_link_libraries (AmuleApiConfigTest
	muleunit
	webcommon
	mulecommon
	wxWidgets::BASE
)

# amuleapi auth state — revocation set, rate limiter, header
# extraction, ISO-8601 formatter. Same vendoring pattern as the
# Config test: Auth.cpp is pulled in directly because amuleapi is
# the only other consumer.
add_executable (AuthTest
	AuthTest.cpp
	${CMAKE_SOURCE_DIR}/src/webapi/Auth.cpp
)
add_test (NAME AuthTest COMMAND AuthTest)
target_include_directories (AuthTest
	PRIVATE ${CMAKE_SOURCE_DIR}/src/webapi
)
target_link_libraries (AuthTest
	muleunit
	webcommon
	mulecommon
)

# EC password-exchange throttle: exercises the limiter at the values the
# listener ships with. The limiter lives in mulecommon, so nothing
# webapi-specific is needed here.
add_executable (RateLimiterTest
	RateLimiterTest.cpp
)
add_test (NAME RateLimiterTest COMMAND RateLimiterTest)
target_link_libraries (RateLimiterTest
	muleunit
	mulecommon
)

# amuleapi state cache — snapshot semantics + concurrent-reader
# correctness on the shared_timed_mutex.
add_executable (StateTest
	StateTest.cpp
	${CMAKE_SOURCE_DIR}/src/webapi/State.cpp
)
add_test (NAME StateTest COMMAND StateTest)
target_include_directories (StateTest
	PRIVATE ${CMAKE_SOURCE_DIR}/src/webapi
)
target_link_libraries (StateTest
	muleunit
	mulecommon
)

# amuleapi refresher — the pure EC-tag-to-State translation layer
# (Refresher.cpp) is linkable without the CamuleapiApp dependency
# tree because the orchestration body lives in RefresherTick.cpp
# (kept out of this target). Tests cover the EC_TAG_FILE_REMOVED
# delete marker, the alive-marker enqueue path, the unknown-ECID-
# no-op defence (#713-class race), and partial-update-active.
add_executable (RefresherTest
	RefresherTest.cpp
	# Refresher.cpp decodes EC_TAG_KAD_ID into a CUInt128; the class
	# lives outside libec, so the archive's ECUInt128.cpp cannot
	# resolve on its own. Mirrors amuleapi's own webapi/CMakeLists.txt.
	${CMAKE_SOURCE_DIR}/src/kademlia/utils/UInt128.cpp
	${CMAKE_SOURCE_DIR}/src/webapi/PrefsSchema.cpp
	${CMAKE_SOURCE_DIR}/src/webapi/Refresher.cpp
	${CMAKE_SOURCE_DIR}/src/webapi/State.cpp
	# OtherFunctions.cpp ships the `CastIto*` / `CastSecondsToHM`
	# formatters that `CEC_StatTree_Node_Tag::GetDisplayString` pulls
	# in via libec.a's `FormatValue`. Without this the ParseStatsTree
	# test can't link.
	${CMAKE_SOURCE_DIR}/src/OtherFunctions.cpp
	# RLE.cpp ships `PartFileEncoderData` + the stateful `RLE_Data`
	# decoder Refresher.cpp uses for `progress.parts`. Required even
	# for tests that don't directly invoke the decoder — the public
	# Apply{Update,Full} signatures take `std::map<ecid,
	# PartFileEncoderData>&` so the class is part of the link.
	${CMAKE_SOURCE_DIR}/src/RLE.cpp
	# LoggerConsole.cpp provides the no-op DoECLogLine + theLogger
	# symbols that libec.a's Debug build references unconditionally
	# (ECPacket / ECSocket / ECTag debug-print paths). Release builds
	# preprocess these calls to `do {} while(0)` via the ECLog.h
	# define, so the symbols are unused there; macOS Debug + Ubuntu
	# Debug builds link them in. Mirror amuleapi's own webapi/
	# CMakeLists.txt, which already pulls this TU in.
	${CMAKE_SOURCE_DIR}/src/LoggerConsole.cpp
)
add_test (NAME RefresherTest COMMAND RefresherTest)
target_include_directories (RefresherTest
	PRIVATE ${CMAKE_SOURCE_DIR}/src/webapi
	PRIVATE ${CMAKE_SOURCE_DIR}/src
	PRIVATE ${CMAKE_SOURCE_DIR}/src/include
	PRIVATE ${CMAKE_BINARY_DIR}
)
target_link_libraries (RefresherTest
	muleunit
	ec
	mulecommon
	mulesocket
	webcommon
	wxWidgets::NET
)

# CompleteSourcesNeedRecompute decides whether the complete-sources throttle
# must be bypassed because the upload list went empty. #1052 added that bypass
# but guarded it on the scalar alone, leaving Hi stale (#1065), so the
# predicate now lives in a header of its own: CKnownFile reaches theApp and
# cannot be linked into a test.
add_executable (JsonDepthScanTest
	JsonDepthScanTest.cpp
)

add_test (NAME JsonDepthScanTest
	COMMAND JsonDepthScanTest
)

target_include_directories (JsonDepthScanTest
	PRIVATE ${CMAKE_BINARY_DIR}
	PRIVATE ${CMAKE_SOURCE_DIR}/src
	PRIVATE ${CMAKE_SOURCE_DIR}/src/include
	PRIVATE ${CMAKE_SOURCE_DIR}/src/webapi
)

# mulecommon even though the header is inline-only: macOS links without it,
# GNU ld and mingw do not.
target_link_libraries (JsonDepthScanTest
	muleunit
	mulecommon
)

add_executable (CompleteSourcesThrottleTest
	CompleteSourcesThrottleTest.cpp
)

add_test (NAME CompleteSourcesThrottleTest
	COMMAND CompleteSourcesThrottleTest
)

target_include_directories (CompleteSourcesThrottleTest
	PRIVATE ${CMAKE_BINARY_DIR}
	PRIVATE ${CMAKE_SOURCE_DIR}/src
	PRIVATE ${CMAKE_SOURCE_DIR}/src/include
)

# mulecommon even though the header is inline-only: macOS links without it,
# GNU ld and mingw do not.
target_link_libraries (CompleteSourcesThrottleTest
	muleunit
	mulecommon
)

# The browse ("View Files") state machine. Five separate defects have been the
# same machine going wrong in a place only a peer of the right kind could reach
# -- a flat browse that completes without ever being marked finished, a peer we
# decline to contact, a terminal browse still on the pending list (#1071 and
# its follow-ups). None were reachable from a test while the logic sat on
# CUpDownClient, so it lives in a pair of its own that touches no theApp.
add_executable (BrowseLifecycleTest
	BrowseLifecycleTest.cpp
	${CMAKE_SOURCE_DIR}/src/BrowseLifecycle.cpp
)

add_test (NAME BrowseLifecycleTest
	COMMAND BrowseLifecycleTest
)

target_include_directories (BrowseLifecycleTest
	PRIVATE ${CMAKE_BINARY_DIR}
	PRIVATE ${CMAKE_SOURCE_DIR}/src
	PRIVATE ${CMAKE_SOURCE_DIR}/src/include
)

target_link_libraries (BrowseLifecycleTest
	muleunit
	mulecommon
)

# The rules about browses -- one per peer, a terminal record outliving its
# client, a disconnect after success not being a failure. They used to live on
# CBrowseManager next to the notify and the log line, which put them out of a
# test's reach; the two defects found after the lifecycle was extracted were
# both here rather than in the state machine. The store identifies peers by an
# opaque key and touches no theApp, so all of it is drivable.
add_executable (BrowseStoreTest
	BrowseStoreTest.cpp
	${CMAKE_SOURCE_DIR}/src/BrowseStore.cpp
	${CMAKE_SOURCE_DIR}/src/BrowseLifecycle.cpp
)

add_test (NAME BrowseStoreTest
	COMMAND BrowseStoreTest
)

target_include_directories (BrowseStoreTest
	PRIVATE ${CMAKE_BINARY_DIR}
	PRIVATE ${CMAKE_SOURCE_DIR}/src
	PRIVATE ${CMAKE_SOURCE_DIR}/src/include
)

target_link_libraries (BrowseStoreTest
	muleunit
	mulecommon
)

# CListColumnStore maps a column id to its persistence key and default width.
# The key is the on-disk config format, so the id a caller registers has to be
# the id it looks the column up by -- a registry that renumbered stored ids
# would hand a saved width back to the wrong column, with nothing to see. Needs
# no GUI: the registry half of the class is plain wxString bookkeeping.
add_executable (ListColumnStoreTest
	ListColumnStoreTest.cpp
	${CMAKE_SOURCE_DIR}/src/ListColumnStore.cpp
)

add_test (NAME ListColumnStoreTest
	COMMAND ListColumnStoreTest
)

target_include_directories (ListColumnStoreTest
	PRIVATE ${CMAKE_BINARY_DIR}
	PRIVATE ${CMAKE_SOURCE_DIR}/src
	PRIVATE ${CMAKE_SOURCE_DIR}/src/include
)

target_link_libraries (ListColumnStoreTest
	muleunit
	mulecommon
)

# RangeFileBody is the Boost.Beast Body amuleapi serves shared content with: a
# bounded [first, last] window read through a fixed 64 KiB buffer. Header-only
# and free of wx and of the rest of the webapi tree, so the test compiles on
# its own -- it only needs the Boost headers amuleapi already builds against.
add_executable (RangeFileBodyTest
	RangeFileBodyTest.cpp
)

add_test (NAME RangeFileBodyTest
	COMMAND RangeFileBodyTest
)

target_include_directories (RangeFileBodyTest
	PRIVATE ${CMAKE_SOURCE_DIR}/src/webapi
	PRIVATE ${Boost_INCLUDE_DIR}
)

target_link_libraries (RangeFileBodyTest
	muleunit
	mulecommon
	${Boost_LIBRARIES}
)

# The two part-index predicates behind `next_requested_part_index` /
# `downloading_part_index` on the per-file client rows. Every branch that
# matters turns on a value a live peer will not produce on request -- the
# 0xffff sentinel, the boundary at part_count, a file with more than 65535
# parts, a source whose state flips to "downloading" -- so PartIndex.h is
# header-only and free of wx, Beast and EC, and the test needs nothing but
# the harness.
# The chunk-bar palette and the two legends the "?" beside the bar column
# headers opens. Header-only and wx-free on purpose (see PartBarLegend.h): the
# colours, the rows of each legend, which legend a column gets and where the "?"
# is clickable are all decided without a toolkit, so a headless run can check
# the half of the feature that can drift silently. Needs no app and no daemon.
add_executable (PartBarLegendTest
	PartBarLegendTest.cpp
)

add_test (NAME PartBarLegendTest
	COMMAND PartBarLegendTest
)

target_include_directories (PartBarLegendTest
	PRIVATE ${CMAKE_SOURCE_DIR}/src
)

# mulecommon even though the header is inline-only: macOS links without it,
# GNU ld and mingw do not (see JsonDepthScanTest).
target_link_libraries (PartBarLegendTest
	muleunit
	mulecommon
)

add_executable (PartIndexTest
	PartIndexTest.cpp
)

add_test (NAME PartIndexTest
	COMMAND PartIndexTest
)

target_include_directories (PartIndexTest
	PRIVATE ${CMAKE_SOURCE_DIR}/src/webapi
)

# mulecommon even though the header is inline-only: macOS links without it,
# GNU ld and mingw do not (see JsonDepthScanTest).
target_link_libraries (PartIndexTest
	muleunit
	mulecommon
)
