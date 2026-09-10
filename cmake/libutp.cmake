# Makes the vendored libutp available as Utp::Utp.
#
# Included only when ENABLE_UTP is on. Nothing in aMule links the target yet:
# this exists so the uTP transport work can be reviewed against a dependency
# that is already in the tree and already builds.
#
# ENABLE_UTP gates whether the target exists at all, so a default build has no
# libutp in its graph and nothing to skip. The library is therefore built as
# part of `all` whenever the switch is on -- deliberately, since ENABLE_UTP is
# in AMULE_EXPERIMENTAL_OPTIONS and that is what gives the vendored snapshot CI
# coverage. Adding EXCLUDE_FROM_ALL here would be a second lock on a door the
# option has already locked, and its only effect would be to leave the snapshot
# compiled by no job.
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

add_subdirectory ("${AMULE_LIBUTP_DIR}")

if (NOT TARGET libutp)
	message (FATAL_ERROR
		"The vendored libutp did not define the target 'libutp'. The snapshot "
		"in ${AMULE_LIBUTP_DIR} is not the pinned upstream revision.")
endif()

# Upstream sets the language standard only for a standalone build, deferring to
# the parent otherwise. aMule now pins C++17 tree-wide, so this would inherit
# the right standard anyway; it stays because upstream marks the standard
# REQUIRED when standalone, making C++17 its requirement rather than ours to
# lend. If the tree-wide pin ever moves, this target must not move with it.
set_target_properties (libutp PROPERTIES
	CXX_STANDARD 17
	CXX_STANDARD_REQUIRED ON)

add_library (Utp::Utp ALIAS libutp)
