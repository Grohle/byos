# Audio formats, Bluetooth codecs and native earbud integration (HiBy R3 II)

Date: 2026-11-10. Research only: nothing was run on the device and nothing in `src/` was changed.

The owner asked: *"Can we add more audio codecs, so every possible type is supported, and get native connection with all Samsung Buds models, the Apple equivalents (AirPods), etc.?"*

Evidence tags:
- **[V]** verified in files: the extracted rootfs `~/hiby/rootfs`, the player strings `~/hiby/hp_strings.txt` / `hp_wstrings.txt`, the disassembly, `readelf`, or library DBs pulled from the device.
- **[I]** inferred, or taken from public reverse-engineering work.
- **[D]** has to be checked on the device.

## 0. Short answer

| Question | Answer |
|---|---|
| More file formats? | **Yes, with a background converter (`hibyconv`).** The stock player already decodes almost everything common (FLAC, ALAC, APE, WAV, AIFF, DSF/DFF/ISO, MP3/MP2, AAC/M4A/M4B, Vorbis, Opus, WMA, MQA, CUE). What's missing is mostly **WavPack, TTA, MKA/WebM, Ogg-FLAC, CAF, W64/RF64, Musepack, TAK, AC3**. hibyconv converts or remuxes them in the background to FLAC (or to a container the player already reads), keeping tags and cover art. About 3–4 weeks of work. |
| More Bluetooth codecs? | **Practically no.** The device already sends **SBC (incl. XQ), AAC, aptX, aptX HD, LDAC and HiBy UAT/UAT-TWS**. The rest are either proprietary with no open encoder (aptX Adaptive/LL, LHDC, Samsung Seamless/SSC), impossible on this hardware (LE Audio/LC3 needs BT 5.2 ISO; the chip is a BCM4343A1, BT 4.x), or only useful with peers nobody owns (Opus-A2DP, FastStream, LC3plus). Adding codecs would also mean rebuilding bluealsa, and that would lose HiBy's closed UAT/HBC patches. **What is worth doing:** tune AAC (the only good codec AirPods and Galaxy Buds accept on non-Samsung sources) and add a codec picker. |
| Native AirPods / Galaxy Buds? | **Yes, feasible without proprietary SDKs.** We open a raw L2CAP socket (AirPods "AAP", PSM 0x1001) or an RFCOMM socket (Galaxy Buds, Samsung SPP UUID) from a small daemon, next to the A2DP stream. That gives battery for L/R/case, in-ear state, and ANC/transparency/adaptive/ambient, plus EQ, touch lock and Find-my-buds on Samsung. A low-duty BLE scanner adds the AirPods "lid open" popup. For generic headsets, battery is possible through HFP AT commands if bluealsa also runs `hfp-ag` (risk to evaluate). About 5–6 weeks including the iOS-style UI. |

## 1. File formats decoded by the stock player

### 1.1 How decoding is organised [V]

- **Decoders built into `hiby_player`.** The strings name the modules:
  - demuxers: `Wav Demux`, `AIF Demux`, `Flac Demux`, `M4A Demux`, `AAC Demux`, `MP3 Demux`, `OGG Demux` (recognises `vorbis`, `OpusHead`, `Speex`, `theora`), `ASF Demux`, `Ape Demux`, `DSF Demux`, `DFF Demux`, `SACD Demux` (with the `SACDMTOC` master TOC), `TS Demux`;
  - decoders: `Flac Decoder`, `ALAC Decoder`, `Ape Decoder`, `DCA Decoder` + `DTS Parser`, `AAC Parser`, `MPEG Audio`, and `Atrac3` (name only);
  - an MQA core (`Mqa::IDecoder`, `mqa_decoder`, `MQA_TAG`, `report_mqa_info`).
- **Plugin decoders, loaded with `dlopen`** ("load lib: %s", "HiByModule not found!"). Every one of them exports the symbol `HiByModule`:

  | Library | Contents [V] |
  |---|---|
  | `/usr/lib/libvorbis.so` | full libvorbis + ogg |
  | `/usr/lib/libmp3.so` | mpg123 |
  | `/usr/lib/libwma.so` | WMA decoder with Pro (`drcDecodeBlockPro`, `freqex*`) and Lossless (`prvDecMCLMSPredictor…`) code |
  | `/usr/lib/libaac.so` | FDK-AAC decoder with SBR/PS (HE-AAC v1/v2) |
  | `/usr/lib/libopus.so` | full libopus; the **encoder** is also inside |
  | `/usr/lib/libdsd2pcm.so` | DSD→PCM (`DSD2PCM_DSD64…DSD512_Q`) |

  All of them depend only on libc/libm.
- **Scanner and file-browser extension table** (UTF-16 strings in the player, `hp_wstrings.txt` lines 11–27, 40, 64):
  ```
  .mp3 .mp2 .wav .wave .wma .flac .ape .ogg .m4a .aac .dsf .dff .aif .aiff .opus .m4b   + .cue  .iso  (.m3u/.m3u8 playlists)
  ```
  **A file whose extension is not in this list never reaches the library or the browser.** That is why "unsupported" means "invisible", not "error".
- **The library DB `MEDIA_TABLE.format` column holds a RIFF `wFormatTag`-style code** [V, `~/hiby/big.db`]:
  - `85` = 0x0055 (MPEG Layer 3), for the `.mp3` rows;
  - `61868` = 0xF1AC (FLAC), for the `.flac` rows.

  The other codes follow the same scheme [I].
- Also on the rootfs, but not used by the player: `libsndfile 1.0.28` [V]. It is built **without** FLAC/Ogg ("compiled without FLAC support") and nothing links it. `libfdk-aac.so.2` (encoder+decoder) is used by bluealsa.

### 1.2 Support table

| Format | Stock player | Evidence | Note |
|---|---|---|---|
| FLAC (16/24-bit, up to 384 kHz) | ✅ | V (Flac Demux/Decoder, `.flac`) | 32-bit-int FLAC: [D] |
| ALAC (.m4a) | ✅ | V (ALAC Decoder, M4A Demux) | |
| APE (Monkey's) | ✅ | V (Ape Demux/Decoder, `.ape`) | Extra High/Insane CPU load [D] |
| WAV PCM 16/24 | ✅ | V | |
| WAV 32-bit int / 32-bit float / WAVE_FORMAT_EXTENSIBLE | ❓ | I | [D] test files; float is the likeliest gap |
| W64 / RF64 (>4 GB WAV) | ❌ | V (no extension, no strings) | |
| AIFF / AIF | ✅ | V (AIF Demux, `.aif/.aiff`) | AIFC (`.aifc`) not in the extension list |
| DSF / DFF | ✅ | V | DSD64–512 via dsd2pcm, or DoP/native bypass (`dsd_bypass`) |
| SACD ISO | ✅ | V (SACD Demux, `.iso`) | DST-compressed ISO/DFF: [D] (no "DST" decoder string found) |
| MP3 / MP2 | ✅ | V (libmp3 = mpg123) | |
| AAC (.aac ADTS), M4A, M4B | ✅ | V (libaac, M4A/AAC Demux) | HE-AAC v1/v2 ✅; `.mp4`/`.m4r`/`.3gp` extensions not listed |
| Ogg Vorbis (.ogg) | ✅ | V | `.oga` extension not listed |
| Opus (.opus) | ✅ | V | Opus inside `.ogg` probably works too (OGG demux knows `OpusHead`) [I] |
| Ogg FLAC | ❌ | I (OGG demux knows only vorbis/Opus/Speex/theora) | |
| Speex | ❌ | I (detected by the demux; no decoder lib) | rare |
| WMA (Std / Pro / Lossless) | ✅ | V (`.wma`, libwma with Pro+Lossless code) | Pro/Lossless playback [D] |
| WavPack (.wv, .wvc, DSD-WavPack) | ❌ | V (no extension, no strings) | **most requested gap** |
| TTA | ❌ | V | |
| MKA / WebM audio | ❌ | V (no EBML/Matroska) | |
| CAF (Apple) | ❌ | V | |
| Musepack (.mpc) | ❌ | V | |
| TAK | ❌ | V | no open-source encoder; decoder only in ffmpeg (LGPL) |
| AC3 / E-AC3 files | ❌ | V | |
| DTS | ⚠️ | V (DCA Decoder + DTS Parser exist) | DTS-in-WAV probably plays [I]; `.dts` is not in the extension list |
| MQA | ✅ | V (MQA core + MQA DB columns) | in FLAC |
| CUE sheets | ✅ | V (`.cue`, `cue_id`, `cuesheet` / `METADATA_BLOCK_CUESHEET` tags) | external and embedded |
| M3U / M3U8 | ✅ | V | |

### 1.3 Two ways to add formats

1. **Native decoder plugins in the player** — not recommended now. We would have to:
   - RE the `HiByModule` ABI (6 existing samples, so it is doable);
   - **binary-patch** the hard-coded `dlopen` path list and the UTF-16 extension table, through `docs/PATCHER.md`. There are no spare slots: `.m4b` → `.wv` changes the string length, so the table has to be rebuilt.

   Effort is high (≥ 3–4 weeks, risky). It is the only way to get gapless, seek-accurate *native* playback of `.wv` files. Keep it as a phase-4 option.
2. **Background converter `hibyconv`** — recommended. It converts once, the player sees a normal `.flac`, and everything keeps working: library, tags, covers, gapless, MQA detection and replaygain.

### 1.4 `hibyconv` design

**Process**
- A separate static binary: `zig cc -target mipsel-linux-musleabihf -mcpu=mips32r2 -O2 -static`, the same recipe as `tools/build_hibyplay.sh`. It is launched by the shell; it is not a resident daemon.
- Note: the rootfs libraries (libopus, libvorbis, libsndfile) are glibc `.so` files. **A static musl binary cannot `dlopen` them**, so every decoder we need is linked statically into hibyconv. That is fine, because each one is small.

**Work queue**
1. Walk the music roots: `a:\` = internal storage and the SD card (`/data/mnt/sd_0`).
2. Find files with the extensions in the table below.
3. Skip files that already have a newer `.flac` sibling.
4. Keep the queue in `/usr/data/hibymod/conv/queue.txt`, so that it resumes after a reboot.

**Actions per input** (remux = no re-encode, bit-exact, I/O-bound)

| Input | Action | Library (licence) | Effort |
|---|---|---|---|
| `.wv` PCM (+ `.wvc` correction file) | decode → FLAC | libwavpack static (BSD-3) | 1 d |
| `.wv` DSD | repack → **DSF** (lossless, native DSD playback kept) | libwavpack | +0.5 d |
| `.tta` | decode → FLAC | own decoder written from the TTA format description (≈ 600 lines; avoids the LGPL ffmpeg code) | 2 d |
| `.mka` / `.webm` | EBML demux, then: Opus → `.opus` (Ogg remux) · Vorbis → `.ogg` (Ogg remux, rebuild granule positions) · FLAC → `.flac` remux · PCM → FLAC · AAC → `.m4a` (minimal MP4 mux) or decode → FLAC · ALAC → FLAC | own EBML parser (small) | 3–4 d |
| `.oga` (Ogg FLAC) | remux → native `.flac` | own code | 0.5 d |
| `.oga` (Vorbis/Opus) | rename → `.ogg` / `.opus` | — | trivial |
| `.mp4` / `.m4r` / `.3gp` audio-only | rename/hard-link → `.m4a` | — | trivial |
| `.caf` | ALAC → FLAC (Apple ALAC decoder, Apache-2.0) · PCM → FLAC · AAC → `.m4a` remux | Apple ALAC | 1.5 d |
| `.w64` / `.rf64` / float or 32-bit WAV (if [D] confirms the gap) | parse → FLAC. Float → 24-bit with TPDF dither (float audio has a 24-bit mantissa, so nothing audible is lost) | own parsers | 1 d |
| `.mpc` (Musepack SV7/SV8) | decode → FLAC 16/24 | libmpcdec (BSD) | 1 d |
| `.ac3` / `.eac3` / `.dts` / `.tak` / `.spx` | optional, phase 4. These are lossy (or TAK) inputs: decode → FLAC inflates size 4–6× | ffmpeg's decoders (LGPL-2.1, only in a separate binary with sources offered) | 2–3 d |

**FLAC encoder**
- Use **libFLAC 1.4.x static** (BSD-3, ~300 KB code), levels 0–8, default **level 2**:
  - level 2 uses only fixed predictors plus mid/side, so it is fast;
  - files are ~3–5 % bigger than level 5, and compression can be configured.
- A hand-written fixed-predictor encoder (~400 lines) would be smaller still, but libFLAC is already battle-tested, gives `--verify`, and writes STREAMINFO MD5 and seek tables. Not worth the risk.
- Output 16 or 24-bit FLAC. 32-bit int sources get 24-bit output with dither unless [D] shows the player decodes 32-bit FLAC.

**Tags and cover**
- Tag sources mapped to Vorbis comments:
  - APEv2 (WV, TTA, MPC);
  - ID3v2 (TTA, WAV `id3 ` chunk);
  - RIFF `LIST/INFO`;
  - Matroska `SimpleTag`s;
  - CAF `info` chunk.
- Covers become a FLAC `PICTURE` type 3, taken from:
  - APEv2 `Cover Art (Front)`;
  - ID3 `APIC`;
  - MKA attachments `cover.*`.
- ReplayGain keys are copied (the player reads `replaygain_*` [V]).
- An embedded cuesheet goes to the `CUESHEET` comment, which the player reads [V].
- Write the tags with `src/hibytag.c` (FLAC writer + `ht_set_cover` already exist) or with libFLAC's metadata API.

**Safety**
- Write to `name.flac.part` in the same folder, then `fsync`, then `rename`. The original is never touched.
- Default policy: **keep the original**. The player cannot see it anyway, because of the extension filter.
- Options: "move originals to `.hibyconv/originals`" or "delete after verify". Verify = decode the FLAC again and compare the MD5 with the source PCM.

**Library**
- After each album, either insert the rows with `ht_db_*` (hibytag already updates `usrlocal_media.db`) or ask for the player's rescan. Choose after looking at `research_play_list.md`.

**CPU / RAM budget**
- The CPU is a single-core XBurst at ~1104 MHz [V, research_overclock_power.md]. MemAvailable is ~12–13 MB with the player running [V, research_mpd_player.md].
- hibyconv runs under `SCHED_IDLE` + `nice 19` + `ioprio idle`, so it only takes CPU nobody else wants. Playback and 60 fps UI are not affected [I]. The scheduler class is the key: on one core, `nice` alone still steals slices.
- Default schedule: run only when **charging, or screen off and not playing**, or on demand from a settings page with progress.
- It pauses when MemAvailable < 6 MB, or when the shell signals that an animation or a heavy app is active (SIGSTOP/SIGCONT from the shell).
- RSS target ≤ 2 MB: libFLAC encoder state ~0.5 MB, WavPack decoder < 0.3 MB, 64 KB I/O buffers.
- Speed [I, to be measured]:
  - WavPack-normal decode + FLAC-2 encode: ~15–25× real time, i.e. a 40-minute album in ~2–3 min of idle CPU;
  - remux jobs are limited by SD speed.
- Battery: a 1,000-file WavPack library ≈ 1 h of CPU, done while charging.

**UI**
- Settings › Music › "Unsupported files": count by format, a "Convert now" button, a progress card ("Converting 12 of 87 · WavPack → FLAC"), and the originals policy.
- When a new unsupported file appears, a notification in the shell's banner style.
- All of it in EN/FR/ES plus the 13 languages of `tr_lang()`.

**Effort:** core and queue 3–4 d · WavPack 1.5 d · TTA 2 d · MKA/WebM 3–4 d · Ogg/rename cases 0.5 d · CAF 1.5 d · W64/RF64/float 1 d · Musepack 1 d · UI and i18n 2 d · PC tests (golden files, MD5 round-trip) 2 d. **≈ 17–20 days.**

## 2. Bluetooth codecs

### 2.1 What is on the rootfs [V]

**Stack**
- `bt_init` starts `bluetoothd -E -C` (BlueZ **5.54**), `bt-agent`, and **`bluealsa -p a2dp-source --a2dp-volume`**. No `-c`, so no explicit codec list.
- bluealsa is **v4.1.1, patched by HiBy**:
  - it sends `BT:CODEC %s Sampling %d` / `Playback` messages to sys_server;
  - it reads `/usr/data/bluealsa` and `/usr/data/alsa.conf` (`codec "ldac"`, `ldac_eqmid`, `uat_eqmid`);
  - its source files include `a2dp-uat.c`, `a2dp-uat_tws.c`, `codec-uat.c` and a statically linked **HiBy "HBC" codec** (`../hbc/hbc_encoder.c`, `hbc_mdct.c`…).
- bluealsa's NEEDED libraries: `libsbc`, `libfdk-aac`, `libopenaptx`, `libldacBT_enc`, `libldacBT_abr`, `libldacdec`.

**A2DP source codecs compiled into bluealsa**

| Codec | Source module | Notes |
|---|---|---|
| SBC | `a2dp-sbc.c` | `--sbc-quality` with XQ/XQ+ (SBC Dual Channel HD) |
| AAC | `a2dp-aac.c` | fdk-aac; `--aac-bitrate`, `--aac-vbr`, `--aac-afterburner`, `--aac-true-bps` |
| aptX | `a2dp-aptx.c` | openaptx |
| aptX HD | `a2dp-aptx-hd.c` | openaptx |
| LDAC | `a2dp-ldac.c` | `LDAC_HQ`/`SQ`/`ABR` |
| UAT, UAT_TWS | `a2dp-uat.c`, `a2dp-uat_tws.c` | HiBy's own codec: `UAT_1200`/`900`/`600` |

- The names `MP3/MPEG`, `aptX-AD`, `aptX-LL`, `aptX-TWS`, `FastStream`, `LC3plus` appear only in the generic codec-name table (`a2dp-codecs.c`). **There is no implementation**: no `a2dp-faststream.c`, `a2dp-lc3plus.c`, `a2dp-mpeg.c` or opus module.
- **Runtime enablement:** upstream v4.1.1 enables only SBC and AAC by default (aptX/LDAC carry no `.enabled = true`). HiBy's player offers the menu values `ldac_audio/ldac_auto/ldac_aptx/ldac_sbc`, `bt_aptx on/off`, `aptX-HD`, `LDAC_HQ/SQ/ABR`, `UAT_1200/900/600` [V, hp_strings], and sys_server calls `SelectCodec` (`BT:CODEC:OK/FAIL`) [V]. So HiBy's patch enables them at runtime [I].
  - To confirm on the device [D]: `dbus-send --system --print-reply --dest=org.bluealsa /org/bluealsa org.freedesktop.DBus.ObjectManager.GetManagedObjects`, then `bluealsa-cli codec <pcm-path>`.
- **AAC defaults** (upstream ba-config.c v4.1.1, HiBy does not override them on the command line): **220 kbps CBR, afterburner off, VBR off, LATM v1**.

### 2.2 Controller [V]

- `bt_init` loads `/lib/firmware/bt_bcm/BCM4343A1_001.002.009.{1010.1030|0122.0538}.hcd` with `brcm_patchram_plus` on `/dev/ttyS0` at **3 Mbaud** (H4). The module is an **AP6212** or "aw-nb372sm", i.e. a Broadcom/Cypress **BCM43438 (CYW43438)**: 2.4 GHz Wi-Fi + BT on one antenna.
- This chip is a **Bluetooth 4.1/4.2-class controller** [I]; on the Raspberry Pi with the same chip, `hciconfig -a` reports HCI 4.1. That means:
  - no LE 2M PHY;
  - no LE Isochronous Channels (BT 5.2), so **no LE Audio / LC3 / Auracast**, ever, on this hardware;
  - the kernel is 4.4.94 [V]: no `BTPROTO_ISO` (Linux ≥ 5.18 needed); BlueZ 5.54 has no BAP/ASCS (≥ 5.66). Three independent blockers.
- Confirm with `hciconfig hci0 version` [D].

### 2.3 Candidate codecs

| Codec | Status | Verdict |
|---|---|---|
| LE Audio LC3 (Buds2 Pro/Buds3, AirPods: none) | needs BT 5.2 ISO controller + kernel ≥ 5.18 + BlueZ ≥ 5.66 | **Impossible** (hardware) |
| Samsung Seamless / Scalable Codec (SSC, "UHQ") | proprietary, only licensed inside Samsung phones; no public encoder | **Not possible** |
| aptX Adaptive / aptX LL / aptX Lossless | proprietary Qualcomm; openaptx covers only aptX/aptX HD | **Not possible** |
| LHDC / LLAC | Savitech licence; no open encoder | **Not realistic** (legal + no code) |
| Opus A2DP (Android vendor codec) | open, but almost no headphone sinks (some Pixel/Android-to-BlueZ setups) | Low value |
| FastStream | open (in upstream bluealsa), only cheap dongles/old Sennheiser | Low value |
| LC3plus over A2DP | Fraunhofer licence, rare sinks | No |
| AAC tuning (AirPods, Galaxy Buds, most TWS) | already present | **Do it** (see below) |
| SBC XQ (SBC Dual Channel HD) | already present | Expose it for SBC-only buds |

**Why not rebuild bluealsa with more codecs?**
- Upstream bluealsa is MIT and builds with zig. But HiBy's UAT/HBC patches and its `BT:CODEC` messages to sys_server are closed: HiBy's bluealsa source is not published.
- Replacing the binary loses UAT and can break the player's BT state machine.
- No codec we could add helps AirPods or Galaxy Buds, which only accept SBC and AAC (Galaxy Buds also take SSC, but only from Samsung phones).

**AAC tuning** — the change that matters for the owner's earbuds. AirPods and Galaxy Buds both use AAC here.
1. A/B on the device: default 220 kbps CBR vs `--aac-afterburner --aac-vbr` (Apple sources send ~256 kbps VBR) vs `--aac-bitrate=320000`.
2. Measure bluealsa CPU (`top`), dropouts with Wi-Fi on, and listen.
3. Afterburner roughly doubles encoder CPU [I]. On this single core, expect bluealsa to go from ~5 % to ~10 % [D].
4. Mechanism: our overlay provides a wrapper `bluealsa` placed earlier in `PATH` for `bt_init`, or restarts bluealsa with the new arguments after boot. The player and sys_server have to survive a bluealsa restart [D] — do it only while BT is off.

**Codec picker in our Wireless device page**
- Reuse the player's own settings (the `ldac_*` / `bt_aptx` / `uat_*` values) through `hs_set` when they are exposed, so the player stays the owner of the choice.
- Show the active codec: parse the `BT:CODEC` messages, or read `org.bluealsa.PCM1.Codec` with dbus-send.
- 1–2 d.

## 3. Native earbud integration

### 3.1 Building blocks on this stack

**[V] from strings/binaries**
- BlueZ 5.54 exposes:
  - `ProfileManager1` (custom RFCOMM/L2CAP profiles);
  - `Device1` with `UUIDs`, `Modalias`, `ManufacturerData` and `RSSI`;
  - `Battery1`, via the built-in `battery` plugin: GATT Battery Service only, i.e. LE devices.
- There is **no** `BatteryProviderManager1` (added in 5.56) and **no** `AdvertisementMonitor` (5.56+).
- The kernel has the L2CAP, RFCOMM (socket + TTY), SCO and HIDP socket layers built in.
- Userland tools: `hcitool`, `sdptool`, `btmon`, `l2test`, `rctest`, `bluetoothctl`, `dbus-send`, `dbus-monitor`.

**Our shell is static musl and has no libdbus** (`docs/WIRELESS.md` uses `dbus-send` forks). Raw `AF_BLUETOOTH` sockets need only syscalls plus local definitions of `sockaddr_l2` / `sockaddr_rc`. So the earbud logic goes into a small static daemon, **`hibybudsd`**, that:
- watches connections, by polling `GetManagedObjects` (shared with the Wireless worker) or with a `dbus-monitor --system "type='signal',interface='org.freedesktop.DBus.Properties'"` child that wakes on `Connected` changes;
- identifies the model, opens the vendor channel, and publishes a small state struct in shared memory (`/dev/shm/hibybuds`, a seqlock like `hm_state`);
- receives commands (set ANC mode…) over a Unix datagram socket from the shell.

RSS < 1 MB; zero CPU while nothing is connected.

**Model identification**

| Brand | How |
|---|---|
| Apple / Beats | `Device1.Modalias` = `bluetooth:v004Cp20xx…`. DID vendor 0x004C; the product ID is the model, the same code as in the BLE advert [I]. Also the AAP metadata packet. |
| Samsung | `Device1.UUIDs` contains `2e73a4ad-332d-41fc-90e2-16bef06523f2` (Buds+ and later) or `00001102-0000-1000-8000-00805f9b34fd` (Galaxy Buds 2019). Name prefix ("Galaxy Buds (", "Galaxy Buds+", "Buds Live", "Buds Pro", "Buds2", "Buds2 Pro", "Buds FE", "Buds3", "Buds3 Pro"…) picks the sub-model. |
| Generic | anything else with the A2DP sink UUID `0000110b-…` |

**Audio-stutter risk**

| Activity | Air time | Risk to A2DP |
|---|---|---|
| AAP/RFCOMM control traffic (a few packets per event) | negligible, rides on the existing ACL | **none** [I] |
| BR/EDR inquiry (`bluetoothctl scan on`, transport auto) | high, page/inquiry trains | **high**. Note for the Wireless app: discovery while streaming will stutter; restrict it to `transport le` or block it while A2DP is active |
| LE scan through BlueZ discovery | kernel 4.4 mgmt discovery uses scan interval = window = 0x12, i.e. **100 % duty** [I, kernel `DISCOV_LE_SCAN_*`] | **high** while LDAC/UAT streams; acceptable for SBC/AAC at ≤ 320 kbps [D] |
| Our own LE scan on a raw HCI socket (passive, window 30 ms / interval 640 ms ≈ 5 %) | low | **low** [I]. The Broadcom coex scheduler favours ACL/A2DP slots. Needs an on-device test with LDAC 990 + Wi-Fi |

Design rules:
- **Never** BR/EDR inquiry during playback.
- BLE scanning happens only:
  - (a) while the screen is on, BT is on and **no A2DP stream** exists (pre-connection popup), or
  - (b) in short 3 s low-duty bursts when the user opens the headphone card.
- When connected, all data comes over AAP/RFCOMM, not adverts.
- About raw HCI from userland while bluetoothd owns the adapter: `hcitool lescan` does the same thing, but bluetoothd/kernel background scanning may fight for the LE scan state [I]. The fallback is BlueZ `SetDiscoveryFilter({Transport: "le", DuplicateData: true})` + `StartDiscovery` from a `bluetoothctl` child, accepting 100 % duty only when not streaming.

### 3.2 AirPods and Beats

**Licence situation**
- LibrePods (github.com/kavishdevar/librepods) is **GPL-3.0** [V, `gh api`]. Our licence is the custom non-commercial "HiByOS Community License 1.0", which adds restrictions the GPL forbids, so **no LibrePods code may be copied**.
- Protocol facts (byte layouts, opcodes) are not copyrightable. We implement from our own notes, written below in our own words, and from captures made with our own AirPods.
- Other public references: tyalie/AAP-Protocol-Defintion (Kaitai spec), pabloaul/apple-wireshark (dissector), furiousMAC/continuity (BLE adverts).

**Connection [I, from public docs]**
- L2CAP connection-oriented channel, **PSM 0x1001**, to the AirPods' BR/EDR address. It needs the ACL that A2DP already holds: `socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP)`, `BT_SECURITY_MEDIUM`.
- Every packet starts `04 00 04 00` followed by a little-endian 16-bit opcode.
- The host must first send a **hello** (`00 00 04 00 01 00 02 00` + 8 zero bytes). Then it subscribes to notifications with opcode `0x000F` and a 32-bit mask of all ones.
- An optional "host capabilities" message (opcode `0x004D`, `FF` feature byte) unlocks conversational awareness during playback and Adaptive mode on AirPods Pro 2-class models.

**Notifications from the buds**

| Opcode | Content |
|---|---|
| `0x0004` battery | count N, then N × {component, `01`, level 0–100, status, `01`}. Component: `02` right, `04` left, `08` case. Status: `01` charging, `02` discharging, `04` not connected |
| `0x0006` in-ear | two bytes (primary, secondary): `00` in ear, `01` out, `02` in case. Use it for auto-pause/resume with `pc_pause()`/`pc_play()` from `playctl.inc.c` |
| `0x0009` control/setting change | 1-byte setting ID + up to 4 data bytes. ID `0x0D` = listening mode: `01` off, `02` ANC, `03` transparency, `04` adaptive. ID `0x28` = conversational awareness (`01` on / `02` off). ID `0x2E` = adaptive noise level 0–100. ID `0x1A` = which modes the stem long press cycles (bit mask) |
| `0x004B` | conversational-awareness "user speaking" level. Use it to duck our volume, as iOS does |
| `0x001D` | metadata: NUL-separated strings — name, model number (e.g. A3048), manufacturer, serial, firmware version |

**Commands we send:** the same opcode `0x0009` with the setting ID — listening mode, CA on/off, adaptive level, long-press mode set, mic side (`0x01`), ear detection on/off (`0x0A`). Rename uses opcode `0x001E`.

**Feature matrix**

| Model | Battery L/R/case | In-ear | ANC / transparency | Adaptive / CA | Notes |
|---|---|---|---|---|---|
| AirPods 1/2 | ✅ | ✅ | — | — | |
| AirPods 3 | ✅ | ✅ | — | — | |
| AirPods 4 | ✅ | ✅ | — | — | |
| AirPods 4 ANC | ✅ | ✅ | ✅ | ✅ [I] | |
| AirPods Pro 1 | ✅ | ✅ | ✅ | — | |
| AirPods Pro 2 / Pro 2 USB-C / Pro 3 | ✅ | ✅ | ✅ | ✅ | |
| AirPods Max | ✅ (single + no case) | ✅ (on-head) | ✅ | — | |
| Beats | ✅ | model-dependent | Studio Buds/Fit Pro/Solo Pro/Studio Pro have ANC [I] | — | |

**Vendor-ID spoof (optional)**
- BlueZ's `main.conf` `DeviceID = bluetooth:004C:…` makes us look like an Apple host. LibrePods reports that this unlocks transparency customisation, hearing-aid functions and loud-sound reduction.
- BlueZ 5.54 knows `DeviceID` [V string]; our rootfs `main.conf` has none.
- Risk: other headsets may change behaviour, and it misrepresents the device. Keep it off by default as an advanced toggle, and never needed for the basics.

**BLE "proximity pairing" advert** (for the popup and for battery before AAP is up)
- Apple manufacturer data, company `0x004C`, message type `0x07`, length 0x19. Layout [I, furiousMAC]:

  | Offset | Field |
  |---|---|
  | +0 | prefix `01` |
  | +1..2 | model (LE), e.g. `0E 20` = 0x200E AirPods Pro |
  | +3 | status (which bud is in ear or case, which is primary) |
  | +4 | battery nibbles, ×10 %, 0xF = unknown; left/right order flips with a status "flip" bit |
  | +5 | high nibble = charging flags (L/R/case), low nibble = case battery ×10 % |
  | +6 | lid-open counter (it changes when the lid opens, which is the popup trigger) |
  | +7 | colour |
  | +8 | `00` |
  | +9..24 | 16 bytes encrypted |

- The encrypted part holds 1 %-precision battery. It needs the "encryption key" that AAP opcode `0x0030` returns on a paired session. That is optional; LibrePods uses OpenSSL AES for it.
- Model codes to verify against captures [I]:

  | Code | Model |
  |---|---|
  | 0x2002 | AirPods 1 |
  | 0x200F | AirPods 2 |
  | 0x2013 | AirPods 3 |
  | 0x2019 | AirPods 4 |
  | 0x201B | AirPods 4 ANC |
  | 0x200E | AirPods Pro |
  | 0x2014 | AirPods Pro 2 |
  | 0x2024 | AirPods Pro 2 USB-C |
  | 0x200A | AirPods Max |
  | 0x201F | AirPods Max USB-C (?) |
  | 0x2003 | Powerbeats3 |
  | 0x200B | Powerbeats Pro |
  | 0x200C | Solo Pro |
  | 0x2005 | BeatsX |
  | 0x2006 | Solo3 |
  | 0x2009 | Studio3 |
  | 0x2010 | Flex |
  | 0x2011 | Studio Buds |
  | 0x2012 | Fit Pro |
  | 0x2016 | Studio Buds+ |
  | 0x2017 | Studio Pro |

- Apple's one-tap pairing is iCloud-only. For **unpaired** AirPods our popup has to say "Press and hold the button on the case", then run a normal `Pair`/`Connect`, which the Wireless app already does. For **paired** AirPods, opening the lid nearby shows the battery popup and auto-connects with `Device1.Connect`.

### 3.3 Samsung Galaxy Buds

**Licence**
- GalaxyBudsClient (github.com/timschneeb/GalaxyBudsClient) is **GPL-3.0** [V], so the same clean-room rule applies.
- Its README-level "Galaxy Buds Plus RFComm Protocol Notes.md", its enums and its device specs were read for facts only.

**Transport [I]**
- RFCOMM (SPP) to the buds while A2DP is connected. Find the channel with SDP:
  - run `sdptool browse <mac>` once and cache the result, or
  - do an SDP query inside the daemon.

  Then `socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM)`.
- Service UUIDs:

  | Models | UUID |
  |---|---|
  | Galaxy Buds (2019) | `00001102-0000-1000-8000-00805f9b34fd` (Samsung "legacy") |
  | All later models | `2e73a4ad-332d-41fc-90e2-16bef06523f2` |

**Framing**

| Field | Legacy (Buds 2019) | Newer models |
|---|---|---|
| Start-of-message | `0xFE` | `0xFD` |
| Header | `type`, `size` (1 byte each) | 2 bytes LE: payload size in 10 bits, bit 13 = fragment, bit 12 = response |
| Then | `msg_id` (1 byte), payload | `msg_id` (1 byte), payload |
| Checksum | CRC-16/CCITT (XMODEM, poly 0x1021, init 0) over id + payload | same |
| End-of-message | `0xEE` | `0xDD` |

Size counts id + payload + CRC.

**Messages we need** (IDs in hex)
- Receive:
  - `0x60` STATUS_UPDATED (battery L/R/case, wearing/placement);
  - `0x61` EXTENDED_STATUS_UPDATED (full state at connect: revision, battery L/R/case, coupled, main side, placement per bud — 1 wearing, 2 not wearing, 3 in case, 4 in closed case — ambient/ANC state, EQ preset, touch lock/options, colour…). **The layout depends on model and revision byte**;
  - `0x77` NOISE_CONTROLS_UPDATE;
  - `0x9B` NOISE_REDUCTION_MODE_UPDATE;
  - `0x63` VERSION_INFO.
- Send:
  - `0x78` NOISE_CONTROLS (mode `0` off, `1` ANC, `2` ambient, `3` adaptive);
  - `0x98` SET_NOISE_REDUCTION (Buds Live on/off);
  - `0x80` SET_AMBIENT_MODE and `0x84` AMBIENT_VOLUME (Buds/Buds+);
  - `0x86` EQUALIZER (preset 0–5: normal, bass boost, soft, dynamic, clear, treble boost);
  - `0x90` LOCK_TOUCHPAD;
  - `0x92` SET_TOUCHPAD_OPTION;
  - `0xA0`/`0xA1` FIND_MY_EARBUDS start/stop;
  - `0xA2` MUTE_EARBUD;
  - `0xA7` UPDATE_TIME;
  - `0x88` MANAGER_INFO (client type, "is Samsung device" = 2, SDK). Send it as the reply after the first extended status.

**Per-model differences [I]**

| Model | Framing | Battery L/R/case | Wear detect | Noise control | Other |
|---|---|---|---|---|---|
| Galaxy Buds (2019) | legacy FE/EE, legacy UUID | ✅ | ✅ | ambient on/off + voice focus + volume 0–4 | EQ, touch lock |
| Buds+ | new | ✅ | ✅ | ambient (+extra-loud), no ANC | EQ, touch |
| Buds Live | new | ✅ | ✅ | ANC on/off (`0x98`) | EQ, touch |
| Buds Pro | new | ✅ | ✅ | off/ANC/ambient (`0x78`), ANC level, voice detect | |
| Buds2 | new | ✅ | ✅ | off/ANC/ambient | |
| Buds2 Pro | new, fragmented messages | ✅ | ✅ | off/ANC/ambient + detect conversations | 360 audio (n/a) |
| Buds FE | new | ✅ | ✅ | ANC/ambient | |
| Buds3 / Buds3 Pro (also Buds Core, Buds3 FE, Buds4/4 Pro) | new, fragmented | ✅ | ✅ | off/ANC/ambient/**adaptive** | 9-band custom EQ, ambient level max 2 |

Because the extended-status layout varies by revision, ship parsers per model and build **our own test vectors** by capturing each model with `btmon`. Do not copy GalaxyBudsClient's `.bin` test data (GPL).

**Codec:** Galaxy Buds accept SBC, AAC and SSC; SSC is Samsung-only. Here they will use **AAC**, so the AAC tuning in §2.3 applies.

### 3.4 Generic headphones (Sony, Bose, Jabra, Nothing…)

- **HFP battery** — the industry standard. Headsets report it with `AT+IPHONEACCEV` (Apple), `AT+XEVENT=BATTERY` or `AT+BIEV` once an HFP Service Level Connection exists.
  - HiBy runs bluealsa with **A2DP only**, so no SLC exists and no battery is reported.
  - bluealsa 4.1.1 **does** parse `+IPHONEACCEV`/`+XAPL` [V strings] and exposes a `Battery` property on its D-Bus RFCOMM object [V source]. Its `RegisterBatteryProvider` call will fail on BlueZ 5.54 (harmless, logged).
  - So adding `-p hfp-ag` would give battery for most headsets, with no vendor code.
  - Risks [D]: the headset may treat us as a phone (call buttons, "connected to phone" voice prompts), multipoint interactions, an extra SDP record, and the player/sys_server reaction to the new `org.bluealsa` PCMs (SCO PCMs appear). Evaluate on one headset first; keep it optional.
- **GATT Battery Service** (`Battery1`): already works for LE devices through BlueZ's built-in plugin [V]. Few classic earbuds expose it.
- **AVRCP:** absolute volume is already on (`--a2dp-volume`). AVRCP has no "battery of the sink" message toward the source.
- **Sony WH/WF-1000XM\*** (optional, later): proprietary RFCOMM protocol (UUID `96CC203E-5068-46AD-B32D-E316F5E069BA`), reverse engineered by Gadgetbridge (AGPL, same clean-room rule). It provides battery, ANC/ambient and EQ. Effort ~4 d.

### 3.5 UI, iOS-faithful

- **Connect popup** (AirPods-style bottom sheet, 300 ms spring): model render (our own drawings or icons per family), name, L/R/case battery rings with charging bolts, "Connected"/"Connect" button. It triggers on connect, or on the lid-open counter change (AirPods).
- **Status bar:** a headphone glyph + battery of the lowest bud when connected (iOS shows it in the Batteries widget, not the bar, so make it a setting).
- **Batteries widget / Control Centre:** a CC module with the headphone card (battery ×3) and a long-press sheet "Noise Control: Off / Transparency / Adaptive / Noise Cancellation" with iOS's segmented icons, plus "Conversation Awareness".
- **Wireless › device (i) page**, the same as iOS's AirPods page:
  - noise control;
  - press-and-hold modes;
  - ear detection toggle;
  - mic side;
  - rename;
  - for Samsung: EQ presets, touch lock, Find my earbuds (red warning sheet: "Loud beep, remove from ears");
  - firmware version and serial (read-only).
- **Auto-pause:** taking a bud out pauses (`pc_pause`), putting it back resumes, if playback was paused by us. This is iOS's "Automatic Ear Detection".
- Texts in the 13 languages, with EN/FR/ES first.

## 4. Feasibility summary

| Item | Feasible | Effort | Risk |
|---|---|---|---|
| hibyconv: WavPack, TTA, MKA/WebM, Ogg-FLAC, CAF, W64/RF64, Musepack → FLAC/remux | ✅ | 17–20 d | low (separate process, originals kept) |
| Native decoder plugins (HiByModule + player patch) | ⚠️ | 15–20 d | high (binary patch of the player) |
| AAC tuning (afterburner/VBR/bitrate) | ✅ | 1 d + device A/B | low-medium (bluealsa restart) |
| Codec picker / active-codec display | ✅ | 1–2 d | low |
| New BT codecs (LC3, SSC, aptX Adaptive, LHDC) | ❌ | — | hardware/licensing |
| Opus/FastStream/LC3plus A2DP | ⚠️ technically | 5+ d (rebuild bluealsa, lose UAT) | high, little value |
| AirPods/Beats: battery, in-ear, ANC/transparency/adaptive, CA, rename | ✅ | 3 d protocol + daemon share | low (clean-room; AAP on existing ACL) |
| AirPods lid-open popup (BLE adverts) | ✅ | 3 d | medium (scan vs. audio; raw HCI coexistence) |
| Galaxy Buds (all models): battery, wear, ANC/ambient/adaptive, EQ, touch lock, Find | ✅ | 4–5 d | medium (per-model layouts; need hardware for each) |
| Generic battery via HFP-AG in bluealsa | ⚠️ | 1–2 d | medium (behaviour of headsets/player with HFP) |
| Sony protocol | ✅ optional | 4 d | low |
| UI (popup, CC module, device pages, status bar, i18n) | ✅ | 5–6 d | low |

## 5. Phased plan

**Phase 0 — device facts, 0.5 d, read-only, adb by the owner**
1. `hciconfig hci0 version`, to confirm HCI/LMP 4.x.
2. bluealsa `GetManagedObjects` with a headset connected; check the codec lists.
3. `cat /usr/data/bluealsa`.
4. Test files: WAV float32 / int32 / extensible, 32-bit FLAC, DST ISO, WMA Pro/Lossless, Opus-in-.ogg.
5. `btmon` capture of AirPods and Galaxy Buds connecting, to get Modalias and UUIDs.

**Phase 1 — quick wins, about 1 week**
1. AAC A/B and an optional `bluealsa` wrapper with better AAC settings.
2. Active codec shown in the Wireless page, plus the codec picker.
3. `hibybudsd` skeleton: connection watcher, model detection, shared state, command socket. 2–3 d.
4. AirPods AAP: hello, notifications, battery, in-ear, listening mode, CA, metadata. 3 d.
5. Headphone card in CC + status-bar glyph. 2 d.

**Phase 2 — Samsung, plus the converter core, about 2 weeks**
1. Galaxy Buds framing/CRC, legacy and new; status/extended parsers per model; noise control, ambient, EQ, touch lock, Find. 4–5 d, plus captures per model.
2. hibyconv core + WavPack + Ogg-FLAC/rename + W64/RF64/float, with the settings page. ~8 d.

**Phase 3 — polish, about 2 weeks**
1. BLE low-duty scanner + AirPods popup (lid counter) + connect flow. 3 d.
2. Device (i) pages, iOS-style. 3 d.
3. hibyconv: TTA, MKA/WebM, CAF, Musepack. ~8 d.
4. Optional HFP-AG battery experiment. 1–2 d.

**Phase 4 — optional**
- Sony protocol.
- Lossy-legacy decoders (AC3/DTS/TAK/Speex) in hibyconv.
- Research spike on native `HiByModule` plugins (WavPack natively in the player).
- Apple DID spoof toggle.

**Total for the realistic scope (phases 0–3): ≈ 45–50 developer-days**, plus hardware for testing (at least AirPods Pro 2 and one or two Galaxy Buds generations).

## 6. Open items [D]

- Whether the player tolerates a bluealsa restart with new arguments while BT is off.
- BLE scan duty vs. LDAC 990 / UAT 1200 dropouts with Wi-Fi on.
- Whether raw HCI LE scan commands coexist with bluetoothd 5.54 / kernel 4.4 background scanning.
- Whether AirPods accept AAP from a host whose DID is HiBy's, without the spoof. LibrePods on stock Linux says yes for battery/ANC/ear detection.
- Galaxy Buds behaviour when the "manager" is not a Samsung phone (MANAGER_INFO = 2): firmware may hide some features.
- Player decoding of WAV float, 32-bit FLAC, DST, and WMA Pro/Lossless.
