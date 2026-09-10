# libutp vendoring provenance

This directory is a verbatim partial snapshot of an upstream libutp revision.
It carries no aMule changes, and nothing here may be edited locally.

| | |
| --- | --- |
| Upstream | <https://github.com/transmission/libutp> |
| Commit | `490874c44a2ecf914404b0a20e043c9755fff47b` |
| Commit date | 2024-11-16 |
| Version | 3.4 |
| License | MIT — see [`LICENSE`](LICENSE) |
| Local patches | none |

`transmission/libutp` rather than the original `bittorrent/libutp`: the original
is unmaintained, while Transmission's fork is still maintained and is the one
that carries the CMake packaging and the `libutp/` include prefix this vendoring
relies on.

## What is vendored, and what is not

[`SHA256SUMS`](SHA256SUMS) lists every vendored file. It covers exactly the
upstream paths selected below and not this file, so verification does not depend
on aMule-authored text.

Vendored: the library's sources and headers, its own `CMakeLists.txt` and
`config.cmake.in`, plus `LICENSE` and `README.md`.

Deliberately not vendored, because none of it is needed to build the library and
all of it would age independently of the pin: upstream's `.github/` CI,
`.gitignore`, `Makefile`, the Visual Studio project files and `prop_sheets/`, the
`ucat.c` sample and `parse_log.py`.

`libutp_inet_ntop.{cpp,h}` is kept even though it is only compiled on Windows,
since dropping a platform's source would make the snapshot no longer a snapshot.

## Verifying this snapshot

```sh
git clone https://github.com/transmission/libutp /tmp/libutp-upstream
git -C /tmp/libutp-upstream checkout 490874c44a2ecf914404b0a20e043c9755fff47b

# every vendored file is byte-identical to upstream at that commit
cd src/extern/libutp
awk '{ print $2 }' SHA256SUMS | while read -r f; do
	cmp "$f" "/tmp/libutp-upstream/$f" || echo "DIFFERS: $f"
done

# and the manifest still describes this directory
shasum -a 256 -c SHA256SUMS
```

## Updating the pin

Re-run the copy for the paths in `SHA256SUMS`, regenerate the manifest, and
update the table above in the same commit. A pin bump is its own commit: keeping
it separate from aMule code is what lets a reviewer diff it against upstream
instead of reading it.
