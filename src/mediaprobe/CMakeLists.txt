# Also usable standalone for helper tests.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.12)
  project(AMuleMediaProbe LANGUAGES CXX)
  option(BUILD_TESTING "Build tests" ON)
  enable_testing()
endif()
# The helper does not link wxWidgets, so it must request large-file stdio itself.
# Apply this to both executables, including standalone helper builds.
if(NOT WIN32)
  add_compile_definitions(_FILE_OFFSET_BITS=64)
endif()
option(MEDIA_PROBE_REQUIRE_FIXTURES "Require ffmpeg/LAME/x264 for duration regressions" OFF)
find_package(PkgConfig REQUIRED)
# FFmpeg 4.4 (Ubuntu 22.04); Debian 12 provides 5.1. No FFmpeg executable is needed.
if(MEDIA_PROBE_STATIC)
  set(PKG_CONFIG_ARGN --static)
  if(MINGW)
    set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
  endif()
endif()
pkg_check_modules(MEDIA_AV REQUIRED IMPORTED_TARGET
  libavformat>=58.76.100 libavcodec>=58.134.100 libavutil>=56.70.100)
add_executable(amule-media-probe main.cpp)
target_compile_features(amule-media-probe PRIVATE cxx_std_17)
target_link_libraries(amule-media-probe PRIVATE PkgConfig::MEDIA_AV)
# Same directory as the core, including relocated bundles and portable installs.
set_target_properties(amule-media-probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src")
if(NOT AMULE_MEDIA_PROBE_INSTALL_DIR)
  set(AMULE_MEDIA_PROBE_INSTALL_DIR bin)
endif()
install(TARGETS amule-media-probe RUNTIME DESTINATION "${AMULE_MEDIA_PROBE_INSTALL_DIR}")
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME MediaProbeBackends COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_backends.py $<TARGET_FILE:amule-media-probe>)
  # Each case starts the helper; on Windows runners that can exceed the 10 s fast lane.
  set_tests_properties(MediaProbeBackends PROPERTIES LABELS "slow")
  if(BUILD_TESTING)
    add_executable(media-probe-io-test test_io.cpp)
    target_compile_features(media-probe-io-test PRIVATE cxx_std_17)
    target_link_libraries(media-probe-io-test PRIVATE PkgConfig::MEDIA_AV)
    add_test(NAME MediaProbeIOLimits COMMAND media-probe-io-test)
    add_test(NAME MediaProbeHostileFiles COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/test_hostile.py $<TARGET_FILE:amule-media-probe>)
    set(DURATION_ARGS)
    if(MEDIA_PROBE_REQUIRE_FIXTURES)
      list(APPEND DURATION_ARGS --require-fixtures)
    endif()
    add_test(NAME MediaProbeDuration COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/test_duration.py $<TARGET_FILE:amule-media-probe>
      ${DURATION_ARGS})
    set_tests_properties(MediaProbeHostileFiles MediaProbeDuration PROPERTIES LABELS "slow" TIMEOUT 180)
  endif()
endif()
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../../packaging/media/licenses/"
  DESTINATION share/licenses/amule-media-probe)
