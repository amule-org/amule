add_subdirectory (man)

install (FILES
	CHANGELOG.md
	EC_Protocol.md
	INSTALL.md
	IP2Country.md
	README.md
	THIRDPARTY.md
	amulesig.md
	DESTINATION "${CMAKE_INSTALL_DOCDIR}"
)
