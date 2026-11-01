cmake_minimum_required(VERSION 3.12)

# Exercise the real Boost setup without depending on a host Boost installation.
# The fixtures expose only Boost_INCLUDE_DIRS, as Boost's config package does.
if (NOT DEFINED AMULE_SOURCE_DIR OR NOT DEFINED TEST_BINARY_DIR)
	message(FATAL_ERROR "AMULE_SOURCE_DIR and TEST_BINARY_DIR are required")
endif()

include(CMakePackageConfigHelpers)
set (_work "${TEST_BINARY_DIR}/boost-config-work")
set (_source "${_work}/source")
# Start fresh, and remove only this test's own workspace on completion.
file(REMOVE_RECURSE "${_work}")
file(MAKE_DIRECTORY "${_source}/consumer")
foreach (_version 1.71.0 1.74.0)
	set (_prefix "${_work}/boost-${_version}")
	file(MAKE_DIRECTORY "${_prefix}/include/boost/system")
	file(WRITE "${_prefix}/include/boost/system/error_code.hpp" "")
	file(WRITE "${_prefix}/include/boost/asio.hpp" "")
	file(WRITE "${_prefix}/include/boost/selected.hpp"
		"#define SELECTED_BOOST_VERSION \"${_version}\"\nstatic int unused_boost_fixture;\n")
	file(WRITE "${_prefix}/BoostConfig.cmake"
		"set(Boost_INCLUDE_DIRS \"${_prefix}/include\")\n")
	write_basic_package_version_file("${_prefix}/BoostConfigVersion.cmake"
		VERSION ${_version} COMPATIBILITY AnyNewerVersion)
endforeach()

file(WRITE "${_source}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.12)
project(BoostConfigTest LANGUAGES CXX)
set(CMAKE_FIND_ROOT_PATH "${TEST_SEARCH_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(MIN_BOOST_VERSION 1.70)
include("${AMULE_SOURCE_DIR}/cmake/boost.cmake")
add_subdirectory(consumer)
]=])
file(WRITE "${_source}/consumer/CMakeLists.txt" [=[
add_executable(consumer main.cpp)
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # This external header must not trip the project's warning-as-error policy.
    target_compile_options(consumer PRIVATE -Wunused-variable -Werror)
endif()
target_compile_definitions(consumer PRIVATE EXPECTED_VERSION="${EXPECTED_VERSION}")
]=])
file(WRITE "${_source}/consumer/main.cpp" [=[
#include <boost/selected.hpp>
#include <string_view>
static_assert(std::string_view(SELECTED_BOOST_VERSION) == EXPECTED_VERSION);
int main() { return 0; }
]=])

set (_configure_args)
if (DEFINED TEST_CMAKE_GENERATOR)
	list(APPEND _configure_args -G "${TEST_CMAKE_GENERATOR}")
endif()
foreach (_variable CMAKE_CXX_COMPILER CMAKE_CXX_COMPILER_ARG1 CMAKE_TOOLCHAIN_FILE
	CMAKE_GENERATOR_PLATFORM CMAKE_GENERATOR_TOOLSET CMAKE_MAKE_PROGRAM)
	if (DEFINED TEST_${_variable})
		list(APPEND _configure_args "-D${_variable}=${TEST_${_variable}}")
	endif()
endforeach()

function(check_configure version api succeeds)
	# Reuse the build directory to exercise changes to Boost_DIR and API options.
	set (_prefix "${_work}/boost-${version}")
	set (_selection "-DBoost_DIR=${_prefix}")
	set (_environment "Boost_DIR=" "BOOST_DIR=")
	if ("${ARGV3}" STREQUAL "environment")
		set (_selection -UBoost_DIR)
		set (_environment "Boost_DIR=${_prefix}" "BOOST_DIR=")
	elseif ("${ARGV3}" STREQUAL "cache-over-environment")
		set (_environment "Boost_DIR=${_work}/boost-1.74.0" "BOOST_DIR=")
	elseif ("${ARGV3}" STREQUAL "cached")
		set (_selection)
		set (_environment "Boost_DIR=${_work}/boost-1.74.0" "BOOST_DIR=")
	elseif ("${ARGV3}" STREQUAL "default")
		set (_selection -UBoost_DIR "-DCMAKE_PREFIX_PATH=${_prefix}")
		# An uppercase BOOST_DIR must not override CMake's normal search.
		set (_environment "Boost_DIR=" "BOOST_DIR=${_work}/boost-1.71.0")
	endif()
	file(MAKE_DIRECTORY "${_work}/build")
	# A positional source path works with the project's CMake 3.12 minimum.
	execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${_environment}
		"${CMAKE_COMMAND}" "${_source}" ${_configure_args} ${_selection}
		"-DAMULE_SOURCE_DIR=${AMULE_SOURCE_DIR}"
		"-DTEST_SEARCH_ROOT=${_prefix}"
		"-DEXPECTED_VERSION=${version}" "-DBUILD_AMULEAPI=${api}"
		-DCMAKE_CXX_STANDARD=17
		WORKING_DIRECTORY "${_work}/build"
		RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
	if (succeeds)
		if (NOT _result EQUAL 0)
			message(FATAL_ERROR "Boost ${version}, API ${api}:\n${_out}\n${_err}")
		endif()
		execute_process(COMMAND "${CMAKE_COMMAND}" --build "${_work}/build" --config Release
			RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
		if (NOT _result EQUAL 0)
			message(FATAL_ERROR "Selected Boost headers did not reach consumer:\n${_out}\n${_err}")
		endif()
	elseif (_result EQUAL 0 OR NOT "${_out}${_err}" MATCHES "1.74")
		message(FATAL_ERROR "Expected rejection requiring Boost 1.74:\n${_out}\n${_err}")
	endif()
endfunction()

check_configure(1.71.0 OFF TRUE)
check_configure(1.74.0 ON TRUE)
check_configure(1.71.0 ON FALSE)
check_configure(1.71.0 OFF TRUE)
check_configure(1.74.0 ON TRUE environment)
check_configure(1.71.0 OFF TRUE cache-over-environment)
check_configure(1.71.0 OFF TRUE cached)
check_configure(1.74.0 ON TRUE default)
file(REMOVE_RECURSE "${_work}")
