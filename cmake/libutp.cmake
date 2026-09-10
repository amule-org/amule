# Makes the vendored libutp available as Utp::Utp.
#
# Included only when ENABLE_UTP is on. Nothing in aMule links the target yet:
# this exists so the uTP transport work can be reviewed against a dependency
# that is already in the tree and already builds.
#
# EXCLUDE_FROM_ALL, so an ENABLE_UTP=YES build with no consumer does not compile
# the library as a side effect of `cmake --build`. To build it on its own --
# which is how CI proves the snapshot compiles -- name the target:
#
#     cmake --build build --target libutp
#
# The vendored CMakeLists.txt is upstream's and requires CMake 3.12, above this
# project's 3.10 minimum. That is why the include is conditional rather than
# unconditional with an internal guard: a default build never enters it, so the
# floor only rises for builds that ask for uTP.

set (AMULE_LIBUTP_DIR "${CMAKE_SOURCE_DIR}/src/extern/libutp")

if (NOT EXISTS "${AMULE_LIBUTP_DIR}/CMakeLists.txt")
	message (FATAL_ERROR
		"ENABLE_UTP is on but the vendored libutp is missing from "
		"${AMULE_LIBUTP_DIR}. See src/extern/libutp/AMULE_PROVENANCE.md.")
endif()

if (CMAKE_VERSION VERSION_LESS 3.12)
	message (FATAL_ERROR
		"ENABLE_UTP requires CMake 3.12 or newer (the vendored libutp asks for "
		"it); this is CMake ${CMAKE_VERSION}. Build without ENABLE_UTP, or "
		"upgrade CMake.")
endif()

add_subdirectory ("${AMULE_LIBUTP_DIR}" EXCLUDE_FROM_ALL)

if (NOT TARGET libutp)
	message (FATAL_ERROR
		"The vendored libutp did not define the target 'libutp'. The snapshot "
		"in ${AMULE_LIBUTP_DIR} is not the pinned upstream revision.")
endif()

add_library (Utp::Utp ALIAS libutp)
