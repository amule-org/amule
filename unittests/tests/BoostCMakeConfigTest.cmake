cmake_minimum_required(VERSION 3.12)

# Exercise the real Boost setup without depending on a host Boost installation.
# The fixtures expose only Boost_INCLUDE_DIRS, as Boost's config package does.
if (NOT DEFINED AMULE_SOURCE_DIR OR NOT DEFINED TEST_BINARY_DIR)
	message(FATAL_ERROR "AMULE_SOURCE_DIR and TEST_BINARY_DIR are required")
endif()

include(CMakePackageConfigHelpers)
set (_source "${TEST_BINARY_DIR}/source")
file(MAKE_DIRECTORY "${_source}/consumer")
foreach (_version 1.71.0 1.74.0)
	set (_prefix "${TEST_BINARY_DIR}/boost-${_version}")
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
set(CMAKE_FIND_ROOT_PATH "${Boost_DIR}")
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

function(check_configure version api succeeds)
	# Reuse the build directory to exercise changes to Boost_DIR and API options.
	execute_process(COMMAND "${CMAKE_COMMAND}"
		-S "${_source}" -B "${TEST_BINARY_DIR}/build"
		"-DAMULE_SOURCE_DIR=${AMULE_SOURCE_DIR}"
		"-DBoost_DIR=${TEST_BINARY_DIR}/boost-${version}"
		"-DEXPECTED_VERSION=${version}" "-DBUILD_AMULEAPI=${api}"
		-DCMAKE_CXX_STANDARD=17
		RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
	if (succeeds)
		if (NOT _result EQUAL 0)
			message(FATAL_ERROR "Boost ${version}, API ${api}:\n${_out}\n${_err}")
		endif()
		execute_process(COMMAND "${CMAKE_COMMAND}" --build "${TEST_BINARY_DIR}/build"
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
file(REMOVE_RECURSE "${TEST_BINARY_DIR}")
