# Makes the system ngtcp2 QUIC stack available as Quic::Ngtcp2.
# This file is included only for an explicitly enabled core build.

find_package (PkgConfig REQUIRED)
pkg_check_modules (NGTCP2 REQUIRED IMPORTED_TARGET
	libngtcp2>=1.0.0
	libngtcp2_crypto_gnutls
	gnutls
)

add_library (Quic::Ngtcp2 INTERFACE IMPORTED)
target_link_libraries (Quic::Ngtcp2 INTERFACE PkgConfig::NGTCP2)
