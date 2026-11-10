# Hidden firmware features and audio improvements (HiBy R3 II 2025)

Date: 2026-11-09.

**Tags used in this document:**
- **[V]** verified: in the rootfs, by disassembly, or with read-only reads on the device.
- **[I]** inferred: not tested on the hardware.

**New tools in `~/hiby/tools/`:**

| Tool | What it does |
|---|---|
| `xref.py` | References to strings in `hiby_player` |
| `hpdis.py`, `d3.sh` | Disassemble a range with annotations |
| `callers.py`, `callargs.py` | Calls to a function and their `$a0` |
| `memref.py` | Accesses to globals |
| `jt.py` | Jump tables |
| `kodis.py`, `korel.py` | Disassembly and relocations of the `.ko` files |
| `ctl.py` | ES9219 ALSA controls |

They require capstone and pyelftools, which are already installed in `~/hiby/venv`. The disassembly cache is in `~/hiby/hp_dis.pkl`.

---

## 0. Summary

1. **There is no hidden power** [V]. The High/Low "gain" is not analog: it is digital attenuation of the ES9219 volume register.
   - **High** uses the `HDB` table: at volume 100 it writes 0, i.e. 0 dB, the maximum.
   - **Low** uses the `MDB` table, not `LDB`: at volume 100 it writes 12, about −6 dB.
   - The `LDB` table (−12 dB) is not used.
   - With High gain and volume 100 the output is already at full scale. There is no "turbo" or "super gain" mode in the binary or in the kernel.
   - Line Out follows **the same analog path** as the 3.5 mm headphone output, with the volume fixed at maximum.
2. **The digital filter is broken or crippled: only 2 of the ES9219's 8 filters work** [V].
   - The UI offers 3 options.
   - The player writes `0` for the first and `6` for the other two.
   - On top of that, the kernel driver turns any value other than 6 into 0.
   - That is why "Slow" and "Minimum phase" sound the same, and filters 1, 2, 3, 4 and 7 are unreachable.
   - **They can be enabled by patching 1 instruction of the module** (section 2.2). This is the most valuable real audio improvement.
3. **EU volume limit** [V]. `config.json` (2025 profile) sets `vol_warn_enable=1`, with a warning at 34 (3.5 mm) and 28 (4.4 mm).
   - There is a hard cap of 50/40 (`lock_vol`), but only if the developer option **"Volume lock"** is enabled.
   - In the user's `user.ini` that option is **disabled** (offset 0x77c = 0). There is no active cap; only the "turn up above the recommended level?" dialog and the prolonged-exposure reminder.
4. **Developer mode is already enabled** on the device (param 0x7b, `user.ini`+0x780 = 1) [V]. It only provides "Volume lock" and screenshots with the Play button.
5. There are **hidden switches triggered by files on the SD card** [V]:
   - factory mode;
   - *monkey test* (dangerous!);
   - battery log;
   - cover art dump.
6. There are **features disabled by configuration** [V] that can be enabled via bind-mount:
   - car mode;
   - double tap to wake;
   - standby;
   - song DB on the SD card;
   - image cache on the SD card;
   - instruction manual;
   - "USB current limited" in DAC mode.
7. **"Lab" sound settings** exposed in ALSA and not used by the player [V]:
   - ES9219 THD C2/C3 compensation, with a factory value of 0;
   - a sysfs interface for raw register writes: `/sys/bus/i2c/devices/3-0048/write_reg_val`.

---

## 1. Gain, volume and power

### 1.1 The gain tables and what each mode uses [V]

**Where the tables come from.** `usr/resource/ot_devices_2025.json` contains `VOLUMES[HW].Gains` with three tables:
- `HDB`: 255, 176 … 1, 0;
- `MDB`: … 13, 12;
- `LDB`: … 25, 24.

All three have 101 steps and apply to the ES9219 volume register, in 0.5 dB steps.

**Selection in the code.** Function `0x47d060` in `hiby_player` picks the table:

```
gain != 0  ->  ot_set_gain("HDB")    (High)
gain == 0  ->  ot_set_gain("MDB")    (Low)
```

`LDB` is only imported ("Imported %d LDB gains") and never selected. The UI has only two icons (`menu/gain_l.png` and `gain_h.png`) and `/data/menu_cfg` includes the `gain` tile in the drop-down menu.

**Hardware and software volume.**
- With `SET_HW_SW_VOL_BOTH: 1`, the user volume is applied in **software** (`SW.HDB` table, in 0.1 dB: −120 dB … 0).
- The HW register only sets the gain ceiling.
- In a live read, `amixer` shows `Left/Right Playback Volume = 0` [V], i.e. 0 dB: consistent with High gain.

**Driver.**
- `es9219_vol_left_put` writes register 0x0F and `..._right_put` writes 0x10, on both chips.
- The *master trim* (registers 0x11–0x14) is `0x7FFFFFFF` (`es9219_reg_def_*`), which is the maximum.
- **There is no digital headroom left.**

**Ports and amplifier.**
- The ES9219 is a DAC + integrated headphone amplifier. There is no external amplifier chip with gain control.
- `codec_board_ot_put` only touches: GPIO `ch_sel` PA15, `mute` PC27, the `aldo3` regulator, the DAC power (PA10/PA11) and the HBC3000 GPO outputs 0x1b/0x1c (section 1.2).
- No "analog gain" bit has been found in the driver.

What we don't know [I]: the ES9219C might have publicly undocumented mode or gain registers for the internal amplifier.
- The default tables contain registers with no known name: 0x1A=0x62, 0x1B=0xC4, 0x20=0x1B, 0x27=0xCC, 0x86=0xCC (only on the "negative" chip), 0x92=0xFC and 0x93=0x01.
- Without the ES9219C datasheet, touching them is **experimenting blind, with a risk of full-scale noise in the headphones**. Not recommended.

**Conclusion: the only way to get more power is the 4.4 mm balanced output**, which uses both ES9219s in bridge mode, or an external amplifier.

### 1.2 `Output Port Switch` (0–7): actual meaning [V]

This answers the open question in `research_mpd_player.md`. Disassembling `codec_board_ot_put` in `x1600_hiby_r3ii_sound_card.ko` gives:

| Value | Output | What it does |
|---|---|---|
| 0, 5, 6, 7 | none | `hbc3000_disable`, DAC1 and DAC2 off, GPO 0x1b and 0x1c to 0, regulator off |
| 1 | 3.5 mm headphones | mute, `spdif_mode(0)`, `hbc3000_enable`, **DAC1 on, DAC2 off**, GPO 0x1c 0→1, `ch_sel=0`, `aldo3` on, unmute |
| 2 | Line Out | **identical to 1**. Switching 1↔2 does nothing in the driver. Only the fixed volume changes (`LO_VOLUME_INDEX: 100`) |
| 3 | 4.4 mm balanced | **DAC1 and DAC2 on**, GPO 0x1c and 0x1b on, `ch_sel=1`, regulator on, unmute |
| 4 | S/PDIF | `spdif_mode(1)`, HBC3000 on, both DACs off |

Values 5–7 do nothing new: there are no hidden modes.

When idle, the player leaves the port at 0. `amixer` showed `Output Port Switch=0` and `headset=1` [V].

### 1.3 EU volume limits and warnings [V]

**Configuration file.** `/usr/resource/config.json` (`z:\config.json`) corresponds to the "R3II_2025" profile. Its `volume` section:

```json
"vol_warn_enable":1,
"lock_vol":[{"headset":50},{"balance":40}],
"default_vol":[{"headset":20},{"balance":15}],
"warn_vol":[{"headset":34},{"balance":28}]
```

The old `config_r3ii.json` (unused) had `vol_warn_enable:0`, `lock_vol:100` and `warn_vol:102`.

**Parsing.** Done by `0x42dfe0`, which stores it in global `0x969594`. The getter is `0x42ddc0(type)`:

| Type | Field |
|---|---|
| 0 | `vol_warn_enable` |
| 1 | `lock_vol[port]` |
| 2 | `default_vol` |
| 3 | `warn_vol` |

**`warn_vol`.**
- `0x42a0a0` returns `warn_vol[port]`, or 101 if there is no configuration.
- `0x420b58` compares it with the volume and shows the `vol_ask` dialog: "Want to turn up the volume above the recommended level?".

**`lock_vol`.**
- `0x42aa20` returns `lock_vol[port]` **only if param 0x7a == 1**; otherwise it returns 100.
- `0x42ca60` then clamps the "maximum volume" (param 0x10) and the current volume to that value.
- Param 0x7a is the **Developer options → "Volume lock"** option (`0x4e7c40`: list `volume_locked` / `screen_short`).
- In the user's `user.ini`, offset 0x77c (param 0x7a) is **0**: **there is no cap**.
- In the same file, offset 0x238 (param 0x10, "Max volume") is 100.

**`volume_long_time_high`.** This is the prolonged-exposure warning (EN 50332-3), dialog `volume_long_time_high.dlg`. It is controlled by param 0x77 (`0x535580`).

**Region.** It does not affect the limits. `/usr/data/region` is `lang=en` and the country in `user.ini` is "ES". The limits come only from `config.json`.

### 1.4 Other `ot_devices_2025.json` keys understood by the binary [V/I]

The parser `0x73d230` reads these keys:
- `VERSION`, `SET_HW_SW_VOL_BOTH`, `CHANGE_PRIORITY`;
- `OLD_LIBSMART_AUDIO`;
- `DOP_MAX_VOLUME`, `USB_OUT_MAX_VOLUME`, `SPDIF_MAX_VOLUME`, `SPDIF_MAX_192K`;
- `USE_VOLUME_CHIP`, `LO_VOLUME_INDEX`, `A2DP_FIXED_GAIN`;
- `ANALOG_PCM_HW_SAMPLE_MAX`, `SW_VOLUME_FILTER`, `SW_MIX_GAIN_ENABLE`;
- `SampleBit`;
- `AnalogDsdD2p/Dop/Native`.

The R3 II 2025 file **sets only 4 of them**. The exact semantics of the rest have not been disassembled [I]. The interesting ones:

- `LO_VOLUME_INDEX` (currently 100): fixed Line Out level. Lowering it (e.g. to 80) gives an attenuated line out for sensitive amplifiers. Low risk, since it lowers the level.
- `AnalogDsdD2p: "dop"`. Even if the user picks "DSD → PCM" in the UI, output goes out as DoP.
  - Changing it to `"d2p"` [I] would use `libdsd2pcm.so`, which exists in `usr/lib`. DSD would then go through PEQ/MSEB and the software volume.
  - High CPU cost for DSD256 on a single core. Needs testing.
- `SET_HW_SW_VOL_BOTH` / `USE_VOLUME_CHIP` [I]: could move the volume to the ES9219 register, which works in 32 bits inside the chip, instead of software.
  - **Risk:** if misconfigured, the HW stays at 0 dB with the software volume bypassed, which means maximum volume.
  - Test only with headphones unplugged.

### 1.5 Output bit depth [V/I]

- `pcm_tiny_alsa_init` (`0x7385c0`) opens the PCM in 32 bits if the chain delivers 32 (`PCM_FORMAT_PCM 32bit`) and in 16 bits otherwise [V].
- If a 16-bit FLAC goes out at 16 bits **with software volume**, at volume 30 (≈ −20 dB) about 3.3 bits of effective resolution are lost [I].
- It cannot be checked live: the kernel has no `/proc/asound` [V].
- **To do:** log `SNDRV_PCM_IOCTL_HW_PARAMS` with the existing LD_PRELOAD shim. If it goes out at 16 bits, forcing 32 bits (or HW volume) would be an audible improvement at low volumes.

---

## 2. Digital filters and THD compensation

### 2.1 The bug [V]

**Player.** `system_if_set_digital_filter(type)` is at `0x480a80`. The internal strings are "fast rolloff(default)", "slow rolloff" and "minimum phase".

```
type 0 -> writes 0 to ALSA 'Digital Filter'
type 1 or 2 -> writes 6
```

**Kernel.** In `codec_es9219.ko`, the function `es9219_digital_filter_put` (`.text+0x1674`) does:

```
xori  v0, a0, 6
movn  a0, zero, v0        ; if value != 6 -> 0
sll   a0, a0, 5           ; -> es9219_set_digital_filter(): reg 0x07, mask 0xE0
```

**Effective `filter_shape` values (bits 7:5 of register 7):**
- `0`: fast, linear phase;
- `6`: per the Sabre family naming (ES9038Q2M and similar), "hybrid fast roll-off, minimum phase" [I].

The UI (`settings_digital_filter.dlg`: sharp / slow / mp_sharp) promises 3 filters, but "slow" and "minimum phase" are **the same filter**.

### 2.2 How to enable all 8 filters (recommended)

The register 7 `filter_shape` values in the Sabre family are [I, to be confirmed on the ES9219]:

| Value | Filter |
|---|---|
| 0 | fast, linear phase (default) |
| 1 | slow, linear phase |
| 2 | fast, minimum phase |
| 3 | slow, minimum phase |
| 4 | apodizing, fast, linear phase |
| 5 | reserved |
| 6 | hybrid, fast, minimum phase |
| 7 | brickwall |

**Module patch** (`module_driver/codec_es9219.ko`, original md5 `0d698e6f04fe301a063aec53b726f7cd`):

| File offset | Original | New | Effect |
|---|---|---|---|
| `0x16e4` | `0b 20 02 00` (`movn a0,zero,v0`) | `00 00 00 00` (`nop`) | The ALSA value *v* goes straight through: filter *v* (`v<<5`) |
| `0x260c` and `0x2610` | `06 00 00 00` ×2 (max and platform_max) | `07 00 00 00` ×2 | Allows value 7 (brickwall) |

**Behavior with the patch:**
- `user_filter_setting` persists in the kernel, and `es9219_dai_prepare` reapplies it every time the PCM is opened. It is enough to write it once after the player starts or changes the filter: `amixer -c0 cset name='Digital Filter' N`.
- The player will still write 0 or 6 when the user touches the original setting. Options: hide that setting in the theme, or have `hibyshell` reapply the chosen value (stored in `/usr/data/hibymod/audio.conf`).

**Low risk.**
- No value of register 7 can damage anything: it only changes the filter shape.
- If the `.ko` were broken, the sound card would not load (no audio), but the system still boots. It can be recovered with `hibymod reinstall` or with the recovery.

**Alternative without touching the kernel:** `echo "7 20" > /sys/bus/i2c/devices/3-0048/write_reg_val` (register 7 = 0x20 → filter 1). Problem: `es9219_dai_prepare` overwrites it on every track, because it calls `es9219_set_digital_filter(user_filter_setting)`, which rewrites bits 7:5. It would have to be repeated after every `SNDRV_PCM_IOCTL_PREPARE`, with a hook in the LD_PRELOAD shim. It is a worse solution.

### 2.3 THD C2/C3 compensation [V]

- The ALSA controls `ES9219 THD C2_CH1/CH2` and `C3_CH1/CH2` range from 0 to 65535 and are currently 0 [V].
- They write registers 0x16/0x17 (C2) and 0x18/0x19 (C3), plus 0x30=0, on both chips.
- **The player never uses them** (there is no reference in `hiby_player`).
- They are meant for calibrating the cancellation of the 2nd and 3rd harmonics of each board. They can also be used as "coloring" (injecting H2, a "tube" effect, similar to the "Harmonic Controller" on other HiBy models).
- Without an audio analyzer, tuning is by ear.
- **Risk:** it cannot damage anything. Extreme values produce audible distortion.
- **Recommendation:** experimental, with gentle presets (e.g. C2 = ±200…800) and always with a "reset to 0" button.

### 2.4 Raw sysfs register access [V]

In `/sys/bus/i2c/devices/3-0048/` there are three attributes, all **write-only** (mode 0200):
- `write_reg_val`: `"%x %hhx"`. Writes the register on the powered DACs **and stores it in the default-values table**, which is reapplied on every `prepare`.
- `reg_val`: `"w reg val"` or `"r reg n"`. Reading prints nothing: useless.
- `write_lr_flag`.

`/dev/i2c-3` and `CONFIG_I2C_CHARDEV` also exist [V]. With `I2C_SLAVE_FORCE`, the actual registers of chips 0x48 and 0x49 could be **read** from a custom tool. Useful to confirm the filter and THD state.

⚠️ Do not write other registers without the datasheet. Registers 0x01 (input), 0x0C (DPLL) and 0x0F/0x10 (volume; 0 = 0 dB) can produce **noise or a full-scale signal**. Always test with headphones unplugged.

---

## 3. Hidden or disabled features

### 3.1 `set_functions.json` (System settings) [V]

Format: `{"key":0/1}`. Keys set to 0 out of the box:

| Key | What it is | Assessment |
|---|---|---|
| `usb_mode` | USB mode selection (already enabled in phase 1 of the mod) | — |
| `car_mode` / `car_mode_auto_play` | Car mode: plays when USB power is connected and shuts down when it is removed (`car_mode_shutdown`) | Useful in the car; low risk |
| `standby` | Deep standby (`test_standby.dlg` exists) | Test power draw; low risk |
| `double_touch_wakeup` | Double tap to wake | Needs gt9xx support [I]; may drain battery |
| `dac_charge_disable` | "USB current limited": does not charge from the phone in DAC mode | Useful with phones; no risk |
| `operating_instruction` | Manual with QR code | Irrelevant |

The binary knows more keys that do not appear in the JSON:
- `update_type`, `brightness`, `bkl_time`;
- `stream_media`: **can be used to hide Tidal/Qobuz**, which the user asked for;
- `step_record`: pedometer. There is no g-sensor driver in `module_driver`, so it does not apply;
- `wireless_set`, `bluetooth_set`.

### 3.2 `config.json` → `function.fcn0` [V]

| Key | Default value | What it does |
|---|---|---|
| `tf_music_db_enable` | 0 | Stores the song DB on the SD card (`/data/mnt/sd_0/.temp/usrlocal_media.db`). The UI itself recommends it "when there are many songs". Reduces UBIFS wear. **Interesting for large libraries**, but its effect on performance and RAM needs testing |
| `tf_image_cache_enable` | 0 | Cover art cache on the SD card (`.temp/image_cache.db`) |
| `lyric_color_enable` | 0 | Lyrics color (already enabled in the mod) |
| `playmenu_eq_disable` | 0 | — |
| `dark_theme_enable`, `screen_short_enable`, `dac_to_store`, `book_set_percent`, `most_played` | 1 | — |

### 3.3 Developer mode [V]

- It is enabled from About with several taps ("Perform 2 more steps…", `0x52b820`) and is stored in param 0x7b.
- **It is already active on this device** (`user.ini`+0x780 = 1).
- Options: **Volume lock** (EU cap 50/40, currently disabled) and **screenshot with a short press of Play** (param 0x80).
- The string `gain_enable` ("Enable gain adjustment") exists in `developer_options.ini`, but it is not in the options list of this version.

### 3.4 File switches on the SD card (`/data/mnt/sd_0/…`) [V]

| File | Effect | Risk |
|---|---|---|
| `hiby_linux_factory_mode` | Starts the factory activity `hiby_test` (`test/hiby_test.view`), with tests for LCD, keys, LED, audio, BT, charging, OTG, FM, RF, touch, WiFi, TF, version, QR and standby | Low-medium. May change settings. Remove the file to exit |
| `hiby_linux_auto_test` | **Monkey test**: injects random taps (`MONKEY`, `autotest_capture`) | ⚠️ **High**: may delete songs or change settings. Do not use |
| `battery_log.txt` (if present) | Enables param 0x16: periodically appends a "date percentage" line | None. Useful for measuring battery life |
| `get_cover_switch` | Creates `list_cover/` and `plane_cover/` on the SD card and enables a cover art dump [I: probably the already-decoded thumbnails] | Low. Could let `hibyshell` reuse cover art |

Other leads not investigated in depth: `a:\log.txt`, `debug.txt` (GUI heap dump) and `/etc/bluetooth/pts_enable` (BT PTS tests).

### 3.5 Features from other models that do not apply here [V]

The same binary also serves the TempoTec V3 (`V3_ANALOG_2025`), Hidizs AP80 PRO MAX and R3PROII. These features do not apply to the R3 II 2025:
- "Darwin" FPGA filters and `osr_bypass` (`/sys/devices/platform/hm100`, which does not exist);
- "Harmonic Controller" (`/data/harmonic.json`, `getEarphoneSet`);
- NOS, DRE (`NOS_EN`/`DRE_EN`: those ALSA controls do not exist);
- sound field.

Forcing them on has no hardware behind it.

### 3.6 Battery charging (general improvement) [V/I]

- `module_driver/axp2101.sh` loads the driver with `charge_voltage_limit=4400` and `charge_term_current=100`.
- However, the battery reports `POWER_SUPPLY_VOLTAGE_MAX_DESIGN=4350000`, and at the end of charging ("Full") it measured 4.313 V [V].
- The 4400 may be just a nominal driver value [I].
- Lowering it to 4350 (or to 4200 in a "battery care" mode) would extend battery life at the cost of about 5–10 % capacity. Low risk, because lowering is safe.
- Before changing it, `/sys/module/axp2101/parameters/charge_voltage_limit` should be read. That was not possible: adb disconnected because the device went to sleep.

---

## 4. Prioritized recommendations

| # | Action | Benefit | Hearing/damage risk | Brick risk | Implementation |
|---|---|---|---|---|---|
| 1 | **Digital filter patch** (0x16e4 + max=7) and an 8-filter selector in `hibyshell` | Real and audible, the only "quality" improvement available | None | Low (no audio in the worst case; recovery) | Patch the `.ko` when building the rootfs + `amixer cset` from the shell |
| 2 | Remove EU warnings: override `config.json` with `vol_warn_enable:0` (and `warn_vol` 102 / `lock_vol` 100 so that "Volume lock" does nothing) | Convenience: no `vol_ask` dialog or repeated warnings | **A note must be left for the user**: it removes a hearing protection, it does not add power | None (JSON; if parsing fails, the original is restored) | Bind-mount from `/usr/data/hibymod/override/usr/resource/config.json` |
| 3 | Verify the output bit depth (LD_PRELOAD shim that logs `HW_PARAMS`); if it is 16 bits with software volume, force S32 | Better resolution at low volume | Low (test without headphones) | None | Existing shim (`hibymod_disp`) + `ioctl` hook |
| 4 | `tf_music_db_enable:1` (and maybe `tf_image_cache_enable:1`) | Fewer writes and less NAND use with large libraries | None | Low (requires a rescan) | Override `config.json` or enable the setting in the UI if it appears |
| 5 | `car_mode`, `dac_charge_disable`, `standby` in `set_functions.json`; `stream_media:0` to hide streaming | Hidden stock features | None | None | Override `set_functions.json` |
| 6 | THD C2/C3 as experimental "coloring" with reset | Sound customization | Low (audible distortion) | None | `amixer cset` from `hibyshell` |
| 7 | Adjustable `LO_VOLUME_INDEX` (attenuated line out) | Better matching with external amplifiers | Lowers the level: safe | None | Override `ot_devices_2025.json` |
| 8 | `AnalogDsdD2p:"d2p"` | PEQ and MSEB with DSD | Low | None | JSON override; measure CPU with DSD128/256 |
| 9 | Charge limit at 4.35 V | Battery life | None | Low | Change `axp2101.sh` in the rootfs |
| ✗ | "More power" or extra gain via software | **Does not exist**: High = 0 dB, full scale | — | — | Use the 4.4 mm output or an external amplifier |
| ✗ | Unknown ES9219 registers (0x1A, 0x1B, 0x86…) | Unknown | **High** (possible full-scale noise) | Low | No, not without a datasheet |
| ✗ | `hiby_linux_auto_test` | — | — | Data damage | Do not use |

---

## 5. How to enable it: the most important items

### 5.1 Filters (kernel patch)

When building the rootfs (the mod's `tools/`, before `mksquashfs`):

```python
p='rootfs/module_driver/codec_es9219.ko'; b=bytearray(open(p,'rb').read())
import hashlib; assert hashlib.md5(b).hexdigest()=='0d698e6f04fe301a063aec53b726f7cd'
assert b[0x16e4:0x16e8]==bytes.fromhex('0b200200'); b[0x16e4:0x16e8]=b'\0\0\0\0'
for o in (0x260c,0x2610):
    assert b[o:o+4]==b'\x06\0\0\0'; b[o:o+4]=b'\x07\0\0\0'
open(p,'wb').write(b)
```

At runtime, when the shell starts and when a filter is chosen: `amixer -c 0 cset name='Digital Filter' <0..7>`. Store the choice in `/usr/data/hibymod/audio.conf`.

It must be reapplied after the player starts or when the user touches the original setting. One approach: watch `/usr/data/user.ini` with inotify, or do cheap polling of `amixer cget`.

To check that it works, read register 7 of chip 0x48 through `/dev/i2c-3` with a custom read-only tool, or do a listening test with a sweep.

### 5.2 Removing the EU volume warnings

1. Copy `usr/resource/config.json` to `/usr/data/hibymod/override/usr/resource/config.json`.
2. Change `"vol_warn_enable":1` to `0`. Optionally, set `lock_vol` to 100 and `warn_vol` to 102 on both ports, as in the original R3 II.
3. Reboot: the configuration is only read at startup.
4. Check that `vol_ask` no longer appears when going above 34.

**Note for the user:** this does not increase power. It removes the hearing-safety warnings.

### 5.3 `set_functions.json` features

1. Override `usr/resource/set_functions.json` with, for example, `"car_mode":1`, `"car_mode_auto_play":1` and `"dac_charge_disable":1`.
2. To hide Tidal/Qobuz, add `{"stream_media":0}` [I: the key exists in the binary; its effect on the UI needs checking].

### 5.4 Pending tests on the device (with the user, without headphones on)

1. `amixer cget name='Output Port Switch'` while playing through 3.5 mm (should give 1), through 4.4 mm (3) and with Line Out (2).
2. Log `HW_PARAMS` with the shim to find out whether the output is S16 or S32 with 16-bit FLAC.
3. Read `/sys/module/axp2101/parameters/charge_voltage_limit`.
4. Read registers 0x00–0x40 and 0x80–0x95 of the ES9219s 0x48/0x49 through `/dev/i2c-3` (read-only) to document the actual state.
