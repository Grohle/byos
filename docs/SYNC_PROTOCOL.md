# BTP/1: the ByOS sync protocol

Status: **normative draft, version 1**. It describes what `hibysync` (the device daemon) and
every client (the Kotlin core used by the Android app and `byos-cli`, see
[BYOS_CLI.md](BYOS_CLI.md)) must do. Where this document and an implementation disagree, the
implementation is wrong. The byte-level test vectors in `tests/btp/vectors.json`, written by
`python tools/btp_test_vectors.py`, are part of the specification. Every implementation must
reproduce them exactly.

The key words MUST, MUST NOT, SHOULD and MAY have their RFC 2119 meaning.

Background and the design decisions behind this protocol are in
[research_companion_apps.md](research_companion_apps.md) (Recommendations, and the critic's
Decisions 0 to 7). Nothing in BTP/1 is copied from CompasOS or Compás Tools (GPL). The ideas
were re-implemented from their descriptions.

## Contents

1. [Overview](#1-overview)
2. [Transports and sessions](#2-transports-and-sessions)
3. [Frame format](#3-frame-format)
4. [Common encodings](#4-common-encodings)
5. [Message types](#5-message-types)
6. [Error codes](#6-error-codes)
7. [HELLO, versions and capabilities](#7-hello-versions-and-capabilities)
8. [Wi-Fi security: pairing, reconnect and encrypted frames](#8-wi-fi-security-pairing-reconnect-and-encrypted-frames)
9. [Paths and namespaces](#9-paths-and-namespaces)
10. [File operations](#10-file-operations)
11. [Uploads (PUT) and resume](#11-uploads-put-and-resume)
12. [Downloads (GET)](#12-downloads-get)
13. [Library database: snapshot, staging, rescan](#13-library-database-snapshot-staging-rescan)
14. [ByOS update staging and install](#14-byos-update-staging-and-install)
15. [Device status](#15-device-status)
16. [Discovery (UDP)](#16-discovery-udp)
17. [Limits and timeouts](#17-limits-and-timeouts)
18. [How the shell runs hibysync](#18-how-the-shell-runs-hibysync)
19. [Test vectors](#19-test-vectors)
20. [Security notes](#20-security-notes)
21. [Still to verify on the device](#21-still-to-verify-on-the-device)

## 1. Overview

- One binary framed protocol over any reliable byte stream. In BTP/1 that stream is TCP port
  **7340**.
- The **device** (the HiBy, `hibysync`) is the server. The **client** (desktop app, Android app,
  `byos-cli`) connects to it.
- **USB**: the client runs `adb forward tcp:7340 tcp:7340` and connects to `127.0.0.1:7340` on
  the computer. ADB delivers the connection to `127.0.0.1:7340` on the device. That link is
  physical and cleartext is allowed.
- **Wi-Fi**: the device listens on its Wi-Fi address only while the user has turned on
  *Wi-Fi sync*. Every Wi-Fi session MUST complete pairing (first time) or the AUTH handshake
  (later) before any other request. After that, every frame is encrypted and authenticated.
- `hibysync` is started on demand by the ByOS shell and never at boot (section 18).
- File operations are confined to the SD card (`/usr/data/mnt/sd_0`), plus two small
  allow-listed staging areas, one for ByOS updates and one for the library database. The
  protocol never exposes a shell or arbitrary paths.

Byte order: every integer in a BTP/1 frame, a payload or a UDP packet is **big-endian**
(network order). The only little-endian values are X25519 keys and scalars, which are opaque
32-byte strings encoded as in RFC 7748.

## 2. Transports and sessions

### 2.1 Listening sockets

| Socket | When it is open | Session kind |
|---|---|---|
| TCP `127.0.0.1:7340` | Always while `hibysync` runs | **local**: cleartext allowed, no authentication |
| TCP `<wlan0 IPv4>:7340` | Only while Wi-Fi sync is on (shell setting), and Wi-Fi is up | **remote**: pairing or AUTH required, then encrypted |
| UDP `0.0.0.0:7340` | Same as the Wi-Fi TCP socket | discovery probes (section 16) |

The device decides the session kind from the socket that accepted the connection. It never
uses the peer address for this. `hibysync` MUST NOT bind TCP to `0.0.0.0`. When the Wi-Fi
address changes, the Wi-Fi socket is closed and re-bound.

### 2.2 Session states

```
              connect
                 |
                 v
          AWAIT_HELLO --(5 s, or anything but HELLO)--> close
                 |  client HELLO, device HELLO
        +--------+-------------------------+
   local|                                  |remote
        v                                  v
      OPEN  <--(AUTH ok / PAIR accepted)-- SECURING (only PAIR_*, AUTH_*, PING, BYE)
```

- The client MUST send HELLO first, on stream 0. The device answers with its own HELLO.
- **local**: after the HELLO exchange the session is OPEN. A local client MAY still run AUTH to
  get an encrypted session, for example a client that cannot tell whether its tunnel is really
  USB. The device accepts AUTH on local sessions too.
- **remote**: until the session is secured, the device answers any other request with
  `ERROR E_AUTH_REQUIRED` (END) and closes the connection.
- Only one pairing at a time on the whole device. At most **2** concurrent sessions in total.
  A third connection receives `ERROR E_BUSY` on stream 0 and is closed.
- So that a host on the LAN cannot hold the slots, the device also:
  - accepts at most **one** remote session that is not yet OPEN; another remote connection
    meanwhile gets `ERROR E_BUSY`;
  - gives a remote session **15 s** from connect to finish AUTH or reach PAIR_REVEAL (the
    user's Allow/Deny then has its own 120 s). Otherwise `ERROR E_TIMEOUT` and close. `PING`
    is answered while SECURING but does not count as activity;
  - counts such a timeout (and a HELLO timeout) against the source address like a failed AUTH
    (section 17), and refuses connections from a blocked address with `E_RATE_LIMIT`;
  - lets a local client that finds both slots taken **evict** the oldest remote session that
    is not OPEN (one waiting for the user's Allow only if there is no other). The evicted
    session receives `ERROR E_BUSY` on stream 0 and is closed. OPEN sessions are never evicted.
  - A `PAIR_COMMIT` that is never revealed frees the pairing slot at the 15 s deadline and is
    not counted as a failed pairing attempt (the user saw nothing).

### 2.3 Streams

- Every frame carries a 16-bit **stream id**. Stream 0 is for the connection itself: HELLO, PING,
  PONG, BYE, the pairing and AUTH messages, and fatal errors.
- Each client request opens a new stream. The client picks ids from 1 to 65535, increasing and
  wrapping around while skipping 0, and MUST NOT reuse an id that is still open.
- A request ends with exactly one **terminal frame** from the device with the END flag set:
  either the specific reply (`OK`, `STAT_R`, `PUT_DONE`, ...) or `ERROR`. Intermediate device
  frames (`LIST_R` pages, `PUT_READY`, `GET_R`, `DATA`) have END clear. A `LIST_R` page can also
  be terminal: the last page carries END.
- **Execution is serial.** The device runs requests one at a time, in the order received.
  Clients MAY pipeline requests (send several before the replies come back), and replies come
  back in the same order. `PING` and `CANCEL` are handled immediately, even while a transfer is
  running.
- `CANCEL` (on the stream to cancel, empty payload) stops the running or queued request. The
  device answers on that stream with `ERROR E_CANCELLED` (END), unless the request had already
  ended. A cancelled PUT keeps its `.part` file, so it can be resumed.
- hibysync keeps reading while a job runs: up to 4 pipelined requests (16 KiB of payload in
  total) wait in a side queue, so a `PING` or `CANCEL` sent behind them is still handled at once,
  and a `CANCEL` of a queued request removes it (it never runs). Once the queue is full, the
  device stops reading that connection until the running job ends.

### 2.4 Closing

Either side MAY send `BYE` (stream 0, empty payload) and then close. The device closes without
BYE on a protocol error (after a stream 0 `ERROR` when it can still send one), on a timeout
(section 17), or when the shell tells it to quit.

## 3. Frame format

### 3.1 Cleartext frame

```
offset size field
0      2    magic     0x42 0x54 ("BT")
2      1    version   0x01
3      1    type      message type (section 5)
4      1    flags     bit 0 = END; bits 1..7 MUST be 0
5      1    reserved  MUST be 0
6      2    stream    u16 big-endian
8      4    length    u32 big-endian, payload length, 0 .. MAX_PAYLOAD
12     n    payload
```

`MAX_PAYLOAD` = **65544** bytes (an 8-byte DATA offset plus 64 KiB of data). A receiver MUST:

| Condition | Action |
|---|---|
| magic is not `BT` | `E_PROTO`, close |
| version is not 1 | `E_VERSION`, close |
| a reserved flag bit or the reserved byte is set | `E_PROTO`, close |
| length > the receiver's `max_payload` (announced in HELLO) | `E_TOO_LARGE`, close (without reading the payload) |
| unknown type | `ERROR E_UNKNOWN_TYPE` (END) on that stream; the session continues (the payload is read and discarded) |
| payload shorter or longer than its message layout | `E_PROTO` (END) on that stream; close if the stream is 0 |

Example (from the vectors), client HELLO:

```
4254 01 01 00 00 0000 00000027   header: HELLO, stream 0, 39-byte payload
01 01 00 00 00010008 000001ff    proto 1..1, role client, flags 0, max_payload 65544, caps 0x1ff
01 000e 62796f732d636c6920302e312e30   TLV SOFTWARE "byos-cli 0.1.0"
02 0007 4465736b746f70                 TLV NAME "Desktop"
```

### 3.2 Encrypted record (secured sessions)

Once a session is secured (section 8), the stream carries **records** instead of bare frames,
in both directions:

```
offset size field
0      4    box_len   u32 big-endian = 16 + inner_len
4      16   tag       Poly1305 tag
20     m    ciphertext of the inner frame (a complete cleartext frame as in 3.1)
```

- `box = secretbox(inner_frame, nonce, key)`, using NaCl `crypto_secretbox_xsalsa20poly1305` in
  the "tag first" form: `tag || ciphertext`. This is what libsodium calls
  `crypto_secretbox_easy`. The original NaCl API writes 16 zero bytes before the box, and those
  are not sent.
- The header, the type and the stream id are all inside the ciphertext, so the whole frame is
  authenticated and an observer only sees record lengths.
- The receiver MUST check `16 + 12 <= box_len <= 16 + 12 + max_payload` before reading. Then it
  opens the box (verifying the tag, in constant time) and parses the inner frame exactly as in
  3.1.
- **Any** failure (a bad length, a bad tag, a malformed inner frame) is fatal: close the
  connection without sending anything. A record that fails to open is never answered.

**Keys**: one key per direction, `k_c2d` (client to device) and `k_d2c` (device to client).

**Nonce** (24 bytes):

```
nonce[0]      = direction: 0x01 client->device, 0x02 device->client
nonce[1..15]  = 0x00
nonce[16..23] = counter, u64 big-endian
```

Each direction has its own counter. It starts at 0 with the first encrypted record in that
direction and goes up by 1 per record. Counters are never sent: both sides track them. A
sender MUST close the session before a counter reaches 2^32, and the client reconnects.
Because of the implicit counter, a replayed, dropped or reordered record fails to open. TCP
never reorders, so this always means an attack or a bug.

## 4. Common encodings

| Notation | Encoding |
|---|---|
| `u8`, `u16`, `u32`, `u64` | unsigned, big-endian |
| `i64` | signed two's complement, big-endian. Times are Unix seconds (UTC). |
| `str16` | `u16` byte length + UTF-8 bytes, no terminating NUL |
| `str8` | `u8` byte length + UTF-8 bytes (discovery only) |
| `bytes[n]` | exactly n raw bytes |
| `tlv` | `u8` tag + `u16` length + value. Receivers MUST skip unknown tags. |

Strings MUST be valid UTF-8. Receivers MUST reject invalid UTF-8 with `E_PROTO`. Paths have
extra rules (section 9). Human-readable names (client name, device name) are at most 64 bytes,
and longer ones are rejected with `E_PROTO`.

## 5. Message types

"C" means client to device, "D" means device to client. "S0" means stream 0.

| Code | Name | Dir | Payload | Reply |
|---|---|---|---|---|
| 0x01 | HELLO | C, D, S0 | section 7 | device HELLO |
| 0x02 | ERROR | D | `code u16, message str16` (English, at most 200 bytes, for logs) | terminal |
| 0x03 | OK | D | empty | terminal |
| 0x04 | PING | C/D, S0 | `bytes[8]` | PONG with the same 8 bytes |
| 0x05 | PONG | C/D, S0 | `bytes[8]` | none |
| 0x06 | BYE | C/D, S0 | empty | none (the sender closes) |
| 0x07 | CANCEL | C | empty, on the stream to cancel | `ERROR E_CANCELLED` on that stream |
| 0x10 | PAIR_COMMIT | C, S0 | `commit bytes[32]` | PAIR_DEVICE |
| 0x11 | PAIR_DEVICE | D, S0 | `De bytes[32], Nd bytes[16]` | |
| 0x12 | PAIR_REVEAL | C, S0 | `Ce bytes[32], Nc bytes[16]` | (keys switch on) |
| 0x13 | PAIR_CONFIRM | C, S0, encrypted | `Cs bytes[32], client_name str16` | PAIR_RESULT or ERROR |
| 0x14 | PAIR_RESULT | D, S0, encrypted | `status u8 (0), Ds bytes[32], device_id bytes[8], device_name str16` | |
| 0x18 | AUTH_INIT | C, S0 | `key_id bytes[16], Ce bytes[32]` | AUTH_REPLY or ERROR |
| 0x19 | AUTH_REPLY | D, S0 | `De bytes[32]` | (keys switch on) |
| 0x1A | AUTH_FINISH | C, S0, encrypted | empty | AUTH_OK |
| 0x1B | AUTH_OK | D, S0, encrypted | empty | |
| 0x20 | STAT | C | `ns u8, path str16` | STAT_R |
| 0x21 | STAT_R | D | `kind u8, size u64, mtime i64` | terminal |
| 0x22 | LIST | C | `ns u8, flags u8, path str16` | LIST_R pages |
| 0x23 | LIST_R | D | `count u16, count x entry` | the last page has END |
| 0x24 | DF | C | empty | DF_R |
| 0x25 | DF_R | D | `sd_state u8, sd_fs u8, sd_total u64, sd_free u64, data_total u64, data_free u64` | terminal |
| 0x26 | MKDIR | C | `ns u8, flags u8, path str16` | OK |
| 0x27 | MOVE | C | `ns u8, flags u8, src str16, dst str16` | OK |
| 0x28 | DELETE | C | `ns u8, flags u8, path str16` | OK |
| 0x30 | PUT_BEGIN | C | `ns u8, mode u8, size u64, mtime i64, sha256 bytes[32], path str16` | PUT_READY or PUT_DONE (skipped) or ERROR |
| 0x31 | PUT_READY | D | `offset u64` | |
| 0x32 | DATA | C/D | `offset u64, data` (1 .. 65536 bytes) | |
| 0x33 | PUT_END | C | empty | PUT_DONE or ERROR |
| 0x34 | PUT_DONE | D | `result u8, size u64` | terminal |
| 0x38 | GET | C | `ns u8, flags u8, offset u64, path str16` | GET_R, DATA..., GET_END |
| 0x39 | GET_R | D | `size u64, mtime i64, offset u64` | |
| 0x3A | GET_END | D | `sha256 bytes[32]` | terminal |
| 0x40 | DB_SNAPSHOT | C | empty | GET_R, DATA..., GET_END |
| 0x41 | DB_STAGE | C | `flags u8, base_sha256 bytes[32]` | DB_STATUS_R |
| 0x42 | DB_STATUS | C | empty | DB_STATUS_R |
| 0x43 | DB_STATUS_R | D | `staged u8, staged_sha256 bytes[32], live_sha256 bytes[32], live_size u64, last_apply u8, data_free u64` | terminal |
| 0x44 | DB_UNSTAGE | C | empty | DB_STATUS_R |
| 0x45 | LIB_RESCAN | C | `mode u8, count u16, count x path str16` | OK |
| 0x50 | UPDATE_STAGE | C | `flags u8` | UPDATE_STAGE_R |
| 0x51 | UPDATE_STAGE_R | D | `version str16, restart u8, needs_firmware u8, size u64` | terminal |
| 0x52 | UPDATE_INSTALL | C | `flags u8` | OK |
| 0x53 | UPDATE_STATUS | C | empty | UPDATE_STATUS_R |
| 0x54 | UPDATE_STATUS_R | D | `state u8, permille u16, error u16, version str16` | terminal |
| 0x60 | STATUS | C | empty | STATUS_R |
| 0x61 | STATUS_R | D | `battery u8, charging u8, sd_state u8, tlv...` | terminal |

All other type codes are reserved. Codes 0x70 to 0x7F are reserved for experiments, and
release builds MUST NOT send them.

## 6. Error codes

`ERROR.code` (u16). The `message` text is for logs only: clients show their own translated
text, chosen by the code.

| Code | Name | Meaning |
|---|---|---|
| 1 | E_PROTO | Malformed frame or payload, or a message not allowed in this state |
| 2 | E_VERSION | No common protocol version |
| 3 | E_UNKNOWN_TYPE | Unknown message type (the session continues) |
| 4 | E_AUTH_REQUIRED | Remote session not secured yet |
| 5 | E_AUTH_FAILED | Unknown key id, a bad commitment, or a failed key confirmation |
| 6 | E_PAIR_CLOSED | Pairing is not open on the device (the user has not opened *Pair new device*) |
| 7 | E_PAIR_REJECTED | The user tapped *Deny*, or did not answer within 120 s |
| 8 | E_BUSY | Too many sessions, a pairing already running, or the operation lock held (update installing) |
| 9 | E_TOO_LARGE | Frame longer than `max_payload` |
| 10 | E_RATE_LIMIT | Too many failed pairing or AUTH attempts (section 17) |
| 11 | E_TIMEOUT | The peer went silent |
| 12 | E_CANCELLED | Cancelled by CANCEL or by the shell |
| 13 | E_UNSUPPORTED | Capability not available (see HELLO caps), for example LIB_RESCAN mode 0 without the shell |
| 14 | E_INTERNAL | Unexpected device error |
| 32 | E_BAD_PATH | Path breaks the rules of section 9, or names a file outside the allow-list |
| 33 | E_NOT_FOUND | |
| 34 | E_EXISTS | Target exists and the mode or flags do not allow replacing it |
| 35 | E_NOT_DIR | |
| 36 | E_IS_DIR | |
| 37 | E_NOT_EMPTY | DELETE of a non-empty folder without RECURSIVE |
| 38 | E_NO_SPACE | Not enough free space (checked before writing) |
| 39 | E_IO | Read or write error from the file system |
| 40 | E_SD_UNAVAILABLE | SD card absent, not mounted, or exported over USB mass storage |
| 41 | E_HASH_MISMATCH | Upload content does not match the announced SHA-256 or size |
| 42 | E_OFFSET | DATA offset is not the expected one |
| 43 | E_FILE_TOO_BIG | Over the file-system limit (4 GiB - 1 on FAT32) or the namespace limit |
| 44 | E_NOT_ALLOWED | Operation not allowed in this namespace or on this session kind |
| 64 | E_SIGNATURE | Update not signed by a ByOS release key |
| 65 | E_UPDATE_INVALID | Update JSON or tar inconsistent (size, sha256, name, version) |
| 66 | E_DOWNGRADE | Older than the installed version and ALLOW_DOWNGRADE not set |
| 67 | E_BATTERY | Battery under 30 % and not charging |
| 68 | E_NEEDS_FIRMWARE | The update needs a newer ByOS firmware (Patcher) first |
| 69 | E_NOT_STAGED | UPDATE_INSTALL without a successful UPDATE_STAGE, or DB_STAGE without an uploaded DB |
| 80 | E_DB_INVALID | Staged file is not a usable SQLite library database |
| 81 | E_DB_STALE | The live DB changed since the snapshot the client built from |

## 7. HELLO, versions and capabilities

HELLO payload (both directions):

```
proto_min   u8    lowest BTP version supported (1)
proto_max   u8    highest BTP version supported (1)
role        u8    0 = client, 1 = device
flags       u8    device only (clients send 0), see below
max_payload u32   largest payload this side accepts; MUST be >= 65544 in BTP/1
caps        u32   capability bits, see below
tlv...            until the end of the payload
```

Device `flags`:

| Bit | Name | Meaning |
|---|---|---|
| 0 | LOCAL | This session was accepted on the loopback socket (USB/adb forward); cleartext allowed |
| 1 | PAIRING_OPEN | The shell has the *Pair new device* screen open |
| 2 | SD_MOUNTED | The SD is mounted and usable (not exported over USB storage) |
| 3 | SHELL_ATTACHED | The daemon has a control pipe to the shell (pairing, update install, rescan possible) |

`caps` (the client sets the bits it understands; the device sets what it implements in this
session):

| Bit | Name | Messages |
|---|---|---|
| 0 | FILES | STAT, LIST, DF, MKDIR, MOVE, DELETE, PUT, GET |
| 1 | RESUME | PUT resume and GET offset |
| 2 | DB_SNAPSHOT | DB_SNAPSHOT |
| 3 | DB_STAGE | NS_DB, DB_STAGE, DB_STATUS, DB_UNSTAGE |
| 4 | LIB_RESCAN | LIB_RESCAN |
| 5 | UPDATE | NS_UPDATE, UPDATE_STAGE, UPDATE_INSTALL, UPDATE_STATUS |
| 6 | PAIRING | PAIR_* and AUTH_* |
| 7 | ENCRYPTION | encrypted records |
| 8 | STATUS | STATUS |

A request whose capability bit the device did not set gets `E_UNSUPPORTED`.

TLV tags:

| Tag | Name | Value | Sent by |
|---|---|---|---|
| 0x01 | SOFTWARE | UTF-8, for example `hibysync 0.1.0`, `byos-cli 0.1.0` | both |
| 0x02 | NAME | UTF-8, at most 64 bytes, user-visible name | both |
| 0x03 | DEVICE_ID | 8 bytes = first 8 bytes of SHA-256(Ds) | device |
| 0x04 | BYOS_VERSION | installed payload version (`/usr/data/hibymod/payload_version`) | device |
| 0x05 | FIRMWARE | ByOS rootfs version ("ByOS X.Y.Z" line of `/usr/bin/hibymod`) | device |
| 0x06 | MODEL | `R3II-2025` | device |

**Version negotiation**: the session version is `v = min(proto_max_c, proto_max_d)`. If
`v < max(proto_min_c, proto_min_d)`, the device answers `ERROR E_VERSION` on stream 0 instead of
its HELLO and closes. A client that receives a device HELLO with no common version closes.
BTP/1 implementations always send 1/1.

DEVICE_ID in HELLO is not authenticated. A client MUST NOT trust it until AUTH succeeds (AUTH
proves possession of the Ds that the client stored at pairing time). On a remote session the
device sends DEVICE_ID, NAME and the versions in cleartext. This is accepted: they are also in
the discovery announce.

## 8. Wi-Fi security: pairing, reconnect and encrypted frames

### 8.1 Keys and stored state

- **Device static key** `ds` (32 random bytes from `getrandom`/`/dev/urandom`), public
  `Ds = X25519(ds, 9)`. It is created the first time `hibysync` needs it and stored in
  `/usr/data/hibymod/sync/device.key` (mode 0600, written to `.tmp` + fsync + rename).
  `device_id = SHA-256(Ds)[0..8]`.
- **Client static key** `cs`, public `Cs`, one per client installation. It is stored by the
  client (see BYOS_CLI.md). `key_id = SHA-256(Cs)[0..16]`.
- **Peers**: `/usr/data/hibymod/sync/peers`, one line per paired client:
  `<Cs 64 hex> <paired unix time> <name, percent-encoded>`. At most 8 peers; pairing a 9th
  replaces the oldest after the user confirms on the device. The shell can list and forget
  peers.
- **Ephemeral keys** `ce`, `de` are fresh random values for every handshake and are erased
  after the keys have been derived.
- X25519 is RFC 7748. After every X25519, both sides MUST check that the result is not all
  zeros (low-order point). If it is, they send `E_AUTH_FAILED` and close.

Primitives: SHA-256, HMAC-SHA256, HKDF-SHA256 (RFC 5869), X25519 and
XSalsa20-Poly1305 secretbox. On the device they come from the TweetNaCl-derived code already in
`src/ota_net.inc.c` (SHA-256, and the field arithmetic shared with Ed25519 verification). X25519
and secretbox are added from the public-domain TweetNaCl algorithms.

`HKDF(salt, ikm)` below means `PRK = HMAC-SHA256(key = salt, msg = ikm)`, and
`Expand(PRK, info, L)` is RFC 5869 expand. Info strings are ASCII without NUL. `||` is
concatenation. `H()` is SHA-256. `hello_c` and `hello_d` are the HELLO **payloads** (without the
12-byte header) exactly as sent.

### 8.2 Pairing (first connection of a client)

Pairing is possible only while the user has *Settings › ByOS Sync › Pair new device* open on
the HiBy (`PAIRING_OPEN`). Otherwise the device answers `PAIR_COMMIT` with `E_PAIR_CLOSED`.
All messages use stream 0.

```
Client                                              Device
HELLO(hello_c)                    ------>
                                  <------           HELLO(hello_d)    [PAIRING_OPEN set]
Nc = 16 random bytes; ce random; Ce = X25519(ce,9)
commit = H("BTP/1 commit" || Ce || Nc)
PAIR_COMMIT(commit)               ------>
                                  <------           PAIR_DEVICE(De || Nd)   de random, Nd 16 random
PAIR_REVEAL(Ce || Nc)             ------>
                                                    check commit == H("BTP/1 commit" || Ce || Nc)
both: th  = H("BTP/1 pair" || H(hello_c) || H(hello_d) || commit || De || Nd || Ce || Nc)
      ss  = X25519(ce, De) = X25519(de, Ce)
      PRK = HKDF(salt = th, ikm = ss)
      SAS = u32be(Expand(PRK, "BTP/1 sas", 4)) mod 1000000, as 6 decimal digits with leading zeros
      k_c2d = Expand(PRK, "BTP/1 c2d", 32);  k_d2c = Expand(PRK, "BTP/1 d2c", 32)
------------------------------- from here on: encrypted records -------------------------------
                                                    shell shows SAS "815 365", client name, Allow / Deny
user types the code shown on the HiBy into the client; the client compares it (constant time) with its SAS
PAIR_CONFIRM(Cs || name)  [c2d #0] ------>          (only after the code matched)
                                                    waits for Allow (at most 120 s)
                                  <------           PAIR_RESULT(0 || Ds || device_id || name) [d2c #0]
                                                    stores Cs in peers; session is OPEN
client stores (device_id, Ds, name); session is OPEN
```

Rules:

- The client commits to `Ce` before it sees `De`. A man in the middle therefore has to fix its
  keys before it knows the other side's, and gets one chance in 10^6 per attempt to make both
  SAS values equal. The commitment check is mandatory. On mismatch the device sends
  `E_AUTH_FAILED` (in clear) and closes.
- The user check is done twice: the client refuses to send `PAIR_CONFIRM` unless the code the
  user typed equals its SAS, and the device refuses unless the user taps *Allow* while the same
  code is on its screen. A wrong code typed on the client: the client sends `BYE` and closes.
  The device counts this as a failed attempt.
- If the user taps *Deny*, or 120 s pass, the device sends `ERROR E_PAIR_REJECTED` (stream 0,
  END) as an encrypted record and closes.
- `PAIR_RESULT.status` is 0 (the only defined value). Rejection is always an ERROR.
- The encrypted DF exchange after PAIR_RESULT in the vectors shows that, after pairing, the same
  keys carry on: the session continues with the c2d counter at 1 and the d2c counter at 1.

### 8.3 Reconnect (AUTH) of a paired client

```
Client                                              Device
HELLO(hello_c)                    ------>
                                  <------           HELLO(hello_d)
ce random, Ce = X25519(ce,9)
AUTH_INIT(key_id || Ce)           ------>
                                                    find Cs with H(Cs)[0..16] == key_id, else E_AUTH_FAILED + close
                                  <------           AUTH_REPLY(De)          de random
both: ee = X25519(ce, De)  = X25519(de, Ce)
      es = X25519(cs, De)  = X25519(de, Cs)
      se = X25519(ce, Ds)  = X25519(ds, Ce)
      th  = H("BTP/1 auth" || H(hello_c) || H(hello_d) || key_id || Ce || De || Cs || Ds)
      PRK = HKDF(salt = th, ikm = ee || es || se)
      k_c2d = Expand(PRK, "BTP/1 c2d", 32);  k_d2c = Expand(PRK, "BTP/1 d2c", 32)
------------------------------- from here on: encrypted records -------------------------------
AUTH_FINISH()             [c2d #0] ------>          opens it: proves the client holds cs
                                  <------           AUTH_OK()  [d2c #0]: proves the device holds ds
session is OPEN
```

- Only the holder of `cs` can compute `es`, and only the holder of `ds` can compute `se`, so a
  record that opens proves who the other side is. `ee` gives forward secrecy.
- If the record does not open, the device closes without answering (section 3.2) and counts a
  failed attempt. If `AUTH_OK` does not open, the client closes and reports that the device
  identity changed. Clients MUST NOT silently re-pair in that case.
- The client MUST compare the HELLO DEVICE_ID with the `device_id` it stored. If they differ, it
  stops before AUTH_INIT ("different device or reset").

### 8.4 After the device forgets a peer

The device answers `AUTH_INIT` with an unknown `key_id` with `ERROR E_AUTH_FAILED` (in clear) and
closes. The client then offers to pair again.

## 9. Paths and namespaces

### 9.1 Namespaces (`ns`)

| ns | Name | Root on the device | Allowed |
|---|---|---|---|
| 0 | NS_SD | `/usr/data/mnt/sd_0` | every file operation, subject to the path rules |
| 1 | NS_UPDATE | `SD/ByOS/ota/` | PUT, STAT, DELETE of the allow-listed names only (14.1) |
| 2 | NS_DB | `/usr/data/hibymod/sync/db/` | PUT, STAT, DELETE of `staged.db` only (13.2) |

Anything else (LIST, MKDIR, MOVE or GET in ns 1 and 2, an unknown ns) gets `E_NOT_ALLOWED`.

### 9.2 Path rules

A path is a relative UTF-8 string that uses `/` as its separator. The device MUST reject
(`E_BAD_PATH`) any path that:

| Rule | Reason name in the vectors |
|---|---|
| is empty, except as the root for LIST, STAT and DF | `empty` |
| is longer than 1024 bytes | `too_long` |
| is not valid UTF-8 | `bad_utf8` |
| starts with `/` | `absolute` |
| has more than 32 components | `too_deep` |
| has an empty component (`a//b`, trailing `/`) | `empty_component` |
| has a component `.` or `..` | `dot_component` |
| has a component starting with `.` (hidden, including `.temp`, `.part` files) | `hidden` |
| contains a character U+0000 to U+001F or U+007F | `control_char` |
| contains `\ : * ? " < > \|` (not allowed on FAT and exFAT) | `forbidden_char` |
| has a component ending with a space or `.` | `trailing_dot_space` |
| has a component longer than 255 bytes | `component_too_long` |

The checks run in the order of the table. The component rules are applied one component at a
time, from left to right, and inside a component `control_char` and `forbidden_char` are
reported for whichever character comes first. Only the reason name matters for the vectors: the
device answers `E_BAD_PATH` in every case. Clients MUST normalise paths to Unicode NFC before
sending them, because the stock database stores NFC. The device does not normalise. LIST hides
names starting with `.` unless flag bit 0 (HIDDEN) is set. Even then, such entries are shown
but cannot be addressed.

### 9.3 Confinement on the device

- Every operation opens the namespace root once (`open(root, O_DIRECTORY)`) and then walks the
  path one component at a time with `openat(dirfd, comp, O_NOFOLLOW | O_DIRECTORY)`. The last
  component uses `O_NOFOLLOW` too. Symbolic links are never followed. A symlink anywhere in the
  path gives `E_BAD_PATH`.
- New files are created with `O_CREAT | O_EXCL | O_NOFOLLOW`, mode 0644. New folders get 0755.
- **SD availability**: before any NS_SD or NS_UPDATE operation the device checks that
  `/usr/data/mnt/sd_0` is a mount point (its `st_dev` differs from `/usr/data/mnt`), that it is
  listed in `/proc/mounts`, and that the SD is not exported over USB mass storage (the same test
  `hibyguard` uses). If any check fails: `E_SD_UNAVAILABLE`. The check runs again before the
  rename at PUT_END. An SD that disappears mid-transfer gives `E_IO` or `E_SD_UNAVAILABLE`, and
  the `.part` stays (or disappears with the card).
- Free space is checked with `statvfs` before writing (11.2).

## 10. File operations

**STAT** `ns, path` returns `STAT_R kind, size, mtime`. `kind`: 0 file, 1 folder, 2 other
(device, fifo; never followed). `size` is 0 for folders.

**LIST** `ns, flags, path` (flags bit 0 = HIDDEN) returns one or more `LIST_R` pages, the last
with END. Entry:

```
kind  u8      0 file, 1 folder, 2 other
size  u64
mtime i64
name  str16   component name only
```

A page holds as many entries as fit in `max_payload`. Order is unspecified (readdir order), and
clients sort. An empty folder gives a single page with `count = 0` and END. A path that names a
file gives `E_NOT_DIR`.

**DF** returns `DF_R`:

| Field | Values |
|---|---|
| `sd_state` | 0 mounted, 1 no card, 2 exported over USB storage, 3 mounted read-only |
| `sd_fs` | 0 unknown, 1 vfat (FAT32: files at most 4 GiB - 1), 2 exfat |
| `sd_total`, `sd_free` | bytes (`statvfs`: f_blocks and f_bavail times f_frsize); 0 when not mounted |
| `data_total`, `data_free` | the same for `/usr/data` |

DF works even when the SD is unavailable, which is how clients learn `sd_state`.

**MKDIR** `ns, flags, path`. Flags bit 0 = PARENTS (create missing parents, like `mkdir -p`; an
existing folder is not an error). Without PARENTS an existing target is `E_EXISTS`. Folders are
fsynced, as is their parent.

**MOVE** `ns, flags, src, dst`. Flags bit 0 = REPLACE. Without REPLACE, an existing `dst` is
`E_EXISTS`. The check and the rename are done with `renameat2(RENAME_NOREPLACE)` when the
kernel has it. Linux 4.4 vfat does not, so the device takes a per-daemon lock, checks with
`fstatat` and then calls `renameat`. `hibysync` is the only writer that uses this lock, and the
window is documented. Moving a folder into itself is `E_BAD_PATH`. Cross-namespace moves do not
exist.

**DELETE** `ns, flags, path`. Flags bit 0 = RECURSIVE. Deleting the namespace root is
`E_BAD_PATH`. A non-empty folder without RECURSIVE is `E_NOT_EMPTY`. A recursive delete walks
with `openat`/`unlinkat`, never follows links, and sends no progress. Clients SHOULD warn the
user before sending RECURSIVE.

All of them answer `OK` (END) on success.

## 11. Uploads (PUT) and resume

### 11.1 Exchange

```
C  PUT_BEGIN(ns, mode, size, mtime, sha256, path)
D  PUT_READY(offset)              or PUT_DONE(result=1) (skipped)   or ERROR
C  DATA(offset, bytes) ...        consecutive, starting at the offset from PUT_READY
C  PUT_END()
D  PUT_DONE(result=0, size)       or ERROR
```

`PUT_BEGIN` fields:

| Field | Meaning |
|---|---|
| `mode` | 0 CREATE: fail with `E_EXISTS` if the target exists (default, never overwrites silently). 1 REPLACE: replace an existing file atomically. 2 SKIP_IDENTICAL: if the target exists with the same size and SHA-256, answer `PUT_DONE(result=1)` at once; if it exists and differs, `E_EXISTS`. |
| `size` | final size in bytes |
| `mtime` | modification time to set after the rename; 0 = leave it at "now" |
| `sha256` | SHA-256 of the whole file, known by the client before it starts (it hashes the file first) |

For SKIP_IDENTICAL the device hashes the existing target. This costs an SD read and is done in
64 KiB blocks, at idle I/O priority.

### 11.2 Device behaviour

1. Check the namespace, the path, the SD, and the file-system limit (`vfat`: `size <= 4294967295`,
   else `E_FILE_TOO_BIG`). Check that the parent folder exists (`E_NOT_FOUND`; clients create
   it first with MKDIR PARENTS). Apply `mode` to an existing target.
2. **Part file**: `<parent>/.btp-<id>.part`, where
   `id = hex(SHA-256(path_bytes || 0x00 || sha256 || u64be(size))[0..8])` (16 hex digits). The
   same path, content and size always map to the same part file, and any change starts a new
   one.
3. Free space: `needed = size - existing_part_size + 1 MiB`. If `f_bavail * f_frsize < needed`:
   `E_NO_SPACE`.
4. **Resume**: if the part exists, `offset = floor(part_size / 65536) * 65536`, capped at
   `size`. The device truncates the part to `offset` (a torn tail is dropped) and re-reads
   `[0, offset)` to rebuild the streaming SHA-256 state. Otherwise it creates the part with
   `O_EXCL` and `offset = 0`. It replies `PUT_READY(offset)`.
5. DATA: `offset` MUST equal the next expected byte (`E_OFFSET` otherwise; the part stays).
   Data length is 1 to 65536. The device writes, updates SHA-256, and calls `fdatasync` every
   4 MiB. Bytes past `size` are `E_HASH_MISMATCH` (the part is deleted).
6. PUT_END: if `received != size` or the hash differs: `E_HASH_MISMATCH`, and the part is
   **deleted** (its content is wrong, so it cannot be resumed). Otherwise: `fsync(part)`, apply
   `mode` again (CREATE: target must still not exist; it uses the 10 MOVE no-replace method),
   `renameat(part, target)`, `fsync(parent dir)`, set `mtime` with `utimensat` when non-zero.
   Reply `PUT_DONE(result=0, size)`.
7. If the connection drops, or on CANCEL, or on `E_OFFSET`: the part is kept with what was
   written. Stale parts older than 7 days are removed by the daemon when it starts, but only in
   folders it touches. They are never listed (hidden).

The client SHOULD send DATA frames of 65536 bytes, except for the last one. It MAY keep sending
without waiting: TCP flow control is enough. The device does not acknowledge DATA.

### 11.3 Client resume

A client that loses the connection reconnects, sends the same PUT_BEGIN (same path, size and
sha256), and continues from the returned offset. A client MUST NOT assume an offset: only
PUT_READY says where to continue.

`PUT_DONE.result`: 0 written, 1 skipped (identical).

## 12. Downloads (GET)

```
C  GET(ns, flags, offset, path)          flags bit 0 = SHA (compute the whole-file SHA-256)
D  GET_R(size, mtime, offset)
D  DATA(offset, bytes) ...                64 KiB frames, from offset to size
D  GET_END(sha256)                        END; all zeros when SHA was not requested
```

- `offset > size` gives `E_OFFSET`. `offset == size` gives `GET_R` followed by `GET_END`.
- With SHA and `offset > 0`, the device first hashes `[0, offset)` from the file. The hash then
  covers the **whole file**, so a resumed download can be checked end to end.
- If the file changes size during the transfer, the device stops with `E_IO`. Clients compare
  `GET_R.size` and `mtime` with what they saw when they decide to resume a partial local file.
- Only NS_SD allows GET. The DB snapshot has its own message (13.1).

## 13. Library database: snapshot, staging, rescan

The stock player keeps its library in SQLite at `/usr/data/usrlocal_media.db`, or on the SD at
`SD/.temp/usrlocal_media.db` when `tf_music_db_enable=1`. The daemon reads that setting the
same way Settings does and uses the right path. Below, "live DB" means whichever applies.

### 13.1 DB_SNAPSHOT

Reply: `GET_R(size, mtime, 0)`, then `DATA` frames, then `GET_END(sha256)` (always computed).

The device produces a **consistent** copy without stopping the player: it runs
`hibytag dbsnapshot <live> <tmp>`, which uses the SQLite online backup API with
`busy_timeout 5000`. The copy goes to `/usr/data/hibymod/sync/db/snap.db`, or to
`SD/ByOS/sync/snap.db` when `/usr/data` has less than `2 x live size + 1 MiB` free. The daemon
streams that file and deletes it afterwards. `GET_END.sha256` is the **revision** of the
library: clients send it back as `base_sha256` in DB_STAGE. No resume (the database is a few MB).
If `hibytag` is missing or fails: `E_UNSUPPORTED` or `E_IO`.

### 13.2 DB_STAGE (PC-built database)

1. The client uploads the database it built with PUT in NS_DB, path `staged.db` (the only
   allowed name), mode REPLACE. The daemon stores it as `/usr/data/hibymod/sync/db/incoming.db`.
   NS_DB limit: 64 MiB.
2. The client sends `DB_STAGE(flags, base_sha256)`. Flags bit 0 = FORCE (skip the staleness
   check).
3. The device checks, in this order:
   - an `incoming.db` exists, from a PUT that completed in **this** session (`E_NOT_STAGED`);
   - its first 16 bytes are `SQLite format 3\0`, its page size is valid, and its size is a
     multiple of the page size (`E_DB_INVALID`);
   - `hibytag dbcheck incoming.db` runs `pragma integrity_check` and checks that the tables the
     stock player needs exist (`MEDIA_TABLE`, `VERSION_TABLE` = 20181015, and the others listed
     in the PC-side builder spec) (`E_DB_INVALID`);
   - unless FORCE: SHA-256 of the live DB == `base_sha256` (`E_DB_STALE`; the client pulls a new
     snapshot and rebuilds or merges);
   - free space on `/usr/data` for the live, staged and backup copies: `data_free >= 2 x
     max(live, staged) + 1 MiB` (`E_NO_SPACE`; the client then advises turning on
     `tf_music_db_enable`).
4. `incoming.db` is renamed to `staged.db` and the device writes `staged.json`
   (`{"sha256": ..., "base_sha256": ..., "size": ..., "time": ...}`, written to `.tmp` + fsync +
   rename). Reply: `DB_STATUS_R`.
5. **Applying** the staged DB is not done by `hibysync`. The display shim
   (`hibymod_disp.so`, OTA-updatable) does it at the next start of `hiby_player`, before the
   player's `main()` opens the database: integrity check, keep `usrlocal_media.db.bak`, atomic
   rename, record the result in `/usr/data/hibymod/sync/db/apply.result`. This follows critic
   Decision 6. The database cannot be swapped under a running player, and restarting the player
   without a reboot freezes the UI, so the client tells the user a restart is needed. The device
   never reboots on its own because of DB_STAGE.

`DB_STATUS_R` fields:

| Field | Meaning |
|---|---|
| `staged` | 1 when a staged DB is waiting to be applied |
| `staged_sha256` | zeros when none |
| `live_sha256` | SHA-256 of the live DB file; zeros when it could not be read |
| `live_size` | bytes |
| `last_apply` | 0 never, 1 applied, 2 rejected (integrity), 3 rejected (no space), 4 rejected (other) |
| `data_free` | free bytes on `/usr/data` |

`DB_UNSTAGE` deletes `staged.db`, `staged.json` and `incoming.db`, then replies `DB_STATUS_R`.

What a PC-built database must keep from the live one (history, favourites, playlists, queue
with `MANAGER_TABLE`, books, `MEDIA2_TABLE` as a root-folder cache, an empty `SEARCH_TABLE`) is
the builder's job and is specified with the PC-side DB builder. BTP/1 only moves and checks
files.

### 13.3 LIB_RESCAN

`mode u8, count u16, paths`:

- mode 0 FULL (count MUST be 0): asks the shell, over the control pipe, to trigger the stock
  player's library rescan. Without `SHELL_ATTACHED`, or when the shell reports it cannot:
  `E_UNSUPPORTED`. How the shell triggers a stock rescan is still an open question in the
  research. Until it is solved the shell answers "unsupported" and the device does not set caps
  bit 4 for mode 0. Clients learn which modes work from the error.
- mode 1 PATHS (1 to 256 NS_SD paths): runs `hibytag dbupdate` for each file, which refreshes
  rows that **already exist** (tags, cover and lrc paths). Files not in the database are
  skipped silently. They need a full rescan or a staged DB.

Reply: `OK` (END) when done.

## 14. ByOS update staging and install

Updates through BTP/1 use the **same** release files, the same Ed25519 check and the same
installer as online updates ([OTA.md](OTA.md)). There is no second installer.

### 14.1 Upload (NS_UPDATE)

The client uploads the two release assets with PUT in ns 1, mode REPLACE:

| Allowed name | Limit | Stored as |
|---|---|---|
| `hibyos-update.json` | 64 KiB | `SD/ByOS/ota/sync-update.json` (kept apart from the OTA page's own verified copy until it is verified) |
| `hibyos-payload-<version>.tar`, where the name passes the OTA `ota_name_ok()` check | 64 MiB (`OTA_PAYLOAD_MAX`) | `SD/ByOS/ota/hibyos-payload-<version>.tar` (exactly where the OTA download leaves it) |

Any other name is `E_BAD_PATH`. The PUT `.part` lives in `SD/ByOS/ota/`, like every other PUT.

### 14.2 UPDATE_STAGE

`flags u8`: bit 0 = ALLOW_DOWNGRADE. The device:

1. Reads `sync-update.json` (at most 64 KiB) and runs the **same** functions as the OTA worker:
   `ota_sig_check_keys()` with the compiled-in `OTA_PUBKEY_HEX`/`OTA_PUBKEY2_HEX`
   (`E_SIGNATURE`), then `ota_parse()` (`E_UPDATE_INVALID`). It does this before using anything
   in the JSON. The code is shared by compiling `ota_net.inc.c` with `OTA_CORE_ONLY` into
   `hibysync`.
2. Checks `product`, `min_version` and `min_firmware`/`needs_firmware` against the installed
   payload and rootfs (`E_NEEDS_FIRMWARE`, `E_UPDATE_INVALID`), and the version against the
   installed one (`E_DOWNGRADE` unless ALLOW_DOWNGRADE; the same version is also `E_DOWNGRADE`).
3. Checks that the payload tar named in the JSON was uploaded, and its size and SHA-256
   (`E_NOT_STAGED`, `E_UPDATE_INVALID`).
4. Renames `sync-update.json` over `SD/ByOS/ota/hibyos-update.json`. It is now the "last
   verified" JSON that the Software Update page shows, re-verified whenever the page opens.

Reply: `UPDATE_STAGE_R(version, restart, needs_firmware, size)`. `restart`: 0 none, 1 shell,
2 reboot, as announced by the JSON. The installer recomputes it from the manifests.

### 14.3 UPDATE_INSTALL and UPDATE_STATUS

`UPDATE_INSTALL(flags)`: bit 0 = NO_CONFIRM, allowed **only on local sessions** (USB); on remote
sessions it is ignored. Bit 1 = ALLOW_DOWNGRADE.

- It requires a staged update: `SD/ByOS/ota/hibyos-update.json` and its tar, from an earlier
  UPDATE_STAGE in this session or in an earlier one. The device runs all the UPDATE_STAGE checks
  again before handing over (`E_NOT_STAGED` when nothing is staged, otherwise the same errors as
  14.2). With ALLOW_DOWNGRADE in flags bit 1, a downgrade is accepted, as in 14.2.
- It requires `SHELL_ATTACHED` (`E_UNSUPPORTED`: the update stays staged, and the user can
  install it from *Software Update*, which finds the verified JSON and the complete tar).
- The daemon sends `UPDATE <version> <confirm|noconfirm>` to the shell (section 18). The shell
  runs its normal OTA install path from the staged files: battery at least 30 % or charging,
  SD checks, size and sha256 again, extraction to `ota/stage`, swap with `ota/previous`,
  `hibyos_install.sh install`, rollback on failure. With `confirm`, it first shows the standard
  OTA confirmation alert, and the user's answer decides.
- Reply: `OK` as soon as the shell accepted the request. Progress comes from `UPDATE_STATUS`.
- While an install runs, every other request except STATUS, UPDATE_STATUS, DF, PING and BYE
  gets `E_BUSY`.

`UPDATE_STATUS_R`:

| Field | Meaning |
|---|---|
| `state` | 0 idle, 1 staged, 2 waiting for the user on the HiBy, 3 installing, 4 done (restart pending), 5 failed, 6 refused by the user |
| `permille` | install progress 0 to 1000 (state 3) |
| `error` | an error code from section 6 when state is 5 (for example `E_BATTERY`, `E_IO`) |
| `version` | the staged or installed version |

When the install needs a **shell restart**, the shell stops `hibysync` (QUIT) and exits, and its
supervisor starts the new one, so the client sees the connection drop after state 4. It then waits for the user (or polls) and reconnects
once the user reopens *ByOS Sync*. For a **reboot**, the shell shows its usual *Restart Now*,
and BTP/1 never reboots the device by itself.

## 15. Device status

`STATUS` returns `STATUS_R`:

| Field | Meaning |
|---|---|
| `battery` | 0 to 100, or 255 when unknown |
| `charging` | 0 or 1 |
| `sd_state` | as in DF_R |
| TLVs | BYOS_VERSION (0x04), FIRMWARE (0x05), optionally SOFTWARE (0x01) |

## 16. Discovery (UDP)

Only while Wi-Fi sync is on.

**PROBE** (client to the broadcast address and to the subnet broadcast, UDP port 7340), 16 bytes:

```
0  4  magic   "BTPD"
4  1  version 1
5  1  type    1 = PROBE
6  2  flags   0
8  8  nonce   random, echoed in the answer
```

**ANNOUNCE** (device to the probe's source address and port; unsolicited announces go to the
broadcast address, UDP port 7341):

```
0  4  magic     "BTPD"
4  1  version   1
5  1  type      2 = ANNOUNCE
6  2  flags     bit 0 PAIRING_OPEN, bit 1 SD_MOUNTED, bit 2 HAS_PEERS
8  8  nonce     the probe's nonce, or zeros when unsolicited
16 2  tcp_port  7340
18 8  device_id first 8 bytes of SHA-256(Ds)
26 1+n name     str8, at most 32 bytes
```

Rules for the device:

- Answer only probes whose source is a private or link-local IPv4 address (10/8, 172.16/12,
  192.168/16, 169.254/16) **on the same subnet as wlan0**.
- Answer at most 10 probes per second. Ignore packets that are not exactly 16 bytes, have the
  wrong magic or version, or have a type other than 1.
- Send one unsolicited ANNOUNCE when Wi-Fi sync is turned on, and one each time pairing opens.
- The socket is bound to every address on port 7340, so a probe sent to the subnet broadcast, to
  255.255.255.255 or **unicast** to the device's address is answered the same way. A client whose
  broadcasts do not arrive (some access points and Wi-Fi power-save modes drop them) can probe
  the addresses of its subnet one by one.

The announce is not authenticated. Clients use it to find the address, and the identity comes
from AUTH (8.3). A client that receives several announces with the same device_id keeps the
most recent address.

Clients on Android need a `WifiManager.MulticastLock` to receive broadcast packets.

## 17. Limits and timeouts

| Item | Value |
|---|---|
| `max_payload` | 65544 (BTP/1, both sides) |
| DATA data length | 1 to 65536 |
| Concurrent sessions | 2 (`E_BUSY`). When both are taken, a new client gets the slot of an open session that has received nothing for 45 s (3/8 of the idle limit) and runs nothing; that session gets `BYE`. A USB client may take any such session; a Wi-Fi client only a Wi-Fi session from its own address (the same phone reconnecting), never a USB session or another host's |
| HELLO deadline after connect | 5 s |
| Pairing: from PAIR_REVEAL to Allow/Deny | 120 s |
| Idle session (no frame received) | 120 s; clients send PING every 30 s while idle. The clock restarts when a job (GET, LIST, ...) ends |
| Remote session from connect to AUTH done / PAIR_REVEAL | 15 s (`E_TIMEOUT`); one such session at a time (2.2) |
| Idle during a PUT (no DATA) | 30 s, then `E_TIMEOUT`, the part is kept, and the session closes |
| Failed pairing attempts | 3 within 10 minutes close the pairing window (the shell must reopen it) |
| Failed AUTH attempts | 5 per minute per source address, then 60 s of `E_RATE_LIMIT` |
| Path | at most 1024 bytes, 255 per component, 32 components |
| Names (HELLO NAME, client name) | at most 64 bytes |
| NS_UPDATE | JSON at most 64 KiB, tar at most 64 MiB |
| NS_DB | at most 64 MiB |
| LIB_RESCAN paths | at most 256 per request |
| Peers stored | 8 |
| Record counter | close before 2^32 records per direction |
| Daemon memory | at most 300 KB RSS idle, at most 1.5 MB while transferring (two 68 KiB frame buffers, one 64 KiB file buffer, no heap growth per file) |

## 18. How the shell runs hibysync

### 18.1 Binary and start

- Binary: `/usr/data/hibymod/shell/hibysync`, next to the other helpers (`hibynet`, `hibytag`,
  `hibyguard`), a static mipsel-musl C program built with zig by `tools/wsl/zbuild_sync.sh`
  (output `bin/hibysync`). It is part of the payload, so OTA updates it. Source: `src/hibysync.c`
  with `src/btp.h`, `src/btp_crypto.inc.c`, `src/btp_proto.inc.c` and `src/ota_net.inc.c`
  (`OTA_CORE_ONLY`).
- **Never started at boot**: S95 and the init scripts do not know about it. The shell starts it
  on demand:
  - *Settings › ByOS Sync* (or *Connect to computer*) opens the page and starts
    `hibysync serve` (loopback only);
  - with the *Wi-Fi sync* toggle on, it starts with `--wifi`; changing the toggle while it runs
    sends `WIFI on|off` (18.3) instead of restarting it;
  - *Pair new device* sends `PAIRING on` (with `--wifi` running).
- Command line used by the shell:

  ```
  hibysync serve --control 3 [--wifi] [--idle-exit SECONDS] [--name NAME]
  ```

  `NAME` is percent-encoded like the control-pipe text (`HiBy%20R3%20II`); the default is
  `HiBy R3 II`. `hibysync id` prints the device id (and creates `device.key` if needed).

  fd 3 is a pipe pair passed by the shell (or a socketpair: one fd for both directions). stdin,
  stdout and stderr go to `/dev/null` and a log file. As with the shell's other children, the
  daemon inherits no other descriptors (the shim's input socket must not stay open).
- The shell starts it with `nice 19` and the idle I/O class (`ioprio_set` IOPRIO_CLASS_IDLE).
  It is in hibyguard's helper list. The daemon alone writes its `oom_score_adj` (the shell's
  resource guard only sets its nice): `-700` while any session is open (USB or Wi-Fi,
  authenticated or not, which covers pairing, transfers and update checks) or the pairing window
  is open, `600` when idle. While `-700` it rewrites the value every 10 s. hibyguard and the
  shell's memory watchers never cut it at `-700`; idle, hibyguard cuts it only when it holds
  more than 1 MB or memory is severe (below 3 MB), and the shell's critical watcher (below
  4.5 MB) stops it, then starts it again after a minute when Wi-Fi sync is on.
- `--idle-exit`: exit after that many seconds with no session (the shell uses 600 when it starts
  the daemon only for USB and the user leaves the page).
- **Fallback without the shell** (USB only): the client MAY start it with
  `adb shell /usr/data/hibymod/shell/hibysync serve --idle-exit 300 --detach`. In that mode there
  is no control pipe (`SHELL_ATTACHED` clear), no Wi-Fi, no pairing, no UPDATE_INSTALL and no
  LIB_RESCAN mode 0. A second instance refuses to start: the lock file
  `/tmp/hibysync.pid` is held with `flock` and the TCP bind fails anyway.

### 18.2 Stop

- The shell writes `QUIT`, waits up to 3 s, then sends SIGTERM, then SIGKILL after another 2 s.
- Control pipe EOF (the shell exited or was killed): with no session open the daemon exits at
  once. With sessions open it keeps serving them, without the shell (no pairing, RESCAN or
  UPDATE_INSTALL; an install waiting for the shell reports state 5 `E_CANCELLED`), closes its
  listeners so it takes no new connection, and exits when the last session closes or when no
  session has had work running (a PUT, a GET or another job, queued requests, unsent data) for
  30 s, whichever comes first; the sessions get `BYE` and the clients reconnect to the daemon of
  the new shell. A client that only sends PING does not keep it alive.
- A second instance (the lock is held) exits with code 75; the shell starts it again 10 s later
  without counting a failed start.
- Shell rejuvenation (the shell replaces itself with `exec`, same process): the daemon is not
  stopped. The control socket is kept across the exec (environment `HIBYSYNC_ADOPT`), the new
  shell takes it over and sends `RESYNC`. Sessions and transfers do not notice. The shell does
  not rejuvenate while a pairing, an install, or a `RESCAN`/`UPDATE` it has read but not handled
  yet is pending.
- On SIGTERM: close the listeners, send `ERROR E_CANCELLED` on running streams and `BYE`, fsync
  open parts, and exit within 1 s.

### 18.3 Control pipe (shell <-> hibysync)

Line-based ASCII, one message per line ending in `\n`, at most 512 bytes, fields separated by
one space. Free text (names, paths) is percent-encoded (`%20` for a space, `%25`, and every byte
outside 0x21 to 0x7E). `<sid>` is a session number given by the daemon.

Daemon to shell:

| Line | Meaning / what the shell does |
|---|---|
| `READY <port> <wifi:0/1> <device_id hex>` | Daemon is listening. The page shows "Ready" with the device name and, with Wi-Fi, the address. |
| `CONN <sid> usb\|wifi <client-name>` | A session opened (Wi-Fi: after AUTH or pairing). The page lists it. |
| `GONE <sid>` | Session closed. |
| `SAS <sid> <6 digits> <client-name>` | **Pairing code**: the shell shows a full-screen alert, "Pair with <client-name>?", the code in large digits grouped 3+3 ("815 365"), the line "Check that the same code is on the other device", and *Deny* / *Allow*, with a 120 s countdown. The code is never shown anywhere else and never logged. |
| `PAIRED <sid> <key_id hex> <client-name>` | Pairing finished. The shell closes the alert and shows a toast. |
| `PAIRFAIL <sid> <reason>` | `rejected`, `timeout`, `commit`, `mismatch` (the client sent BYE after a wrong code), `closed`. The alert closes. |
| `XFER <sid> put\|get <done> <total> <path>` | Progress, at most 4 lines per second. The page shows a progress row. The shell keeps the screen-off timer and power management as they are. |
| `UPDATE <version> confirm\|noconfirm [again]` | Install the staged update (14.3). `again`: repeated in the answer to `RESYNC` because no answer came yet; the shell ignores it when it already has this request. |
| `RESCAN [again]` | Trigger a full library rescan (13.3). `again` as for `UPDATE`. |
| `DBSTAGED <size>` | The page shows "Library ready: restart to apply". |
| `ERR <text>` | Logged in the shell's log. |
| `BYE` | Daemon exits. |

Shell to daemon:

| Line | Meaning |
|---|---|
| `ALLOW <sid>` / `DENY <sid>` | Answer to SAS |
| `PAIRING on\|off` | Open or close the pairing window (closes by itself after 5 minutes or 3 failures) |
| `WIFI on\|off [<idle-exit s>]` | Turn the Wi-Fi listener and discovery on or off without a restart (off closes Wi-Fi sessions with BYE and the pairing window; USB sessions stay). The optional number replaces `--idle-exit` (0 = never). Answered with `READY`. The shell turns Wi-Fi off only when no Wi-Fi client is connected or pairing. |
| `RESYNC` | Answered with `READY`, then one `CONN` line per open session, then `RESCAN again` if a rescan waits for the shell and `UPDATE <version> confirm\|noconfirm again` if an install request waits for it (used by a shell that took the daemon over after an `exec`) |
| `FORGET <key_id hex>` | Remove a peer (open sessions of that peer are closed) |
| `PEERS` | The daemon answers with lines `PEER <key_id hex> <paired time> <name>` and then `PEERS END` |
| `UPDATE_STATE <state> <permille> <error>` | Install progress for UPDATE_STATUS (state numbers as in 14.3) |
| `RESCAN_RESULT ok\|unsupported\|failed` | Result of RESCAN |
| `SD <0..3>` | SD state changed (the shell already watches mounts); the daemon updates HELLO flags and refuses SD operations |
| `QUIT` | Exit now (18.2) |

The shell owns all user interface and every decision that needs the user. `hibysync` never
draws, never reboots, and never installs by itself.

### 18.4 Recipe for the shell (not wired into the UI yet)

1. Create the pipe: `socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)`. Fork; in the
   child move `sv[1]` to fd 3 (without `CLOEXEC`), close every other descriptor, redirect 0-2
   to `/dev/null` and the log (`/tmp/hibysync.log`), `nice(19)`, then
   `execl(".../hibysync", "hibysync", "serve", "--control", "3", ["--wifi",] "--idle-exit", "600", 0)`.
2. In the parent keep `sv[0]` non-blocking in the main poll loop. Wait for `READY` (about 50 ms;
   a missing `READY` after 3 s means the port is taken or the key could not be written: show
   the `ERR` text and kill the child).
3. **Pairing code**: on `SAS <sid> <digits> <name>`, open the full-screen alert described in
   18.3 and answer with `ALLOW <sid>` or `DENY <sid>`. A `PAIRFAIL <sid> ...` or `GONE <sid>`
   closes the alert. Never log the line.
4. Send `SD 2` when the shell switches to USB storage and `SD 0` when it switches back.
5. To stop: write `QUIT`, wait for `BYE`/the child's exit (3 s), then SIGTERM, then SIGKILL after
   2 s. Closing `sv[0]` also stops it (the daemon exits on EOF).
6. Wi-Fi sync toggle: `WIFI on` / `WIFI off 600` to the running daemon (no restart). *Pair new
   device*: `PAIRING on` (accepted only while Wi-Fi is on), and `PAIRING off` when the screen
   closes.
7. Never stop the daemon while it is needed: a session open, a pairing, a transfer or an install,
   or a session opened or closed in the last 2 minutes. Only a start that never reaches `READY`
   counts as a failed start (three of them stop the automatic restarts until the page opens).

### 18.5 Test builds

`-DBTP_TEST` builds (used only by `tests/btp/btp_test.sh`, never shipped) read their paths from
the environment: `HIBYSYNC_SD` (SD root), `HIBYSYNC_DATA` (`/usr/data`), `HIBYSYNC_HM`
(`/usr/data/hibymod`, which holds `sync/`), `HIBYSYNC_MOUNTS`, `HIBYSYNC_HIBYTAG`,
`HIBYSYNC_CLI`, `HIBYSYNC_BATT`, `HIBYSYNC_USB`, `HIBYSYNC_LOCK`, `HIBYSYNC_RESCONF`,
`HIBYSYNC_PORT`, and `HIBYSYNC_WIFI_ADDR` (a fixed address such as `127.0.0.2` that stands for
wlan0, so remote sessions, pairing, AUTH and discovery can be tested on loopback), and
`HIBYSYNC_IDLE_S`, `HIBYSYNC_PUT_IDLE_S`, `HIBYSYNC_SECURE_S` (shorter timeouts). Test builds
also skip the `st_dev` mount-point check and accept 127/8 discovery probes. Release builds have
none of these hooks.

### 18.6 Implementation status (hibysync 0.1.0)

- Implemented and tested on the PC: everything in sections 2 to 17 except what is listed here.
- `hibytag dbsnapshot` and `hibytag dbcheck` (13.1, 13.2) do not exist in `src/hibytag.c` yet.
  Until they do, DB_SNAPSHOT answers `E_IO` and DB_STAGE `E_DB_INVALID` on the device (the PC
  tests use a stub `hibytag`). If the binary is missing the answer is `E_UNSUPPORTED`.
- The display shim does not apply `staged.db` yet (13.2 step 5).
- Long work inside one request still blocks the event loop: the payload tar hash of
  `UPDATE_STAGE`/`UPDATE_INSTALL` (a few seconds for 64 MiB), the `hibytag` helpers
  (`dbcheck`, `dbsnapshot`, `dbupdate`), the live-DB hash of `DB_STATUS`/`DB_STAGE`, and a
  recursive `DELETE`. The time is credited back to every session's idle, PUT-idle, HELLO,
  securing and pairing clocks, so no session is timed out because of it, and SIGTERM stops the
  helper or the hash at once. Control-pipe lines (ALLOW, QUIT...) wait until the request ends.
  The other hashes run as jobs, a few MiB per poll turn: PUT `SKIP_IDENTICAL`, the rehash of a
  kept part on resume, and the prefix of a GET resumed with a hash.
- Two sessions sending the same PUT (same `.part`): the newer `PUT_BEGIN` takes the part over
  and the older put gets `E_CANCELLED` (its data is kept and synced first).
- UPDATE_INSTALL right after UPDATE_STAGE does not hash the payload tar again when it is the same
  file (same inode, size and mtime as the one UPDATE_STAGE verified).
- Session buffers are given back with `MADV_DONTNEED` on close (PC check: anonymous RSS of a
  static x86 build goes from 164 kB with a session after a 1 MiB PUT to 92 kB after close).
- No device measurements yet: RSS, throughput, XSalsa20-Poly1305 cost on the X1600, renameat2
  on vfat (section 21).

## 19. Test vectors

`python tools/btp_test_vectors.py` writes `tests/btp/vectors.json`. It uses the standard library
only, and its output is identical on every run. It contains:

- `primitives`: X25519 (RFC 7748), HSalsa20 (NaCl), Poly1305 (RFC 8439), secretbox, and
  HKDF-SHA256 (RFC 5869). The script checks its own implementations against the published
  values before it writes anything (`--check` runs only the checks).
- `frames`: one or more frames of every message type, with their decoded fields, including a
  full 64 KiB DATA frame (given by its SHA-256) and a resume at offset 65536.
- `invalid_frames`: inputs a receiver must reject, and the expected error.
- `paths`: valid paths and invalid ones, with the reason names of 9.2.
- `part_files` and `resume_offsets`: PUT part-file names (11.2 step 2) and the durable resume
  offset for given part sizes (11.2 step 4).
- `discovery`: PROBE and ANNOUNCE packets.
- `pairing`: fixed static and ephemeral keys, the commitment, the transcript hash, the shared
  secret, PRK, the SAS (`815365`), both keys, the cleartext frames, the encrypted records with
  their nonces, and one tampered record that must be rejected.
- `auth`: the same for a reconnect (the three DH values, the transcript hash, keys, and an
  encrypted PUT exchange).
- `sas_examples`: SAS derivation from given PRKs.

The C implementation (checked by `tests/btp/btp_test.c`, run with `tests/btp/btp_test.sh`) and the Kotlin core
(`core/src/test/.../BtpVectorsTest.kt`) MUST load this file and reproduce every byte. A change
to this specification that changes bytes MUST change the script, regenerate the file and bump
the protocol version if deployed peers would break.

## 20. Security notes

- **Wi-Fi ADB defeats all of this.** If the stock adbd on TCP 5555 accepts unauthenticated root
  connections (open question 12 of the research), anyone on the LAN already has root, and BTP/1
  encryption protects nothing against them. Wi-Fi ADB must be off by default, or checked, before
  Wi-Fi sync is advertised as secure.
- Local sessions are not authenticated. Any process on the HiBy can connect to
  `127.0.0.1:7340`. ByOS runs everything as root anyway, so this adds no new access. Over USB,
  ADB authorisation (if any) is the gate.
- The SAS gives about 20 bits: a man in the middle succeeds with probability 10^-6 per attempt,
  and the attempt limits (section 17) bound the attempts. The SAS value is slightly non-uniform
  (2^32 mod 10^6), which is negligible.
- No compression is used, so there are no compression oracles. Record lengths leak file sizes
  to an observer on the Wi-Fi, which is accepted.
- Updates are trusted only through the Ed25519 release signature, exactly as for OTA. A paired
  client cannot install anything that is not signed by a ByOS release key. A paired client
  **can** read, write and delete anything on the SD, and stage a library database.
- The path rules and `openat`/`O_NOFOLLOW` confinement make symlink and `..` tricks fail even
  for a paired client. The staging namespaces accept only fixed names.
- Secrets (`ds`, ephemeral keys, session keys, the SAS) are never logged. Key material is wiped
  from memory after use. The comparisons of tags, commitments and codes are constant-time.

## 21. Still to verify on the device

These do not change the bytes on the wire, but they decide whether parts of the protocol can
be enabled:

- Throughput of TCP over `adb forward` and over Wi-Fi, and the cost of XSalsa20-Poly1305 per MB
  on the X1600 (TweetNaCl is slow; a faster C implementation of the same primitives would keep
  the bytes identical).
- `renameat2(RENAME_NOREPLACE)` on this kernel and vfat (probably absent; the lock fallback of
  section 10 is used).
- How to trigger a stock library rescan from the shell (LIB_RESCAN mode 0).
- Whether the shim can swap a staged DB before `main()` safely, and how much space `/usr/data`
  really has (13.2).
- Whether the stock player rescans on boot or on SD mount and overwrites a PC-built DB.
- The adbd authentication question in section 20.
