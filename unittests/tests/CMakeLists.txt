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

add_executable (JwtTest
	JwtTest.cpp
)
add_test (NAME JwtTest COMMAND JwtTest)
target_link_libraries (JwtTest
	muleunit
	webcommon
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
# gating. Pulls EventBus + State along with EventDiff itself — none
# touch wxWidgets or EC, so the exe is pure stdlib + muleunit.
add_executable (EventDiffTest
	EventDiffTest.cpp
	${CMAKE_SOURCE_DIR}/src/webapi/EventBus.cpp
	${CMAKE_SOURCE_DIR}/src/webapi/EventDiff.cpp
	${CMAKE_SOURCE_DIR}/src/webapi/State.cpp
)
target_include_directories (EventDiffTest
	PRIVATE ${CMAKE_SOURCE_DIR}/src/webapi
)
add_test (NAME EventDiffTest COMMAND EventDiffTest)
target_link_libraries (EventDiffTest
	muleunit
	mulecommon
)

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
