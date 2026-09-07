# aMule External Connections Protocol — v2.0

> follow the white rabbit

## Preface

EC is under heavy construction; the protocol itself is considered stable
and you can rely on it, but opcodes, tagnames, tag content formats, and
values are still changing. If you decide to implement an application
using aMule EC, include `ECcodes.h` for the values, check this document
often, or read the source itself ([`src/ExternalConn.cpp`](../src/ExternalConn.cpp)
is a good start).


## Section 1 — Protocol definition

The EC protocol consists of two layers: a low-level **transmission
layer**, and a high-level **application layer**.


### Section 1.1 — Transmission layer

The transmission layer is completely independent of the application
layer and holds only transport-related information.

It consists of a single `uint32`, referenced below as **flags**, which
describes flags for the current send/receive operation. This is the
only value in the whole protocol that is transmitted **LSB first**, with
zero bytes omitted (an empty transmission flags value is sent as
`0x20`, not `0x20 0x00 0x00 0x00`).

#### Bit description

| Bit(s)            | Name                       | Meaning |
| ----------------- | -------------------------- | ------- |
| `0`               | Compression (`EC_FLAG_ZLIB`) | When set, zlib compression is applied to the application layer's data. |
| `1`               | Compressed numbers (`EC_FLAG_UTF8_NUMBERS`) | When set (presumably on small packets that aren't worth zlib-compressing), all numbers used in the protocol are encoded as a wide char converted to UTF-8 to avoid sending zero bytes. |
| `2`               | Has ID                     | When set, a `uint32` follows the flags — the packet ID. The response must echo the same ID. The only requirement is that IDs be unique within one session (or at least don't repeat for a reasonably long time). |
| `3`               | Encrypted (`EC_FLAG_ENCRYPTED`) | When set, the application-layer data is sealed with the negotiated AEAD: the body is ciphertext followed by a 16-byte authentication tag, and the transmission-layer length covers both. See Section 1.3. Both sides must have negotiated a cipher during authentication for the bit to appear. This bit was formerly reserved, so a peer predating the feature rejects such a packet outright rather than misparsing ciphertext as tags. |
| `4`               | Large tag count (`EC_FLAG_LARGE_TAG_COUNT`) | When set, indicates the sender uses the sentinel-extended `TAGCOUNT` encoding (see Section 1.2). Both sides must have advertised `EC_TAG_CAN_LARGE_TAG_COUNT` in their auth packet for the bit to appear in any subsequent flags. Without it, the historical 16-bit `TAGCOUNT` is used, capping any tag at 0xFFFE children for safe interoperation with old peers. |
| `5`               | Always 1                   | Distinguishes from older (pre-rc8) clients. |
| `6`               | Always 0                   | Distinguishes from older (pre-rc8) clients. |
| `7`, `15`, `23`   | Extension                  | Indicates that the next byte of flags is present. |
| `8`–`14`, `16`–`22`, `24`–`32` | Reserved      | Set to 0. |

#### Example

```
0x30 0x23 <appdata>
```

Client uses no extensions on this packet, and indicates that it can
accept zlib compression and compressed numbers.

#### Notes

* In the *accepts* value, the predefined flags (bits `5` and `6`) must
  be set to their predefined values — this can act as a sort of sanity
  check.
* Bits marked **Reserved** must always be set to 0.
* Bit `3` was reserved before transport encryption existed, and older peers
  reject any packet that sets it. That is deliberate: the failure is closed.


### Section 1.2 — Application layer

Data transmission is done in **packets**. A packet is a special tag —
no data of its own, no tag-length field, but always with a `tagCount`
field. All numbers in the application layer are transmitted in **network
byte order** (MSB first).

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
* **TAGNAME** identifies the tag content — see `ECcodes.h`.
* **TAGTYPE** identifies the data type of this tag — see `ECPacket.h`.
* **TAGLEN** is the total tag length, *including* sub-tag lengths but
  *excluding* the size of the `TAGNAME`, `TAGTYPE`, and `TAGLEN` fields
  themselves. The lowest bit of `TAGNAME` is **not** part of the name
  itself (see below) — clear it before comparing.

Tags may contain sub-tags. A `TAGCOUNT` field is present only when the
tag has sub-tags; presence is indicated by the **lowest bit of
`TAGNAME`** being set. When a tag contains sub-tags, the sub-tags are
sent before the tag's own data. Tag-data length can be calculated by
subtracting all sub-tags' total length from `TAGLEN`.

When `EC_FLAG_LARGE_TAG_COUNT` is in effect, the sub-tag `TAGCOUNT`
field uses the same sentinel-extended encoding as the packet-level
`TAGCOUNT` (a `uint32` follows when the `uint16` reads `0xFFFF`).


### Section 1.3 — Transport encryption

Everything after authentication may be encrypted. It is negotiated during the
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
encryption, and the session continues in clear.

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
info = "aMule EC AEAD v1" || cipher_id || <the offered cipher list, as received>
okm  = HKDF-SHA256(X25519(client_pubkey, server_pubkey), salt, info, 2*keylen + 8)
```

`okm` splits into a client-to-server key, a server-to-client key, and a 4-byte
nonce prefix for each direction. Keys are per direction so the two packet
counters cannot produce a colliding nonce.

Including the offered list and the chosen id in `info` binds the handshake: if
either is altered in transit the two sides derive different keys and the first
sealed packet fails to authenticate, instead of the session silently dropping
to a weaker cipher.

**Key confirmation.** An anonymous key exchange authenticates nobody: an
attacker can complete one exchange with each side and relay between them. Since
the password no longer keys the channel, it is what closes that instead, as an
explicit proof over the transcript.

```
client_confirm = HKDF-SHA256(md5(password), transcript, "ec-confirm-client", 32)
server_confirm = HKDF-SHA256(md5(password), transcript, "ec-confirm-server", 32)
```

The client sends `EC_TAG_AEAD_CLIENT_CONFIRM` with `EC_OP_AUTH_PASSWD`; the
server checks it — in constant time — before authenticating, and returns
`EC_TAG_AEAD_SERVER_CONFIRM` in the sealed `EC_OP_AUTH_OK`, which the client
checks in turn. A relay runs a different exchange on each leg, so the two
transcripts differ and at least one check fails.

A missing, malformed or mismatched tag fails authentication on either side.
There is no fallback to a password-derived key or to clear: a downgrade an
attacker could force by dropping one tag would be little better than no defence.
The transcript covers both public keys, so substituting one is caught here even
though the exchange itself would succeed.

**Per-packet nonce.** 12 bytes: the 4-byte derived prefix followed by a 64-bit
big-endian counter, starting at zero and incremented once per packet in that
direction. The counter is *not* transmitted — the stream is ordered and any
desync is already fatal — so a sealed body costs exactly 16 bytes more than its
plaintext.

**Ordering.** Serialise, then compress if `EC_FLAG_ZLIB` is set, then seal. The
receiver reverses it. A failed authentication tag is not recoverable and the
connection is dropped.

**When it starts.** The last plaintext packet from the client is
`EC_OP_AUTH_PASSWD`; the server replies with `EC_OP_AUTH_OK` already sealed,
carrying its confirmation tag. If authentication fails no session key is
installed, so `EC_OP_AUTH_FAIL` is sent in clear.

**Policy.** Whether to encrypt is the client's choice: only the client knows the
address it dialed, and a server's view of the peer address misclassifies
tunnelled connections. Every aMule client offers encryption by default. A server
may be configured to *require* it (`RequireEncryption`), in which case a session
that negotiated no cipher is refused at authentication time.

## Section 2 — Data types

### Integer types

Integer types (`uint8`, `uint16`, `uint32`, …) are always transmitted
in network byte order (MSB first).

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

About the flags (which are part of the transmission layer): when
developing an EC application, this is the last thing you want to care
about, and that's fine. Just keep sending `0x20` as flags, and aMule
will never want to use any of the extensions described in
[Section 1.1](#section-11--transmission-layer). You only have to
*tolerate* the *accepts* value aMule sends in its first reply.

The example packets below are real-life EC packets, transcribed to
textual form.

### Example 1 — Authentication

This is the very first packet you send, otherwise aMule may drop the
connection.

```
EC_OP_AUTH_REQ (0x02)
    +-- EC_TAG_CLIENT_NAME            (0x06) (optional)
    +-- EC_TAG_PASSWD_HASH            (0x04)
    +-- EC_TAG_PROTOCOL_VERSION       (0x0c)
    +-- EC_TAG_CLIENT_VERSION         (0x08) (optional)
    +-- EC_TAG_VERSION_ID             (0x0e) (required for CVS versions, must
                                              not be present for releases)
    +-- EC_TAG_CAN_ZLIB               (0x0c) (optional, advertises capability)
    +-- EC_TAG_CAN_UTF8_NUMBERS       (0x0d) (optional, advertises capability)
    +-- EC_TAG_CAN_NOTIFY             (0x0e) (optional, advertises capability)
    +-- EC_TAG_CAN_LARGE_TAG_COUNT    (0x11) (optional, advertises capability)
    +-- EC_TAG_CAN_PARTIAL_UPDATE     (0x12) (optional, advertises capability)
    +-- EC_TAG_CAN_MULTI_SEARCH       (0x15) (optional, advertises capability)
    +-- EC_TAG_CAN_CHAT               (0x16) (optional, advertises capability)
    +-- EC_TAG_CAN_SHAREDDIRS_CONFIG  (0x17) (optional, advertises capability)
    +-- EC_TAG_CAN_SEARCH_LIST        (0x1a) (optional, advertises capability)
    +-- EC_TAG_CAN_CHAT_SESSIONS      (0x27) (optional, advertises capability)
```

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
`EC_TAG_CAN_SEARCH_PROGRESS_UNION`. The server
echoes each of these in its `EC_OP_AUTH_OK` response when it supports it,
so the client learns what is negotiated for this connection.

For a feature capability the echo is **load-bearing, not informational**.
A client that does not see one echoed must not send the operations it
gates: an opcode the server does not know reaches the unknown-opcode
branch of its request dispatcher, which asserts before it can answer
`EC_OP_FAILED`. Asking an older server takes it down rather than
receiving a polite refusal.

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

What gets transmitted (all numbers hexadecimal, the `0x` prefix omitted
for readability):

```
20                                FLAGS — using ECv2
02                                EC_OP_AUTH_REQ
  00 05                           Number of children (tags)
    00 06                         EC_TAG_CLIENT_NAME
      0?                          EC_TAGTYPE_STRING
      00 00 00 09                 Length 9
      61 4d 75 6c 65 63 6d 64 00  "aMulecmd" + trailing zero
    00 08                         EC_TAG_CLIENT_VERSION
      0?                          EC_TAGTYPE_STRING
      00 00 00 04                 Length 4
      43 56 53 00                 "CVS"
    00 0c                         EC_TAG_PROTOCOL_VERSION
      0?                          EC_TAGTYPE_UINT??
      00 00 00 02/4/8             Length 2/4/8 (16/32/64-bit value follows)
      00? 00? 01 f2               0x0200 (current protocol version for CVS)
    00 04                         EC_TAG_PASSWD_HASH
      0?                          EC_TAGTYPE_HASH
      00 00 00 10                 Length 16
      5d 41 40 2a bc 4b 2a 76     16 bytes md5sum of EC password
      b9 71 9d 91 10 17 c5 92
    00 0e                         EC_TAG_VERSION_ID
      0?                          EC_TAGTYPE_CUSTOM
      00 00 00 21                 Length 33
      62 66 39 64 64 32 36 35     33 bytes of unique CVS version ID
      32 36 34 35 31 36 63 39     (CVS only — size, content, anything
      34 35 38 36 38 66 61 39     can change without notice; for releases
      30 38 66 62 37 64 39 38     this tag MUST NOT be present)
      00
```

The reply, hopefully:

```
30                                FLAGS — server sends an "accepts" flag
23                                the "accepts" flag itself; just take
                                  care that your program tolerates it
04                                EC_OP_AUTH_OK
  00 01                           Number of children
    00 76                         EC_TAG_SERVER_VERSION
      0?                          EC_TAGTYPE_STRING
      00 00 00 04                 Length 4
      43 56 53 00                 "CVS"
```

This shows the minimum a server must reply with. A current daemon also
appends one empty tag per feature capability it accepted, so the child
count is higher and `EC_TAG_SERVER_VERSION` is followed by the echoed
`EC_TAG_CAN_*` tags. Parse the children rather than assuming a count.

### Example 2 — Simple stats request

```
EC_OP_STAT_REQ
    +-- EC_TAG_DETAIL_LEVEL (with EC_DETAIL_CMD value)
```

```
20                                FLAGS
0a                                EC_OP_STAT_REQ
  00 01                           TagCount: 1
    00 10                         EC_TAG_DETAIL_LEVEL
      0?                          EC_TAGTYPE_UINT8
      00 00 00 01                 Length 1
      00                          0 = EC_DETAIL_CMD
```

The reply (assuming core is connected to a server):

```
EC_OP_STATS
    +-- EC_TAG_STATS_UL_SPEED
    +-- EC_TAG_STATS_DL_SPEED
    +-- EC_TAG_STATS_UL_SPEED_LIMIT
    +-- EC_TAG_STATS_DL_SPEED_LIMIT
    +-- EC_TAG_STATS_CURR_UL_COUNT
    +-- EC_TAG_STATS_TOTAL_SRC_COUNT
    +-- EC_TAG_STATS_CURR_DL_COUNT
    +-- EC_TAG_STATS_TOTAL_DL_COUNT
    +-- EC_TAG_STATS_UL_QUEUE_LEN
    +-- EC_TAG_STATS_BANNED_COUNT
    +-- EC_TAG_CONNSTATE
        +-- EC_TAG_SERVER
            +-- EC_TAG_SERVER_NAME
```

The interesting part of the reply packet:

```
20                                FLAGS
0c                                EC_OP_STATS
  00 0b                           Number of first-level tags: 11
    00 14 [...]                   EC_TAG_STATS_UL_SPEED
    00 16 [...]                   EC_TAG_STATS_DL_SPEED
    00 18 [...]                   EC_TAG_STATS_UL_SPEED_LIMIT
    00 1a [...]                   EC_TAG_STATS_DL_SPEED_LIMIT
    00 1c [...]                   EC_TAG_STATS_CURR_UL_COUNT
    00 22 [...]                   EC_TAG_STATS_TOTAL_SRC_COUNT
    00 1e [...]                   EC_TAG_STATS_CURR_DL_COUNT
    00 20 [...]                   EC_TAG_STATS_TOTAL_DL_COUNT
    00 26 [...]                   EC_TAG_STATS_UL_QUEUE_LEN
    00 24 [...]                   EC_TAG_STATS_BANNED_COUNT
    00 13                         EC_TAG_CONNSTATE — odd tagname means
                                  the tag has children. The true tagname
                                  is <found>-1: EC_TAG_CONNSTATE = 0x0012,
                                  and 0x0013 - 1 = 0x0012, so this is it.
                                  Odd-tagname tags also carry a tagcount
                                  field.
      0?                          EC_TAGTYPE_UINT32
      00 00 00 26                 TagLen: 38 (own content + children with headers)
      00 01                       TagCount: 1
        00 61                     EC_TAG_SERVER (has children)
          0?                      EC_TAGTYPE_IPV4
          00 00 00 1a             TagLen: 27 (own content 6 + child content 14 + child header 7)
          00 01                   TagCount: 1
            00 62                 EC_TAG_SERVER_NAME
              0?                  EC_TAGTYPE_STRING
              00 00 00 0e         TagLen: 14
              52 61 7a 6f 72 62 61 63  Content: "Razorback 2.0"
              6b 20 32 2e 30 00
          c3 f5 f4 f3 12 35       EC_TAG_SERVER content: Server IP:Port
                                  (195.245.244.243:4661)
      90 cc 83 52                 EC_TAG_CONNSTATE content: current UserID
```

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

The IP inside a GUI_ID uses the same byte order as
`EC_TAG_CLIENT_USER_IP`.

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

The reply is the server's **complete** session set. A session the client
is tracking that is absent from it was closed — by another client, or by
eviction — which is the only signal a close produces. No expiry tag is
needed, and a client must drop such a session rather than assume it
still exists.

#### `EC_OP_GET_CHAT_MESSAGES` (`0x5B`) → `EC_OP_CHAT_MESSAGES` (`0x5C`)

Non-destructive backfill of **one** session, for a client opening a
conversation it has no transcript for. Takes a required
`EC_TAG_CHAT_CLIENT_ID` and an optional `EC_TAG_CHAT_MSG_ID` cursor, and
replies with the same shape as above containing a single session
container.

#### `EC_OP_CHAT_SEND` (`0x65`)

Takes `EC_TAG_CHAT` (the text, non-empty) plus exactly one target:

| Target tag              | Addresses |
| ----------------------- | --------- |
| `EC_TAG_CHAT_CLIENT_ID` | A GUI_ID — replying needs no lookup, it is the id messages arrive with |
| `EC_TAG_CLIENT`         | A live peer by ECID |
| `EC_TAG_FRIEND`         | A friend by ECID — resolved through the friend's stored address, so an **offline** friend is reachable |

The server creates the session when it does not exist, so this doubles
as "start a chat with this address".

**Reply:** `EC_OP_NOOP` with `EC_TAG_CHAT_CLIENT_ID` (the resolved
GUI_ID) and `EC_TAG_CHAT_MSG_ID` (the id assigned), so the sender can
correlate without waiting for the next poll. `EC_OP_FAILED` with an
`EC_TAG_STRING` on an unknown target or empty text.

Note that the core's own send returning `false` means *queued while
connecting*, not *failed*, and does not produce an `EC_OP_FAILED`.

#### `EC_OP_CHAT_CLOSE_SESSION` (`0x66`)

Takes `EC_TAG_CHAT_CLIENT_ID`; drops the session from the store and
resets the peer's chat state. Replies `EC_OP_NOOP`, or `EC_OP_FAILED`
when there is no such session.

Closing is **global**, matching the semantics search tabs already have:
the core state is destroyed for every client, and the others learn of it
from the session's absence in the next `EC_OP_CHAT_SESSIONS` reply.

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

> **Note**: `EC_TAG_CONN_MAX_DL`, `EC_TAG_CONN_MAX_UL`, and
> `EC_TAG_CONN_SLOT_ALLOCATION` were widened from `uint16` to `uint32`
> to support speeds above 65534 KiB/s (~537 Mbps) required on modern
> gigabit connections. EC clients reading these tags should use
> `GetInt()` (which handles any integer width); clients sending them
> should encode them as 32-bit values.

### Peer vendor capabilities (`EC_TAG_CLIENT_MOD_CAPABILITIES = 0x0632`)

| Tag                              | Code     | Type     | Description |
| -------------------------------- | -------- | -------- | ----------- |
| `EC_TAG_CLIENT_MOD_CAPABILITIES` | `0x0632` | `uint32` | Peer's eMuleAI vendor capability bitfield |

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
