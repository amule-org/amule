# Makes the vendored libutp available as Utp::Utp.
#
# Included only when ENABLE_UTP is on. amule and amuled link it for the
# experimental uTP transport.
#
# ENABLE_UTP gates whether the target exists at all, so a default build has no
# libutp in its graph and nothing to skip. The library is therefore built as
# part of `all` whenever the switch is on -- deliberately, since ENABLE_UTP is
# in AMULE_EXPERIMENTAL_OPTIONS and that is what gives the vendored snapshot CI
# coverage. Adding EXCLUDE_FROM_ALL here would be a second lock on a door the
# option has already locked, and its only effect would be to leave the snapshot
# compiled by no job.

set (AMULE_LIBUTP_DIR "${CMAKE_SOURCE_DIR}/src/extern/libutp")

if (NOT EXISTS "${AMULE_LIBUTP_DIR}/CMakeLists.txt")
	message (FATAL_ERROR
		"ENABLE_UTP is on but the vendored libutp is missing from "
		"${AMULE_LIBUTP_DIR}. See src/extern/libutp/AMULE_PROVENANCE.md.")
endif()

# Upstream's own switches, pinned before the subdirectory sees them. option()
# leaves an existing cache entry alone, and INTERNAL keeps these out of
# `cmake -LAH`, which docs/INSTALL.md tells users to run.
#
# Two of them are actively harmful here and none of them is ours to offer.
# LIBUTP_BUILD_PROGRAMS wants ucat.c, which AMULE_PROVENANCE.md records as
# deliberately not vendored, so turning it on fails configure with a missing
# source that reads like a broken checkout. LIBUTP_ENABLE_INSTALL would make
# `make install` write libutp.a, its headers and a cmake package into aMule's
# prefix: a library we vendor for our own use is not one we ship. LIBUTP_SHARED
# would build an uninstalled .so nothing links, and it defaults from
# BUILD_SHARED_LIBS, so it is reachable without naming libutp at all.
# FORCE, not just CACHE: a -D on the command line creates the entry before this
# file runs, and set(CACHE) without FORCE adopts the type while keeping the
# user's value. Without it the pin is decorative, which a probe run with
# -DLIBUTP_BUILD_PROGRAMS=ON showed by still failing on the missing ucat.c.
set (LIBUTP_SHARED OFF CACHE INTERNAL "aMule links the vendored libutp statically" FORCE)
set (LIBUTP_ENABLE_INSTALL OFF CACHE INTERNAL "aMule does not install the vendored libutp" FORCE)
set (LIBUTP_ENABLE_WERROR OFF CACHE INTERNAL "aMule does not gate on upstream's warnings" FORCE)
set (LIBUTP_BUILD_PROGRAMS OFF CACHE INTERNAL "ucat.c is not vendored" FORCE)

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
