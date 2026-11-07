# Third-party components

aMule binary distributions (AppImage, Flatpak, Linux static tarball, macOS
bundle, Windows installer and zip) include code, artwork and data from third
parties under their own licenses. Permissive
components (MIT, BSD) require copyright-notice reproduction. LGPL components
require shipping the licence text and publishing the exact sources of the
bundled versions, whether the build links them statically or dynamically.
This file reproduces the copyright notice and license terms for each, as
required by their respective binary-distribution clauses. aMule's
own code is under GPLv2-or-later — see [LICENSE.md](../LICENSE.md).

## picojson

JSON parser used by `libwebcommon` (the REST API + SSE auth surface
shared by amuleapi). License: BSD 2-Clause Simplified.

Upstream: <https://github.com/kazuho/picojson> — version 1.3.0,
vendored at [`src/libwebcommon/picojson.h`](../src/libwebcommon/picojson.h).
Full license text in
[`src/libwebcommon/picojson.LICENSE`](../src/libwebcommon/picojson.LICENSE).

> Copyright 2009-2010 Cybozu Labs, Inc.
> Copyright 2011-2014 Kazuho Oku
> All rights reserved.
>
> Redistribution and use in source and binary forms, with or without
> modification, are permitted provided that the following conditions are met:
>
> 1. Redistributions of source code must retain the above copyright notice,
>    this list of conditions and the following disclaimer.
>
> 2. Redistributions in binary form must reproduce the above copyright notice,
>    this list of conditions and the following disclaimer in the documentation
>    and/or other materials provided with the distribution.
>
> THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
> AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
> IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
> ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
> LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
> CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
> SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
> INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
> CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
> ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
> POSSIBILITY OF SUCH DAMAGE.

The vendored header is upstream 1.3.0 with two comment lines added at the
top that turn clang-format off; the code itself is unmodified. The
`PICOJSON_USE_INT64` toggle is defined at use sites rather than as a
modification to the header.

The bundled copy is used by default. Distributions may configure with
`-DUSE_SYSTEM_PICOJSON=ON` to use a system-installed `picojson.h` version
1.3.0 or newer instead.

## muleunit

Lightweight unit test framework used by the C++ test suite at
`unittests/tests/`. License: GNU LGPL v2.1.

Source ships at [`unittests/muleunit/`](../unittests/muleunit/); full
license text in
[`unittests/muleunit/license.txt`](../unittests/muleunit/license.txt).
The framework is statically linked into every test binary but is NOT
linked into shipped daemon binaries (`amule`, `amuled`, `amulegui`,
`amuleweb`, `amuleapi`), so the LGPL relinking clause does not constrain
end-user binary distribution.

## libutp

Micro Transport Protocol (uTP) library, vendored for the forthcoming uTP
transport. License: MIT.

Upstream: <https://github.com/transmission/libutp> — version 3.4, pinned at
commit `490874c44a2ecf914404b0a20e043c9755fff47b`, vendored verbatim at
[`src/extern/libutp/`](../src/extern/libutp/). Full license text in
[`src/extern/libutp/LICENSE`](../src/extern/libutp/LICENSE); the pin, the list
of vendored files and how to verify them against upstream are recorded in
[`src/extern/libutp/AMULE_PROVENANCE.md`](../src/extern/libutp/AMULE_PROVENANCE.md).

> Copyright (c) 2010-2013 BitTorrent, Inc.
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in
> all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
> THE SOFTWARE.

The snapshot carries no local patches. It is built only with
`-DENABLE_UTP=YES`, which is OFF by default. With that switch on, `amule` and
`amuled` link it. A default build neither compiles nor ships it, so this notice
applies only to binaries built with the switch on.

## ngtcp2

QUIC transport library, linked from the system when aMule is built with
`-DENABLE_QUIC=YES`. License: MIT.

Upstream: <https://github.com/ngtcp2/ngtcp2>.

> The MIT License
>
> Copyright (c) 2016 ngtcp2 contributors
>
> Permission is hereby granted, free of charge, to any person obtaining
> a copy of this software and associated documentation files (the
> "Software"), to deal in the Software without restriction, including
> without limitation the rights to use, copy, modify, merge, publish,
> distribute, sublicense, and/or sell copies of the Software, and to
> permit persons to whom the Software is furnished to do so, subject to
> the following conditions:
>
> The above copyright notice and this permission notice shall be
> included in all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
> EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
> MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
> NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
> LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
> OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
> WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

ngtcp2 is not vendored; it is located at build time through pkg-config. A
default build (`-DENABLE_QUIC` is OFF) neither compiles nor ships it, so this
notice applies only to binaries built with `-DENABLE_QUIC=YES`.

## GnuTLS, Nettle, GMP (QUIC builds only)

When `-DENABLE_QUIC=YES`, aMule links `ngtcp2_crypto_gnutls`, which pulls
in GnuTLS and its dependencies Nettle and GMP.

| Library | Licence options |
|---------|----------------|
| GnuTLS  | LGPL-2.1-or-later (§3 also permits use under GPL) |
| Nettle  | LGPL-3.0-or-later or GPL-2.0-or-later (dual) |
| GMP     | LGPL-3.0-or-later or GPL-2.0-or-later (dual) |

Upstream: <https://www.gnutls.org/>, <https://www.lysator.liu.se/~nisse/nettle/>,
<https://gmplib.org/>.

aMule is GPL-2.0-or-later. Under LGPL-2.1 §3, GnuTLS may be used under
the GPL. Nettle and GMP offer a GPL-2.0-or-later option. A packaged build
that enables QUIC may therefore link these libraries statically under the
GPL, provided it:

1. Ships the licence texts for GnuTLS, Nettle, and GMP.
2. Publishes the exact source tarballs of the bundled versions (whether
   the build is static or dynamic, and whether the libraries are modified
   or not).

For comparison, eMuleAI's `Licenses/THIRD-PARTY-NOTICES.txt` lists ngtcp2
under MIT, GnuTLS under LGPL-2.1-or-later, and Nettle and GMP with their
GPL-2.0-or-later option.

A default build (`-DENABLE_QUIC` is OFF) does not link any of these
libraries, and this section does not apply to it.

## Desktop menu icons — Bootstrap Icons

Selected SVGs from Bootstrap Icons 1.13.1, commit `ce0e49dd063243118a115f17ad1fe1fe7576d552`.
Source: https://github.com/twbs/icons. Normalized to paths for NanoSVG, coloured at runtime
using the system menu text colour, with generated PNG fallbacks. Original artwork,
source checksums and regeneration instructions are in `src/icons/vendor/` and
`src/icons/ARTWORK.md`.

```text
The MIT License (MIT)

Copyright (c) 2019-2024 The Bootstrap Authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## Country flags — flag-icons

SVGs from flag-icons 7.5.0, commit `7aa5b2bdddd570ece62c812c0cb588ccdc099e2e`.
Source: https://github.com/lipis/flag-icons, `flags/4x3`. Rasterized with librsvg
to embedded PNGs at 16×12, 32×24 and 48×36. Original SVGs are retained in
`src/icons/vendor/flag-icons/`.

```text
The MIT License (MIT)

Copyright (c) 2013 Panayiotis Lipiridis

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

The retained legacy `an.png` and `unknown.png` flags are FamFamFam artwork by
Mark James (http://www.famfamfam.com/lab/icons/flags/), distributed as public
domain; attribution appreciated but not required, per the original notice.

## Web UI libraries - Preact and htm

The Web UI that amuleapi serves loads these from
[`src/webapi/static/js/vendor/`](../src/webapi/static/js/vendor/). They ship
with amuleapi, next to their license files, in every package that includes it.

| Library | Version | Upstream | License | Files |
|---------|---------|----------|---------|-------|
| Preact and `preact/hooks` | 10.29.2 | <https://github.com/preactjs/preact> | MIT | `preact.module.js`, `hooks.module.js`, `preact.LICENSE` |
| htm | 3.1.1 | <https://github.com/developit/htm> | Apache-2.0 | `htm.module.js`, `htm.LICENSE` |

Each file is the upstream build, with its name and version on the first line.

```text
The MIT License (MIT)

Copyright (c) 2015-present Jason Miller

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

htm is under the Apache License 2.0. Its full text ships as `htm.LICENSE`, and
upstream has no NOTICE file. Apache-2.0 is compatible with GPL-3.0, and aMule's
"or later" clause allows the combination to be distributed under GPL-3.0.

```text
Copyright 2018 Google Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```

## Libgcrypt `bithelp.h`

Bit-rotation helpers from Libgcrypt, at
[`src/utils/aLinkCreator/src/bithelp.h`](../src/utils/aLinkCreator/src/bithelp.h),
built into `alc` and `alcc`. License: LGPL-2.1-or-later. Section 3 of the LGPL
allows applying the GPL to it instead, so it is distributed in those binaries
under aMule's GPL-2.0-or-later.

> Copyright (c) 1999-2011 Free Software Foundation, Inc.
>
> This file is part of Libgcrypt.
>
> Libgcrypt is free software; you can redistribute it and/or modify
> it under the terms of the GNU Lesser general Public License as
> published by the Free Software Foundation; either version 2.1 of
> the License, or (at your option) any later version.

## macOS backtrace code - Edd Dawson

The macOS branch of
[`src/libs/common/MuleDebug.cpp`](../src/libs/common/MuleDebug.cpp) (stack
frame list and demangling) is modified from code by Edd Dawson under the Boost
Software License 1.0. It is compiled into the macOS binaries only. The BSL
requires the notice in source copies, not in object code; the text is at
[`src/libs/common/LICENSE_1_0.txt`](../src/libs/common/LICENSE_1_0.txt).

```text
Copyright 2007 Edd Dawson.

Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
```

## Toolbar and client skins

The optional skins in [`src/skins/`](../src/skins/) are installed for `amule`
and `amulegui` (`INSTALL_SKINS`). They were contributed to aMule for distribution with it. Only
`tango.zip` carries its own notice; for the others, no license beyond that
contribution was recorded.

| Skin | Added | Origin |
|------|-------|--------|
| `tango.zip` | 2007, updated 2008 | Assembled by mischamajskij from Tango Desktop Project, GNOME 2.18, Pidgin and aMule community icons. Its `README` licenses the icons under CC BY-SA 2.5 or GPL-2.0, depending on their origin. |
| `gnome.zip`, `kde4.zip`, `xfce.zip` | 2007 | Shipped by johkra and later updated by deloun, named after the GNOME, KDE 4 and Xfce desktops. No license was recorded. |
| `Mac_Gray.zip`, `priscilla.zip` | 2008 | Toolbar skins by greek. No license was recorded. |
| `papirus.zip` | 2019 | Added by jEsuSdA, named after the Papirus icon theme (GPL-3.0). No license was recorded. |

## IP-to-country data - DB-IP and MaxMind GeoLite2

aMule does not ship a country database. When country flags are enabled, it
downloads one at runtime from the source chosen in the preferences:

- **DB-IP** (default): DB-IP's free IP to Country Lite database, licensed
  CC BY 4.0. Attribution: "IP-to-Country data by DB-IP.com"
  (<https://db-ip.com>).
- **MaxMind GeoLite2**: needs a license key and is under the GeoLite2 End User
  License Agreement. Attribution: "This product includes GeoLite2 data created
  by MaxMind, available from <https://www.maxmind.com>."

The desktop preferences show the attribution for the loaded database, and the
Web UI About page credits DB-IP.

## Libraries bundled in the binary packages

Each binary package also carries the license files of the libraries it bundles
or links statically:

| Package | Location |
|---------|----------|
| AppImage | `usr/share/licenses/<package>/`: the Debian copyright file of each bundled library, plus wxWidgets, libupnp and libayatana-appindicator, which are built from source |
| Flatpak | `/app/share/licenses/<module>/` for each module the manifest builds |
| macOS | `aMule.app/Contents/Resources/licenses/<formula>/` and the same in `aMuleGUI.app`, from the Homebrew kegs |
| Windows installer and zip | `licenses/<package>/`, from the MSYS2 packages, plus libupnp |
| Linux static tarball | `licenses/`: Crypto++, wxWidgets and libupnp, plus `alpine-packages.tsv` with the version, SPDX license and upstream of each Alpine package linked in |

Alpine packages ship no license texts, so the static tarball lists those
libraries rather than carrying their texts.

Linux builds that find GNU libbfd (the AppImage and the static tarball) use it
for crash backtraces. libbfd is GPL-3.0-or-later, and aMule's "or later"
clause lets those binaries be distributed under GPL-3.0.
