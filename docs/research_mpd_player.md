# Replace hiby_player with MPD + a custom UI? Research (HiBy R3 II 2025)

Date: 2026-11-09. Tags: **[V]** = verified on the device or in the extracted rootfs; **[I]** = inferred or estimated (not tested on the hardware).

## 1. Audio path: feasible with standard ALSA, no proprietary ioctls

- **ALSA controls on card 0 [V]** (`amixer contents`): `Left/Right Playback Volume` (0–255), `Mute Output`, `Soft Mute`, `Digital Filter` (0–6), `ES9219 THD C2/C3_CH1/CH2` (THD compensation, 0–65535), `MQA Enable`, `MQA Lock`, `Output Port Switch` (0–7) and `DOP_EN` (bool). The PCMs are `pcmC0D0p` (I2S→ES9219) and `pcmC0D1p` (S/PDIF, dai `x1600-spdif`) [V].
- **Sample-rate and DSD-mode switching happen inside the kernel [V]**: the sound card driver (`x1600_hiby_r3ii_sound_card.ko`) calls `hbc3000_set_samplerate`, `hbc3000_set_spdif_mode` and `es9219_set_power_dac1/2` from `hw_params` and from `dop_en_put`/`codec_board_ot_put`. The HBC3000 auxiliary chip (which the kernel programs over JTAG/SPI) manages the clocks. A normal `snd_pcm_hw_params` is enough.
- **No proprietary ioctls are needed for audio [V]**: the stock player only opens `/dev/sa-config`, which merely returns the model `r3ii_2025`, and `/dev/sa_hgl_dma`, a user-space DMA used for graphics (6 MB of reserved memory), not for audio. For the analog output it writes PCM using raw ALSA ioctls (`ot_tiny_alsa`, `SNDRV_PCM_IOCTL_*`), and for Bluetooth it uses libasound (`bluealsa:DEV=%s`).
- **Stock configuration [V]** (`/usr/resource/ot_devices_2025.json`):
  - Analog sample rates from 32 k to **768 kHz**; the S/PDIF output goes up to 192 k.
  - DSD is **always sent as DoP** on the analog output (`AnalogDsdNative: "dop"`), so the maximum is DSD256 (DoP at 705.6 k).
  - Bluetooth up to 192 k and external USB DAC (card 3) up to 384 k.
- **Volume and gain [V]**: high/medium/low gain is **not an analog relay**. It is three attenuation tables (HDB/MDB/LDB) applied to the ES9219 volume register, with 101 steps, where 0 means 0 dB and 255 means silence. On top of that there is a software volume table (`SET_HW_SW_VOL_BOTH`) and `LO_VOLUME_INDEX: 100` for line out. All of this is easy to replicate. **Risk:** with the player idle, both volumes are 0 (= 0 dB, the maximum). Our daemon must set a safe volume **before** opening the PCM.
- **Ports [V/I]**:
  - [V] `sa_sound_switch` publishes jack detection in `/sys/class/switch/{headset,balance}/state`.
  - [I] The player selects the port by writing to `Output Port Switch`. The mapping of values 0–7 to headset, balanced, lineout and S/PDIF **still needs confirming**: just read `amixer cget name='Output Port Switch'` while the user switches outputs.
  - Note: `/sys/devices/platform/hm100` (the FPGA "Darwin" filters) **does not exist** on this model [V]. Only the 7 internal ES9219 filters are available here.
- **Bit-perfect [I]**: MPD with `device "hw:0,0"`, `mixer_type "none"` and no resampler should be bit-perfect. Volume is set by writing the L/R controls from our daemon, because MPD only handles a single mixer control. The stock player uses 16- and 32-bit PCM [V, strings], so MPD will have to negotiate S32 for 24-bit.
- **DSD with MPD [I]**: use `dop "yes"`. The problem is that `DOP_EN` must be enabled only while DSD is playing, and MPD does not manage per-format controls. Two solutions:
  - a small patch (~30 lines) to MPD's ALSA output (the robust option);
  - have the UI enable it when it sees `audio: dsd…` in the status (it may arrive late and cause a pop or initial noise).
  - We need to check whether `DOP_EN=on` affects normal PCM.
  - Native DSD and HiBy-style D2P do not apply. MPD does have its own DSD→PCM conversion, but at a high CPU cost [I].
- **What is lost**:
  - **Lost entirely**: MSEB, HiBy PEQ/EQ, MQA rendering (the ES9219 supports it, but the unfolding is done by the player) and L/R balance.
  - **Lost unless reimplemented**:
    - AirPlay: `shairport -o ot` sends audio **through hiby_player** via `/var/run/airplay_socket_server` [V]. We would have to switch to shairport-sync with ALSA output.
    - DLNA (`dmrd`) [V].
    - USB DAC mode (UAC gadget + `/dev/uac_sa`, managed by the player) [V].
  - **Kept**: gapless and ReplayGain (MPD supports both out of the box) and the digital filters (`Digital Filter`).

## 2. MPD on this hardware

- **CPU/ABI [V]**: XBurst at 1.1 BogoMIPS·10³, single core, no DSP ASEs declared. The stock userland is **hard-float** (hiby_player's FP ABI is "Hard float"), so MPD should be built for `mipsel-linux-musleabihf` rather than soft-float, because faad, opus, vorbis and DSD conversion use floating point.
- **Build [V/I]**: current MPD requires **C++23 (GCC 14 / clang 19)**, Meson ≥1.2 and fmt (mpd.readthedocs.io). zig 0.17.1 ships recent clang/libc++, so it is feasible [I]. It would have to be built statically, with minimal dependencies:
  - libFLAC, libmad or mpg123, faad2, opus and libogg/vorbis (or tremor, integer-only), wavpack;
  - dsf/dff are already built into MPD;
  - ALAC/M4A would need a stripped-down FFmpeg (+1–1.5 MB).
  - Estimate: 1–2 weeks to get a stable toolchain [I].
- **RAM [I]**: the core takes about 3–5 MB of RSS if `audio_buffer_size` is lowered to 1–2 MB (the default is 4 MB). **The problem is the database**: MPD keeps it entirely in memory, at about 0.4–1 KB per track (less with a minimal `metadata_to_use`). In practice:

  | Tracks | Database in RAM | Fits in ~12 MB? |
  |---|---|---|
  | 2 500 | ~1–2.5 MB | Yes |
  | 10 000 | ~4–10 MB | Barely |
  | 50 000 | ~20–50 MB | No |

  For large collections: MPD without a database (playback by path) plus our own SQLite index in the UI.
- **Startup [I]**: 1–3 s with 10 k tracks from the SD card. The first scan of 50 k tracks would take tens of minutes.
- **Decoding cost [I]** (by analogy with ARM11 at 700 MHz / Cortex-A5): FLAC 16/44 ~5–10 %, FLAC 24/96 ~10–20 %, MP3 (libmad) ~10–20 %, Opus/AAC ~15–25 % in hard-float. DoP DSD256 is cheap (just packing). DSD→PCM in float goes up to 30–60 %.

## 3. Custom UI (fb0 + evdev)

- **Recommended base**: LVGL v9 with its `lv_linux_fbdev` and `lv_evdev` drivers. The goodix touchscreen is type-A multitouch in pixels [V], and LVGL uses the first point. Keys are read from event0/2/3 [V].
- **RAM [I]**:
  - Draw buffers: 2 × 1/10 of the screen = 60 KB, or 1 full buffer of 300 KB (320×480×2).
  - The mmapped fb (600 KB) is kernel memory.
  - LVGL heap: 0.5–1 MB.
  - 300×300 RGB565 cover art: 180 KB.
  - Pre-rendered Latin fonts (lv_font_conv, 4 bpp): 50–150 KB per size.
  - For CJK, `tiny_ttf` (stb_truetype) with a glyph cache, because pre-rendered fonts would take MBs.
  - **UI total: ~3–6 MB of RSS. With MPD, ~7–12 MB**: it fits in the budget only if the stock player is not running [I].
- **iOS-style transitions [I]**: no need to re-render LVGL every frame.
  1. Take an `lv_snapshot` of the source screen and another of the destination screen.
  2. Animate an eased slide by copying rows (300 KB per frame, ~1–2 ms).
  3. Present it by panning the double buffer (yoffset).
  - Avoid full-screen opacity, shadows and blur.
  - The goal is to stay in sync with the panel's ~44 Hz [V], or 22 Hz in heavy scenes.

## 4. Integration and risks

- **[V] `hiby_player.sh` runs `reboot` when hiby_player exits.** The launcher must be changed before anything else. The active player is already `/usr/data/hibymod/fast/hiby_player`, so there is an injection point.
- **Exclusivity**: hiby_player keeps fb0, `sa_hgl_dma` and all 5 evdev devices open [V], and uses about 14 MB of RSS [V]. With MemAvailable at ~12–13 MB [V], **the two players with UI cannot coexist**. A hybrid has to be sequential: one stops and the other is launched.
- **Safe fallback**: a launcher that reads a flag (e.g. `/usr/data/hibymod/player=stock|mpd`) and goes back to stock if the alternative fails N times or if a key is pressed at boot. The alternative must run **without a reboot loop**.
- **sys_server** [V] would still be required, because it handles SD and USB-MSC mounting (`MOUNT:*` via `/var/run/sys_server`), bluez/bluealsa D-Bus, shairport on/off and MPRIS. The UI would have to speak that socket protocol, which is undocumented.
- **Bluetooth [V/I]**: bluealsa is already running and the `libasound_module_pcm_bluealsa.so` plugin is present [V]. MPD can use `device "bluealsa:DEV=…"` as an output [I]. Pairing over D-Bus would be up to our UI.
- **USB DAC (gadget mode)**: implemented by the stock player. Redoing it is expensive.

## 5. Effort and recommendation

**Recommendation:** **(a)** for daily use, and **(b)** only as a prototype with go/no-go criteria. **Do not go for (c) for now.** PCM and DoP audio is technically feasible without blobs, but reaching real parity (MSEB/PEQ, USB DAC, AirPlay/DLNA, MQA, power management and a large library in 56 MB of RAM) would cost about 3–6 person-months [I]. In addition, the most valued features of the stock player would be lost or would have to be rewritten.

| Phase | Content | Effort [I] |
|---|---|---|
| 0. Verification (gate) | Static hard-float MPD. With the user present and the stock player stopped: `aplay --dump-hw-params` on hw:0,0, `Output Port Switch` mapping, DoP test with `DOP_EN`, RSS and CPU measurement with 2.5 k and 10 k tracks | 1–2 wk |
| 1. Headless engine | MPD + control daemon (HDB/MDB/LDB tables, ports, filter, DoP, safe volume) + launcher with fallback | 2–3 wk |
| 2. Basic LVGL UI | Now playing, queue, browsing, settings, keys, snapshot transitions | 4–6 wk |
| 3. Library | Own SQLite index, cover-art cache, CJK fonts | 2–3 wk |
| 4. System | BT/bluealsa, suspend and battery, sys_server protocol | 3–5 wk |
| 5. Extras | PEQ (custom MPD filter with biquads), AirPlay with shairport-sync, (USB DAC) | 3–6 wk |

**Next steps if (b) is pursued:**
1. Replace `hiby_player.sh` with a launcher that has a selector and fallback, without `reboot`.
2. Run Phase 0 and decide with these criteria:
   - bit-perfect output at 44.1–768 k;
   - DoP working;
   - MPD + UI under 10 MB of RSS with the real library.
3. If MPD's database does not fit, consider a lighter custom engine (dr_flac/minimp3 + ALSA inside the same app), which would be around 2–4 MB.
