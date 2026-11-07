# Also usable standalone for helper tests.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.12)
  project(AMuleMediaProbe LANGUAGES CXX)
  option(BUILD_TESTING "Build tests" ON)
  enable_testing()
endif()
# The helper does not link wxWidgets, so it must request large-file stdio itself.
if(NOT WIN32)
  add_compile_definitions(_FILE_OFFSET_BITS=64)
endif()
# Native container parsers: no external libraries.
set(MEDIA_PROBE_PARSERS probe.cpp tags.cpp codecs.cpp riff.cpp isobmff.cpp matroska.cpp audio.cpp ogg.cpp
  legacy.cpp)
add_executable(amule-media-probe main.cpp ${MEDIA_PROBE_PARSERS})
target_compile_features(amule-media-probe PRIVATE cxx_std_17)
# Same directory as the core, including relocated bundles and portable installs.
set_target_properties(amule-media-probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src")
if(NOT AMULE_MEDIA_PROBE_INSTALL_DIR)
  set(AMULE_MEDIA_PROBE_INSTALL_DIR bin)
endif()
install(TARGETS amule-media-probe RUNTIME DESTINATION "${AMULE_MEDIA_PROBE_INSTALL_DIR}")
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME MediaProbeHelper COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_helper.py $<TARGET_FILE:amule-media-probe>)
  add_test(NAME MediaProbeHostile COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_hostile.py $<TARGET_FILE:amule-media-probe>)
  # Each case starts the helper; on Windows runners that can exceed the 10 s fast lane.
  set_tests_properties(MediaProbeHelper MediaProbeHostile PROPERTIES LABELS "slow")
endif()
