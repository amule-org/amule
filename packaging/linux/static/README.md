# Fully static aMule daemon (musl/Alpine)

Produces `amuled`, `amulecmd` and `amuleapi` as **fully static** binaries —
one self-contained file each, with no runtime shared-library dependencies.
`scp` them to any Linux host of the right architecture and run; nothing to
install.

```sh
packaging/linux/build.sh static            # host arch → tarball in dist/
packaging/linux/build.sh static x86_64     # cross-arch (needs setup-cross-arch)
packaging/linux/build.sh static aarch64
```

Output: `dist/aMule-<version>-Linux-<x64|arm64>-static.tar.gz`.

## What's included

Feature set is complete for a headless node: **UPnP**, **IP2Country**
(GeoIP; supply your own `GeoLite2`/`dbip` `.mmdb`), **BFD** crash
backtraces, **NLS**, and **wxWebRequest** HTTP downloads.

The monolithic **GUI is intentionally excluded** — wxGTK `dlopen()`s
pixbuf/theme/input-method modules at runtime, so it cannot be meaningfully
static-linked. This track is daemon-only.

## Why musl (Alpine), not glibc

glibc's NSS `dlopen()`s `libnss_*` even inside a `-static` binary, so
hostname resolution would still need those `.so`s on the target — not
self-contained. musl builds NSS into libc, so `-static` is genuinely
standalone. (macOS static linking isn't possible at all: Apple ships no
static `libSystem`.)

## How it's built

[`Dockerfile`](Dockerfile) on an Alpine base (`ALPINE_STATIC_BASE`),
driven by `build.sh` which feeds versions from
[`../versions.env`](../versions.env):

- **wxWidgets** built static, `--disable-gui`, with the libcurl backend
  (`WX_*` pins, SHA256-verified).
- **Crypto++** built static (`CRYPTOPP_TAG_SUFFIX`).
- **pupnp/libupnp** built static from the pinned source archive
  (`LIBUPNP_*`, SHA256-verified; autotools bootstrapped in-tree).
- `libmaxminddb` / `binutils` (BFD) / `gettext` (libintl) from Alpine's
  static packages.
- Final `-static` link pulls the `libcurl → openssl → nghttp2/brotli/…`
  closure via `pkg-config --static`.

The build gates on a `file`/`ldd` static-linking check, so a regression
fails the build rather than shipping a dynamically-linked binary.

## Runtime notes

Neither affects the static property:

- **HTTPS needs a CA bundle** present on the host (`/etc/ssl/certs`) for
  certificate verification — that's data, not a library.
- These are **daemon binaries only**; there is no GUI.
