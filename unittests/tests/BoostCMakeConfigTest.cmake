cmake_minimum_required(VERSION 3.12)

# A Boost picked with -DBoost_DIR must reach every target's compile. The fixture
# exposes only Boost_INCLUDE_DIRS, as Boost's config package does.
if (NOT DEFINED AMULE_SOURCE_DIR OR NOT DEFINED TEST_BINARY_DIR)
	message(FATAL_ERROR "AMULE_SOURCE_DIR and TEST_BINARY_DIR are required")
endif()

include(CMakePackageConfigHelpers)
set (_work "${TEST_BINARY_DIR}/boost-config-work")
set (_source "${_work}/source")
set (_prefix "${_work}/boost")
file(REMOVE_RECURSE "${_work}")
file(MAKE_DIRECTORY "${_source}/consumer" "${_prefix}/include/boost/system")
file(WRITE "${_prefix}/include/boost/system/error_code.hpp" "")
file(WRITE "${_prefix}/include/boost/asio.hpp" "")
# Only this fixture has the header, and its unused parameter fails -Werror unless
# the include is a system one.
file(WRITE "${_prefix}/include/boost/selected.hpp" "inline int fixture(int unused) { return 0; }\n")
file(WRITE "${_prefix}/BoostConfig.cmake" "set(Boost_INCLUDE_DIRS \"${_prefix}/include\")\n")
write_basic_package_version_file("${_prefix}/BoostConfigVersion.cmake"
	VERSION 1.74.0 COMPATIBILITY AnyNewerVersion)

file(WRITE "${_source}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.12)
project(BoostConfigTest LANGUAGES CXX)
set(MIN_BOOST_VERSION 1.74)
include("${AMULE_SOURCE_DIR}/cmake/boost.cmake")
add_subdirectory(consumer)
]=])
file(WRITE "${_source}/consumer/CMakeLists.txt" [=[
add_executable(consumer main.cpp)
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	target_compile_options(consumer PRIVATE -Wunused-parameter -Werror)
endif()
]=])
file(WRITE "${_source}/consumer/main.cpp" "#include <boost/selected.hpp>\nint main() { return 0; }\n")

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

file(MAKE_DIRECTORY "${_work}/build")
execute_process(COMMAND "${CMAKE_COMMAND}" "${_source}" ${_configure_args}
	"-DBoost_DIR=${_prefix}" "-DAMULE_SOURCE_DIR=${AMULE_SOURCE_DIR}"
	WORKING_DIRECTORY "${_work}/build"
	RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if (NOT _result EQUAL 0)
	message(FATAL_ERROR "Configure failed:\n${_out}\n${_err}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${_work}/build" --config Release
	RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if (NOT _result EQUAL 0)
	message(FATAL_ERROR "Selected Boost headers did not reach the target:\n${_out}\n${_err}")
endif()
file(REMOVE_RECURSE "${_work}")
