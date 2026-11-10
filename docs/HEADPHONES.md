# Headphones app ("Auriculares") and the hibybudsd daemon

Native control of Bluetooth headphones on the HiBy R3 II: battery (left / right / case), wear state with auto-pause, noise control, EQ, gestures, touch lock, find, rename, firmware, for AirPods / Beats, Galaxy Buds, Nothing / CMF, Sony (read-only for now) and any Google Fast Pair headset (battery).

Status: **builds with zero warnings (MIPS and PC), PC tests pass, runs in the QEMU harness against simulated headphones. Never run against real headphones yet.** Everything marked [I] below is our reading of public protocol notes and has to be confirmed by captures (section 8).

Evidence tags: **[V]** verified (rootfs, our tests against the documented layouts, the QEMU harness); **[I]** inferred from public notes; **[D]** to be confirmed on the device / with the owner's headphones.

## 1. Files

| File | What |
|---|---|
| `src/hibybudsd.c` | the daemon: BlueZ watcher, connection manager, sockets (L2CAP / RFCOMM), SDP client, state file, command FIFO, `--fake`, `--list`, `--probe`, `--matrix` |
| `src/buds_core.inc.c` | shared vocabulary: `struct buds_st`, capabilities, state file and command formats, CRC-16/XMODEM, BlueZ dump parser, the brand-module interface (`struct buds_brand`, `struct bud`) |
| `src/buds_apple.inc.c` | Apple / Beats module (AAP over L2CAP) |
| `src/buds_samsung.inc.c` | Samsung module (legacy and new framing over RFCOMM) |
| `src/buds_nothing.inc.c` | Nothing / CMF module (RFCOMM, CRC-16/MODBUS) |
| `src/buds_sony.inc.c` | Sony MDR V1/V2 module, **read-only** unless `HIBYBUDS_SONY_WRITES=1` |
| `src/buds_fastpair.inc.c` | generic fallback: Google Fast Pair Message Stream (battery, model id, firmware) |
| `src/buds_all.inc.c` | brand registry (priority order) + model catalog + matrix generator (single source of truth) |
| `src/buds_sim.inc.c` | simulated headphones (Apple, Samsung, Nothing) for `--fake` and the tests |
| `src/buds.inc.c` | the shell app, the reader thread, the public API (CC card, popup, widget) |
| `src/lang/buds_tr.inc.c` | texts in the 13 languages |
| `tools/wsl/zbuild_buds.sh` | builds `out/agent_buds/hibybudsd` and `out/agent_buds/hibyshell` |
| `tests/buds_test.c`, `tests/buds_test.sh` | protocol tests, fake-daemon end to end, doc matrix check, translations |
| `tests/buds_i18n_test.c` | the 13 languages (keys, `%s`, glyphs of the firmware font) |
| `tests/buds_render.c`, `tests/buds_render.sh` | PNG frames of every page, the popup and the widget items (5 languages) |
| `tests/buds_vm/*.cmds` | `vmdrive.py` scripts for the QEMU harness |

Edits outside these files: one `#include "buds.inc.c"` in `src/hibyshell.c`; the include + `budtr_lookup` line in `src/i18n.inc.c`; the Batteries widget in `src/widgets.inc.c` (section 6.3).

## 2. Architecture

```
 BlueZ (bluetoothd 5.54)                      headphones
   │  D-Bus signals                              ▲  vendor channel on the existing ACL
   ▼                                             │  (L2CAP PSM 0x1001 / RFCOMM channel from SDP)
 dbus-monitor ──► hibybudsd ─────────────────────┘
 dbus-send   ◄──┘   │  brand modules (pure: bytes in → state, command → bytes out)
                    │
   /tmp/hibybuds.state  (text, tmp + rename)    /tmp/hibybuds.evt (1 byte per change)
   /tmp/hibybuds.cmd    (FIFO, one command per line)
                    │
                    ▼
 hibyshell: reader thread (bu_thread) ──► snapshot ──► app pages, CC card, popup, widget
                                     └──► auto-pause (pc_toggle) when a bud leaves the ear
```

**Daemon** (`hibybudsd`, static musl hard-float, 165 KB, one instance via `flock` on `/tmp/hibybudsd.pid`):
- **BlueZ watching:** one `dbus-monitor --system` child filtered to BlueZ `PropertiesChanged` and `ObjectManager` signals. A line that mentions `Connected`, `ServicesResolved`, `Paired`, `Alias` or interfaces added/removed schedules one `GetManagedObjects` (`dbus-send`) 400 ms later. Without signals: one refresh per minute (every 8 s if dbus-monitor cannot run). Idle CPU is zero (poll with long timeouts).
- **Identification** (`identify()` of each brand, in priority order Apple → Samsung → Nothing → Sony → Fast Pair): Modalias (`v004C` Apple, `v0075` Samsung), the service UUID list, the remote name. Paired audio devices that no module claims are listed as "other" (no controls).
- **Connection:** when BlueZ says connected, wait 0.8–2.5 s (A2DP first), then open the vendor channel: L2CAP SEQPACKET PSM 0x1001 for AAP; RFCOMM on the channel found by our own **SDP client** (ServiceSearchAttributeRequest for the UUID128, attribute 0x0004; fallback `sdptool browse`, present on the rootfs [V]). The channel is cached in `/usr/data/hibymod/buds/known.txt` and looked up again after a failed connect. `BT_SECURITY_MEDIUM` (the existing bond).
- **Backoff:** 2 s, 5 s, 15 s, 30 s, 1 min, 5 min; after 6 failures the device shows "Controls not available" until it reconnects. A channel open but silent for 12 s is marked the same way.
- **No inquiry, no scan, no discovery, ever** (A2DP stutter, see section 9).
- **Commands** come from the FIFO; asynchronous jobs (`Device1.Connect` can take 30 s, `Disconnect`, rename = BlueZ `Alias`) run as `dbus-send` children polled in the main loop.
- **Popup event:** `popseq` of a device increases on the first status of a connection, unless the same device dropped less than 30 s before (reconnect blips do not pop up).

**Shell side** (`buds.inc.c`): a detached thread sleeps on the event FIFO (3 s timeout, 0.5 s while a command is pending), re-reads the state file only when it changed (inode / mtime / size), resolves pending commands, raises the popup, runs the auto-pause, nudges the widgets (`wg_fresh`), and restarts the daemon if its pid is gone (at most every 30 s). The UI never blocks: it copies a snapshot only when the generation changes.

**Commands are optimistic:** the UI shows the new value at once (an override over the snapshot); the daemon answers `ack=SEQ` when it was sent, or `err=SEQ,CODE` (`-1` not supported, `-2` not connected, `-3` I/O, `-4` out of range, `-5` busy); on error or after 4 s without answer the override disappears (the true value comes back) and a toast explains why.

### 2.1 Brand-module interface (how to add a brand)

```c
struct buds_brand {
    int brand; const char *tag;
    const struct buds_model *(*identify)(const struct bd_dev *d);  /* model or 0; no I/O */
    int transport; uint16_t psm;                                  /* BT_L2CAP + PSM, or BT_RFCOMM */
    const char *(*uuid)(const struct bud *b);                     /* RFCOMM service UUID (SDP) */
    void (*start)(struct bud *b);                                 /* channel open: handshake */
    void (*rx)(struct bud *b, const uint8_t *p, int n);           /* bytes / one L2CAP packet */
    int (*cmd)(struct bud *b, const struct bcmd *c);              /* BE_OK or BE_* */
    void (*tick)(struct bud *b, double now_ms);                   /* every second while open */
    void (*fill)(struct bud *b);                                  /* derive capabilities */
};
```
A module only sees `struct bud`: `s` (the published state it updates), `tx()` (send; the daemon socket, or the simulator / a test capture), `log()`, `now`, a reassembly buffer and 768 bytes of private state. It never does I/O, so it runs unchanged in the daemon, in the simulator and in the PC tests. Adding a brand: one file, one line in `BUDS_BRANDS[]` (`buds_all.inc.c`), its models in `buds_catalog()`.

### 2.2 State file and commands

```
v=1  pid=  fake=  gen=  ack=  err=SEQ,CODE
dev=AA:BB:CC:DD:EE:FF        (one block per device, connected first, ended by "end")
name= model= modelno= fw= serial= brand= family= model_id= color= conn= caps=
batt=L,R,case  chg=  wear=L,R  noise=mode,mask  anc=lvl,max  amb=lvl,max  adapt=  conv=  eq=idx,n
bands=n,g1..  lock= eardet= game= cycle=mask  gestl=tap,double,triple,hold  gestr=  gopts=
find=L,R  eqset=set,min,max  bass=lvl,max  fit=L,R  busy=  popseq=
```
Commands: `SEQ ADDR VERB a b c` with VERB `noise anclvl amblvl adaptlvl conv eq lock eardet game cycle gest find bass fit connect disconnect refresh`, or `SEQ ADDR rename NAME` / `SEQ ADDR bands g1,g2,g3`. `ADDR` `-` = the current device.

## 3. Apple / Beats (AAP) — protocol notes in our own words

- L2CAP, PSM 0x1001, SEQPACKET: one read = one packet. Packets start `04 00 04 00` + opcode (LE16) + data [I].
- Host → buds: hello `00 00 04 00 01 00 02 00` + 8 zero bytes; on adaptive-capable models the "host features" packet (opcode `0x004D`, byte `FF`); then the notification request (opcode `0x000F`, mask `FF FF FF FF`) [I].
- Buds → host: `0x0004` battery (count, then `component 01 level status 01`; component `04` left, `02` right, `08` case, `01` single; status `01` charging, `02` discharging, `04` absent); `0x0006` ear state of primary / secondary bud (`00` in ear, `01` out, `02` in case); `0x0009` setting (id + 4 bytes); `0x001D` metadata (NUL-separated strings: name, model number, manufacturer, serial, firmware), parsed by shape so extra fields do not break it [I].
- Settings (`0x0009`): `0x0D` listening mode (`01` off, `02` ANC, `03` transparency, `04` adaptive), `0x28` conversation awareness (`01` on, `02` off), `0x2E` adaptive level 0–100, `0x1A` press-and-hold cycle (bits `01` off, `02` ANC, `04` transparency, `08` adaptive; at least two), `0x0A` ear detection (`01`/`02`) [I].
- Model: Modalias product id (`0x2014` AirPods Pro 2...) [I]; without Modalias, the name; refined by the model number of the metadata (`A2084` → AirPods Pro).
- **Apple-host features** (hearing health, transparency customisation, loud-sound reduction) need the BlueZ `DeviceID` spoof; they are listed as unavailable (`BC_SPOOF`), the spoof is not implemented.
- Not implemented: per-side press-and-hold action (Siri / noise control) and AirPods 1/2 double-tap actions (setting ids not documented reliably: capture first), "find" (not part of AAP over BR/EDR), rename inside the buds (we set the BlueZ alias).
- Wear state is "primary / secondary", not left / right: it is used only for auto-pause.

## 4. Samsung Galaxy Buds

- RFCOMM; UUID `2e73a4ad-332d-41fc-90e2-16bef06523f2` (Buds+ and later), `00001102-0000-1000-8000-00805f9b34fd` (Buds 2019) [I].
- Frame: new `FD hdr(LE16: size bits 0–9, bit 12 response, bit 13 fragment) id payload crc DD`; legacy `FE type size id payload crc EE`; size = id + payload + CRC; CRC-16/XMODEM over id + payload [I]. **The CRC byte order is learnt** from the first valid frame of the buds (they speak first); we accept both and send little-endian until then [D].
- Robust stream parser: resync on garbage, a false start byte with a large size does not stall the stream (lookahead for a later valid frame), fragments (bit 13) concatenated until the last.
- Messages: `0x60` status, `0x61` extended status (answered once with `0x88` manager info `01 02 1E` [I]), `0x63` version (longest printable run = firmware), `0x77` noise mode update, `0x9B` ANC update (Live). Unknown ids are logged once per connection with their payload (hex).
- Status layouts per model family (`SAM_LAYOUTS`): only the common prefix (battery L/R, placement nibbles `1` worn / `2` out / `3`–`4` case, case battery) is well established; EQ, lock, touch nibbles and noise mode offsets are our best reading and are **range-checked** (an out-of-range value becomes "unknown") [I, D].
- Commands: `0x78` noise mode (`0` off, `1` ANC, `2` ambient, `3` adaptive — Pro and later), `0x80` ambient on/off (Buds, Buds+), `0x98` ANC on/off (Live), `0x84` ambient level, `0x83` ANC level (Pro), `0x86` EQ preset 0–5, `0x90` touch lock, `0x92` touch-and-hold action per side (codes `1` voice, `2` noise / ambient / ANC by model, `3` volume, `5` Spotify [I]), `0xA0`/`0xA1` find + `0xA2` mute the other side. A find sound stops by itself after 2 minutes.
- Not implemented: game mode (id not certain), custom 9-band EQ of Buds3 Pro, 360 audio, voice detect.

## 5. Nothing / CMF, Sony, Fast Pair

From `docs/research_headphones_nothing_sony_jbl.md` (our clean-room spec). JBL: not implemented (capture first); JBL with Fast Pair get battery through the generic module.

- **Nothing / CMF:** UUID `aeac4a03-dff5-498f-843a-34487cf133eb`; frame `55 | ctl LE16 | cmd LE16 | len LE16 | seq | payload | CRC-16/MODBUS LE` when `ctl & 0x20`. Our builder reproduces the published Ear (2) frame and the spec's vectors byte for byte [V]. Initial read-out with one get in flight (firmware, battery, ANC, EQ line, custom EQ, Ultra Bass, in-ear, low latency, gestures), 2 s timeout, one retry; **the control word (`0x0160` vs `0x0120`) is a per-model table entry**; if a model answers nothing at all we switch to the other value once [D]. Battery poll every 5 minutes. Set: 6 noise modes (high/mid/low ANC = our ANC level, adaptive, transparency, off), EQ presets (Ear line and CMF "listening modes"), 3-band custom EQ (float32 payload) and 8-band advanced EQ, Ultra Bass (×2 or 3 steps), in-ear detection, low latency, gestures per side (double, triple, hold), find per side / single / over-ear, ear-tip fit test (`E00D` result). Nothing does not report wear state, so no auto-pause from our side (the buds pause by themselves).
- **Sony MDR (read-only):** V1 `96CC203E-…`, V2 `956C7B26-…`; frame `3E type seq len(BE32) payload sum 3C` with escaping; every data frame is ACKed with seq 1−s, one command in flight. Init `00 00`, dialect from the reply size, then firmware, battery (single / L+R / case), ambient sound control, EQ. Our frames equal the spec's vectors [V]. Writes (ASM, EQ) are implemented but refused (`BC_RO`) until a capture per dialect exists; the UI shows the values with a "Read only" note.
- **Fast Pair Message Stream:** UUID `df21fe2c-2515-4fdb-8886-f12c4d67927c`; `group code len(BE16) data`; `03/03` battery L/R/case (bit 7 charging, `0x7F` unknown), `03/01` model id, `03/09` firmware. Read-only (ringing may need a MAC [D]).

## 6. Shell integration (for the coordinator)

### 6.1 API (`src/buds.inc.c`)

| Call | Use |
|---|---|
| `buds_preload()` | once at shell start: starts the thread, which starts `ASSETS/hibybudsd` (with `--fake` when `ASSETS/hibybuds.fake` exists) |
| `buds_app_at(ix, iy)` / `buds_app()` | open the app (zoom from the icon); `own_home_gesture` uses `app_by_id("headphones")` |
| `buds_status(&st)` | current headphones (READY first): 1 if connected. For the CC card |
| `buds_set_mode(BN_OFF / BN_ANC / BN_AMBIENT / BN_ADAPTIVE)` | noise control, optimistic; -1 if not possible |
| `buds_popup_pending(id, cap)` / `buds_popup_needed()` | a connect card is due (20 s window); `id` = address |
| `buds_popup_draw(d, progress)` | draws the card sliding from the bottom over `d` (dims the screen behind slightly) |
| `buds_popup_hit(x, y, progress)` | 1 close button, 2 card (open the app), 0 outside |
| `buds_popup_ack()` / `buds_popup_dismiss()` | shown / closed |
| `buds_wg_items(...)`, `buds_glyph(...)` | Batteries widget items and the per-family glyph |
| `buds_draw_product(d, cx, cy, scale, family, color)` | product drawing (AirPods, Pro, Max, Beats buds / ear-hook / headphones, Galaxy round / Live / stem, generic buds / headphones) |

The launcher icon and the CC card are the coordinator's (needs the forward declaration `static void buds_app_at(int, int);` before `launcher_touch.inc.c`, an `apps[]` entry `"headphones"`, and the daemon binary installed as `ASSETS/hibybudsd`).

### 6.2 Pages
List (connected first, battery line, spinner while connecting) › device page: product drawing + battery rings (L / R / case or one ring; grey when not connected, bolt when charging, red ≤ 20 %), Noise Control segmented control (only the model's modes) with the level slider of the selected mode, conversation awareness / voice focus, press-and-hold cycle (Apple) or Gestures (per side, per gesture, action picker), Equalizer (preset curve picture, presets, 3-band custom sliders on Nothing), bass boost, low latency, ear detection, "Pause when removed", touch lock, fit test, Find (two big buttons, loud-sound warning first), Name (rename), About (model, model number, firmware, serial, address), Connect / Disconnect. "Supported Devices" lists the catalog grouped by brand with six feature ticks. All texts in 13 languages. iOS gestures: back from both edges, bottom bar to home / multitasking, inertia scroll with rubber band.

### 6.3 Changes in `src/widgets.inc.c` (Batteries widget)
- `struct wdata` gained `hp[3]`/`nhp` (L / R / case of the first connected headphones, medium widget) and `hps[2]`/`nhps` (one merged item per device, small widget), filled in `wg_snap()` from the app's snapshot (`buds_wg_items`, no I/O).
- `WG_BATT` rendering: when headphones are connected, small = HiBy + headphones (2 rings), medium = HiBy + L / R / case (4 rings), with the per-family glyph (`buds_glyph`) inside the ring; colours / bolt as before. With no headphones the old behaviour (BlueZ `Battery1` devices) is unchanged.
- `wg_sig()` hashes the new items, so the sprite is rebuilt only when a value changes; the app's reader thread sets `wg_fresh` only when a battery-relevant value changes (no redraw churn).
- Forward declarations of `buds_wg_items` / `buds_glyph` at the top (the widgets file is included before `buds.inc.c`).

### 6.4 Auto-pause
For models with wear reporting (`BC_WEAR`) and the per-device preference on (default; stored in `/usr/data/hibymod/buds/shell.conf`): fewer buds in the ear while music plays → `pc_toggle()`; back in the ear within 5 min and we paused it → `pc_toggle()` again. The playing state is read from the shim's shared memory (thread-safe), not `np_get()`.

## 7. Supported models (generated)

The same tables drive the daemon, the app's "Supported Devices" page and this matrix (`hibybudsd --matrix`; `tests/buds_test.sh` fails if this section is stale). "Ear detection" = in-ear sensing or the device setting.

<!-- matrix:begin (generated by `hibybudsd --matrix`; do not edit by hand) -->
| Brand | Model | Battery | Noise control | EQ | Gestures | Ear detection | Find | Notes |
|---|---|---|---|---|---|---|---|---|
| Apple / Beats | AirPods | L/R/case | - | - | - | yes | - |  |
| Apple / Beats | AirPods 2 | L/R/case | - | - | - | yes | - |  |
| Apple / Beats | AirPods 3 | L/R/case | - | - | - | yes | - |  |
| Apple / Beats | AirPods 4 | L/R/case | - | - | - | yes | - |  |
| Apple / Beats | AirPods 4 (ANC) | L/R/case | off/ANC/ambient/adaptive | - | yes | yes | - | Apple-host features unavailable |
| Apple / Beats | AirPods Pro | L/R/case | off/ANC/ambient | - | yes | yes | - | Apple-host features unavailable |
| Apple / Beats | AirPods Pro 2 | L/R/case | off/ANC/ambient/adaptive | - | yes | yes | - | Apple-host features unavailable |
| Apple / Beats | AirPods Pro 2 (USB-C) | L/R/case | off/ANC/ambient/adaptive | - | yes | yes | - | Apple-host features unavailable |
| Apple / Beats | AirPods Pro 3 | L/R/case | off/ANC/ambient/adaptive | - | yes | yes | - | Apple-host features unavailable |
| Apple / Beats | AirPods Max | single | off/ANC/ambient | - | yes | yes | - | Apple-host features unavailable |
| Apple / Beats | AirPods Max (USB-C) | single | off/ANC/ambient | - | yes | yes | - | Apple-host features unavailable |
| Apple / Beats | Powerbeats3 | single | - | - | - | - | - |  |
| Apple / Beats | BeatsX | single | - | - | - | - | - |  |
| Apple / Beats | Beats Solo3 | single | - | - | - | - | - |  |
| Apple / Beats | Beats Studio3 | single | off/ANC | - | - | - | - |  |
| Apple / Beats | Powerbeats Pro | L/R/case | - | - | - | yes | - |  |
| Apple / Beats | Beats Solo Pro | single | off/ANC/ambient | - | yes | - | - |  |
| Apple / Beats | Beats Flex | single | - | - | - | - | - |  |
| Apple / Beats | Beats Studio Buds | L/R/case | off/ANC/ambient | - | yes | - | - |  |
| Apple / Beats | Beats Fit Pro | L/R/case | off/ANC/ambient | - | yes | yes | - |  |
| Apple / Beats | Beats Studio Buds+ | L/R/case | off/ANC/ambient | - | yes | - | - |  |
| Apple / Beats | Beats Studio Pro | single | off/ANC/ambient | - | yes | - | - |  |
| Apple / Beats | Powerbeats Pro 2 | L/R/case | off/ANC/ambient | - | yes | yes | - |  |
| Samsung | Galaxy Buds | L/R | off/ambient | yes | - | yes | yes |  |
| Samsung | Galaxy Buds+ | L/R/case | off/ambient | yes | yes | yes | yes |  |
| Samsung | Galaxy Buds Live | L/R/case | off/ANC | yes | yes | yes | yes |  |
| Samsung | Galaxy Buds Pro | L/R/case | off/ANC/ambient + level | yes | yes | yes | yes |  |
| Samsung | Galaxy Buds Core | L/R/case | off/ANC/ambient | yes | yes | yes | yes |  |
| Samsung | Galaxy Buds FE | L/R/case | off/ANC/ambient | yes | yes | yes | yes |  |
| Samsung | Galaxy Buds2 | L/R/case | off/ANC/ambient | yes | yes | yes | yes |  |
| Samsung | Galaxy Buds2 Pro | L/R/case | off/ANC/ambient | yes | yes | yes | yes |  |
| Samsung | Galaxy Buds3 | L/R/case | off/ANC/ambient/adaptive | yes | yes | yes | yes |  |
| Samsung | Galaxy Buds3 FE | L/R/case | off/ANC/ambient | yes | yes | yes | yes |  |
| Samsung | Galaxy Buds3 Pro | L/R/case | off/ANC/ambient/adaptive | yes | yes | yes | yes |  |
| Nothing / CMF | CMF Buds Pro 2 | L/R/case | off/ANC/ambient/adaptive + level | yes | yes | yes | yes | bass; fit test; low latency; custom EQ |
| Nothing / CMF | CMF Buds Pro | L/R/case | off/ANC/ambient + level | yes | yes | yes | yes | low latency |
| Nothing / CMF | CMF Buds 2 Plus | L/R/case | off/ANC/ambient/adaptive + level | yes | yes | yes | yes | bass; low latency; custom EQ |
| Nothing / CMF | CMF Buds 2a | L/R/case | off/ANC/ambient | yes | yes | - | yes | bass; low latency |
| Nothing / CMF | CMF Buds 2 | L/R/case | off/ANC/ambient/adaptive + level | yes | yes | yes | yes | bass; low latency; custom EQ |
| Nothing / CMF | CMF Buds | L/R/case | off/ANC/ambient | yes | yes | yes | yes | bass; low latency |
| Nothing / CMF | Nothing Ear (stick) | L/R/case | - | yes | yes | yes | - |  |
| Nothing / CMF | Nothing Ear (open) | L/R/case | - | yes | yes | - | yes | low latency; custom EQ |
| Nothing / CMF | Nothing Ear (1) | L/R/case | off/ANC/ambient + level | yes | yes | yes | yes | low latency |
| Nothing / CMF | Nothing Ear (2) | L/R/case | off/ANC/ambient/adaptive + level | yes | yes | yes | yes | fit test; low latency; custom EQ |
| Nothing / CMF | Nothing Ear (3a) | L/R/case | off/ANC/ambient/adaptive + level | yes | yes | yes | yes | fit test; low latency; custom EQ |
| Nothing / CMF | Nothing Ear (3) | L/R/case | off/ANC/ambient/adaptive + level | yes | yes | yes | yes | bass; fit test; low latency; custom EQ |
| Nothing / CMF | Nothing Ear (a) | L/R/case | off/ANC/ambient/adaptive + level | yes | yes | yes | yes | bass; fit test; low latency; custom EQ |
| Nothing / CMF | Nothing Headphone (1) | single | off/ANC/ambient/adaptive + level | yes | - | yes | yes | bass; low latency; custom EQ |
| Nothing / CMF | Nothing Headphone (a) | single | off/ANC/ambient/adaptive + level | yes | - | - | yes | bass; low latency; custom EQ |
| Nothing / CMF | Nothing Ear | L/R/case | off/ANC/ambient/adaptive + level | yes | yes | yes | yes | bass; fit test; low latency; custom EQ |
| Sony | Sony WH-1000XM3 | single | off/ANC/ambient | yes | - | - | - | read-only until captured; bass |
| Sony | Sony WH-1000XM4 | single | off/ANC/ambient | yes | - | - | - | read-only until captured; bass |
| Sony | Sony WH-1000XM5 | single | off/ANC/ambient | yes | - | - | - | read-only until captured; bass |
| Sony | Sony WH-1000XM6 | single | off/ANC/ambient | yes | - | - | - | read-only until captured; bass |
| Sony | Sony WF-1000XM3 | L/R/case | off/ANC/ambient | yes | - | - | - | read-only until captured; bass |
| Sony | Sony WF-1000XM4 | L/R/case | off/ANC/ambient | yes | - | - | - | read-only until captured; bass |
| Sony | Sony WF-1000XM5 | L/R/case | off/ANC/ambient | yes | - | - | - | read-only until captured; bass |
| Sony | Sony LinkBuds S | L/R/case | off/ANC/ambient | yes | - | - | - | read-only until captured; bass |
| Sony | Sony LinkBuds | L/R/case | - | yes | - | - | - | read-only until captured; bass |
| Fast Pair | any other (battery only) | L/R/case | - | - | - | - | - |  |
<!-- matrix:end -->

## 8. Tests and device test plan

### 8.1 Off device (done)
- `bash tests/buds_test.sh` (WSL): 2172 protocol checks (all brands: CRCs with check values, the published Nothing / Sony frames, builders, parsers, random stream splits, garbage resync, fragments, CRC order learning, timeouts/retries, 3000 fuzz packets per module under ASan/UBSan, model identification, catalog consistency, state and command round trips, BlueZ dump, SDP and `sdptool` parsers); the fake daemon end to end through the FIFO (AirPods Pro 2 transparency, Galaxy Buds2 Pro connect / ANC / EQ / touch-and-hold, range error); the doc matrix; 129 keys × 13 languages.
- `bash tests/buds_render.sh` → `out/buds_render/` (136 PNG: every page, popup, widget items, es/en/fr/ja/de). PC render ≤ 3.6 ms per page, popup ≤ 0.2 ms per frame (cached sheet, rings per frame).
- QEMU: private copy `VMDIR=~/hiby/vm_buds bash apps/harness_vm/shell_vm_prep.sh <test shell>`, `run_vm.sh --cpu 24Kf --port 5571`; copy `out/agent_buds/hibybudsd` + an empty `hibybuds.fake` into `share/hs/shell/`; test shell = a copy of `src/` where the Wireless icon calls `buds_app_at` (sed on `launcher_touch.inc.c`, never in the repo). `HIBY_PORT=5571 python3 apps/harness_vm/vmdrive.py tests/buds_vm/flow.cmds OUT`. Result: app open 377 ms, frames 22.6 ms during scroll and back, commands answered, popup events.

### 8.2 On the device (owner; read-only first; never during a firmware update)
Install `out/agent_buds/hibybudsd` as `/usr/data/hibymod/shell/hibybudsd`. All steps with music playing over the headphones (stutter check) and `tail -f /tmp/hibybudsd.log`.
1. **H0 identification (read-only, no channel):** `hibybudsd --list` with each pair connected: brand, model, caps. Send us the output plus `dbus-send --system --print-reply --dest=org.bluez / org.freedesktop.DBus.ObjectManager.GetManagedObjects` (Modalias, UUIDs).
2. **H1 probe:** `hibybudsd --probe <MAC> -v` (opens the channel for 15 s, prints every packet). AirPods: hello answered, battery / ear / mode / metadata. Galaxy Buds (each generation): CRC order line, the extended status hex (this fixes the layouts), version string. Nothing: which control word answered, the replies. Sony: protocol V1/V2 line, battery. Fast Pair headsets (JBL, …): `03/03`.
3. **H2 app read-only:** batteries, modes and EQ shown equal the vendor app's. Take buds out / in: wear values (AirPods, Galaxy) and auto-pause / resume.
4. **H3 writes, one at a time, each undone:** noise modes and levels; EQ preset; touch lock; one gesture (restore it); Find with the buds out of the ears; Nothing custom EQ; rename (BlueZ alias).
5. **H4 robustness:** buds into the case and out (popup once, no popup for a 5 s blip), out of range and back, BT off/on, reboot, 1 h of playback: no audio dropouts, daemon RSS < 1 MB, `top` idle 0 %.
6. **Captures we still need** (Android HCI snoop, procedure in `research_headphones_nothing_sony_jbl.md` §6.1): Galaxy Buds of every generation the owner has (extended status per model, touch codes, game mode, MANAGER_INFO effect); AirPods press-and-hold per side and AirPods 1/2 double tap; Nothing gestures per model; JBL model + family.

## 9. Risks

| Risk | Mitigation |
|---|---|
| A2DP stutter | no inquiry / scan / discovery in the daemon; control traffic is a few packets per user action on the existing ACL; SDP once per device (cached). [D] measure with LDAC and Wi-Fi on |
| Reconnect loops | backoff 2 s → 5 min, give up after 6 failures until BlueZ reconnects; popup debounced 30 s |
| Wrong layouts (Samsung tail fields, touch codes, Nothing control word) | range checks, unknown → "unknown"; every unknown id / status payload logged once; Sony read-only |
| Another device owns the vendor channel (multipoint phone app) | connect refused → backoff → "Controls not available" |
| Commands that reboot the buds (Nothing LDAC / dual connection) | not implemented |
| Daemon death | the shell thread restarts it (≤ every 30 s); commands fail fast with a toast |
| Audio routing after our `Device1.Connect` | same open question as the Wireless app (`docs/WIRELESS.md` §6) |
| Licences | LibrePods, GalaxyBudsClient (GPL-3.0), Gadgetbridge, ear (web) (AGPL): facts only, no code; all vectors are ours |
