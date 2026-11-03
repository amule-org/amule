add_subdirectory (man)

install (FILES
	CHANGELOG.md
	CHANGELOG-2.x.md
	CHANGELOG-1.x.md
	EC_Protocol.md
	EC_SRP.md
	IPv6_Server_Sources.md
	NAT_T_Protocol.md
	INSTALL.md
	LOGGING.md
	IP2Country.md
	README.md
	THIRDPARTY.md
	amulesig.md
	DESTINATION "${CMAKE_INSTALL_DOCDIR}"
)
