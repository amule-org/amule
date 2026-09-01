# syntax=docker/dockerfile:1
#
# Fully static aMule daemon (musl/Alpine) — self-contained amuled, amulecmd
# and amuleapi binaries with no runtime shared-library dependencies. Feature
# set: UPnP, IP2Country (GeoIP), BFD backtraces, NLS, and wxWebRequest
# downloads. The monolithic GUI is intentionally excluded: wxGTK dlopen()s
# loader/theme/input-method modules, so it cannot be meaningfully static.
#
# Driven by packaging/linux/build.sh, which feeds the ARGs below from
# packaging/linux/versions.env (the single source of truth for pinned
# versions). Build from the repository root:
#
#   packaging/linux/build.sh static            # host arch, tarball in dist/
#
# or directly (context is the checkout):
#
#   docker build -f packaging/linux/static/Dockerfile \
#     --build-arg ALPINE_STATIC_BASE=3.20 --build-arg WX_VERSION=... \
#     --output type=local,dest=out .
#
# Build args have no defaults so a missing pin fails loud — the caller must
# source versions.env.

ARG ALPINE_STATIC_BASE

FROM alpine:${ALPINE_STATIC_BASE} AS builder

ARG WX_VERSION
ARG WX_GIT_URL
ARG WX_COMMIT
ARG CRYPTOPP_TAG_SUFFIX
ARG LIBUPNP_VERSION
ARG LIBUPNP_TARBALL_URL
ARG LIBUPNP_SHA256

RUN apk add --no-cache \
        build-base cmake git python3 pkgconf wget \
        autoconf automake libtool \
        linux-headers \
        boost-dev \
        zlib-dev zlib-static \
        expat-dev expat-static \
        curl-dev curl-static \
        openssl-dev openssl-libs-static \
        nghttp2-dev nghttp2-static \
        brotli-dev brotli-static \
        c-ares-dev c-ares-static \
        zstd-dev zstd-static \
        libidn2-dev libidn2-static \
        libunistring-dev libunistring-static \
        libpsl-dev libpsl-static \
        binutils-dev \
        libmaxminddb-dev libmaxminddb-static \
        gettext gettext-dev gettext-static

# --- Crypto++ (static libcryptopp.a) ---
RUN git clone --depth 1 --branch "CRYPTOPP_${CRYPTOPP_TAG_SUFFIX}" \
        https://github.com/weidai11/cryptopp.git /tmp/cryptopp \
 && make -C /tmp/cryptopp -j"$(nproc)" libcryptopp.a \
 && make -C /tmp/cryptopp PREFIX=/usr/local install

# --- wxWidgets (static, base + net, no GUI) ---
# From the 3.2 branch at a pinned commit rather than a release tarball: no
# 3.2.x release carries the wxEpollDispatcher unregister fix amuled needs
# (amule-org/amule#1136). Revert to the tarball when 3.2.12 ships.
RUN git init /tmp/wx \
 && cd /tmp/wx \
 && git remote add origin "${WX_GIT_URL}" \
 && git fetch --depth 1 origin "${WX_COMMIT}" \
 && git checkout FETCH_HEAD \
 && git submodule update --init --depth 1 --recursive \
 && ./configure \
        --disable-shared \
        --disable-gui \
        --with-zlib=builtin \
        --with-expat=builtin \
        --prefix=/usr/local \
 && make -j"$(nproc)" && make install

# --- libupnp / pupnp (static; no static Alpine package, build from source) ---
# pupnp 22.x is CMake-only (upstream dropped autotools), so build with CMake:
# static libs only (UPNP_BUILD_SHARED=OFF), no samples/tests.
# CMAKE_INSTALL_LIBDIR=lib keeps libupnp.pc out of lib64. The install also
# drops a UPNP CMake package config that exposes only a UPNP::Static target,
# but aMule links UPNP::Shared -- so remove lib/cmake/{UPNP,IXML} to make
# aMule's find_package(UPNP CONFIG) fall back to pkg-config, which synthesises
# UPNP::Shared from the static libupnp.a / libixml.a (the same path the 1.14.x
# autotools build relied on). 22.x no longer needs the musl "-include stddef.h"
# workaround the 1.14.x threadutil sources did.
RUN wget -qO /tmp/libupnp.tar.gz "${LIBUPNP_TARBALL_URL}" \
 && echo "${LIBUPNP_SHA256}  /tmp/libupnp.tar.gz" | sha256sum -c - \
 && mkdir -p /tmp/libupnp && tar -xf /tmp/libupnp.tar.gz -C /tmp/libupnp --strip-components=1 \
 && cd /tmp/libupnp \
 && cmake -B build -DCMAKE_BUILD_TYPE=Release \
        -DUPNP_BUILD_SHARED=OFF -DUPNP_BUILD_STATIC=ON \
        -DUPNP_BUILD_SAMPLES=OFF -DUPNP_BUILD_TESTS=OFF -Dreuseaddr=ON -Dipv6=ON \
        -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_INSTALL_LIBDIR=lib \
 && cmake --build build -j"$(nproc)" && cmake --install build \
 && rm -rf /usr/local/lib/cmake/UPNP /usr/local/lib/cmake/IXML

# Force static resolution for deps that ship BOTH .so and .a. CMake's
# find_library prefers the .so dev symlink and hands its ABSOLUTE path to the
# linker, which -static then rejects ("attempted static link of dynamic
# object"). -DCMAKE_FIND_LIBRARY_SUFFIXES=.a doesn't stick (the platform module
# resets it during project()). Dropping just the libX.so dev symlinks — while
# keeping libX.so.N for dynamic build tools like msgfmt — makes find_library
# resolve to the static archive. Every lib below has a .a present.
RUN for l in bfd opcodes ctf sframe zstd z maxminddb intl; do \
        rm -f "/usr/lib/lib$l.so" "/lib/lib$l.so"; \
    done

# --- aMule (static daemon, full feature set: UPnP + IP2Country + BFD + NLS) ---
# Built from the repository checkout (the build context), so CI validates the
# code under review. The only feature dropped is the optional
# CHTTPDownloadThread CURLOPT tuning (CMAKE_DISABLE_FIND_PACKAGE_CURL) — its
# config-mode target drags in libcurl.so; downloads still work via wx's own
# (static) libcurl webrequest backend.
COPY . /src
WORKDIR /src
RUN export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/lib/pkgconfig \
 && cmake -S . -B build-static \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_MONOLITHIC=OFF \
        -DBUILD_DAEMON=ON \
        -DBUILD_AMULECMD=ON \
        -DBUILD_AMULEAPI=ON \
        -DENABLE_UPNP=ON \
        -DENABLE_IP2COUNTRY=ON \
        -DENABLE_BFD=ON \
        -DENABLE_NLS=ON \
        -DENABLE_VERSION_CHECK=OFF \
        -DwxWidgets_USE_STATIC=ON \
        -DwxWidgets_CONFIG_EXECUTABLE=/usr/local/bin/wx-config \
        -DZLIB_USE_STATIC_LIBS=ON \
        -DCMAKE_DISABLE_FIND_PACKAGE_CURL=ON \
        -DCMAKE_EXE_LINKER_FLAGS="-static -static-libgcc -static-libstdc++" \
        -DCMAKE_CXX_STANDARD_LIBRARIES="$(pkg-config --static --libs libcurl)" \
 && cmake --build build-static -j"$(nproc)" \
 && mkdir -p /out \
 && for b in amuled amulecmd amuleapi; do \
        cp "$(find build-static -type f -name "$b" | head -1)" /out/; \
    done \
 && strip /out/amuled /out/amulecmd /out/amuleapi

# Sanity gate: fail the build unless each binary is a static executable.
RUN for b in amuled amulecmd amuleapi; do \
        file "/out/$b"; \
        ldd "/out/$b" 2>&1 | grep -qE "Not a valid dynamic program|not a dynamic executable" \
          || { echo "FAIL: $b is dynamically linked"; ldd "/out/$b"; exit 1; }; \
    done

FROM scratch AS export
COPY --from=builder /out/ /
