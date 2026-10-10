cmake_minimum_required(VERSION 3.10)

# Standalone option-logic test: configures a stub project that includes only
# cmake/options.cmake. No host toolchain or third-party libraries are required.
#
# Checks:
#   1. ENABLE_QUIC defaults to OFF, and is listed in AMULE_EXPERIMENTAL_OPTIONS.
#   2. Requesting ENABLE_QUIC=ON without a core executable fails configure.
#   3. ENABLE_ALL_EXPERIMENTAL without a core executable configures, with QUIC
#      off and every other experimental switch on.
#   4. ENABLE_QUIC=ON is accepted when BUILD_EVERYTHING supplies amuled.

if (NOT DEFINED AMULE_SOURCE_DIR OR NOT DEFINED TEST_BINARY_DIR)
	message(FATAL_ERROR "AMULE_SOURCE_DIR and TEST_BINARY_DIR are required")
endif()

# ------------------------------------------------------------------
# Write a minimal CMakeLists.txt that includes only the option file and
# records the state the rest of the project would see.
# ------------------------------------------------------------------
set (_stub_dir "${TEST_BINARY_DIR}/quic-option-stub")
file(MAKE_DIRECTORY "${_stub_dir}")
file(WRITE "${_stub_dir}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.10)
project(QuicOptionStub LANGUAGES NONE)
include("${AMULE_SOURCE_DIR}/cmake/options.cmake")
get_directory_property(_defs COMPILE_DEFINITIONS)
file(WRITE "${CMAKE_BINARY_DIR}/probe.cmake"
	"set(probe_ENABLE_QUIC [==[${ENABLE_QUIC}]==])\n"
	"set(probe_defs [==[${_defs}]==])\n"
	"set(probe_experimental [==[${AMULE_EXPERIMENTAL_OPTIONS}]==])\n")
]=])

set (_work_dir "${TEST_BINARY_DIR}/quic-option-work")

# Configures the stub in a fresh build directory with the given -D arguments.
# Sets _r, _o and _e, and on success the probe_* variables.
macro(configure_stub)
	file(REMOVE_RECURSE "${_work_dir}")
	execute_process(
		COMMAND "${CMAKE_COMMAND}"
			"-DAMULE_SOURCE_DIR=${AMULE_SOURCE_DIR}"
			${ARGN}
			-S "${_stub_dir}" -B "${_work_dir}"
		RESULT_VARIABLE _r
		OUTPUT_VARIABLE _o
		ERROR_VARIABLE  _e
	)
	unset(probe_ENABLE_QUIC)
	unset(probe_defs)
	unset(probe_experimental)
	if (_r EQUAL 0)
		include("${_work_dir}/probe.cmake")
	endif()
endmacro()

# ------------------------------------------------------------------
# Case 1: default (no explicit ENABLE_QUIC) → must be OFF
# ------------------------------------------------------------------
configure_stub(-DBUILD_MONOLITHIC=ON -DBUILD_DAEMON=OFF)
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
if ("ENABLE_QUIC" IN_LIST probe_defs)
	message(FATAL_ERROR "Default configuration defined ENABLE_QUIC")
endif()
if (NOT "ENABLE_QUIC" IN_LIST probe_experimental)
	message(FATAL_ERROR "ENABLE_QUIC is not in AMULE_EXPERIMENTAL_OPTIONS")
endif()

# ------------------------------------------------------------------
# Case 2: ENABLE_QUIC=ON without core (no BUILD_MONOLITHIC, no BUILD_DAEMON)
#         → configure must fail, not turn QUIC off
# ------------------------------------------------------------------
configure_stub(-DBUILD_MONOLITHIC=OFF -DBUILD_DAEMON=OFF -DENABLE_QUIC=ON)
if (_r EQUAL 0)
	message(FATAL_ERROR "No-core configuration with ENABLE_QUIC=ON did not fail:\n${_o}\n${_e}")
endif()
if (NOT _e MATCHES "ENABLE_QUIC=YES needs BUILD_MONOLITHIC=YES or BUILD_DAEMON=YES")
	message(FATAL_ERROR "No-core configuration with ENABLE_QUIC=ON failed for another reason:\n${_o}\n${_e}")
endif()

# ------------------------------------------------------------------
# Case 3: ENABLE_ALL_EXPERIMENTAL without core
#         → configures, QUIC stays off with a notice, the rest are on
# ------------------------------------------------------------------
configure_stub(-DBUILD_MONOLITHIC=OFF -DBUILD_DAEMON=OFF -DENABLE_ALL_EXPERIMENTAL=ON)
if (NOT _r EQUAL 0)
	message(FATAL_ERROR "No-core configuration with ENABLE_ALL_EXPERIMENTAL=ON failed:\n${_o}\n${_e}")
endif()
if (probe_ENABLE_QUIC OR "ENABLE_QUIC" IN_LIST probe_defs)
	message(FATAL_ERROR "ENABLE_ALL_EXPERIMENTAL turned QUIC on in a no-core configuration")
endif()
if (NOT _o MATCHES "ENABLE_QUIC stays OFF")
	message(FATAL_ERROR "ENABLE_ALL_EXPERIMENTAL left QUIC off without a notice:\n${_o}")
endif()
foreach (_option IN LISTS probe_experimental)
	if (NOT _option STREQUAL "ENABLE_QUIC" AND NOT _option IN_LIST probe_defs)
		message(FATAL_ERROR "ENABLE_ALL_EXPERIMENTAL did not turn ${_option} on in a no-core configuration")
	endif()
endforeach()

# ------------------------------------------------------------------
# Case 4: ENABLE_QUIC=ON with BUILD_EVERYTHING and BUILD_MONOLITHIC=OFF
#         → BUILD_EVERYTHING turns amuled on, so QUIC must stay on
# ------------------------------------------------------------------
configure_stub(-DBUILD_EVERYTHING=ON -DBUILD_MONOLITHIC=OFF -DENABLE_QUIC=ON)
if (NOT _r EQUAL 0)
	message(FATAL_ERROR "BUILD_EVERYTHING configuration with ENABLE_QUIC=ON failed:\n${_o}\n${_e}")
endif()
if (NOT probe_ENABLE_QUIC OR NOT "ENABLE_QUIC" IN_LIST probe_defs)
	message(FATAL_ERROR "BUILD_EVERYTHING configuration did not keep ENABLE_QUIC on")
endif()

# ------------------------------------------------------------------
# Clean up scratch directories
# ------------------------------------------------------------------
file(REMOVE_RECURSE "${_stub_dir}" "${_work_dir}")
