# Third-party components

aMule binary distributions (AppImage, Flatpak, .deb, macOS bundle, Windows
installer) include code from third parties under their own permissive
licenses. This file reproduces the copyright notice and license terms for
each, as required by their respective binary-distribution clauses. aMule's
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

The vendored header is unmodified upstream 1.3.0; the
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
`-DENABLE_UTP=YES`, which is OFF by default, and no aMule target links it yet —
so a default build neither compiles nor ships it, and this notice applies only
to binaries built with that switch on.
