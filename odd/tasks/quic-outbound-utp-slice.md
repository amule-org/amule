# QUIC prerequisite: outbound uTP decision slice

## Objective
Add the smallest outbound uTP decision/service seam needed before NAT rendezvous and QUIC, without changing listeners or NAT behavior.

## Scope
- Expose one outbound uTP stream creation path through existing context/library abstractions.
- Add a pure dial decision that requires peer capability, usable UDP endpoint, and local outbound service.
- Preserve TCP, callback, buddy, proxy, and unsupported-address fallbacks.
- Add focused unit tests for decision and fallback behavior.

## Out of scope
- NAT rendezvous or hole punching.
- QUIC transport and TLS/ngtcp2.
- Listener changes, dual-stack activation, or protocol capability advertisement changes unless required to keep the advertised bit truthful.
- Changes to unrelated dirty worktrees.

## Selected bounded implementation
The approved slice is unactivated: expose a context/library dial seam and pure
eligibility policy only. BaseClient selection and connection lifecycle stay
unchanged. A returned stream is pending, not an application connection event;
writability must never stand in for connection completion.

- [x] Add a fail-closed outbound context/library seam with per-socket crypt setup.
- [x] Add pure eligibility and legacy-fallback tests.
- [x] Run focused validation and record limitations; do not commit.

## Verification
- Focused uTP/dial tests.
- Full monolithic, daemon, and remotegui build when dependencies permit.
- No field NAT/interoperability claim.

## Observed results
- `c++ -std=c++17 -Wall -Wextra -Werror -fsyntax-only -Isrc unittests/tests/UtpDialPolicyTest.cpp`: passed; compile-time assertions exercise all 128 prerequisite combinations and IPv4/port boundaries.
- `c++ -std=c++17 -Wall -Wextra -Werror -fsyntax-only -Isrc -Isrc/include -x c++ src/UtpContext.h`: passed.
- `c++ -std=c++17 -Wall -Wextra -Werror -fsyntax-only $(wx-config --cxxflags) -Isrc -Isrc/include -Isrc/extern/libutp/include src/UtpLibraryAdapter.cpp`: passed.
- `git diff --check`: passed.
- Linked/runtime tests and full builds were not run: generated build outputs are outside the exact authorized edit surfaces. The CTest target is registered for a later authorized build.
- Automatic header diagnostics lacked include-path configuration; explicit compiler checks above passed. Automatic CMake analysis was unavailable.
- Strict TDD was not activated. No commits were made.

## Seam contract and remaining integration
`CUtpContext::Dial` refuses inactive contexts without reopening the UDP service.
The adapter creates a pending IPv4 stream, installs its crypt parameters before
the SYN, and registers the peer before sending. Immediate failure retains the
caller's empty output and closes/deregisters the temporary stream. Asynchronous
failure remains observable on the returned transport; no automatic retry is added.

The adapter records transport connection state on `UTP_STATE_CONNECT`, with no
application event sink installed by Dial. BaseClient selection, connection
notifications, advertisement, callbacks, buddy lookup and proxy routing are
unchanged. A future activation must add explicit connection-completion plumbing,
security/eligibility gathering, and failure handling; writability is not completion.
The pure policy's `PreserveLegacy` result also preserves unsupported-address
refusal rather than forcing TCP. Live handshake, timeout and interoperability
behavior of the new seam remains untested in this slice.
