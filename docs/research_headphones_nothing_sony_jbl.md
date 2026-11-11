# Headphone control protocols: Nothing / CMF, Sony, JBL, and generic fallbacks (HiBy R3 II)

Date: 2026-11-10. Research and written spec only. Nothing was run on the device, `src/` was not changed and nothing was committed.

This document extends `docs/research_codecs_earbuds.md` §3 (AirPods AAP, Galaxy Buds). It targets the brand modules of the `hibybudsd` daemon: a static musl binary that opens raw RFCOMM / L2CAP sockets (and, for JBL, perhaps LE ATT) next to the A2DP stream, publishes state in `/dev/shm/hibybuds`, and takes commands from the shell over a Unix datagram socket.

## Evidence tags and clean-room rules

| Tag | Meaning |
|---|---|
| **[X]** | Cross-checked: two independent public sources agree, or we recomputed it ourselves (for example a CRC checked against a published capture). |
| **[P]** | Published by one reverse-engineering project (enum, notes, issue thread). Believed correct, not yet seen on our own hardware. |
| **[I]** | Our inference from the published facts. |
| **[D]** | Has to be confirmed with a capture from the owner's hardware, or on the R3 II. |

**Clean room.** The main references are copyleft or have no licence:

| Project | Licence | What we took |
|---|---|---|
| Gadgetbridge (codeberg.org/Freeyourgadget/Gadgetbridge), Nothing + Sony modules | AGPL-3.0 | enum values, byte positions, behaviour notes |
| ear (web), github.com/radiance-project/ear-web | AGPL-3.0 | command numbers, payload layouts, model table (its `ear_config_file.json` is Nothing's own app config) |
| JBLProtocol4, github.com/NeoHBz/JBLProtocol4 | MIT | its written spec `notes/protocol.md` |
| race-toolkit, github.com/auracast-research/race-toolkit | none stated | RACE header layout and UUIDs |
| Google Fast Pair specification (developers.google.com/nearby/fast-pair) | public spec | advert and Message Stream formats |

Byte layouts, opcodes and enum values are facts, so they can be used. **No code from these projects may be copied** into ByOS (our "ByOS Community License 1.0" is incompatible with GPL/AGPL). Everything below is written in our own words. Test vectors must come from **our own captures**, not from their fixtures.

---

## 0. Verdict

| Brand | Transport | Spec completeness | Feasible on our stack | Effort | Verdict |
|---|---|---|---|---|---|
| **Nothing / CMF** | RFCOMM, UUID `aeac4a03-dff5-498f-843a-34487cf133eb` | High. Framing and CRC recomputed against a published capture [X]; command table from two projects | **Yes**. Same pattern as Galaxy Buds | 4–5 d + captures | **Go.** Battery L/R/case, ANC levels, presets + custom/advanced EQ, gestures, in-ear, low latency, find, firmware |
| **Sony** (MDR) | RFCOMM, UUID V1 `96CC203E-…` / V2 `956C7B26-…` | High for V1 (XM3/XM4), medium for V2 (XM5, LinkBuds) | **Yes** | 4–5 d + captures | **Go.** Battery, ANC/ambient level/focus on voice, EQ presets + Clear Bass + 5 bands, buttons, speak-to-chat, DSEE, LDAC priority, codec readout |
| **JBL / Harman** | several families: BLE GATT "Protocol4" (newer), BLE GATT (Live Pro 2/Tune Flex), BES and Airoha variants, older SPP "AA" framing | Partial. Only Protocol4 has a public spec, verified on one model | **Partly.** Needs an LE ATT link and likely an LE bond next to the A2DP link; the owner's model is unknown | 2 d after a capture (Protocol4), more for others | **Capture first.** Ship generic battery first; add JBL control per model once the owner's capture tells us the family |
| **Generic** (any headset) | HFP AT (needs `hfp-ag`), Fast Pair Message Stream (RFCOMM), BLE adverts 0xFE2C, GATT Battery Service | Public specs | **Yes**, with trade-offs | 2–4 d | **Go**, as the fallback layer: Fast Pair Message Stream first (no HFP side effects), HFP-AG second |

---

## 1. Shared building blocks for all brand modules

### 1.1 Finding the RFCOMM channel
- Vendor channels are not on fixed numbers. Example: the Nothing Ear (2) answered on channel 15 in one write-up, but this can change between firmware versions [P]. Always resolve through SDP.
- **Option A (simple):** fork `sdptool browse <mac>` once after the first connection [V tool present, see `research_codecs_earbuds.md` §3.1]. Parse each record's 128-bit "Service Class ID" and its "RFCOMM channel". Cache them in `/usr/data/hibymod/buds/<mac>.sdp`.
- **Option B (in-daemon):** a minimal SDP client over L2CAP PSM 1, with one `ServiceSearchAttributeRequest` (UUID128, attribute 0x0004 ProtocolDescriptorList). About 200 lines; no fork.
- On a failed `connect()` (ECONNREFUSED or EHOSTDOWN), drop the cache and look the channel up again once.

### 1.2 Socket recipe (all RFCOMM brands)
```
fd = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM)
setsockopt(fd, SOL_BLUETOOTH, BT_SECURITY, {level = BT_SECURITY_MEDIUM})   // encrypted link, keys from the existing bond
connect(fd, {AF_BLUETOOTH, bdaddr = <bud MAC, byte-reversed>, channel = <from SDP>})
```
- Only connect **after** A2DP is up (`Device1.Connected = true` and a bluealsa PCM exists), plus a **500 ms** delay. Gadgetbridge reports that Sony fails with an I/O error if the socket opens too early [P]. Use the same delay for every brand.
- RFCOMM is a byte stream: messages can be split or merged. Every brand module needs a resynchronising frame parser (look for the start byte, check length and CRC, otherwise drop one byte and retry).
- Air time is negligible: a few small packets per user action, on the ACL link that A2DP already uses. No audio stutter is expected [I].

### 1.3 Model identification order
1. `Device1.UUIDs` (from SDP, through BlueZ): this picks the **brand module**.
2. `Device1.Name` pattern: this picks the **model**.
3. In-protocol identity (firmware string, product ID, Fast Pair model ID): this confirms it and picks the **feature set**.
4. `Device1.Modalias` (`bluetooth:vXXXXpYYYY…`) when present [D per brand].

### 1.4 One control client at a time
- Nothing, Sony and JBL all appear to accept a **single** control client on their vendor channel. JBLProtocol4 observed this for GATT; for the others it is likely [I].
- If the owner's phone runs the vendor app over multipoint at the same time, our `connect()` may be refused or may steal the session.
- Rule: on refusal, back off (5 s, 30 s, 2 min) and show "Controlled by another device" on the headphone card. Never loop faster.

---

## 2. Nothing and CMF

### 2.1 Transport [X]
- **RFCOMM (SPP-style)**, service UUID **`aeac4a03-dff5-498f-843a-34487cf133eb`**. Both Gadgetbridge and ear (web) use this UUID; the Nothing app does too.
- The same devices also expose the **Fast Pair Message Stream** on RFCOMM UUID `df21fe2c-2515-4fdb-8886-f12c4d67927c` (ear (web) lists it; see §5.3). It is a useful fallback for battery.
- Nothing's own model config has a flag `isUseGattOverBREDR` = 1 on newer models (Ear (a), Ear, CMF Buds Pro 2, Headphone (1)…) [P]. That suggests the app may use GATT over BR/EDR on them. ear (web) still talks to those models over the SPP UUID, so **we use RFCOMM for every model** [I], and verify on the owner's buds [D].

### 2.2 Frame layout [X]
All multi-byte fields are **little-endian**.

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | SOF | always `0x55` |
| 1 | 2 | control (LE16) | see below |
| 3 | 2 | command (LE16) | e.g. `0xC007` is sent as `07 C0` |
| 5 | 2 | payload length (LE16) | payload bytes only |
| 7 | 1 | sequence / operation id | host counter; replies echo it (ear (web) uses it to match replies) [P] |
| 8 | n | payload | |
| 8+n | 2 | CRC (LE16) | only if control bit `0x0020` is set |

**Control word bits** [P, Gadgetbridge masks; ear (web) values]

| Bits | Meaning |
|---|---|
| `0x001F` | response code. Must be 0 in a good reply |
| `0x0020` | CRC present |
| `0x0040` | "sequence counter in use". ear (web) always sets it; Gadgetbridge sets it only for Ear (a), Ear (stick), CMF Buds 2a, and leaves it clear for Ear (1)/(2)/CMF Buds 2/Pro 2. Both work in practice [P] |
| `0x0F00` | device type. `1` = TWS headset |
| `0x5000` | Gadgetbridge sets this extra pattern only on the battery request (control `0x5120`). Meaning unknown; `0x0120`/`0x0160` also work for battery in ear (web) [P] |

- Host frames: use control **`0x0160`** (bytes `60 01`): CRC + counter + type 1. This is what the published captures show.
- Device frames come as `0x0100` (no CRC, e.g. the ear-fit result) or `0x0160` (with CRC). Check bit `0x20` before reading a CRC.

**CRC = CRC-16/MODBUS** [X]
- Polynomial 0x8005, reflected (0xA001), init 0xFFFF, no final XOR. It covers bytes 0..7+n, i.e. SOF, header and payload. It is stored little-endian.
- We checked it ourselves against four frames published by the Ear (2) Linux write-up, for example ANC "transparency":
  - `55 60 01 0F F0 03 00 CB 01 07 00` → CRC `0xAFC5`, sent as `C5 AF`. ✓
- Our own computed vectors (control `0x0120`) to seed unit tests:

  | Purpose | Frame |
  |---|---|
  | battery get, seq 1 | `55 20 01 07 C0 00 00 01 ED 1B` |
  | ANC high, seq 2 | `55 20 01 0F F0 03 00 02 01 01 00 FB 47` |
  | ring left bud, seq 3 | `55 20 01 02 F0 02 00 03 02 01 45 0B` |
  | firmware get, seq 4 | `55 20 01 42 C0 00 00 04 E0 D7` |

**Command-number convention** [I, consistent across all observed traffic]

| Range | Meaning |
|---|---|
| `0xCxxx` | host **get** |
| `0x4xxx` | the device's reply to it (bit 15 cleared, low 12 bits equal) |
| `0xFxxx` | host **set** |
| `0x7xxx` | the device's echo/ack of a set |
| `0xExxx` (or `0x6xxx`) | unsolicited **push** from the device |

`0xF002` is both "ring" (host to device) and an unknown device frame in Gadgetbridge's notes. Dispatch on direction, not on the number alone.

### 2.3 Command table

| Feature | Get → reply | Set | Push | Payload [source] |
|---|---|---|---|---|
| Battery | `C007` → `4007` | — | `E001` | `count`, then count × {`id`, `val`}. id: `02` L, `03` R, `04` case, `06` single-unit headphone. `val`: bit 7 = charging, bits 0–6 = percent. Missing ids = disconnected [X] |
| Noise control | `C01E` → `401E` | `F00F` {`01`, mode, `00`} | `E003` | mode: `01` high ANC, `02` mid, `03` low/"light", `04` adaptive, `05` off, `07` transparency. Status reply: mode at payload[1] [X] |
| Personalised ANC (Ear (2)) | `C020` → `4020` | `F011` {on} | | [P] |
| EQ preset, Ear line | `C01F` → `401F` | `F010` {preset, `00`} | | preset: `00` Balanced, `01` Voice, `02` More Treble, `03` More Bass, `05` Custom. "Advanced" is a separate flag (below) [P] |
| EQ preset, CMF line ("listening mode") | `C050` → `4050` | `F01D` {preset, `00`} | | `01` Rock, `02` Electronic, `03` Pop, `04` Enhance Vocals, `05` Classical, `06` Custom, `07` Dirac/Balanced [X: Gadgetbridge enum + ear (web)] |
| Custom EQ (3-band "simple") | `C044` → `4044` | `F041` | | see §2.4 [P] |
| Advanced EQ on/off | `C04C` → `404C` | `F04F` {on, `00`} | | [P] |
| Advanced EQ bands | `C04D` {profile=`FF`} → `404D` | `F050` | | see §2.4 [P] |
| "Precision driver" EQ (3-driver models) | `C06C`/`C06D` | `F06C`/`F06D` | | same band format [P] |
| Ultra Bass / bass enhance | `C04E` → `404E` | `F051` {on, level} | | level on the wire = UI level × 2 (1..5 → 2..10). On B186/B189/B195/B197: 3 steps, wire `0`/`5`/`10` [X] |
| Spatial audio | `C04F` → `404F` | `F052` | | Gadgetbridge sends {on}; ear (web) sends {mode, head-tracking} on newer models [P, check per model D] |
| In-ear detection | `C00E` → `400E` | `F004` {`01`, `01`, on} | | reply: enabled flag at payload[2] [X] |
| Low latency | `C041` → `4041` | `F040` {state, `00`} | | `01` on, `02` off [X] |
| Gestures | `C018` → `4018` | `F003` {`01`, side, `01`, gesture, action} | | reply: `count`, then count × {side, `01`, gesture, action} [X] |
| Find my earbud | — | `F002` | | Ear (1): {`01`} on / {`00`} off. TWS models: {side `02`/`03`, on/off}. Over-ear (B164/B170/B175/B186/B192): {`06`, on/off} [P] |
| Ear-tip fit test | — | `F014` {`01`} | `E00D` {L, R} | `00` good seal, `01` poor [X] |
| Firmware version | `C042` → `4042` | | | ASCII, e.g. `1.0.1.101` [X] |
| Device info / serial | `C006` → `4006` | | | ASCII block that includes the serial [P] |
| Clock (UTC) | — | `F00A` {u32 **big-endian** epoch seconds} | | not on Ear (1) [P] |
| High-quality codec (LDAC) | `C029` → `4029` | `F01C` {index} | | [P] |
| Dual connection | `C027`/`C028` | `F01A` {on} / `F01B` {connect, MAC×6} | `E00E` | [P] |
| Ear (1) case LED colours | `C017` → `4017` | `F00D` {`05`, 5 × {idx, R, G, B}} | | Ear (1) only [P] |
| Case closed with buds inside | | | `E002` (twice) | [P] |

**Gesture codes** [X: Gadgetbridge enum and Nothing config agree]
- side: `02` left, `03` right. Single-unit headphones use other ids (`04`/`06`) [D].
- gesture: `01` single tap/press, `02` double, `03` triple, `07` tap-and-hold, `09` double-tap-and-hold, `05` slide (stem volume), `0A` rotate/dial, `0B` pinch both.
- action:

  | Code | Action | Code | Action |
  |---|---|---|---|
  | `01` | none | `11` | low-latency toggle |
  | `02` | play/pause | `12` | volume up |
  | `03` | answer/hang up | `13` | volume down |
  | `04` | decline | `17` | volume control |
  | `08` | previous | `18` | camera shutter |
  | `09` | next | `1B` | spatial audio |
  | `0A` | noise control: ANC/transparency/off cycle | `1D` | mic mute |
  | `14` | noise control: ANC/off | `22` | EQ preset |
  | `15` | noise control: transparency/off | `23` | Ultra Bass |
  | `16` | noise control: ANC/transparency | `0B` | voice assistant |

  Not every action is allowed on every gesture. The Nothing config lists the allowed set per model and gesture; we keep a small table per model.

### 2.4 EQ payloads [P]
**Floats are IEEE-754 float32, little-endian.**

**Custom (3-band) EQ, `F041` / reply `4044`**
```
u8   band_count (=3)
f32  total_gain            = -max(0, gains...)   (pre-gain so the sum does not clip)
3 x { u8 filter_type ; f32 gain_dB ; f32 freq_Hz ; f32 Q }
3 x { 00 00 00 }           (3 zero bytes per band, trailing)
```
- filter_type: `0` low shelf, `1` peak, `2` high shelf.
- Defaults from Nothing's config: low shelf 140 Hz Q 0.8; peak 980 Hz Q 0.7 (0.66 on some models); high shelf 3400, 3500 or 6900 Hz Q 1.0, per model.
- UI: three sliders "Bass / Mid / Treble". The range is probably ±6 dB [D].

**Advanced EQ, `F050` / reply `404D`**
```
u8   profile_index (0 when setting; FF when asking for the current one)
u8   band_count (8)
f32  total_gain
8 x { u8 filter_type (1 = peak) ; f32 gain ; f32 freq ; f32 Q }
```
- Default centre frequencies: 55, 110, 220, 440, 1320, 3300, 6600, 13200 Hz. Each band may move inside its own decade (e.g. 20–99, 100–199, …, 12000–20000).
- Enable it with `F04F {01,00}` before sending bands.
- Mutual exclusion: on models flagged `mutuallyExclusive`, Spatial Audio cannot be on together with Advanced EQ or bass enhance. Grey out the other control.

### 2.5 Per-model matrix
Internal codes and Fast Pair model IDs come from ear (web)'s tables. Capability bits come from Nothing's config file. Gadgetbridge notes are added where they differ.

| Model | Code | Fast Pair IDs (examples) | ANC modes | EQ | Custom / Adv. EQ | Bass | In-ear | Low lat. | Find | Other |
|---|---|---|---|---|---|---|---|---|---|---|
| Ear (1) | B181 | `31D53D`, `624011` | high, light, transparency, off | 4 presets (`F010`) | — / — | — | ✅ | fw-dependent | ✅ {01} | case LED, no UTC |
| Ear (stick) | B157 | `1016DD` | **none** (status read only on fw x.x.2.x) | presets | — / ✅ (fw ≥ 1.80) | — | ✅ (GB) | — | — | half-in-ear |
| Ear (2) | B155 | `ACC520`, `DEE8C0` | high, mid, low, adaptive, transparency, off | presets + custom | ✅ / ✅ (fw ≥ 1.0.1.95) | — | ✅ | ✅ | ✅ per side | personalised ANC, fit test, LDAC, dual-connect |
| Ear (2024, "Ear") | B171 | `FEB1C7`, `A20444` | all 6 | presets + custom | ✅ / ✅ | ✅ | ✅ | ✅ | ✅ | fit test, LDAC |
| Ear (a) | B162 | `5E3FBC`, `03464E`… | all 6 | presets + custom | ✅ / — | ✅ | ✅ | ✅ | ✅ | fit test, LDAC |
| Ear (open) | B174 | `FC3AAF`, `CC3444` | **none** (open-ear) | presets | ✅ / ✅ | — | **— (no sensor)** | ✅ | ✅ | |
| Ear (3) / Ear (3a) | B173 / B190 | `7D46E5`… / `7B2328`… | all 6 | presets + custom | ✅ / ✅ | B173 ✅ | ✅ | ✅ | ✅ | spatial, fit test |
| CMF Buds | B168 | `D35E18`… | ANC, transparency, off | listening modes (`F01D`) | — | ✅ | ✅ | ✅ | ✅ | |
| CMF Buds Pro | B163 | `5F8F82`, `2EB1CA`, `ADD2C4` | high, mid, low, transparency, off | presets (`F010`) | — | — | ✅ | ✅ | ✅ | |
| CMF Buds Pro 2 | B172, B195 (Bluetrum chip) | `F29566`… / `F51D3D`… | all 6 | listening modes incl. Custom, Dirac | custom ✅ | ✅ (B195: 3 steps) | ✅ | ✅ | ✅ | spatial, LDAC, fit test, case dial (not supported) |
| CMF Buds 2 / 2 Plus / 2a | B179, B197 / B184 / B185 | | 2a: ANC, transparency, off; others all | listening modes | custom (2, 2 Plus) | ✅ | 2a: — | ✅ | ✅ | |
| Headphone (1) / (a) | B170 / B186 | | all | presets | ✅ / ✅ | ✅ | wear ✅ (B170) | ✅ | {06,on} | single battery id `06` |

**ANC capability bitmask** (config field `ancLevel`) [I]. It decodes consistently as:

| Bit | Mode | Bit | Mode |
|---|---|---|---|
| 0 | high | 3 | adaptive |
| 1 | mid | 4 | transparency |
| 2 | low | 5 | off |

Examples: 53 = Ear (1); 49 = CMF Buds; 63 = all modes. We encode the per-model mode list explicitly, so this decoding is not needed at runtime.

### 2.6 Connection state machine
```
A2DP up ──500 ms──► SDP lookup (cached) ──► RFCOMM connect(aeac4a03 channel)
   │ fail: backoff 5 s/30 s/2 min (max 3 tries per A2DP session)
   ▼
INIT (≈100 ms between frames, one in flight is safest):
   C042 firmware → pick feature table by name + firmware
   C007 battery
   C01E noise control
   C01F or C050 EQ (per model line), C04C advanced flag, C04E bass
   C00E in-ear, C041 low latency, C018 gestures
   F00A UTC time (not Ear (1))
   ▼
READY: pushes E001 battery / E003 ANC / E00D fit / E002 case;
       battery poll C007 every 5 min (some fw push only on change)
   ▼
A2DP down or socket EOF ──► close, clear state, show last values greyed for 10 s
```
- Replies may arrive out of order. Match them by the sequence byte (and by the reply number), not by order.
- **No keep-alive is needed** [P]. If no reply comes within 2 s, retry once, then mark the feature unsupported for this session.

### 2.7 Pitfalls
- **Firmware gating.** Nothing changes features by firmware: config ranges like `*.*.1.75`–`*.*.1.85` for Ear (1), and Advanced EQ only from fw 1.0.1.95 on Ear (2). Always read the firmware first.
- **The second sequence flag** (`0x40`). If a model ignores frames, retry once with the other control value (`0x0120` vs `0x0160`) and remember which one works per model [D].
- **Ear (stick)** has no ANC. Do not send `F00F`.
- The **Spatial Audio vs Advanced EQ/bass** exclusion (§2.4).
- LDAC toggle (`F01C`) and dual-connection changes **reboot or reconnect the buds**. ear (web) reconnects after 10 s with 6 retries. Our UI must warn and must expect an A2DP drop. Our source never sends LDAC (HiBy's bluealsa does send LDAC, but the buds' LDAC mode can force a renegotiation) [D].
- The UTC field is **big-endian** while everything else is little-endian.

### 2.8 Capture and test plan (owner's Nothing buds)
1. **Android HCI snoop** (§6.1) while the Nothing X app goes through: open app; battery; each ANC mode; each EQ preset; custom EQ (each slider to min and max); advanced EQ (one band); each gesture remap and back; in-ear on/off; low latency on/off; find left/right; fit test.
2. **Wireshark:** `btrfcomm && frame contains 55:60:01`. Export host and device frames to CSV and build **our own** test-vector file `tests/buds/nothing_<model>.txt`.
3. **Off-device unit test** (PC build of the parser): CRC, reassembly of split frames, every reply in the capture decodes to the value the app showed.
4. **On the R3 II**, read-only first: battery, ANC status, firmware. Then each set command, checking the effect by ear and in the app later.

---

## 3. Sony (MDR protocol)

### 3.1 Transport [X]
- RFCOMM.
- **V1** service UUID **`96CC203E-5068-46AD-B32D-E316F5E069BA`**: WH-1000XM2/XM3/XM4, WF-1000XM3/XM4, WF-SP800N…
- **V2** service UUID **`956C7B26-D49A-4BA8-B03F-B17D393CB6E2`**: WH-1000XM5, WF-1000XM5, LinkBuds, LinkBuds S, newer WF-C/WH-CH models.
- Selection: if the SDP UUID list holds V2, use V2; else V1; else guess by model [P].
- The **payload dialect** (V1 vs V2 tables) is chosen from the device's answer to the init (§3.4). The UUID alone is not enough.
- Separately, some Sony models (Airoha AB158x chips) expose an Airoha **RACE** channel over GATT `dc405470-a351-4a59-97d8-2e2e3b207fbb` and RFCOMM `8901DFA8-5C7E-4D8F-9F0C-C2B70683F5F0`. We do **not** use it (it is a factory/debug channel, see §4.4).

### 3.2 Frame layout [X]
```
3E | type | seq | len (u32 BIG-endian) | payload[len] | checksum | 3C
```
| Field | Value |
|---|---|
| `3E` / `3C` | start and end markers, never escaped |
| `type` | `01` ACK, `0C` command/data ("MDR"), `0E` command set 2 ("MDR no. 2": voice-guidance settings and similar) |
| `seq` | `0` or `1` (see ACK rules) |
| checksum | sum of all bytes from `type` to the end of payload, modulo 256 |

**Escaping** (applied to type, seq, len, payload and checksum)
- Any byte `3C`, `3D` or `3E` is sent as `3D` followed by `byte & 0xEF`. So `3E` becomes `3D 2E`.
- To decode, drop the `3D` and OR the next byte with `0x10`.

Size check after unescaping: total = len + 9.

**ACK rules** [P]
- For every `0C`/`0E` frame received with sequence *s*, send an ACK frame with seq **1−s**, type `01`, length 0.
- The device ACKs our commands the same way. Our next command uses the seq taken from the last ACK received.
- Keep **one command in flight**: send the next one only after its ACK.

Our own computed frames (unit-test seeds):

| Frame | Bytes |
|---|---|
| init | `3E 0C 00 00 00 00 02 00 00 0E 3C` |
| ACK with seq 1 | `3E 01 01 00 00 00 00 02 3C` |
| battery get (dual), seq 1 | `3E 0C 01 00 00 00 02 10 01 20 3C` |
| NC on (V1, wind-capable layout) | `3E 0C 00 00 00 00 08 68 02 11 02 02 01 00 00 94 3C` |

### 3.3 Payload tables
The first payload byte is the **message id**. Get/reply/set/notify come in families of four (`x6`/`x7`/`x8`/`x9`).

**V1 ids** [P, Gadgetbridge enum]

| Function | Get | Reply | Set | Notify |
|---|---|---|---|---|
| init / protocol info | `00` | `01` | | |
| firmware / device info | `04`, `06` | `05`, `07` | | |
| battery | `10` | `11` | | `13` |
| audio codec | `18` | `19` | | `1B` |
| power off | | | `22` | |
| sound position / surround | `46` | `47` | `48` | `49` |
| equalizer | `56` | `57` | `58` | `59` |
| ambient sound control (NC/ASM) | `66` | `67` | `68` | `69` |
| NC optimizer | `86` | `87` / `85` status | `84` start | `89` |
| volume | `A6` | `A7` | `A8` | `A9` |
| touch sensor / NC button function | `D6` | `D7` | `D8` | `D9` |
| upsampling (DSEE) + LDAC priority | `E6` | `E7` | `E8` | `E9` |
| auto power off / pause when taken off / speak-to-chat on / button modes | `F6` | `F7` | `F8` | `F9` |
| speak-to-chat config | `FA` | `FB` | `FC` | `FD` |
| voice notifications (type `0E`) | `46` | `47` | `48` | `49` |

**V2 ids** that differ from V1 [P]

| Function | Get | Reply | Set | Notify |
|---|---|---|---|---|
| codec | `12` | `13` | | `15` |
| battery | `22` | `23` | power `24` | `25` |
| auto power off | `26` | `27` | `28` | `29` |
| system control (reboot, multipoint commit) | `96` | `97` | `98` | `99` |
| service link | `B6` | `B7` | `B8` | `B9` |
| factory reset (**never send**) | | | `F8` (in V2) | |
| NC/ambient button mode | `FA` | `FB` | `FC` | `FD` |

V2 reuses V1 ids `56/58` (EQ), `66/68` (ASM), `D6/D8`, `E6/E8`, `F6/F8`, `FA/FC` with different sub-selectors (below).

**Battery** [P]
- Request: `{10, type}` in V1 / `{22, type}` in V2.
  - V1 types: `00` single, `01` L+R, `02` case.
  - V2 types: `00` single, `01` or `09` L+R (model-dependent), `0A` case [D which one each model wants].
- Reply:
  - single or case: `{id, type, level, charging}`;
  - dual: `{id, type, L level, L charging, R level, R charging}`.
- `level` is a percentage; charging is `01`. A level of 0 means that bud is not connected.

**Ambient sound control** (NC / ambient / wind, focus on voice, level)

V1, 8 bytes:

| Byte | Value |
|---|---|
| 0 | `68` (set) |
| 1 | `02` |
| 2 | `00` off / `11` on |
| 3 | `02` on wind-capable models (XM3, WF-1000XM3/XM4), else `00` |
| 4 | mode. Wind-capable: `02` NC, `01` wind reduction, `00` ambient. Others: `01` NC, `00` ambient |
| 5 | `01` |
| 6 | focus on voice `00`/`01` |
| 7 | ambient level `00`–`14` (0–20). Ignored for NC/wind |

V2:

| Byte | Value |
|---|---|
| 0 | `68` |
| 1 | `17` (models with wind/"ASC2") or `15` |
| 2 | `01` |
| 3 | enable `00`/`01` |
| 4 | `01` ambient / `00` NC |
| 5 | (wind layout only) `02`, or `03` for wind reduction |
| next | focus on voice |
| last | level 0–20 |

- Notifications (`69`) have the same layout. Parse them by length and by byte 1.
- Adaptive / "auto NC" on XM5-class models is **not documented publicly** [D capture].

**Equalizer** [P]
- Preset set: V1 `{58, 01, preset, 00}`; V2 `{58, 00, preset, 00}`.
- Preset ids:

  | Id | Preset | Id | Preset |
  |---|---|---|---|
  | `00` | off | `15` | treble boost |
  | `10` | bright | `16` | bass boost |
  | `11` | excited | `17` | speech |
  | `12` | mellow | `A0` | manual |
  | `13` | relaxed | `A1` | custom 1 |
  | `14` | vocal | `A2` | custom 2 |

- Custom bands set: V1 `{58, 01, FF, 06, CB, b1, b2, b3, b4, b5}`; V2 `{58, 00, A0, 06, CB, b1…b5}`.
  - Each value is dB + 10 (−10…+10 dB becomes `00`…`14`).
  - CB is **Clear Bass**. The bands are 400 Hz, 1 kHz, 2.5 kHz, 6.3 kHz and 16 kHz [I, Sony app labels].
- Reply (`57`/`59`, 10 bytes): preset at [2], Clear Bass at [4], bands at [5..9].
- WF-1000XM4 has a "simple" EQ variant [P, D].

**Touch / button assignment** [P]
- V1 TWS: `{F8, 06, 02, left, right}`.
- V2: `{F8, 03, 02, left, right}`.
- Values: `00` ambient/NC control (V2 writes `35` on some models), `10` volume control, `20` playback control, `FF` off.
- Over-ear touch pad on/off: `{D8, D2, 01, 00|01}` (V1); in V2 the flag is inverted (`{D8, D2, 00, 00 = enabled}`).
- NC/ambient button cycle (XM3/XM4): `{D8, D1, 02, mode}`.
- Quick Access (V2): `{F8, 0D, 02, double, triple}`.

**Speak-to-chat** [P]
- Enable: V1 `{F8, 05, 01, on}`; V2 `{F8, 0C, on ? 00 : 01, 01}` (inverted).
- Config: V1 `{FC, 05, 00, sensitivity, focus, timeout}`; V2 `{FC, 0C, sensitivity, timeout}`.
- Codes [I, to confirm D]:
  - sensitivity: `00` auto, `01` high, `02` low;
  - timeout: `00` short (~5 s), `01` standard (~15 s), `02` long (~30 s), `03` none.

**DSEE and codec priority** [P]
- DSEE: V1 `{E8, 02, 00, on}`; V2 `{E8, 01, on}`.
- "Priority on sound quality" (LDAC) vs "stable connection":
  - V1 `{E8, 01, 00, 00 = quality / 01 = stable}` (inverted flag);
  - V2 `{E8, 02, 00 = quality / 01 = stable}`.
- **Codec readout:** V1 `{18, 00}` → reply byte 2. V2 `{12, 02}`. Values: `01` SBC, `02` AAC, `10` LDAC, `20` aptX, `21` aptX HD.

**Other settings**
- Pause when taken off: V1 `{F8, 03, 00, on}`; V2 `{F8, 01, on ? 00 : 01}`.
- Auto power off:
  - V1 `{F8, 04, 01, a, b}`. The `a, b` pairs per duration are not public [D].
  - V2 `{28, 05, a, b}`.
- Voice guidance: V1 type `0E` `{48, 01, 01, on}`.
- Power off: V2 `{24, 03, 01}`.
- **Never send** factory reset, NC-optimizer misuse, or reboot.

**Ear detection / wearing state:** no public notification is documented. The app shows it on LinkBuds and XM4/XM5. Capture it [D].

**Firmware:** `{04, 02}` → `{05, 02, len, ascii…}`.

### 3.4 Handshake and state machine [P]
```
RFCOMM up (+500 ms) ──► send init {00 00} (type 0C, seq 0)
   ◄── ACK, then reply payload starting 01:
        4-byte payload → V1 tables; 8-byte payload → V2 tables
   (no reply in 1.25 s → resend, max 2 times, then give up for this session)
INIT queue, one command per ACK:
   firmware {04 02}, codec, battery (single/dual/case per model), ASM get,
   EQ get, DSEE/LDAC get, buttons, speak-to-chat, pause-when-off
READY: notifications 13/25 battery, 69 ASM, 59 EQ, 1B/15 codec → ACK each, update state
Poll battery every 5 min if no notify arrived.
```
**Model identification:** name patterns `WH-1000XM3/4/5/6`, `WF-1000XM3/4/5`, `LinkBuds`, `LinkBuds S`…, plus the V1/V2 reply size.

### 3.5 Per-model matrix [P, Gadgetbridge capability lists]

| Model | Dialect | Battery | NC/ASM | Wind | Focus voice | EQ | Speak-to-chat | DSEE | LDAC prio | Buttons | Other |
|---|---|---|---|---|---|---|---|---|---|---|---|
| WH-1000XM3 | V1 | single | ✅ 0–20 | ✅ | ✅ | custom 5 + CB (**SBC/AAC only**) | — | ✅ | ✅ | NC button, touch on/off | NC optimizer, surround/position |
| WH-1000XM4 | V1 | single | ✅ | ✅ | ✅ | custom | ✅ + config | ✅ | — | touch on/off | NC optimizer, pause when off |
| WH-1000XM5 | V2 | single | ✅ | — | ✅ | custom | ✅ | ? [D] | ? | NC button mode [D] | Quick Access, power off |
| WF-1000XM3 | V1 | L/R/case | ✅ | ✅ | ✅ | custom | — | ✅ | — | L/R modes | power off |
| WF-1000XM4 | V1 | L/R/case | ✅ | ✅ | ✅ | simple | ✅ | ✅ | ✅ | L/R modes | |
| WF-1000XM5 | V2 | L/R/case | ✅ | — | ✅ | custom | ✅ | ✅ | ✅ | L/R + NC button mode | Gadgetbridge marks it experimental |
| LinkBuds (WF-L900) | V2 | L/R/case | **no ANC** | — | — | custom | ✅ | ✅ | — | L/R, wide-area tap | adaptive volume |
| LinkBuds S | V2 | L/R/case | ✅ | — | ✅ | custom | ✅ | ✅ | — | L/R, NC button, Quick Access | |

### 3.6 Pitfalls
- **The ACK discipline is mandatory.** If we miss ACKs, the headset resends and then drops the session.
- XM3 EQ only works while the codec is SBC/AAC. With LDAC the setting is accepted but has no effect; grey it out when the codec readout says LDAC.
- Several **inverted booleans in V2** (`00` = enabled). Keep a per-dialect encoder table.
- Settings changed with the physical buttons arrive as `69`/`59` notifications. Never keep an optimistic UI state without a notify or reply.
- **Sony's app in multipoint** may hold the channel (§1.4).
- **Firmware updates change the V2 tables** (XM5/WF-1000XM5 are still evolving). Unknown replies must be logged, never crash the parser.

### 3.7 Capture and test plan
- The owner does not list Sony gear. If one becomes available: Android HCI snoop with "Sony | Sound Connect" (§6.1), filter `btrfcomm && frame contains 3e:0c`, and go through each setting once.
- Without hardware, ship V1 + V2 parsers with unit tests from our computed frames, but **disable writes** until one real capture per dialect exists.

---

## 4. JBL / Harman

### 4.1 What is publicly known
JBL does **not** have one protocol. The JBL Headphones app (`jbl.stc.com`) bundles several control stacks, picked per product from a server-updatable `product_list.json` [P, JBLProtocol4 recon]:

| Family (app controller) | Transport | Chips | Status of public knowledge |
|---|---|---|---|
| **Protocol4** (`protocol4.*Control`) | **BLE GATT**, service `65786365-6c70-6f69-6e74-2e636f6d0000` | BES and Realtek | **Spec published**, verified on Tune 780NC |
| `bes` / `protocol3` / older Harman command classes | SPP (`SppDevice`) or GATT (`BleDevice`) | Bestechnic | Code hints only: frames like `AA <cmd> <len> <payload>`, battery request `AA 25 00` [P, unverified] |
| `airoha` | Airoha "RACE"-style app commands over GATT/SPP | Airoha AB15xx | RACE framing public (§4.4); JBL's app-level IDs not public |
| BES "TOTA v2" | BLE GATT | BES1306 (Wave Buds 2) | One firmware-RE project [P] |
| `lea` (LE Audio models) | LE | | Not usable: no LE Audio on our controller |

- **JBL Live Pro 2 TWS and JBL Tune Flex:** CERT@VDE advisory **VDE-2024-076 / CVE-2024-2104** says their **GATT server** accepts device-control reads and writes **without authentication**. So these models are controlled over **BLE GATT**; the exact family is not public [P].
- **Tour Pro 2:** family unknown [D].

### 4.2 Protocol4 spec (summary in our words) [P, MIT-licensed notes; one model verified]

**GATT**
- Service `65786365-6c70-6f69-6e74-2e636f6d0000` (ASCII "excelpoint.com").
- Host writes to characteristic `…6d0002` (write with response).
- Device notifies on `…6d0001`. Enable notifications through its CCCD.

**Frame**, all little-endian
```
00 DD | cmd u16 | packet_count u8 | packet_index u8 | payload_size u16 | payload
```
- **No CRC, no encryption.**
- Commands: `0001` GET (payload = list of u16 tags), `0002` SET (payload = TLVs), `0003` device notification, `0004` notify-to-device.
- **Off limits:** `0201`/`02FF` analytics, `00FF` factory reset, `0101`–`0104` OTA.
- **TLV:** `tag u16 | len u16 | value`. Replies and notifications start with a `status u16` (`0000` = OK).
- A SET is answered by a `0003` notification with the new state, then an ack `0002 {0000}`.

**Encodings**
- StatusLevel1 = `(on << 7) | level`.
- StatusLevel2 = `(on << 15) | level` (u16).

**Useful tags**

| Tag | Meaning |
|---|---|
| `0004` | product ID u16 (Tune 780NC = `0x215F`) |
| `000C` | firmware (3 bytes) |
| `000D` | **battery**, StatusLevel1. Percent verified on 2 samples. The standard Battery Service read differently (50 % vs 47 %) |
| `0018` / `0019` / `001A` | **ANC / Ambient Aware / TalkThru**: one exclusive group. Write `FF` to turn one on; write `7F` to `0018` for "all off" |
| `0023` | Voice Aware (`03` off, `81`/`82`/`83` low/mid/high) |
| `0015` | balance |
| `0022` | spatial sound (`81` movie, `82` music, `83` game, `03` off) |
| `0012` | auto power off (StatusLevel2, seconds) |
| `0C40` / `0C41` | voice prompts |
| `0F82`–`0F89` | button gestures. Function ids: `08` play/pause, `C8` EQ on/off, `C2` spatial, `A1` assistant. ANC-button sets: `09`, `0B`, `0C`, `0D` |
| `002F` | host type (SET `01` at connect) |
| `0002` | heartbeat (empty SET about every 3 s while idle) |

- **EQ** (`0E01`/`0E02`) is multi-frame float filter coefficients computed by a native library. **Not decoded.**

**Session rules**
- Example frame, "ANC on": `00 DD 02 00 01 00 05 00 18 00 01 00 FF`.
- Connect sequence: enable notify, then SET HostType=1, then GET a block of tags.
- An **LE bond is required**: unbonded hosts get no replies.
- **Only one GATT control host at a time.**

**Protocol4 models** (PID): Tune 680NC (215E), **Tune 780NC (215F)**, Tune 785NC (21A4), Live 680NC/780NC (2160/2161), Tune 520BT2/530BT/535BT/730BT/735BT (Realtek), Tour One M3 (2101), Live Beam 4 / Live Buds 4 / Live Flex 4 (2179/217B/217D), Tune Flex 3 / Buds 3 / Beam 3, Vibe and Wave "3" series, Endurance Pace / Peak 4 / Run 3, Junior Free, Sense Pro, Soundgear Clips.
- **Not in Protocol4:** Tune 770NC, Live 770NC, and other older models; most likely also Live Pro 2 and Tour Pro 2 [I].

### 4.3 Feasibility on our stack
- **Battery without vendor code** is the first goal for every JBL. Try, in order:
  1. the Fast Pair Message Stream (§5.3), since most 2022+ JBLs are Fast Pair devices [I];
  2. HFP `+IPHONEACCEV`/`+BIEV` (§5.1);
  3. the GATT Battery Service through BlueZ `Battery1` (§5.5).
- **Control (Protocol4 and the GATT family):** needs an **LE link** to the headset while A2DP runs on BR/EDR.
  1. Dual-mode JBLs normally advertise an LE identity (often a different, random address). The JBL app pairs it separately (Android shows a second "LE" entry); some use cross-transport key derivation.
  2. **LE pairing must go through bluetoothd** (`Device1.Pair` on the LE address, with our agent). Our daemon cannot do SMP itself.
  3. After bonding, the daemon opens a raw **ATT socket**: `socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP)` with `l2_cid = 4` (ATT) and `bdaddr_type = LE public/random`, plus `BT_SECURITY_MEDIUM`. It then does its own minimal ATT client: discover by UUID, write the CCCD, Write Request, handle Notifications. Kernel 4.4 supports LE L2CAP fixed channels [I]; about 300 lines.
  4. **Conflict risk:** bluetoothd 5.54 also connects ATT to bonded LE devices (for GATT caching and `Battery1`). Two ATT clients on one link work in the kernel (each socket gets copies of notifications), but CCCD writes and MTU exchange can collide [D]. Fallback: drive GATT through BlueZ D-Bus (`GattCharacteristic1.WriteValue` / `StartNotify`) with `dbus-send`/`dbus-monitor` children. That is slower but safe.
  5. Radio: one extra LE connection (connection interval ≥ 30 ms) next to A2DP. The BCM43438 handles BR/EDR + one LE link, but LDAC 990 + Wi-Fi + LE may stutter [D].
- **Verdict:** Protocol4 control is feasible but **medium risk and effort** (LE bond + ATT client ≈ 4–5 d on top of the 2 d protocol work). Do it only after the owner's capture confirms the model is Protocol4. Unknown families: **no implementation until captured**.

### 4.4 Airoha RACE facts (context only; do not use for control) [P, ERNW / race-toolkit]
- Frame: `05 | type | length u16 LE (= 2 + payload) | command id u16 LE | payload`.
  - type: `5A` command expecting a response, `5B` response, `5C` command without response, `5D` indication.
- Transports:

  | Kind | UUIDs |
  |---|---|
  | GATT service | `5052494D-2DAB-0341-6972-6F6861424C45` (TX char `43484152-2DAB-3241-…`, RX char `43484152-2DAB-3141-…`) |
  | RFCOMM | `00000000-0000-0000-0099-AABBCCDDEEFF`; Sony `8901DFA8-…`; Bose and Beyerdynamic variants; sometimes the plain SPP UUID |

- Known ids are **flash/RAM/link-key/FOTA** commands (e.g. `0403` flash read, `0CC0` link key, `1C0x` FOTA). That is exactly the CVE-2025-20700/20701/20702 problem (unauthenticated RACE).
- **Policy for ByOS:** never send RACE. It is a factory and debug interface, and using it would mean exploiting a vulnerability. Any JBL that is Airoha-based is controlled only through whatever app-level protocol the capture shows.

### 4.5 Capture plan for the owner's JBL (this is what completes the spec)
1. **Identify the model.** Exact name on the box or in the app; firmware version in the JBL app's "About".
2. **Android HCI snoop** (§6.1) with the JBL Headphones app:
   1. force-stop the app before logging;
   2. open it (handshake);
   3. battery (note the %);
   4. each noise mode (ANC, Ambient Aware, TalkThru, off), twice;
   5. ANC level / adaptive, if shown;
   6. every EQ preset; one custom band to min and max, then reset;
   7. each button mapping once, then restore;
   8. auto-off, voice prompts, Find my buds.
   - **Never tap** firmware update or any reset.
   - Write the time of each action in a log (`time,action,value_shown`).
3. **Wireshark triage.** Open `btsnoop_hci.log`:
   - `btatt.opcode == 0x12 || btatt.opcode == 0x52` (writes) and `btatt.opcode == 0x1b` (notifications). If they start with `00 DD`, it is **Protocol4**.
   - `btrfcomm`. Frames starting `AA` = older Harman SPP; `05 5A`/`05 5B` = RACE.
   - `btl2cap.cid == 0x0004` on an LE handle tells you the GATT family even if the dissector shows "Unknown".
   - Note the service UUID of the written characteristic (from the "Find By Type"/"Read By Group" discovery at the start).
4. **Send us:** the log plus the action list. We then write the per-model table in our own words, exactly like Protocol4, and create our own test vectors.
5. **Optional on-device read-only check** (the owner runs it, with our tool): `btmon -w` on the R3 II during the JBL connection; `sdptool browse <mac>` to list RFCOMM services; and, if an LE address appears, `bluetoothctl info <le-addr>` for GATT UUIDs.

---

## 5. Generic fallbacks (any headset)

### 5.1 HFP AT battery (needs us to be an HFP Audio Gateway)
These messages flow only on an HFP **Service Level Connection**. Today bluealsa runs `-p a2dp-source` only, so none arrive (see `research_codecs_earbuds.md` §3.4). If `hfp-ag` is enabled:

| Mechanism | Who sends it | Format | Resolution |
|---|---|---|---|
| Apple `+XAPL` / `+IPHONEACCEV` | most headsets (Sony, JBL, Jabra, Bose…) | HF sends `AT+XAPL=<vid>-<pid>-<ver>,<features>` (feature bit 1, value 2 = battery reporting); AG answers `+XAPL=iPhone,<features>`. Then `AT+IPHONEACCEV=<n>,<key>,<val>,…`: key `1` = battery 0–9 (×10 %, round up), key `2` = dock/charging state | 10 % |
| HF indicators (HFP 1.7) | newer headsets | AG advertises `+BIND: (2)` (indicator 2 = battery); HF sends `AT+BIEV=2,<0–100>` | 1 % |
| `+XEVENT=BATTERY` (Plantronics/Android) | some business headsets | `AT+XEVENT=BATTERY,<level>,<levels>,<minutes>,<charging>` | varies |
| CSR `+CSRBATT` | old CSR-chip headsets | `AT+CSRSF=…` negotiation, then `+CSRBATT` 0–9 | 10 % |

- bluealsa 4.1.1 already parses `+XAPL`/`+IPHONEACCEV` [V, earlier doc]. `+BIEV` support has to be checked [D].
- **Side effects of becoming an AG** (the same list as in the earlier doc): voice prompts saying "connected to phone", call buttons, and SCO PCMs appearing in bluealsa, which may confuse sys_server. Keep it an **opt-in "Headset battery (HFP)" toggle**, tested headset by headset.
- TWS buds report **one** value through HFP (usually the primary bud or the minimum). It is not L/R/case.

### 5.2 AVRCP
- **Absolute volume** is already active (`--a2dp-volume`). It gives volume sync only.
- AVRCP 1.3+ has *InformBatteryStatusOfCT* (PDU `0x18`). The headset (CT) can tell the source a coarse state: `0` normal, `1` warning, `2` critical, `3` external power, `4` full. Very few headsets send it, and BlueZ 5.54 does not expose it [I]. **Not worth the effort.**

### 5.3 Google Fast Pair: Message Stream over RFCOMM (best generic option) [X, public spec]
- **RFCOMM UUID `df21fe2c-2515-4fdb-8886-f12c4d67927c`.** Look it up in the SDP list; Nothing, recent Sony, JBL and many Android-oriented buds expose it.
- Message: `group u8 | code u8 | length u16 BIG-endian | data`.
- What the provider pushes on connect and on change:

  | Group / code | Content |
  |---|---|
  | `03/01` | model ID (3 bytes): the same ID as the BLE advert. Gives exact model identification for any brand |
  | `03/03` | **battery: 3 bytes, L, R, case**. Each is bit 7 = charging, bits 0–6 = 0–100 %; `0x7F` = unknown |
  | `03/04` | remaining battery time in minutes (1 or 2 bytes) |
  | `03/06` | active components (bitmask: `01` right, `02` left), the answer to request `03/05` |
  | `03/09` | firmware version (UTF-8) |
  | `03/0A` | session nonce (8 bytes): used for MAC-protected messages |

- Ring: `04/01 {bits, [timeout s]}`, with bits `01` right, `02` left, `03` both, `00` stop. The provider ACKs within 1 s.
  - Acknowledgement group `FF`: `FF/01` ACK, `FF/02` NAK (with the group and code acknowledged) [I, from the spec's acknowledgement page, to confirm D].
  - Newer spec revisions may require an account-key MAC for some actions. **We use it read-only** (battery, model, firmware) [D whether ring works without a MAC].
  - The host may announce itself with `03/08` platform type. Android sends `{01, sdk}`; we would omit it.
- **Why it matters:** it gives **L/R/case battery at 1 % resolution for any Fast Pair headset, with no vendor protocol and no HFP side effects.** It costs one RFCOMM socket and about 150 lines. Implement it as the generic module, and use it as a cross-check for the vendor modules.

### 5.4 Fast Pair BLE adverts (passive, no connection) [X, public spec]
- **Discoverable (pairing mode):** service data UUID `0xFE2C` holds just the **3-byte model ID**, advertised at ≤ 100 ms. This identifies the model before pairing (Nothing/CMF IDs in §2.5).
- **Not discoverable** (only after an account key has been written by an Android phone):

  | Field | Content |
  |---|---|
  | `00` | version/flags |
  | `LLLL0000` | account-key filter header (`LLLL0010` = hide UI) |
  | filter | *s* bytes |
  | `0x21` | salt header |
  | salt | 2 bytes (older devices: `0x11` + 1 byte) |
  | (optional) `0b0011_0011` | battery header ("show"; `0b0011_0100` = "hide") |
  | 3 battery bytes | L, R, case: bit 7 charging, bits 0–6 percent, `0x7F` unknown |

  Advertised at ≤ 250 ms.
- **The battery bytes are plaintext** and readable without any key. The account-key filter is a Bloom filter over (key, salt, battery) that we cannot check without the key, which is fine because we do not need it.
- Pitfalls:
  - Battery is often only advertised briefly (lid open, or while not connected), and the spec discourages advertising it all the time;
  - the advert uses a rotating random address, so tie it to our bonded device by the model ID plus timing, not by MAC;
  - scanning costs air time (rules in `research_codecs_earbuds.md` §3.1: no scanning while A2DP streams).
- Use: the **pre-connection popup** ("Nothing Ear (a) · L 80 % R 85 % case 40 %") for Fast Pair buds, mirroring the AirPods lid-open popup.

### 5.5 GATT Battery Service
- `0x180F` / characteristic `0x2A19` (u8 percent). BlueZ 5.54's `battery` plugin exposes it as `org.bluez.Battery1.Percentage` on the LE device object, **if** the headset's LE side is bonded and connected [V plugin present].
- Many dual-mode JBLs and others expose it. The value can differ from the vendor value (Tune 780NC: 50 % vs 47 %).
- It is a single value, rarely L/R.

### 5.6 Generic-module decision order
1. Vendor module (AirPods, Galaxy, Nothing, Sony, later JBL) when its UUID is present.
2. Otherwise **Fast Pair Message Stream** if `df21fe2c-…` is in the SDP list.
3. Otherwise `Battery1` if BlueZ shows it.
4. Otherwise HFP battery, if the user enabled the HFP toggle.
5. Otherwise "battery not reported".

---

## 6. Owner capture and test plan

### 6.1 Android HCI snoop procedure (any brand)
1. Settings › About phone › tap *Build number* 7 times (enables Developer options).
2. Developer options › **Enable Bluetooth HCI snoop log** › **Enabled** (not "Filtered", which strips payloads).
3. Turn Bluetooth off and on (this starts a fresh log). Connect the headphones. Force-stop the vendor app.
4. Open the vendor app (this captures the handshake), then do **one action at a time, 5 s apart**, writing the time and the value the app shows (`time,action,value`). Never tap firmware update or reset.
5. Turn Bluetooth off (this flushes the log). Get the log:
   - **Rooted or older Android:** `/data/misc/bluetooth/logs/btsnoop_hci.log` (or `/sdcard/btsnoop_hci.log`).
   - **Android 11+ without root:** `adb bugreport bug.zip`; the log is at `FS/data/misc/bluetooth/logs/btsnoop_hci.log` inside the zip.
   - Samsung phones: also Settings › Developer options › "Bluetooth HCI snoop log", and the file lands in the bugreport the same way.
6. Turn the snoop log **off** afterwards. It contains link keys and addresses: keep it private and share only redacted extracts.
7. Open it in Wireshark. Filters per brand:

   | Brand | Filter |
   |---|---|
   | Nothing | `btrfcomm && frame contains 55:60:01` |
   | Sony | `btrfcomm && frame contains 3e:0c` |
   | JBL | `btatt` (LE) or `btrfcomm` |
   | Fast Pair | `btrfcomm` on the channel for `df21fe2c…` (look at the SDP exchange) |

8. Galaxy Buds and AirPods captures for the other agent's modules use the same procedure (AirPods: iPhone + PacketLogger on a Mac, or an Android phone running a third-party AirPods app).

### 6.2 Owner's hardware: what to capture

| Device | Captures needed | Purpose |
|---|---|---|
| Nothing buds (model to confirm) | full sequence of §2.8 | test vectors, gesture tables, which control flag the model accepts |
| JBL (model to confirm) | full sequence of §4.5 | decides the family; spec for that model |
| any Fast Pair device (Nothing, JBL, Galaxy) | open the Android Bluetooth device page (shows battery) and note the % | Message Stream battery vectors (`03/03`) |
| Galaxy Buds generations, AirPods | as specified in `research_codecs_earbuds.md` | other agent's modules |

### 6.3 Test plan on our side (in order, all off-device first)
1. **PC unit tests** of each parser and encoder:
   - CRC-16/MODBUS (Nothing), Sony sum and escape, Fast Pair framing, Protocol4 TLV;
   - stream reassembly with random splits and merges;
   - every captured reply decodes to the app's value.
2. **Fuzz** each parser with truncated and garbage frames (no crash, bounded memory). The daemon is long-lived on a 12 MB-free device.
3. **R3 II, read-only:** connect, handshake, battery, mode readout; leave it 1 h with music playing; check bluealsa underruns (`BT:` logs) and daemon RSS < 1 MB.
4. **R3 II, writes,** one feature at a time, each with an undo step: ANC/mode, then EQ preset, then gestures (restore originals), then find (out of the ears).
5. **Coexistence:** while the phone app is connected through multipoint, verify our backoff message; reconnect after the buds go back into the case and come out again; A2DP codec switch (AAC↔SBC) during a session.

---

## 7. Daemon integration notes (`hibybudsd` brand modules)
- Module interface (shared with AirPods/Galaxy):
  - `match(uuids, name, modalias)`
  - `connect(mac)`
  - `on_bytes()`
  - `tick()` (timeouts and polls)
  - `cmd(op, args)`
  - `state` (battery L/R/case/single plus charging flags, in-ear L/R, noise mode + level, EQ preset + bands, gestures, toggles, firmware, model id, capability bits).
- The capability bits drive the UI. The device page shows only what the model supports (§2.5, §3.5). The same rule applies to JBL once it is known.
- Text strings for the new controls ("Adaptive", "Transparency", "Ambient Aware", "TalkThru", "Focus on voice", "Speak-to-chat", "Clear Bass", "Ultra Bass", "Low-latency mode", "Ear tip fit test", "Find my earbuds") go into `tr_lang()`, EN/FR/ES first.
- **Effort summary:**

  | Item | Days |
  |---|---|
  | Nothing module | 4–5 |
  | Sony module (V1 + V2, writes behind capture) | 4–5 |
  | Fast Pair Message Stream generic module | 1.5 |
  | Fast Pair advert parser for the popup | 1 (on top of the BLE scanner already planned) |
  | JBL Protocol4 (LE bond + ATT client), only if the owner's model is Protocol4 | 2 + 4–5 |
  | HFP-AG battery toggle | 1–2 (already in the earlier plan) |

## 8. Open items [D]
- Nothing: which control value (`0x0120` vs `0x0160`) each owned model accepts. Gesture side ids on over-ear models. Spatial payload per model. Custom EQ gain range.
- Nothing: whether models flagged `isUseGattOverBREDR` still answer every command on the SPP channel.
- Sony V2: battery type byte per model (`01` vs `09`); adaptive NC; wearing-state notification; auto-power-off codes.
- Fast Pair Message Stream: whether the battery push needs any host message first; whether ring needs a MAC.
- JBL: the owner's model and family; LE bond and CTKD behaviour with BlueZ 5.54; whether bluetoothd's own ATT client collides with ours.
- Coexistence: LE link + LDAC + Wi-Fi on the BCM43438.

## Sources
- Gadgetbridge, Nothing support (`service/devices/nothing/Ear1Support.java`, `devices/nothing/*Coordinator`, `prefs/NothingAudioMode|NothingEqualizer|NothingTapAction`): https://codeberg.org/Freeyourgadget/Gadgetbridge (AGPL-3.0, facts only)
- Gadgetbridge device pages: https://gadgetbridge.org/gadgets/headphones/nothing/ · https://gadgetbridge.org/gadgets/headphones/sony/
- Gadgetbridge Sony protocol (`service/devices/sony/headphones/…` `Message`, `MessageType`, `SonyHeadphonesProtocol`, `SonyHeadphonesSupport`, `impl/v1/PayloadTypeV1`, `impl/v1/SonyProtocolImplV1`, `impl/v2/PayloadTypeV2`, `impl/v2/SonyProtocolImplV2`, `prefs/EqualizerPreset`, `params/AudioCodec`, coordinators): https://codeberg.org/Freeyourgadget/Gadgetbridge
- Gadgetbridge issue on Sony auto-connect (UUID seen in SDP): https://codeberg.org/Freeyourgadget/Gadgetbridge/issues/3053
- Bharadwaj Raju, "Creating a Linux controller for the Nothing Ear (2)" (captured frames used for our CRC check): https://bharadwaj-raju.github.io/posts/nothing-ear-2-on-linux
- ear (web) (AGPL-3.0; command numbers, model and Fast Pair ID tables, Nothing app config): https://github.com/radiance-project/ear-web
- Sony MDR protocol notes (method reference): https://github.com/AndreasOlofsson/mdr-protocol
- JBLProtocol4 (MIT; Protocol4 spec, model family, capture method): https://github.com/NeoHBz/JBLProtocol4
- JBL Wave Buds 2 firmware research (BES TOTA v2): https://github.com/c8dhjp4tyv-bit/jbl-wave-buds2-firmware-research
- CERT@VDE VDE-2024-076 / CVE-2024-2104 (JBL Live Pro 2 TWS, Tune Flex GATT without authentication): https://certvde.com/en/advisories/VDE-2024-076 · https://harman.csaf-tp.certvde.com/.well-known/csaf/white/2025/hbsa-2025-0001.json
- Airoha RACE: ERNW white paper https://ernw.de/en/whitepapers/issue-74.html · 39C3 talk https://app.media.ccc.de/v/39c3-bluetooth-headphone-jacking-a-key-to-your-phone · race-toolkit https://github.com/auracast-research/race-toolkit · CVE-2025-20700 https://dbugs.ptsecurity.com/vulnerability/CVE-2025-20700
- Google Fast Pair specification: provider adverts https://developers.google.com/nearby/fast-pair/specifications/service/provider · battery notification https://developers.google.com/nearby/fast-pair/specifications/extensions/batterynotification · Message Stream https://developers.google.com/nearby/fast-pair/specifications/extensions/messagestream · device information https://developers.google.com/nearby/fast-pair/specifications/extensions/deviceinformation · device action (ring) https://developers.google.com/nearby/fast-pair/specifications/extensions/deviceaction · MAC https://developers.google.com/nearby/fast-pair/specifications/extensions/mac
- Earlier ByOS research (stack facts, AirPods, Galaxy Buds, HFP notes): `docs/research_codecs_earbuds.md`
