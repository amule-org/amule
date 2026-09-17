add_library (CRYPTOPP::CRYPTOPP
	UNKNOWN
	IMPORTED
)

set (CRYPTOPP_SEARCH_PREFIXES "cryptopp" "crypto++")

if (NOT CRYPTOPP_INCLUDE_PREFIX)
	unset (CRYPT_SEARCH CACHE)
	check_include_file_cxx (cryptlib.h CRYPT_SEARCH)

	if (CRYPT_SEARCH)
		set (CRYPTOPP_INCLUDE_PREFIX "" CACHE STRING "cryptopp include prefix" FORCE)
	else()
		foreach (PREFIX ${CRYPTOPP_SEARCH_PREFIXES})
			unset (CRYPT_SEARCH CACHE)
			check_include_file_cxx (${PREFIX}/cryptlib.h CRYPT_SEARCH)

			if (CRYPT_SEARCH)
				message (STATUS "cryptopp prefix found: ${PREFIX}")
				set (CRYPTOPP_INCLUDE_PREFIX ${PREFIX} CACHE STRING "cryptopp include prefix" FORCE)
				break()
			endif()
		endforeach()
	endif()
endif()

if (NOT CRYPTOPP_INCLUDE_PREFIX)
	MESSAGE (FATAL_ERROR "cryptlib.h not found")
endif()

if (WIN32)
	if (NOT CRYPTOPP_LIBRARY_DEBUG)
		unset (CRYPTOPP_LIBRARY_DEBUG CACHE)

		find_library (CRYPTOPP_LIBRARY_DEBUG
			NAMES crypto++d cryptlibd cryptoppd
			PATHS ${CRYPTOPP_LIB_SEARCH_PATH}
		)

		if (CRYPTOPP_LIBRARY_DEBUG)
			message (STATUS "Found debug-libcrypto++ in ${CRYPTOPP_LIBRARY_DEBUG}")
		endif()
	endif()

	if (CRYPTOPP_LIBRARY_DEBUG)
		set_property (TARGET CRYPTOPP::CRYPTOPP
			PROPERTY IMPORTED_LOCATION_DEBUG ${CRYPTOPP_LIBRARY_DEBUG}
		)
	else()
		set (CRYPTO_COMPLETE FALSE)
	endif()

	if (NOT CRYPTOPP_LIBRARY_RELEASE)
		unset (CRYPTOPP_LIBRARY_RELEASE CACHE)

		find_library (CRYPTOPP_LIBRARY_RELEASE
			NAMES crypto++ cryptlib cryptopp
			PATHS ${CRYPTOPP_LIB_SEARCH_PATH}
		)

		if (CRYPTOPP_LIBRARY_RELEASE)
			message (STATUS "Found release-libcrypto++ in ${CRYPTOPP_LIBRARY_RELEASE}")
		endif()
	endif (NOT CRYPTOPP_LIBRARY_RELEASE)

	if (CRYPTOPP_LIBRARY_RELEASE)
		set_property (TARGET CRYPTOPP::CRYPTOPP
			PROPERTY IMPORTED_LOCATION_RELEASE ${CRYPTOPP_LIBRARY_RELEASE}
		)
	else()
		set (CRYPTO_COMPLETE FALSE)
	endif()

	# Packages that ship only the release variant (e.g. MSYS2
	# mingw-w64-x86_64-crypto++ has libcryptopp.dll.a but no *d.dll.a) don't
	# populate IMPORTED_LOCATION_DEBUG. Set plain IMPORTED_LOCATION so CMake
	# falls back to the release library when building with CMAKE_BUILD_TYPE=Debug.
	if (CRYPTOPP_LIBRARY_RELEASE AND NOT CRYPTOPP_LIBRARY_DEBUG)
		set_property (TARGET CRYPTOPP::CRYPTOPP
			PROPERTY IMPORTED_LOCATION ${CRYPTOPP_LIBRARY_RELEASE}
		)
	endif()
else()
	if (NOT CRYPTOPP_LIBRARY)
		unset (CRYPTOPP_LIBRARY CACHE)

		find_library (CRYPTOPP_LIBRARY
			NAMES crypto++ cryptlib cryptopp
			PATHS ${CRYPTOPP_LIB_SEARCH_PATH}
		)

		if (CRYPTOPP_LIBRARY)
			message (STATUS "Found libcrypto++ in ${CRYPTOPP_LIBRARY}")
		endif()
	endif()

	if (CRYPTOPP_LIBRARY)
		set_property (TARGET CRYPTOPP::CRYPTOPP
			PROPERTY IMPORTED_LOCATION ${CRYPTOPP_LIBRARY}
		)
	else()
		set (CRYPTO_COMPLETE FALSE)
	endif()
endif()

if (NOT CRYPTOPP_CONFIG_SEARCH)
	unset (CRYPTOPP_CONFIG_SEARCH CACHE)

	check_include_file_cxx (${CRYPTOPP_INCLUDE_PREFIX}/config.h
		CRYPTOPP_CONFIG_SEARCH
	)
endif()

if (NOT CRYPTOPP_CONFIG_FILE)
	if (CRYPTOPP_CONFIG_SEARCH)
		if (CRYPTOPP_INCLUDE_DIR)
			set (CRYPTOPP_CONFIG_FILE ${CRYPTOPP_INCLUDE_DIR}/${CRYPTOPP_INCLUDE_PREFIX}/config.h
				CACHE FILEPATH "cryptopp config.h" FORCE
			)

			set_target_properties (CRYPTOPP::CRYPTOPP PROPERTIES
				INTERFACE_INCLUDE_DIRECTORIES "${CRYPTOPP_INCLUDE_DIR}"
			)
		else()
			set (CRYPTOPP_CONFIG_FILE ${CRYPTOPP_INCLUDE_PREFIX}/config.h
				CACHE FILEPATH "cryptopp config.h" FORCE
			)
		endif()
	else()
		unset(CRYPTOPP_CONFIG_SEARCH)
	endif()

	unset (CMAKE_REQUIRED_INCLUDES)
endif()

if (NOT CRYPTOPP_CONFIG_FILE)
		MESSAGE (FATAL_ERROR "crypto++ config.h not found")
endif()

if (NOT CRYPTOPP_VERSION)# AND CRYPTO_COMPLETE)
	if (CMAKE_CROSSCOMPILING)
		# try_run() cannot execute a binary built for the host, so read the macro out of
		# the header instead. Kept to the cross path: running the probe is the stronger
		# check, since it proves the header compiles and yields the value the compiler
		# sees, not the one a regex found.
		find_file (CRYPTOPP_VERSION_HEADER
			NAMES ${CRYPTOPP_INCLUDE_PREFIX}/config_ver.h ${CRYPTOPP_INCLUDE_PREFIX}/config.h
			HINTS ${CRYPTOPP_INCLUDE_DIR}
		)

		if (CRYPTOPP_VERSION_HEADER)
			file (STRINGS ${CRYPTOPP_VERSION_HEADER} _cryptopp_version_lines
				REGEX "^[ \t]*#[ \t]*define[ \t]+CRYPTOPP_VERSION[ \t]+[0-9]+"
			)

			if (_cryptopp_version_lines)
				list (GET _cryptopp_version_lines 0 _cryptopp_version_line)
				string (REGEX REPLACE "^.*CRYPTOPP_VERSION[ \t]+([0-9]+).*$" "\\1"
					CRYPTOPP_VERSION "${_cryptopp_version_line}"
				)
			else()
				# Some forks derive CRYPTOPP_VERSION from the parts rather than spelling
				# it out, which the regex above cannot see. Rebuild it the way classic
				# Crypto++ does (config_ver.h:53).
				foreach (_part MAJOR MINOR REVISION)
					file (STRINGS ${CRYPTOPP_VERSION_HEADER} _cryptopp_part_line
						REGEX "^[ \t]*#[ \t]*define[ \t]+CRYPTOPP_${_part}[ \t]+[0-9]+"
					)
					if (_cryptopp_part_line)
						list (GET _cryptopp_part_line 0 _cryptopp_part_line)
						string (REGEX REPLACE "^.*CRYPTOPP_${_part}[ \t]+([0-9]+).*$" "\\1"
							_cryptopp_${_part} "${_cryptopp_part_line}"
						)
					endif()
				endforeach()

				if (DEFINED _cryptopp_MAJOR AND DEFINED _cryptopp_MINOR AND DEFINED _cryptopp_REVISION)
					math (EXPR CRYPTOPP_VERSION
						"${_cryptopp_MAJOR} * 100 + ${_cryptopp_MINOR} * 10 + ${_cryptopp_REVISION}"
					)
				endif()
			endif()
		endif()

		if (NOT CRYPTOPP_VERSION)
			message (FATAL_ERROR
				"could not read CRYPTOPP_VERSION from the crypto++ headers while cross-compiling; "
				"pass -DCRYPTOPP_VERSION=<n> (the integer the headers define, e.g. 890)"
			)
		endif()
	else()
		set (CMAKE_CONFIGURABLE_FILE_CONTENT
			"#include <${CRYPTOPP_CONFIG_FILE}>\n
			#include <stdio.h>\n
			int main(){\n
				printf (\"%d\", CRYPTOPP_VERSION);\n
			}\n"
		)

		configure_file ("${CMAKE_ROOT}/Modules/CMakeConfigurableFile.in"
			"${CMAKE_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/CMakeTmp/CheckCryptoppVersion.cxx" @ONLY IMMEDIATE
		)

		try_run (RUNRESULT
			COMPILERESULT
			${CMAKE_BINARY_DIR}
			${CMAKE_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/CMakeTmp/CheckCryptoppVersion.cxx
			RUN_OUTPUT_VARIABLE CRYPTOPP_VERSION
		)
	endif()

	# Two CRYPTOPP_VERSION encodings exist in the wild:
	#
	#   - Classic Crypto++ (weidai11/cryptopp/config_ver.h:53):
	#         MAJOR*100 + MINOR*10 + REVISION
	#     so 8.9.0 → 890, max for v1.x..v9.x is 999.
	#
	#   - cryptopp-modern (cryptopp-modern/cryptopp-modern, calendar
	#     versioning per their config_ver.h:50 doc comment):
	#         YEAR*10000 + MONTH*100 + INCREMENT
	#     so the 2026.6 build is 20260600. Always ≥ 20,240,000 for any
	#     release from 2024 onwards.
	#
	# The gap between 9,999 and 20,000,000 is enormous, so a single
	# numeric threshold disambiguates without risk of collision either
	# now or in any realistic future (and would also gracefully accept
	# a hypothetical classic Crypto++ 10.x.y in the 1000-9999 range as
	# "clearly newer than MIN_CRYPTOPP_VERSION").
	if (CRYPTOPP_VERSION GREATER_EQUAL 10000)
		MESSAGE (STATUS "crypto++ version ${CRYPTOPP_VERSION} (calendar / cryptopp-modern variant) -- OK")
	else()
		string (REGEX REPLACE "([0-9])([0-9])([0-9])" "\\1.\\2.\\3" CRYPTOPP_VERSION "${CRYPTOPP_VERSION}")

		if (${CRYPTOPP_VERSION} VERSION_LESS ${MIN_CRYPTOPP_VERSION})
			message (FATAL_ERROR "crypto++ version ${CRYPTOPP_VERSION} is too old")
		else()
			MESSAGE (STATUS "crypto++ version ${CRYPTOPP_VERSION} -- OK")
		endif()
	endif()
	set (CRYPTOPP_CONFIG_FILE ${CRYPTOPP_CONFIG_FILE} CACHE STRING "Path to config.h of crypto-lib" FORCE)
	set (CRYPTOPP_VERSION ${CRYPTOPP_VERSION} CACHE STRING "Version of cryptopp" FORCE)
endif()
