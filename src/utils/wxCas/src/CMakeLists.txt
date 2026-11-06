# All of wxCas but wxcas.cpp, which WxCasStatsPanelTest builds without main().
add_library (wxcas_objects OBJECT
	linuxmon.cpp
	onlinesig.cpp
	wxcascte.cpp
	wxcasframe.cpp
	wxcasprefs.cpp
	wxcaspix.cpp
	wxcasprint.cpp
)

target_include_directories (wxcas_objects
	PUBLIC ${amule_BINARY_DIR}
)

target_link_libraries (wxcas_objects
	PUBLIC wxWidgets::CORE
	PUBLIC wxWidgets::NET
)

if (APPLE)
	target_link_libraries (wxcas_objects
		PUBLIC "-framework CoreServices"
	)
endif()

add_executable (wxcas
	wxcas.cpp
)

if (WIN32)
	target_sources (wxcas
		PRIVATE ${CMAKE_BINARY_DIR}/version.rc
			${CMAKE_CURRENT_SOURCE_DIR}/../wxcas.rc
	)

	set_target_properties (wxcas
		PROPERTIES WIN32_EXECUTABLE TRUE
	)
endif()

target_link_libraries (wxcas
	PRIVATE wxcas_objects
)

install (TARGETS wxcas
	RUNTIME DESTINATION bin
)
