# aMule External Connections Protocol — v2.0

> follow the white rabbit

## Preface

EC is under heavy construction; the protocol itself is considered stable
and you can rely on it, but opcodes, tagnames, tag content formats, and
values are still changing. If you decide to implement an application
using aMule EC, take the values from
[`ECCodes.abstract`](../src/libs/ec/abstracts/ECCodes.abstract) (the
build generates `ECCodes.h` from it), check this document
often, or read the source itself ([`src/ExternalConn.cpp`](../src/ExternalConn.cpp)
is a good start).


## Section 1 — Protocol definition

The EC protocol consists of two layers: a low-level **transmission
layer**, and a high-level **application layer**.


### Section 1.1 — Transmission layer

The transmission layer holds only transport information. Every packet
starts with an 8-byte header. Both fields are `uint32` in network byte
order (MSB first):

```c
[uint32]  FLAGS
[uint32]  LENGTH
          <application-layer data, LENGTH bytes>
```

* **FLAGS** tell how this packet is encoded. Each packet has its own
  flags, so two packets on one connection can differ.
* **LENGTH** is the number of bytes after the header, as sent: after
  compression and encryption, if either applies.

#### Bit description

| Bit(s)            | Name                       | Meaning |
| ----------------- | -------------------------- | ------- |
| `0`               | `EC_FLAG_ZLIB`             | The application-layer data is zlib-compressed. |
| `1`               | `EC_FLAG_UTF8_NUMBERS`     | The numbers of the packet and tag structure are compressed. See [Compressed numbers](#compressed-numbers). |
| `2`               | Unused                     | Once meant for a packet ID, which aMule never implemented. A receiver ignores the bit and reads no ID, so do not set it. |
| `3`               | `EC_FLAG_ENCRYPTED`        | The data is sealed with the negotiated AEAD: ciphertext, then a 16-byte authentication tag. LENGTH covers both. The bit appears only after both sides negotiated a cipher. See [Section 1.3](#section-13--transport-encryption). |
| `4`               | `EC_FLAG_LARGE_TAG_COUNT`  | The sender uses the sentinel-extended `TAGCOUNT` (see [Section 1.2](#section-12--application-layer)). The bit appears only after both sides advertised `EC_TAG_CAN_LARGE_TAG_COUNT` during authentication. Without it, a tag has at most 0xFFFE children. |
| `5`               | Always 1                   | |
| `6`               | Always 0                   | |
| `7`, `15`, `23`   | Unused                     | Set to 0. |
| `8`–`14`, `16`–`22`, `24`–`31` | Reserved      | Set to 0. |

With no options set, the flags are `0x00000020`.

A receiver drops the connection when bit 5 is not 1, bit 6 is not 0, or
a reserved bit is set. Peers older than transport encryption reject
bit 3 the same way, because it was reserved then. That is deliberate: a
peer that cannot decrypt fails closed instead of reading ciphertext as
tags.

A sender sets `EC_FLAG_ZLIB`, `EC_FLAG_UTF8_NUMBERS` or
`EC_FLAG_LARGE_TAG_COUNT` only when the peer advertised the matching
`EC_TAG_CAN_*` tag during authentication (see
[Section 3](#section-3--clarifying-things)). A client that advertises
none of them gets `0x20` on every packet. aMule uses zlib for large
packets and compressed numbers for the others. It does not set both on
one packet.

#### Compressed numbers

With `EC_FLAG_UTF8_NUMBERS`, each number of the packet and tag structure
(`OPCODE`, `TAGCOUNT`, `TAGNAME`, `TAGTYPE` and `TAGLEN`, see
[Section 1.2](#section-12--application-layer)) is encoded the way UTF-8
encodes a character code: 1 byte for values below 0x80, 2 bytes below
0x800, and so on. For example, the tag name `0x0200` becomes `c8 80`.

Tag data does not change: a `uint32` tag still holds 4 bytes, MSB
first. `TAGLEN` also keeps its uncompressed value; see
[Section 1.2](#section-12--application-layer).


### Section 1.2 — Application layer

Data transmission is done in **packets**. A packet is a special tag —
no data of its own, no tag-length field, but always with a `tagCount`
field. All numbers in the application layer are transmitted in **network
byte order** (MSB first), unless compressed numbers apply (see
[Section 1.1](#compressed-numbers)).

A packet contains:

```c
[ec_opcode_t]   OPCODE
[uint16]        TAGCOUNT
<[uint32]       EXTENDED_TAGCOUNT>?
                <tags>
```

* **OPCODE** indicates the operation or what the data fields contain.
  Type `ec_opcode_t`, currently `uint8`.
* **TAGCOUNT** is the number of first-level tags in this packet,
  followed by the tags themselves.
* **EXTENDED_TAGCOUNT** (optional, only when `EC_FLAG_LARGE_TAG_COUNT`
  is in effect, see Section 1.1): if `TAGCOUNT == 0xFFFF`, a `uint32`
  follows carrying the actual count. This sentinel-extended encoding
  lifts the historical 65535-tag ceiling so that responses for large
  shared-file libraries (etc.) can carry their full size. Senders
  emit the `0xFFFF` marker only for `count >= 0xFFFF`, and only when
  the receiver has advertised `EC_TAG_CAN_LARGE_TAG_COUNT` in the
  auth handshake. Otherwise `TAGCOUNT` is a plain `uint16` and counts
  larger than `0xFFFE` are silently truncated to `0xFFFE` to avoid
  ambiguity (the value `0xFFFF` is reserved as the sentinel).

A tag contains:

```c
[ec_tagname_t]  TAGNAME
[ec_tagtype_t]  TAGTYPE
[ec_taglen_t]   TAGLEN
<[uint16]       TAGCOUNT>?
                <sub-tags>
                <tag data>
```

* `ec_tagname_t` is `uint16`, `ec_tagtype_t` is `uint8`, `ec_taglen_t`
  is `uint32` (current values; subject to change).
* **TAGNAME** is the tag code shifted left by one bit. The lowest bit
  tells whether the tag has sub-tags (see below). Shift `TAGNAME` right
  by one bit to get the code, as listed in
  [`ECCodes.abstract`](../src/libs/ec/abstracts/ECCodes.abstract).
* **TAGTYPE** identifies the data type of this tag. See
  [Section 2](#section-2--data-types) and
  [`ECTagTypes.abstract`](../src/libs/ec/abstracts/ECTagTypes.abstract).
* **TAGLEN** is the length of the tag's own data, plus the full size of
  each sub-tag: 7 bytes of header (`TAGNAME`, `TAGTYPE`, `TAGLEN`), 2
  more for its `TAGCOUNT` if it has sub-tags (6 if it uses the extended
  count), and its own `TAGLEN`. It excludes this tag's own header and
  `TAGCOUNT`.

`TAGLEN` counts the uncompressed sizes. With compressed numbers the
header fields of a sub-tag are shorter on the wire, but `TAGLEN` still
counts 7 or 9 bytes for them. So do not use `TAGLEN` to skip bytes in
the stream. Read the sub-tags, then read the tag's own data: its length
is `TAGLEN` minus the sizes of all sub-tags.

Tags may contain sub-tags. A `TAGCOUNT` field is present only when the
lowest bit of `TAGNAME` is set. The sub-tags come before the tag's own
data.

When `EC_FLAG_LARGE_TAG_COUNT` is in effect, the sub-tag `TAGCOUNT`
field uses the same sentinel-extended encoding as the packet-level
`TAGCOUNT` (a `uint32` follows when the `uint16` reads `0xFFFF`).


### Section 1.3 — Transport encryption

All authenticated EC traffic must be encrypted. It is negotiated during the
auth exchange, applies to the application layer only — the 8-byte transmission
header stays in clear, because the receiver needs the flags and length to know
a sealed body is coming — and is signalled per packet by `EC_FLAG_ENCRYPTED`.

**Negotiation.** The client sends `EC_TAG_CAN_AEAD`, whose data is the list of
cipher ids it supports in its own preference order, together with 32 random
bytes in `EC_TAG_AEAD_CLIENT_NONCE` and a 32-byte ephemeral X25519 public key in
`EC_TAG_AEAD_CLIENT_PUBKEY`. The server answers in `EC_OP_AUTH_SALT` with the
chosen id in `EC_TAG_AEAD_CIPHER`, 32 random bytes of its own in
`EC_TAG_AEAD_SERVER_NONCE`, and its own ephemeral public key in
`EC_TAG_AEAD_SERVER_PUBKEY`. A server that omits these tags does not support
encryption; authentication fails with no plaintext fallback.

The public key is not optional. A peer that offers `EC_TAG_CAN_AEAD` without one
is malformed, not old — encryption and the key exchange shipped together — and
the offer is refused rather than answered with a weaker derivation.

| id | cipher | preferred |
| -- | ------ | -- |
| `1` | AES-128-GCM (mandatory) | when both sides have hardware support for AES |
| `2` | ChaCha20-Poly1305 (optional) | when both sides have ChaCha20 and at least one lacks hardware support for AES |

Note that Crypto++ (as of 8.9.0) only detects hardware AES on x86 and on Linux
ARM. On macOS and Windows ARM builds the check comes up empty even where the
CPU has the instructions, so those peers offer ChaCha20 first and the channel
settles on it. That is the right outcome while it lasts: the same flag decides
whether Crypto++ itself uses the AES instructions, so a peer that preferred AES
there would get the table-based implementation, which is slower than ChaCha20
and not constant-time.

**Keys.** Both sides derive from the X25519 shared secret, and from nothing
else. In particular *not* from the password: a key derived from something that
outlives the session means a password learned later decrypts a recording made
earlier. The ephemeral private keys are discarded as soon as the secret exists,
so once a session ends there is nothing left that could reopen it.

```
transcript = len(offered) || offered || cipher_id
             || client_nonce || server_nonce
             || client_pubkey || server_pubkey
salt = server_nonce || client_nonce
info = "aMule EC AEAD v1" || cipher_id || transcript
okm  = HKDF-SHA256(X25519(client_pubkey, server_pubkey), salt, info, 2*keylen + 8)
```

`okm` splits into a client-to-server key, a server-to-client key, and a 4-byte
nonce prefix for each direction. Keys are per direction so the two packet
counters cannot produce a colliding nonce.

Including the offered list and the chosen id in `info` binds the handshake: if
either is altered in transit the two sides derive different keys and the first
sealed packet fails to authenticate, instead of the session silently dropping
to a weaker cipher.

**Authentication.** SRP-6a proofs bind the credential to this transcript.
The daemon retains an augmented verifier instead of a password-equivalent MD5
digest. The exact v1 profile, byte encodings, proof context, messages and config
migration are specified in [EC_SRP.md](EC_SRP.md).

**Per-packet nonce.** 12 bytes: the 4-byte derived prefix followed by a 64-bit
big-endian counter, starting at zero and incremented once per packet in that
direction. The counter is *not* transmitted — the stream is ordered and any
desync is already fatal — so a sealed body costs exactly 16 bytes more than its
plaintext.

**Ordering.** Serialise, then compress if `EC_FLAG_ZLIB` is set, then seal. The
receiver reverses it. A failed authentication tag is not recoverable and the
connection is dropped.

**When it starts.** The client sends `EC_OP_AUTH_REQ` and the daemon answers
`EC_OP_AUTH_SALT` in plaintext. `EC_OP_AUTH_PASSWD`, `EC_OP_AUTH_OK` and failures
after the salt are sealed. An encrypted channel is mandatory, including on
loopback; the former encryption opt-outs are removed. No application command is
sent or executed until both SRP proofs have been checked.

## Section 2 — Data types

### Tag types

| `TAGTYPE` | Name                  | Data |
| --------- | --------------------- | ---- |
| `1`       | `EC_TAGTYPE_CUSTOM`   | Raw bytes; the tag decides what they mean. An empty tag, such as a capability, has this type and length 0. |
| `2`       | `EC_TAGTYPE_UINT8`    | 1-byte integer. |
| `3`       | `EC_TAGTYPE_UINT16`   | 2-byte integer. |
| `4`       | `EC_TAGTYPE_UINT32`   | 4-byte integer. |
| `5`       | `EC_TAGTYPE_UINT64`   | 8-byte integer. |
| `6`       | `EC_TAGTYPE_STRING`   | String, see below. |
| `7`       | `EC_TAGTYPE_DOUBLE`   | Floating-point number, see below. |
| `8`       | `EC_TAGTYPE_IPV4`     | 4 bytes of IPv4 address in network order, then a 2-byte port. |
| `9`       | `EC_TAGTYPE_HASH16`   | 16-byte hash, such as an MD4 file hash. |
| `10`      | `EC_TAGTYPE_UINT128`  | 16-byte integer, such as a Kad ID. |

### Integer types

Integer types (`uint8`, `uint16`, `uint32`, …) are always transmitted
in network byte order (MSB first).

aMule sends an integer tag with the smallest type that holds its value.
So the same tag can arrive as `EC_TAGTYPE_UINT8` in one packet and as
`EC_TAGTYPE_UINT32` in the next. A reader must accept any integer type
for an integer tag.

### Strings

Strings are always **UTF-8**, including the trailing zero byte. All
strings coming from the server are untranslated, but their translations
are included in aMule's translation database (`amule.mo`).

### Boolean

This one is tricky:

* **When reading**, the tag's *presence* means `true`; *absence* means
  `false`.
* **When writing**, booleans should always be present — if absent the
  receiver treats it as *unchanged*. The tag must hold a `uint8`: `0`
  is `false`, non-zero is `true`.

Boolean values are mostly used in reading/writing preferences.

### MD5 hashes

Always MSB first.

### Floating-point numbers

`float` and `double` types are converted to their *string*
representation and sent as strings. The decimal point is always `.`
(dot), independent of the current locale.


## Section 3 — Clarifying things

If the above seemed too technical, keep reading. If you understood it
on first read, you can safely skip this section.

Have you seen an XML file? Then think of an EC packet as binary XML.
Otherwise, think of it as a tree: exactly one root, possibly many
branches and leaves. We'll use the tree analogy below.

The examples below show the decrypted application layer. On the wire, every
authenticated packet has `EC_FLAG_ENCRYPTED` and a sealed body as described in
Section 1.3. The 3.1.0 plaintext authentication example has been retired.

### Example 1 — Authentication

Implement the four-message exchange in [EC_SRP.md](EC_SRP.md). Clients must
advertise X25519/AEAD, send A and the credential selector, check the fixed KDF
profile, and verify the sealed server proof before sending any command.
Protocol/snapshot-version checks remain in effect. After repeated failures
from one address, the daemon temporarily refuses further logins.

`EC_TAG_PROTOCOL_VERSION` must be equal to the server's
`EC_CURRENT_PROTOCOL_VERSION`, currently `0x0204`. If not, the server
refuses the login.

`EC_TAG_VERSION_ID` is a 16-byte hash that binds a development snapshot
to the same snapshot on the other side. Only a snapshot built with a
version ID requires it. A release refuses any client that sends it, so
leave it out.

Each `EC_TAG_CAN_*` is an empty tag advertising support for one
extension. They fall into two groups, and the group decides how the
server confirms the capability — which in turn decides what a client may
safely do when the confirmation is absent.

**Wire-format capabilities** change how bytes are framed:
`EC_TAG_CAN_ZLIB`, `EC_TAG_CAN_UTF8_NUMBERS`,
`EC_TAG_CAN_LARGE_TAG_COUNT`. The server may set the matching flag
(`EC_FLAG_ZLIB`, `EC_FLAG_UTF8_NUMBERS`, `EC_FLAG_LARGE_TAG_COUNT`) only
when both sides advertised the capability; a client that omits the tag
gets the historical wire format for that feature. For `ZLIB` and
`UTF8_NUMBERS` the flag appearing on a later packet *is* the
confirmation — the server does not echo those two tags.
`LARGE_TAG_COUNT` is both echoed and flagged.

**Feature capabilities** gate whole operations rather than the framing:
`EC_TAG_CAN_PARTIAL_UPDATE`, `EC_TAG_CAN_MULTI_SEARCH`,
`EC_TAG_CAN_CHAT`, `EC_TAG_CAN_CHAT_SESSIONS`,
`EC_TAG_CAN_SHAREDDIRS_CONFIG`, `EC_TAG_CAN_SEARCH_LIST`,
`EC_TAG_CAN_SEARCH_PROGRESS_UNION`, `EC_TAG_CAN_SEARCH_ALL`,
`EC_TAG_CAN_DELETE_COMPLETE`. The server
echoes each of these in its `EC_OP_AUTH_OK` response when it supports it,
so the client learns what is negotiated for this connection.

For a feature capability the echo is **load-bearing, not informational**.
A client that does not see one echoed must not send the operations it
gates: an opcode the server does not know reaches the unknown-opcode
branch of its request dispatcher, which asserts before it can answer
`EC_OP_FAILED`. Asking an older server takes it down rather than
receiving a polite refusal.

With `EC_TAG_CAN_MULTI_SEARCH`, a search request that names no
`EC_TAG_SEARCH_ID` (results, progress without the union, stop, more)
addresses the last search this connection started. A connection that has
not started one gets the most recent search any client started. A peer
browse is never that default.

`EC_TAG_CAN_SEARCH_PROGRESS_UNION` changes the reply shape of
`EC_OP_SEARCH_PROGRESS`, so it is advertised only alongside
`EC_TAG_CAN_MULTI_SEARCH` — a single-search client has one search and no
use for the union. Once negotiated, **every** `EC_OP_SEARCH_PROGRESS` from
that connection answers in the union shape: one child entry per search,
keyed by `EC_TAG_SEARCH_ID`, whose children are the same tags the per-id
form puts at the top level.

A client should name the searches it is tracking, one `EC_TAG_SEARCH_ID`
per search. That narrows which searches come back and makes the daemon
refresh exactly those in its search LRU, as a per-id poll did; naming none
reports everything the daemon holds. Naming ids does **not** opt back into
the single-search reply — the shape is fixed by the capability, not by the
request. A daemon that keyed the union off "no ids named" would answer a
client that named several about only the first, and the client would read
every other search's absence as an expiry.

Each entry also carries `EC_TAG_SEARCH_NAME`, the query the search was
started with — or, for a browse, the peer's nickname. It is the same tag
and the same string `EC_OP_SEARCH_LIST` reports, so a client polling
progress can label what it is reporting on without also fetching the list.

The union reply carries no `EC_TAG_SEARCH_EXPIRED`: it reports the whole
set, so an id the client asked about and did not get back is one the
daemon no longer holds.

`EC_TAG_CAN_NOTIFY` is the one exception to both patterns: the server
records it and simply pushes notifications or does not, so there is
neither an echo nor a flag to observe.

**A missing echo means "do not send", not "send and see".** An older
daemon has no handler for an operation it predates, so the request
reaches the unknown-opcode path, which logs
`External Connection: invalid opcode received: 0x...`, asserts on
debug builds, and answers `EC_OP_FAILED`. Clients must therefore check
the echo before sending a gated operation and fall back to the older
behaviour when it is absent — for `EC_TAG_CAN_SEARCH_LIST`, for
instance, listing only the searches the client started itself.

Any new operation added to this protocol needs a capability tag of its
own, advertised by the client, echoed by the server and checked before
use. Bumping `EC_CURRENT_PROTOCOL_VERSION` is *not* the mechanism for
this: that constant gates the handshake as a whole, so raising it
severs every mixed-version pairing instead of degrading one feature.

The first packet must be `EC_OP_AUTH_REQ`; other initial opcodes receive
`EC_OP_AUTH_FAIL`. AUTH_REQ must include the SRP and AEAD tags listed in
[EC_SRP.md](EC_SRP.md), in addition to the protocol and optional client fields.

After verifying M1, the server sends a sealed `EC_OP_AUTH_OK` containing
`EC_TAG_SRP_M2`, `EC_TAG_SERVER_VERSION`, accepted capability tags, and
`EC_TAG_SESSION_ID` (a uint64 identifying this daemon process). A changed
session ID means cached ECIDs from the previous process must be discarded.
The version string identifies a release or a development snapshot. Parse tags
by name; do not assume their count or order. Require a valid M2 before using
any of these fields or sending commands.

### Example 2 — Simple stats request

```
EC_OP_STAT_REQ (0a)
    +-- EC_TAG_DETAIL_LEVEL (0004), value EC_DETAIL_CMD (00)
```

```
00 00 00 20                       FLAGS
00 00 00 0b                       LENGTH: 11
0a                                EC_OP_STAT_REQ
  00 01                           TAGCOUNT: 1
    00 08                         EC_TAG_DETAIL_LEVEL (0004 << 1)
      02                          EC_TAGTYPE_UINT8
      00 00 00 01                 TAGLEN: 1
      00                          EC_DETAIL_CMD
```

The reply, from a daemon that is not connected to any network:

```
EC_OP_STATS (0c)
    +-- EC_TAG_STATS_UL_SPEED          (0200)
    +-- EC_TAG_STATS_DL_SPEED          (0201)
    +-- EC_TAG_STATS_UL_SPEED_LIMIT    (0202)
    +-- EC_TAG_STATS_DL_SPEED_LIMIT    (0203)
    +-- EC_TAG_STATS_UL_QUEUE_LEN      (0208)
    +-- EC_TAG_STATS_TOTAL_SRC_COUNT   (0206)
    +-- EC_TAG_STATS_ED2K_USERS        (0209)
    +-- EC_TAG_STATS_KAD_USERS         (020a)
    +-- EC_TAG_STATS_ED2K_FILES        (020b)
    +-- EC_TAG_STATS_KAD_FILES         (020c)
    +-- EC_TAG_STATS_KAD_NODES         (021b)
    +-- EC_TAG_CONNSTATE               (0005)
        +-- EC_TAG_CLIENT_ID           (000a)
```

A connected daemon sends more: Kad statistics, and the server,
`EC_TAG_ED2K_ID` and more inside `EC_TAG_CONNSTATE`.

```
00 00 00 20                       FLAGS
00 00 00 6d                       LENGTH: 109
0c                                EC_OP_STATS
  00 0c                           TAGCOUNT: 12
    04 00 02 00 00 00 01 00       EC_TAG_STATS_UL_SPEED (0200 << 1),
                                  EC_TAGTYPE_UINT8, TAGLEN 1, value 0
    04 02 02 00 00 00 01 00       EC_TAG_STATS_DL_SPEED, the same
    [...]                         9 more tags of the same shape
    00 0b                         EC_TAG_CONNSTATE (0005 << 1 | 1):
                                  the lowest bit is set, so the tag
                                  has sub-tags and a TAGCOUNT
      02                          EC_TAGTYPE_UINT8
      00 00 00 09                 TAGLEN: 9 = sub-tag header 7
                                  + sub-tag data 1 + own data 1
      00 01                       TAGCOUNT: 1, not counted in TAGLEN
        00 14                     EC_TAG_CLIENT_ID (000a << 1)
          02                      EC_TAGTYPE_UINT8
          00 00 00 01             TAGLEN: 1
          00                      0: no ID yet
      08                          EC_TAG_CONNSTATE's own data, after
                                  its sub-tags
```

Every value here is 0, so every integer goes as `EC_TAGTYPE_UINT8`. On a
busy daemon, the same tags arrive as wider types (see
[Section 2](#integer-types)).

The value of `EC_TAG_CONNSTATE` is a set of bits: `01` eD2k connected,
`02` eD2k connecting, `04` Kad connected, `08` Kad firewalled, `10` Kad
running. `EC_OP_GET_CONNSTATE` returns the same tag in an
`EC_OP_MISC_DATA` packet.

### Example 3 - Compressed numbers

`amulecmd` advertises `EC_TAG_CAN_UTF8_NUMBERS` and
`EC_TAG_CAN_LARGE_TAG_COUNT`. After it logs in, the same request looks
like this:

```
00 00 00 32                       FLAGS: 20 + EC_FLAG_LARGE_TAG_COUNT (10)
                                  + EC_FLAG_UTF8_NUMBERS (02)
00 00 00 06                       LENGTH: 6. The header is not compressed.
0a                                EC_OP_STAT_REQ
  01                              TAGCOUNT: 1
    08                            EC_TAG_DETAIL_LEVEL (0004 << 1)
      02                          EC_TAGTYPE_UINT8
      01                          TAGLEN: 1
      00                          EC_DETAIL_CMD. Tag data is not compressed.
```

In the reply, `EC_TAG_CONNSTATE` is `0b 02 09 01 14 02 01 00 08`. The
sub-tag takes 4 bytes on the wire, but `TAGLEN` is still 9: it counts
the sub-tag header as 7 bytes.

Hopefully these examples clarified opcodes, tags, and nested tags.


## Section 4 — Notable tag types

This section documents the data types of selected tags where the type
isn't immediately obvious or has changed across protocol versions.

### Chat (`EC_TAG_CHAT = 0x0900`)

Peer chat is served from a session store in the core, shared by the
built-in GUI and every EC client, so all of them see one transcript.

Gated by `EC_TAG_CAN_CHAT_SESSIONS` (`0x27`), which is **not** the older
`EC_TAG_CAN_CHAT` (`0x16`). The two are deliberately distinct: `0x16`
predates these operations and is echoed by servers that implement none
of them, so a client gating on it would send `EC_OP_GET_CHAT_SESSIONS`
to a server whose dispatcher only knows how to assert on it. A client
must send none of the operations below unless it saw `0x27` echoed.

| Tag                     | Code     | Type     | Description |
| ----------------------- | -------- | -------- | ----------- |
| `EC_TAG_CHAT`           | `0x0900` | `string` | Message text |
| `EC_TAG_CHAT_CLIENT_ID` | `0x0901` | `uint64` | Peer GUI_ID, `(ip << 16) \| port` |
| `EC_TAG_CHAT_SESSION`   | `0x0902` | `uint64` | Session container; value is the GUI_ID |
| `EC_TAG_CHAT_MESSAGE`   | `0x0903` | `string` | Message container; value is the text |
| `EC_TAG_CHAT_MSG_ID`    | `0x0904` | `uint32` | Monotonic message id, also used as a resume cursor |
| `EC_TAG_CHAT_DIRECTION` | `0x0905` | `uint8`  | `0` = incoming, `1` = outgoing |
| `EC_TAG_CHAT_TIMESTAMP` | `0x0906` | `uint32` | Unix seconds, stamped by the core |
| `EC_TAG_CHAT_PEER_NAME` | `0x0907` | `string` | Peer display name; may be empty |
| `EC_TAG_CHAT_PEER_HASH` | `0x0908` | `CMD4Hash` | Peer's stable identity; omitted while the peer is still provisional |

The IP inside a GUI_ID uses the same byte order as
`EC_TAG_CLIENT_USER_IP`.

`EC_TAG_CHAT_PEER_HASH` is additive: a daemon that omits it from a session
predates this tag. It is the only way to address a peer a GUI_ID cannot
express -- an IPv6 route, a provisional session, or an endpoint two
identified peers share -- so a client should prefer it over the GUI_ID
whenever a session carries one.

Gated by its own `EC_TAG_CAN_CHAT_PEER_HASH` (`0x28`), distinct from
`EC_TAG_CAN_CHAT_SESSIONS`: a client that predates the hash tag would
merge two sessions that share one GUI_ID under a single legacy id, so
the daemon includes such a session -- and the hash tag on any session --
only for a connection that advertised it. A client that never saw `0x28`
echoed must not send `EC_TAG_CHAT_PEER_HASH` either; it addresses and
lists chat sessions by GUI_ID only, exactly as a build that predates the
tag would.

Such a client sees conversations the way 3.1.0 kept them: one per IPv4
route. The daemon keys a session by the peer's hash and follows the peer
across routes, so for these connections it lists one view per route the
session exchanged messages on, each holding only that route's messages.
A peer that moves from `a:p` to `b:q` therefore keeps its `a:p`
conversation and gains a `b:q` one. For such a connection,
`EC_OP_GET_CHAT_MESSAGES`, `EC_OP_CHAT_SEND` and
`EC_OP_CHAT_CLOSE_SESSION` act on the view its GUI_ID names: a reply
sent under `a:p` is filed there even though it reaches the peer on
`b:q`, and closing `a:p` drops only that view.

#### `EC_OP_GET_CHAT_SESSIONS` (`0x63`) → `EC_OP_CHAT_SESSIONS` (`0x64`)

The polling workhorse: one roundtrip returns the session list *and*
every message newer than the client's cursor, so an idle connection
costs one small packet and a busy one needs no follow-up query.

**Request:** optional `EC_TAG_CHAT_MSG_ID` — the highest id the client
already holds. Absent or `0` means "everything you still have".

**Reply:** a top-level `EC_TAG_CHAT_MSG_ID` carrying the store's current
last id, then one `EC_TAG_CHAT_SESSION` per session. Each session
container carries `EC_TAG_CHAT_PEER_NAME`, its own
`EC_TAG_CHAT_MSG_ID`, an `EC_TAG_CLIENT` when the peer is online, an
`EC_TAG_FRIEND` when the peer is a friend, and one
`EC_TAG_CHAT_MESSAGE` per message past the cursor.

The top-level cursor is present even when no messages come back, so a
client can advance past ids that were evicted rather than requesting
them forever.

The reply is the server's complete session set **for this connection's
capabilities**: a session with no unique GUI_ID (an IPv6 route, a
provisional session, or an endpoint shared with another peer) is included,
with its `EC_TAG_CHAT_PEER_HASH`, only once the connection advertised
`EC_TAG_CAN_CHAT_PEER_HASH`; otherwise it is omitted exactly as it always
was.

A session the client is tracking that is absent from the reply was
closed — by another client, or by eviction — which is the only signal a
close produces. No expiry tag is needed, and a client must drop such a
session rather than assume it still exists.

#### `EC_OP_GET_CHAT_MESSAGES` (`0x5B`) → `EC_OP_CHAT_MESSAGES` (`0x5C`)

Non-destructive backfill of **one** session, for a client opening a
conversation it has no transcript for. Takes `EC_TAG_CHAT_PEER_HASH` or
`EC_TAG_CHAT_CLIENT_ID` and an optional `EC_TAG_CHAT_MSG_ID` cursor, and
replies with the same shape as above containing a single session
container. `EC_OP_FAILED` when neither target tag is present or there is
no such session.

#### `EC_OP_CHAT_SEND` (`0x65`)

Takes `EC_TAG_CHAT` (the text, non-empty) plus one or both target tags:

| Target tag              | Addresses |
| ----------------------- | --------- |
| `EC_TAG_CHAT_PEER_HASH` | A peer by its stable identity — the only target that reaches an IPv6 route, a provisional session, or a peer sharing an endpoint with another. Prefer this whenever the session carries a hash. |
| `EC_TAG_CHAT_CLIENT_ID` | A GUI_ID — kept for clients that predate the hash tag; refused alone when the GUI_ID is ambiguous or unprojectable |

When both are present, the hash is the identity and the GUI_ID is a dial
hint: it gives the daemon a route for a peer it has no session or live
client for yet, e.g. an offline friend the sender only knows by address
and hash. Sent together, an IPv4 GUI_ID is never discarded just because
a hash also identified the target.

The server creates the session when it does not exist, so this doubles
as "start a chat with this address" for a `EC_TAG_CHAT_CLIENT_ID` target.
A `EC_TAG_CHAT_PEER_HASH` target with no prior session and no usable
GUI_ID dial hint answers `EC_OP_FAILED`: nothing gives the daemon
somewhere to dial.

**Reply:** `EC_OP_NOOP` with `EC_TAG_CHAT_CLIENT_ID` (`0` when the
session's route is not IPv4-projectable), `EC_TAG_CHAT_PEER_HASH` when
the peer has one and this connection advertised
`EC_TAG_CAN_CHAT_PEER_HASH`, and `EC_TAG_CHAT_MSG_ID` (the id assigned),
so the sender can correlate without waiting for the next poll.
`EC_OP_FAILED` with an `EC_TAG_STRING` on an unknown target or empty
text.

Note that the core's own send returning `false` means *queued while
connecting*, not *failed*, and does not produce an `EC_OP_FAILED`.

#### `EC_OP_CHAT_CLOSE_SESSION` (`0x66`)

Takes `EC_TAG_CHAT_PEER_HASH` or `EC_TAG_CHAT_CLIENT_ID`; drops the
session from the store and resets the peer's chat state. Replies
`EC_OP_NOOP`, or `EC_OP_FAILED` when neither tag is present or there is
no such session.

Closing is **global**, matching the semantics search tabs already have:
the core state is destroyed for every client, and the others learn of it
from the session's absence in the next `EC_OP_CHAT_SESSIONS` reply.

From a client without `EC_TAG_CAN_CHAT_PEER_HASH`, a GUI_ID closes one
route's view: its messages are dropped for every client, and the session
itself only once no route holds messages.

### Connection preferences (`EC_TAG_PREFS_CONNECTIONS = 0x1300`)

| Tag                                | Code     | Type     | Description |
| ---------------------------------- | -------- | -------- | ----------- |
| `EC_TAG_CONN_DL_CAP`               | `0x1301` | `uint32` | Download line capacity (KiB/s) |
| `EC_TAG_CONN_UL_CAP`               | `0x1302` | `uint32` | Upload line capacity (KiB/s) |
| `EC_TAG_CONN_MAX_DL`               | `0x1303` | `uint32` | Max download speed (KiB/s) |
| `EC_TAG_CONN_MAX_UL`               | `0x1304` | `uint32` | Max upload speed (KiB/s) |
| `EC_TAG_CONN_SLOT_ALLOCATION`      | `0x1305` | `uint32` | Upload slot allocation |
| `EC_TAG_CONN_MAX_FILE_SOURCES`     | `0x1309` | `uint16` | Max sources per file |
| `EC_TAG_CONN_MAX_CONN`             | `0x130A` | `uint16` | Max connections |
| `EC_TAG_CONN_ADDRESS_FAMILIES`     | `0x131A` | `uint32` | `AddressFamilies` setting: `0` = IPv4 only, `1` = IPv4 and IPv6 |
| `EC_TAG_CONN_ADVERTISE_IPV6`       | `0x131B` | `bool`   | `AdvertiseIPv6` setting |

> **Note**: `EC_TAG_CONN_MAX_DL`, `EC_TAG_CONN_MAX_UL`, and
> `EC_TAG_CONN_SLOT_ALLOCATION` were widened from `uint16` to `uint32`
> to support speeds above 65534 KiB/s (~537 Mbps) required on modern
> gigabit connections. EC clients reading these tags should use
> `GetInt()` (which handles any integer width); clients sending them
> should encode them as 32-bit values.

> **Note** (#1177): only a core built with `ENABLE_IPV6` emits these two
> tags. An absent `EC_TAG_CONN_ADDRESS_FAMILIES` therefore means an old
> daemon or one without `ENABLE_IPV6`, a third state distinct from `0`.
> amulegui then hides both controls and does not send either tag on
> `EC_OP_SET_PREFERENCES`.

### Peer vendor capabilities (`EC_TAG_CLIENT_MOD_CAPABILITIES = 0x0633`)

| Tag                              | Code     | Type     | Description |
| -------------------------------- | -------- | -------- | ----------- |
| `EC_TAG_CLIENT_MOD_CAPABILITIES` | `0x0633` | `uint32` | Peer's eMuleAI vendor capability bitfield |

A child of `EC_TAG_CLIENT`, carrying what the peer advertised in the
eD2k handshake tag `CT_MOD_MISCOPTIONS` (`0xAA`):

| Bit | Meaning |
| --- | ------- |
| 0 | Extended source exchange |
| 1 | Legacy uTP NAT traversal |
| 2 | IPv6 |
| 3 | Serving-buddy pull |
| 4 | QUIC NAT traversal |

Bits 5 and above are reserved. The core masks them off before sending,
so an EC client never has to know which bits are defined — a set bit it
does not recognise cannot reach it.

The tag is additive and follows the usual convention: a reply without it
means an older daemon, which is a **third state**, distinct from a peer
that advertised no capabilities (word `0`). aMule itself advertises
nothing here yet — it implements none of the five features — so the tag
describes the peer only.

### Combined network searches

`EC_SEARCH_ALL` (`5`) starts a global eD2k search and a Kad keyword search
under one public search ID. It uses whichever networks are available at
submission time; neither available is an error. A Kad keyword already in
use by another search is preserved. The new search cannot acquire that Kad
target; if eD2k is connected the new search continues there and logs why its
Kad component could not start. A query with no usable Kad keyword also
continues over eD2k when connected; it fails when Kad is the only available
network.

Combined result rows sum eD2k source reports and take the maximum Kad source
estimate, then display the larger of those two counts because the networks
can overlap. Summed eD2k counts saturate at the unsigned 32-bit maximum.

Only ALL results with a known split carry
`EC_TAG_SEARCHFILE_ED2K_SOURCES` (`0x0718`) and
`EC_TAG_SEARCHFILE_KAD_SOURCES` (`0x0719`) as an optional pair of unsigned
counts. An initial full result includes both halves, including zero counts.
Incremental updates use the value map and omit unchanged halves; clients retain
the last-known value of the omitted half. Without an initial pair the split is
unknown (older daemons or legacy ALL snapshots). Single-network results do not
carry this pair.
Existing aggregate source tags retain their meaning and determine sorting.
Clients display only nonzero network counts: `E:9`, `K:50`, or `E:9 K:50`.
Both zero produces `0`; an unknown breakdown displays the aggregate alone.
A Sources-cell tooltip explains estimated availability, network counts,
complete sources, direct client endpoints when available locally, and overlapping
network estimates. EC does not supply a direct-endpoint count, so remote clients
omit that detail rather than displaying zero.
New saved ALL results retain the split; single-network results do not save it.

For multi-search clients, replacement requests are validated before the existing
eD2k search is stopped. Rejected requests leave it running; standalone Kad searches
run alongside it. The legacy single-search sentinel still replaces its one bucket.

Clients must require `EC_TAG_CAN_SEARCH_ALL` (`0x0029`) in `EC_OP_AUTH_OK`
before submitting this type. Older daemons may interpret an unknown search
type as a local search. This capability is advertised unconditionally by
supporting daemons and requires no new framing or protocol version.

The public ID is used for results, stop, close, and "More". The internal Kad
ID is never exposed. Lifecycle state remains running until both components
finish; stopping or closing the public ID stops both. Successful multi-search
START replies, progress replies, and search-list entries include
`EC_TAG_SEARCH_KAD_ACTIVE` (`0x0717`, integer boolean), indicating whether
that ID still has an active Kad component, independently of the aggregate
lifecycle. Clients use it to gate "More"; its absence on older daemons is
unknown, with the existing Kad kind/state check as the fallback. A restored
All search retains results but has no running network components.

`EC_TAG_SEARCH_ED2K_ACTIVE` (`0x071A`, integer boolean) reports whether this
search still owns the active eD2k slot, independently of its Kad component.
It is included in successful multi-search START replies, progress replies,
and search-list entries. Clients use it to decide whether a new eD2k request
would interrupt existing work. Its
absence on older daemons means unknown; use the lifecycle as a fallback.

### Deleting complete files from disk

`EC_TAG_CAN_DELETE_COMPLETE` in `EC_OP_AUTH_OK` permits
`EC_OP_KNOWNFILE_DELETE`. Do not send this operation without the capability.
The request must contain `EC_TAG_KNOWNFILE` (MD4 hash), `EC_TAG_ECID` (uint32),
`EC_TAG_KNOWNFILE_PATH` (directory string), and `EC_TAG_PARTFILE_NAME` (basename
string), copied from the selected complete file. Paths are values to compare
with the core's record, never arbitrary paths to unlink.

The core rechecks identity, completeness, directory, and name before deleting.
If another shared file with a different hash now owns that path, the stale
completed-row target is rejected and the replacement is preserved.
Success returns `EC_OP_NOOP`, unshares the file, stops its uploads, and clears
its completed transfer row if present. The known-file record remains and the
hash is not added to `canceled.met`. An already missing file also succeeds.
Failure returns `EC_OP_FAILED` with `EC_TAG_STRING`, keeping sharing, uploads,
and the completed row intact. Client confirmation happens before sending the
request. `EC_OP_CLEAR_COMPLETED` remains the operation for clearing a row
without deleting its file.

`EC_OP_PARTFILE_DELETE` rejects completed/completing files, including stale cancel
requests from legacy clients. Optional `EC_TAG_ECID`, `EC_TAG_KNOWNFILE_PATH`, and
`EC_TAG_PARTFILE_NAME` children on each partfile hash are checked before cancellation.
The API sends these children so a renamed or replaced active target cannot be cancelled
using an outdated snapshot.
