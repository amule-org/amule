#! /bin/bash

die () {
	local ERR_CODE=$?
	[[ $ERR_CODE == 0 ]] && return

	local EXIT_CODE=$1
	shift
	local MESSAGE=( "$@" )

	[[ ${MESSAGE[*]} != "" ]] && echo "${MESSAGE[@]}" >&2
	exit "${EXIT_CODE}"
}

usage() {
	echo "Compiles the program"
	echo
	echo "Usage: $0 [-d] [-e] [-s] [--clean] [-h | -?]"
	echo "  --clean      Remove the build folder first, for a full rebuild"
	echo "               (default is an incremental build)"
	echo "  -d           Enable debug compilation (default is release)"
	echo "  -e"
	echo "  --experimental"
	echo "               Also compile every experimental feature"
	echo "               (ENABLE_ALL_EXPERIMENTAL)"
	echo "  -h"
	echo "  --help       Display this help message"
	echo "  -j<n>"
	echo "  --jobs<n>    Run <n> jobs in parallel"
	echo "  -c"
	echo "  --measure-cache"
	echo "               Build against an empty, isolated ccache directory"
	echo "               and print its size at the end, to measure this"
	echo "               project's own ccache footprint. Skips running tests."
	echo "               Implies --clean."
	echo "  -s"
	echo "  --strip      Show the sizes of the stripped executables, as a"
	echo "               stripped install would produce them. The files in"
	echo "               the build folder are left untouched."
}

OPT_CLEAN=0
OPT_DEBUG=Release
OPT_EXPERIMENTAL=NO
OPT_J=1
OPT_MEASURE_CACHE=0
OPT_STRIP=0

# Setup parse options
# -o "j:" means short flag 'j' requires an argument
# --long "jobs:" means long flag 'jobs' requires an argument
if ! PARAMS=$(getopt -o "dehj:cs" -l "clean,debug,experimental,jobs:,help,measure-cache,strip" -n "$0" -- "$@"); then
	# If getopt fails (invalid flag), exit
	usage
	false; die 10
fi

# Re-set the positional parameters to the cleaned version from getopt
eval set -- "$PARAMS"

while true; do
	case "$1" in
	--clean )
		OPT_CLEAN=1
		shift
		;;
	-d | --debug )
		OPT_DEBUG=Debug
		echo "[DEBUG compilation ENABLED]"
		shift
		;;
	-e | --experimental )
		OPT_EXPERIMENTAL=YES
		echo "[EXPERIMENTAL features ENABLED]"
		shift
		;;
	-h | --help )
		usage
		false; die 0
		;;
	-j | --jobs )
		if [[ -n "$2" && "$2" != -* ]]; then
			OPT_J="$2"
			shift 2 # Move past the flag AND the value
		else
			echo "Error: -j requires a non-empty argument." >&2
			usage
			false; die 12
		fi
		;;
	-c | --measure-cache )
		OPT_MEASURE_CACHE=1
		OPT_CLEAN=1
		shift
		;;
	-s | --strip )
		OPT_STRIP=1
		shift
		;;
	-- )
		shift
		break;
		;;
	* )
		usage
		false; die 12 "Error processing command line parameters"
		;;
	esac
done

cmake_configure () {
	if [[ ${OPT_CLEAN} == 1 ]]; then
		rm -rf build
		die 21 "Error trying to remove the build folder"
	fi

	cmake \
		-B build \
		-DCMAKE_BUILD_TYPE=${OPT_DEBUG} \
		-DBUILD_ALC=YES \
		-DBUILD_ALCC=YES \
		-DBUILD_AMULECMD=YES \
		-DBUILD_AMULEAPI=YES \
		-DBUILD_CAS=YES \
		-DBUILD_DAEMON=YES \
		-DBUILD_WXCAS=YES \
		-DBUILD_ED2K=YES \
		-DBUILD_FILEVIEW=YES \
		-DBUILD_MONOLITHIC=YES \
		-DBUILD_REMOTEGUI=YES \
		-DBUILD_TESTING=YES \
		-DBUILD_WEBSERVER=YES \
		-DENABLE_NLS=YES \
		-DENABLE_UPNP=YES \
		-DENABLE_IP2COUNTRY=YES \
		-DENABLE_ALL_EXPERIMENTAL=${OPT_EXPERIMENTAL}

	die 22 "CMake configuration failed"
}

cmake_build() {
	echo Running the CMake build with "${OPT_J} parallel jobs"

	local HAVE_CCACHE=0
	if command -v ccache > /dev/null 2>&1; then
		HAVE_CCACHE=1
		ccache -z > /dev/null
	fi

	cmake --build build -j"${OPT_J}" "$@"
	local BUILD_STATUS=$?

	if [[ ${HAVE_CCACHE} == 1 ]]; then
		echo
		echo "ccache statistics for this build:"
		ccache -s
	fi

	(exit "${BUILD_STATUS}")
	die 3 "CMake build failed"
}

cmake_test() {
	ctest \
		--test-dir build \
		--output-on-failure \
		--timeout 10

	die 4 "CMake test failed"
}

cmake_strip_sizes() {
	local STRIP_DIR
	STRIP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/amule-strip.XXXXXX")
	die 24 "Error creating scratch install directory"

	cmake --install build --strip --prefix "${STRIP_DIR}" > /dev/null
	local INSTALL_STATUS=$?

	if [[ ${INSTALL_STATUS} == 0 ]]; then
		echo
		echo "Sizes of the stripped executables:"
		ls -lhS "${STRIP_DIR}/bin"
	fi

	rm -rf "${STRIP_DIR}"

	(exit "${INSTALL_STATUS}")
	die 5 "CMake stripped install failed"
}

GIT_ROOT=$(git rev-parse --show-toplevel)
[[ ${PWD} == "${GIT_ROOT}" ]]
die 12 \
	" The current path is: '${PWD}'" \
	$'\n' \
	"This script must be run in '${GIT_ROOT}'"

if [[ ${OPT_MEASURE_CACHE} == 1 ]]; then
	CCACHE_DIR=$(mktemp -d "${TMPDIR:-/tmp}/amule-ccache-measure.XXXXXX")
	die 23 "Error creating scratch ccache directory"
	export CCACHE_DIR
	trap 'rm -rf "${CCACHE_DIR}"' EXIT
fi

cmake_configure
cmake_build "$@"

if [[ ${OPT_MEASURE_CACHE} == 1 ]]; then
	echo
	echo "This project's ccache footprint (empty cache, full build):"
	du -sh "${CCACHE_DIR}"
else
	cmake_test
fi

if [[ ${OPT_STRIP} == 1 ]]; then
	cmake_strip_sizes
fi

exit 0
