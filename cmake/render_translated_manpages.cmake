# Build-time rendering of the translated manpages.
#
# po4a renders each page from its English master and po/manpages-<lang>.po, but copies
# @MAN_DATE@ and @PACKAGE_VERSION@ through untouched. configure_file only runs at configure
# time, so its @ONLY pass is re-implemented here with file(READ) + string(CONFIGURE).
#
# po4a skips a page translated below KEEP percent. That page is left out of the install
# instead of failing the build. Earlier renders are deleted first, so a page that falls below
# the threshold is not installed from a previous build.
#
# Caller passes -DPO4A, -DPO4A_CONFIG, -DKEEP, -DPAGES (the rendered .1.in paths, joined with
# "|"), -DSTAMP, -DMAN_DATE and -DPACKAGE_VERSION.

foreach (_var PO4A PO4A_CONFIG KEEP PAGES STAMP)
	if (NOT ${_var})
		message (FATAL_ERROR "render_translated_manpages.cmake: ${_var} must be set.")
	endif()
endforeach()

string (REPLACE "|" ";" _pages "${PAGES}")

foreach (_raw IN LISTS _pages)
	string (REGEX REPLACE "\\.in$" "" _final "${_raw}")
	file (REMOVE "${_raw}" "${_final}")
endforeach()

execute_process (
	COMMAND "${PO4A}" --no-update --keep "${KEEP}" "${PO4A_CONFIG}"
	RESULT_VARIABLE _result
)
if (NOT _result EQUAL 0)
	message (FATAL_ERROR "po4a failed: ${_result}")
endif()

foreach (_raw IN LISTS _pages)
	string (REGEX REPLACE "\\.in$" "" _final "${_raw}")
	if (EXISTS "${_raw}")
		file (READ "${_raw}" _content)
		string (CONFIGURE "${_content}" _content @ONLY)
		file (WRITE "${_final}" "${_content}")
	else()
		get_filename_component (_name "${_final}" NAME)
		message (STATUS "Not installing ${_name}: translated below ${KEEP}%")
	endif()
endforeach()

file (TOUCH "${STAMP}")
