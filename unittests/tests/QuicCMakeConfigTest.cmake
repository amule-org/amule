cmake_minimum_required(VERSION 3.10)

# Standalone option-logic test: exercises only cmake/options.cmake and
# cmake/ngtcp2.cmake without pulling in the full project dependency chain.
# No host toolchain or third-party libraries are required.
#
# Checks:
#   1. ENABLE_QUIC defaults to OFF.
#   2. Requesting ENABLE_QUIC=ON without a core executable forces it back OFF.
#   3. ENABLE_QUIC is listed in AMULE_EXPERIMENTAL_OPTIONS.

if (NOT DEFINED AMULE_SOURCE_DIR OR NOT DEFINED TEST_BINARY_DIR)
	message(FATAL_ERROR "AMULE_SOURCE_DIR and TEST_BINARY_DIR are required")
endif()

# ------------------------------------------------------------------
# Write a minimal CMakeLists.txt that includes only the option files.
# ------------------------------------------------------------------
set (_stub_dir "${TEST_BINARY_DIR}/quic-option-stub")
file(MAKE_DIRECTORY "${_stub_dir}")
file(WRITE "${_stub_dir}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.10)
project(QuicOptionStub LANGUAGES NONE)
# Stub out find_package so ngtcp2.cmake's find_package(Ngtcp2) is harmless.
macro(find_package name)
endmacro()
include("${AMULE_SOURCE_DIR}/cmake/options.cmake")
]=])

set (_work_dir "${TEST_BINARY_DIR}/quic-option-work")

# ------------------------------------------------------------------
# Case 1: default (no explicit ENABLE_QUIC) → must be OFF
# ------------------------------------------------------------------
file(REMOVE_RECURSE "${_work_dir}")
execute_process(
	COMMAND "${CMAKE_COMMAND}"
		"-DAMULE_SOURCE_DIR=${AMULE_SOURCE_DIR}"
		-DBUILD_MONOLITHIC=ON -DBUILD_DAEMON=OFF
		-S "${_stub_dir}" -B "${_work_dir}"
	RESULT_VARIABLE _r
	OUTPUT_VARIABLE _o
	ERROR_VARIABLE  _e
)
if (NOT _r EQUAL 0)
	message(FATAL_ERROR "Stub configure (default) failed:\n${_o}\n${_e}")
endif()
file(READ "${_work_dir}/CMakeCache.txt" _cache)
if (NOT _cache MATCHES "ENABLE_QUIC:BOOL=OFF")
	message(FATAL_ERROR "Default configuration did not set ENABLE_QUIC=OFF")
endif()
if (NOT _cache MATCHES "ENABLE_ALL_EXPERIMENTAL:BOOL=OFF")
	message(FATAL_ERROR "Default configuration did not set ENABLE_ALL_EXPERIMENTAL=OFF")
endif()

# ------------------------------------------------------------------
# Case 2: ENABLE_QUIC=ON without core (no BUILD_MONOLITHIC, no BUILD_DAEMON)
#         → options.cmake must force it back to OFF
# ------------------------------------------------------------------
file(REMOVE_RECURSE "${_work_dir}")
execute_process(
	COMMAND "${CMAKE_COMMAND}"
		"-DAMULE_SOURCE_DIR=${AMULE_SOURCE_DIR}"
		-DBUILD_MONOLITHIC=OFF -DBUILD_DAEMON=OFF -DENABLE_QUIC=ON
		-S "${_stub_dir}" -B "${_work_dir}"
	RESULT_VARIABLE _r
	OUTPUT_VARIABLE _o
	ERROR_VARIABLE  _e
)
if (NOT _r EQUAL 0)
	message(FATAL_ERROR "Stub configure (no-core) failed:\n${_o}\n${_e}")
endif()
file(READ "${_work_dir}/CMakeCache.txt" _cache)
if (NOT _cache MATCHES "ENABLE_QUIC:BOOL=OFF")
	message(FATAL_ERROR "No-core configuration did not force ENABLE_QUIC back to OFF")
endif()

# ------------------------------------------------------------------
# Case 3: ENABLE_QUIC is in AMULE_EXPERIMENTAL_OPTIONS
# ------------------------------------------------------------------
file(REMOVE_RECURSE "${_work_dir}")
file(WRITE "${_stub_dir}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.10)
project(QuicOptionStub LANGUAGES NONE)
macro(find_package name)
endmacro()
include("${AMULE_SOURCE_DIR}/cmake/options.cmake")
if (NOT "ENABLE_QUIC" IN_LIST AMULE_EXPERIMENTAL_OPTIONS)
	message(FATAL_ERROR "ENABLE_QUIC is not in AMULE_EXPERIMENTAL_OPTIONS")
endif()
]=])
execute_process(
	COMMAND "${CMAKE_COMMAND}"
		"-DAMULE_SOURCE_DIR=${AMULE_SOURCE_DIR}"
		-DBUILD_MONOLITHIC=ON -DBUILD_DAEMON=OFF
		-S "${_stub_dir}" -B "${_work_dir}"
	RESULT_VARIABLE _r
	OUTPUT_VARIABLE _o
	ERROR_VARIABLE  _e
)
if (NOT _r EQUAL 0)
	message(FATAL_ERROR "AMULE_EXPERIMENTAL_OPTIONS check failed:\n${_o}\n${_e}")
endif()

# ------------------------------------------------------------------
# Clean up scratch directories
# ------------------------------------------------------------------
file(REMOVE_RECURSE "${_stub_dir}" "${_work_dir}")
