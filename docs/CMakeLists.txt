add_subdirectory (man)

install (FILES
	CHANGELOG.md
	EC_Protocol.md
	IPv6_Server_Sources.md
	NAT_T_Protocol.md
	INSTALL.md
	IP2Country.md
	README.md
	THIRDPARTY.md
	amulesig.md
	DESTINATION "${CMAKE_INSTALL_DOCDIR}"
)
