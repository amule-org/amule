# Server-coordinated NAT-T

## Goal and current scope
Implement the first bounded, experimental wire-codec slice using the external
ed2k-server README contract supplied in the task. No interoperability claim.
This bounded scope supersedes the earlier retry-policy and live-login plan.

## Tasks
- [ ] Add default-OFF `ENABLE_NATT_SERVER_COORDINATION` to the experimental switches.
- [ ] Add dependency-free payload builders/parsers and separate TCP-server,
  UDP-server, and UDP-peer opcode namespaces.
- [ ] Add exact-byte, malformed-length, unknown-value, and namespace tests.
- [ ] Parent: execute focused tests and review the changed files.

## Wire contract
- TCP request 0x60: target ID u32 LE, requester UDP port u16 LE.
- TCP info 0x61: IPv4 octets in wire order, TCP u16 LE, UDP u16 LE,
  user hash (16 bytes), role byte.
- TCP failure 0x62: target ID u32 LE, reason byte.
- UDP server keepalive 0x9F: own user hash (16 bytes).
- UDP peer 0xB3: recognition only; unrelated to TCP OP_ESERVER_BUDDY_REQUEST.

Builders return payloads only, not opcode/protocol/length framing. Parsers require
exact payload lengths. Roles and failure reasons remain opaque bytes: the supplied
contract does not assign their meanings. Parsing does not authorize any action.
IPv4 is represented as four octets, avoiding host-endian integer conversions.

## Deferred work / TODO
- Do not add CT_EMULE_UDPPORTS to server login yet. ServerConnect.cpp currently
  writes four tags; adding an advertisement before a response handler and usable
  coordination path exist would be partial support. A later gated integration
  must validate the tag packing/port source and update/test the login tag count.
- No peer selection, identity policy, retry scheduler, live punching, keepalive
  transmission, socket dispatch, QUIC, or live interoperability in this slice.

## Implementation handoff
Source implementation is present in `cmake/options.cmake`,
`src/include/protocol/Protocols.h`, `src/NatServerHolePunch.h`,
`unittests/tests/CMakeLists.txt`, and `unittests/tests/NatServerHolePunchTest.cpp`.
Tasks remain unchecked pending parent validation. ServerConnect.cpp and
ClientTags.h are unchanged; no partial advertisement was added.

## Validation and ownership
Strict TDD was not activated. Parent explicitly owns compilation and execution;
this writer must not create generated binaries. Focused test target:
`NatServerHolePunchTest` (C++17, standard library only).
Default builds retain unchanged login and network behavior. The test target
explicitly enables the codec gate, even when the application gate is OFF.
No compile, test execution, formatter, or generated binary was run/created by
this writer. C/C++ editor diagnostics reported clean; CMake analysis was
unavailable, so these are not execution evidence. Parent must compile the
standalone test with `ENABLE_NATT_SERVER_COORDINATION` defined (the CMake test
target supplies it), run it, and check the OFF configuration separately.
No commit or push. Unrelated `.gitignore` changes were preserved.
