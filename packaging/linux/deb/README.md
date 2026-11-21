# Debian and Ubuntu packages

Native `.deb` packages for an APT repository. Supported suites: Debian 12
and 13, Ubuntu 22.04, 24.04, 26.04 and 26.10.

The packages use each suite's own libraries, except wxWidgets. The distro
wx is too old on some suites, or built without `wxWebRequest`, so every
suite gets the wx pinned in `../versions.env`. It is built as a private
shared library in `/usr/lib/<triplet>/amule/`, and the aMule binaries find
it through their RUNPATH. It uses the system libpng, libjpeg, zlib and
PCRE2, so their security updates still apply. wx's own translations come
from the distro's `wx3.2-i18n`, where the suite has it.

```sh
packaging/linux/build.sh deb debian13            # release
packaging/linux/build.sh deb debian13 nightly    # snapshot of HEAD
```

The build runs in the suite's Docker image for the host architecture. The
source package and the `.deb` files land in `dist/deb/<suite>/out/`. The
wx tarball is the source package's second orig tarball.

## Packages

The package names match Debian's, so these packages upgrade a distro
install in place.

| Package | Contents |
|---|---|
| `amule` | the monolithic GUI client |
| `amule-daemon` | `amuled` and `amuleapi` |
| `amule-utils` | `amulecmd`, `ed2k`, `cas`, `alcc` |
| `amule-utils-gui` | `amulegui`, `wxcas`, `alc` |
| `amule-common` | the private wx, translations, skins, icons, MIME data, `amule-media-probe` |

`amule-common` is architecture-specific because it carries wx and the
media probe. Every other package depends on its exact version.

Differences from Debian's packaging:

- No init script. Upgrading `amule-daemon` removes Debian's
  `/etc/init.d/amule-daemon` and `/etc/default/amule-daemon`.
- No `amuleweb`. `amuleapi` replaces it.
- No `amule-gnome-support`. The desktop files register the `ed2k:` and
  `magnet:` handlers.

## Versions

Every version has the epoch `1:`, as Debian's packages do. Without it, apt
does not upgrade them.

| Channel | Example |
|---|---|
| release | `1:3.2.0-1~deb13` |
| nightly | `1:3.2.0+git20261114.13382.ec11f2823-1~deb13` |

The suite suffix keeps the file names apart in a shared pool, and orders
the versions so that a distribution upgrade also upgrades aMule. A nightly
sorts above the release it follows and below the next one. A nightly build
needs the full git history, because the commit count orders two nightlies
of the same day.

## Suites

We build every supported Debian release, until its LTS ends, and every
supported Ubuntu release, interim releases included. Each week
`check-releases.py` compares the suites with Debian's `distro-info-data`
and opens an issue when a release comes out or reaches its end of life.

To add a suite, add it to `build_deb()` and the usage text in
`../build.sh`, to the matrix in `.github/workflows/deb.yml`, and to the
list above. To drop one, remove it from the same places.

## Publishing

`.github/workflows/deb.yml` uploads the packages to Cloudsmith, which builds,
signs and serves the APT index. A published GitHub Release goes to
`amule-org/amule`. The nightly schedule goes to `amule-org/amule-nightly`,
whose retention rule deletes old snapshots. The upload uses the
`CLOUDSMITH_API_KEY` secret, the API key of a Cloudsmith service account
with write access to both repositories; without it, the workflow only
builds.
