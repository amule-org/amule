# libwebcommon — pure-logic primitives for the amuleapi REST surface.
#
# Linked into the amuleapi binary and the libwebcommon unit-test
# target. NOT linked into amuleweb: amuleweb's behaviour on 3.0.x is
# bit-identical to 3.0.0 and is built from the same translation units
# it always was. If a downstream consumer (e.g. a hypothetical
# integration test or a third-party amule front-end) wants the same
# JWT / ETag / route-pattern primitives, link against this lib too.

add_library (webcommon STATIC
	ConstantTime.cpp
	Etag.cpp
	HeaderParse.cpp
	JsonWriter.cpp
	Jwt.cpp
	PathPatterns.cpp
)

target_include_directories (webcommon
	PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

# wxWidgets::BASE for wxString (used by JsonWriter + ConstantTime).
# No GUI dependency — these are wxBase-only types. CRYPTOPP::CRYPTOPP
# carries its own INTERFACE_INCLUDE_DIRECTORIES so consumers get
# <cryptopp/sha.h> / <cryptopp/hmac.h> visible without further
# include-path wiring.
target_link_libraries (webcommon
	PUBLIC wxWidgets::BASE
	PUBLIC CRYPTOPP::CRYPTOPP
)
