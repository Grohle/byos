# 60 fps in the HiBy R3 II UI? Panel, LCD controller and options

Date: 2026-11-10.

Tags: **[V]** verified (read-only access on the device, disassembly of the `.ko` files, or datasheet); **[I]** inferred, not tested on the hardware.

Only reads were performed on the device: `devmem <addr> 32` **with no value**, `cat` of sysfs/proc, `/proc/interrupts` and `hibyctl pantime 40` (40 presentations of the same buffer, no visible change). Nothing was written, nothing was rebooted, and no process was killed.

---

## 0. Summary

1. **The panel has no memory of its own.** The controller is a **Sitronix ST77903** (module `lcd_st77903_8bit.ko`). It is a driver **without RAM** ("without Display Ram") connected over **8-bit serial RGB**: each pixel takes 3 DOTCLK cycles. The X1600 DPU **refreshes it continuously** from DDR, using a descriptor chain that points to itself [V].
   - The previous hypothesis was: "SLCD/MCU-8080 with GRAM; each `FBIOPAN` transfers the image". **It is wrong.**
   - `FBIOPAN_DISPLAY` **transfers nothing**: it changes the buffer address and **waits for the end of the current frame**. `pantime` gives times spread between 0.5 and 22.1 ms, not constant ones [V].
2. **The 44 fps is the panel refresh rate.** Exact value: **44.30 Hz (22.57 ms)**. The panel is configured for "45 Hz" (`/sys/class/graphics/fb0/modes` = `U:320x480p-45`). The pixel clock ends up at MPLL/33 = 42.42 MHz and a frame takes 957,681 cycles. The `jzfb` IRQ counts 222 interrupts in 5.02 s, i.e. 44.2 Hz [V].
3. **60 fps is outside the panel specification.** The ST77903 datasheet requires a **line time > 40 µs**.
   - With 480 visible lines plus vertical blanking, that limits the panel to **~50 Hz maximum within spec**.
   - At 60 Hz a line lasts ~33–34 µs, **15–18 % below the minimum**.
   - It can be *tried*, but a correct image is not guaranteed: the risk is insufficient pixel charging (contrast, color, banding, flicker). Physical damage is unlikely [I].
4. **These do not help raise the fps:**
   - **Partial updates:** the panel has no GRAM and the DPU always scans the whole frame; there is no CASET/RASET window in RGB mode.
   - **16-bit bus:** the ST77903 in RGB mode only supports 8-bit serial.
   - **DMA tweaks:** the bandwidth is 13.5 MB/s, trivial.
   - Dirty rows only save CPU on the copy.
5. **Recommendation:**
   - **(a)** Make it perfect at 44.3 Hz: zero dropped frames and animations sampled at the presentation instant (§5).
   - **(b)** If more is wanted: patch `lcd_st77903_8bit.ko` in the firmware for **50 Hz within spec** (+13 %; exact values in §4.2).
   - **(c)** 60 Hz only as a reversible experiment (§4.3).

---

## 1. Display chain (evidence)

### 1.1 Modules and parameters [V]

| Item | Value |
|---|---|
| Panel | `module_driver/lcd_st77903_8bit.ko` ("st77903_8bit lcd panel driver"). Init over 3-wire bit-banged SPI on GPIO (CS=PB04, SCL=PA00, SDA=PA01, RST=PB05) |
| Controller | `module_driver/soc_fb.ko` (Ingenic, `x1600_clean/ingenic.linux/module_driver/soc/x1600/fb/jzfb.c` + `lcdc.c`), `dpu@0x13050000` |
| Parameters | `soc_fb.ko lcd_is_inited=0 frame_num=2 pan_display_sync=1` (`/sys/module/soc_fb/parameters`: 2 / 0 / 1) |
| fb0 | `modes = U:320x480p-45`, `virtual_size = 320,960`, 16 bpp, `stride = 640` |
| Useful sysfs | Only `blank`, `pan`, `rotate`, `mode(s)`, `state`. **There is no clock or timing setting at all.** `/sys/devices/platform/*lcd*` does not exist |

### 1.2 Panel `lcdc_data` (`.data` of the `.ko`, file offset 0x14a0) [V]

The layout is deduced from `auto_calculate_pixel_clock` and `jzfb_blank` in `soc_fb.ko`:

| Off. | Field | Value |
|---|---|---|
| +0x04 | refresh | **45** |
| +0x08/+0x0c | xres/yres | 320 / 480 |
| +0x10 | pixclock | 0 (computed from `refresh`) |
| +0x14 | left_margin (HBP) | 393 |
| +0x18 | right_margin (HFP) | 498 |
| +0x1c | upper_margin (VBP) | 7 |
| +0x20 | lower_margin (VFP) | 19 |
| +0x24 | hsync_len | 72 |
| +0x28 | vsync_len | 1 |
| +0x30 | lcd_mode | **4** (serial TFT RGB: 3 cycles per visible pixel) |

**Driver formula for `lcd_mode == 4`** (disassembly of `auto_calculate_pixel_clock`):

```
pixclock = ((vtotal − yres) × htotal + (htotal + 2·xres) × yres) × refresh
```

- htotal = 72 + 393 + 320 + 498 = **1283**; vtotal = 1 + 7 + 480 + 19 = **507**.
- Cycles per frame = 27×1283 + 1923×480 = **957,681**.
- With refresh 45, 43.10 MHz is requested. `ingenic_cgu_set_rate` uses `div = ceil(parent/rate)`, so the result is **MPLL 1400 / 33 = 42.424 MHz**, which gives **44.30 Hz** and a visible line of 1923 cycles = **45.3 µs**.

### 1.3 Registers read (read-only) [V]

| Address | Value | Meaning |
|---|---|---|
| CPM LPCDR `0x10000064` | `0x44000020` | MPLL source (bits 31:30 = 01), bit 26 (set by the driver), CDR = 0x20 ⇒ /33 |
| CPM MPLL `0x10000014` | `0x0AF0C9CD` | 1400 MHz |
| `0x13059000` (HSYNC) | `0x00480503` | hsync 72, htotal 1283 |
| `0x13059004` (VSYNC) | `0x000101FB` | vsync 1, vtotal 507 |
| `0x13059008` (HDE) | `0x01D10311` | visible pixels 465..785 |
| `0x1305900c` (VDE) | `0x000801E8` | visible lines 8..488 |
| `0x13059010` | `0x00000004` | mode 4 (serial RGB) |
| `0x13051000` (descriptor address) | `0x02525000` | Descriptor in DDR: `next = 0x02525000` (**itself ⇒ continuous scan**), `buf = 0x02600000`, stride 0x140 |
| `0x13052004` (status) | `0x0000000F` | |

### 1.4 What `FBIOPAN_DISPLAY` does (`jzfb_pan_display` → `lcdc_pan_display`) [V]

For `lcd_mode < 6` (TFT):
1. It writes the address of the new descriptor to `0x13051000` and `1` to `0x13051004`.
2. It sets the status to 1 and **sleeps until the end-of-frame interrupt**: `DC_INT_FLAG` `0x13052010` bit 17 ⇒ status = 2. The maximum wait time is 30 jiffies.

With `pan_display_sync = 1` it also waits for that same condition. The SLCD branch (`lcd_mode ≥ 6`) would indeed be "one trigger per frame", but **it is not the one in use**.

**Measurements:**
- `hibyctl pantime 40` (random waits of 5–27 ms between presentations): `5.6 18.1 10.5 15.5 2.5 3.5 17.5 0.5 … 22.1 … 21.0`, all between 0.5 and 22.1 ms. This is a vsync wait with a period ≈ 22.6 ms, not a transfer [V].
- `/proc/interrupts`, IRQ 39 `jzfb`: 32060 → 32282 in 5.02 s, i.e. **44.2 Hz**, with an IRQ on every frame [V].

**Why writing after presenting causes flicker:** when the pan returns, the DPU is already scanning *that* buffer, so writing to it causes tearing. The current rule ("write, then present") is correct.

### 1.5 ST77903 init (decoded from `st77903_8bit_power_on`) [V]

```
F0 C3 / F0 96 / F0 A5            (command unlock)
C1 66 07 4B 0B · C2 66 07 4B 0B · C3 44 04 44 04 · C4 44 04 44 04 · C5 45 00 · D6 00 · D7 00
E0 D2 09 0C 07 06 23 2E 43 46 17 13 13 2D 33   (gamma +)
E1 D2 09 0C 07 05 23 2E 33 46 17 13 13 2D 33   (gamma −)
E5 5A F5 64 33 22 25 10 77 77 77 77 77 77 77 · E6 (same)
EC 00 55 00 00 00 88 · 36 (MADCTL, parameter depends on rotation) · 3A 06 (18 bits)
B2 09 · B3 01 · B4 01 · B5 00 08 00 08 (idle-mode porches) · B6 EF 27 (480 gates, 320 columns)
A4 C0 63 · A5/A6 00 00 00 00 00 0B 2A BA 02 · BA 5A 09 43 00 23 01 00
BB/BC 00 21 00 25 83 87 18 00 · BD 33 44 FF FF 98 A7 7A 89 52 FF 06
ED C3 · E4 40 0F 2F · A0 00 06 06 (RGBIF: RCM=0 DE+SYNC, VBP 6, HBP 6) · 35 00 (TE) · 21 · 38 · 11 · 29
```

The panel runs in DE mode (`A0` RCM = 0). That is why the host's huge porches (HBP 393) work even though the IC says HBP 6: they are delimited by the DE signal.

### 1.6 ST77903 datasheet (Sitronix, P0.5) [V]

Source: [ST77903_SPEC_P0.5.pdf](https://dl.espressif.com/AE/esp-iot-solution/ST77903_SPEC_P0.5.pdf).

- *"400RGB x 480 … without Display Ram"*. Interfaces: QSPI (50 MHz max.) and *"Serial RGB (888, 666, 565)"*; *"only 6bit data format"*, D[7:0]; *"one pixel data needs to take three DOTCLKs"*.
- **p. 30: *"Minimum line time must be > 40us; Vsync width and Vsync porch > 40us"*.** Example: at 60 Hz with 400 lines the line lasts 40.36 µs. **With 480 lines, 60 Hz does not fit.**
- p. 37: *"In without ram mode, hpw+hbp ≥ 60"*. Porch and DOTCLK minimums and maximums are "TBD" (preliminary datasheet).
- Espressif, for this same chip at 320×480 in 8 bits, uses the preset `ST77903_RGB_320_480_PANEL_48HZ_RGB_TIMING` (24 MHz, H porches 3/3/6, V 1/6/6): about **48.8 Hz** with a 41.5 µs line. This is consistent with the ~50 Hz limit ([esp_lcd_st77903_rgb header](https://github.com/espressif/esp-iot-solution/blob/master/components/display/lcd/esp_lcd_st77903_rgb/include/esp_lcd_st77903_rgb.h)).

---

## 2. Theoretical maximum

```
fps = f_DOTCLK / cycles_per_frame
cycles_per_frame = Vblank × htotal + 480 × (hblank + 960)

Panel limit: t_line = (hblank + 960) / f_DOTCLK ≥ 40 µs
  ⇒ fps_max ≈ 1 / (40 µs × (480 + effective_Vblank))
```

- With a realistic minimum vertical blanking (~14 short lines): **≈ 50–51 Hz**.
- Today half of each line is spent in porches: 963 of 1923 cycles. But that is not the limit, because the cap is set by the panel's line time, not by the clock.

---

## 3. Options evaluated

| Option | Achievable fps | Risk | Verdict |
|---|---|---|---|
| Partial updates (CASET/RASET, dirty rows only) | 44.3 (no change) | — | **Not applicable**: the panel has no RAM and the DPU always scans the full frame. Dirty rows only save memcpy |
| 16-bit bus | — | — | **Impossible**: the ST77903 in RGB mode only has 8-bit serial (3 cycles per pixel). The hardware has no more lines wired for that mode |
| RGB565 instead of 666 | 44.3 | — | Changes nothing: still 3 DOTCLKs per pixel |
| DMA, bandwidth | 44.3 | — | The DPU reads 13.5 MB/s from DDR, which is trivial; it is not the bottleneck |
| Suppress the player's presentations during animations | 44.3 **with no drops** | None | **Yes**. Each pan blocks until vsync: if the player and the shell present within the same period, the shell drops a frame |
| Raise only LPCDR (stock porches) to /30 | **48.7** (line 41.2 µs) | Low-medium | Within spec. Reverts by itself when the screen is turned off and on [I] |
| Trimmed porches + refresh 50 in the `.ko` | **50.0** (line 40.9 µs) | Low-medium | **Best option within spec** (§4.2) |
| LPCDR /24 (stock porches) | **60.9** (line 33.0 µs, DOTCLK 58.3 MHz) | **High** (image) | Out of spec by −18 % |
| Trimmed porches + refresh 62 in the `.ko` | **61.1** (line 33.5 µs, DOTCLK 51.85 MHz) | **High** (image) | Out of spec by −16 %, with a lower DOTCLK than the previous one |
| CPU or DDR overclock | 44.3 | High | Does not affect the refresh rate (see `research_overclock_power.md`) |

**Divider table with stock porches** (957,681 cycles per frame, MPLL = 1400 MHz):

| Divider (CDR) | DOTCLK | Refresh | Line | Within spec? |
|---|---|---|---|---|
| /33 (0x20), today | 42.42 MHz | 44.30 Hz | 45.3 µs | Yes |
| /30 (0x1D) | 46.67 MHz | 48.73 Hz | 41.2 µs | Yes |
| /29 (0x1C) | 48.28 MHz | 50.41 Hz | 39.8 µs | Borderline (−0.5 %) |
| /28 (0x1B) | 50.00 MHz | 52.21 Hz | 38.5 µs | No |
| /25 (0x18) | 56.00 MHz | 58.47 Hz | 34.3 µs | No |
| /24 (0x17) | 58.33 MHz | 60.91 Hz | 33.0 µs | No |

The X1600's maximum DOTCLK in TFT mode is not verified: the Mouser datasheet could not be downloaded (blocked by the server). Above ~50 MHz, over an 8-bit bus and an FPC, there is also a signal-integrity risk [I].

---

## 4. How it would be done (NOT applied)

### 4.1 Careful: the clock is reprogrammed on wake [V]

`jzfb_blank` (when the screen is unlocked) calls `clk_set_rate(pclk, pixclock)` and rewrites all the timings. Consequences:
- A change made through `/dev/mem` **is lost every time the screen is turned off and on**, if the player uses `FBIOBLANK`, which is likely [I]. It is a **safety net for experimenting**, but it has to be reapplied.
- The clean, permanent route is to **patch the panel module in the firmware**, as was already done with `codec_es9219.ko` (`research_hidden_firmware.md`).

### 4.2 Recommended if more than 44 is wanted: 50 Hz within spec (patch of `lcd_st77903_8bit.ko`)

Changes in `.data`, 32-bit little-endian words, **file** offset = 0x14a0 + field:

| File | Field | Stock value | New |
|---|---|---|---|
| 0x14a4 | refresh | 45 (`2D 00 00 00`) | **50** (`32 00 00 00`) |
| 0x14b4 | left_margin (HBP) | 393 | **300** (`2C 01 00 00`) |
| 0x14b8 | right_margin (HFP) | 498 | **404** (`94 01 00 00`) |
| 0x14c0 | lower_margin (VFP) | 19 | **6** (`06 00 00 00`) |

Result [I, computed with the driver formula]:
- htotal = 1096, vtotal = 494; 848,624 cycles per frame.
- 42.43 MHz is requested ⇒ `ceil(1400/42.43)` = 33 ⇒ 42.424 MHz (**the same clock as today**) ⇒ **49.99 Hz**.
- Visible line: 1736 cycles = **40.9 µs**, which meets > 40 µs. hsync + HBP = 372, which meets ≥ 60. Vertical blanking: 14 lines ≈ 360 µs, which meets > 40 µs.
- What changes is the shape of the scan, not the bus frequency: the electrical risk is minimal.

Registers the driver should leave, to be checked with `devmem` after boot:

| Register | Expected value |
|---|---|
| `0x13059000` | `0x00480448` |
| `0x13059004` | `0x000101EE` |
| `0x13059008` | `0x017402B4` |
| `0x1305900c` | `0x000801E8` |
| LPCDR | `0x44000020` (unchanged) |

Acceptance test:
- `cat /sys/class/graphics/fb0/modes` must return `U:320x480p-50`.
- IRQ `jzfb` ≈ 250 per 5 s.
- `pantime` with a maximum of ~20 ms.
- Visual inspection: uniform gray, gradients, fine text, no flicker or banding.

**Risk.** If the panel does not accept it, the display comes out wrong and the stock `.ko` has to be reflashed from recovery (the official recovery is in mtd3/mtd4). Before flashing, it is advisable to validate it live with §4.3 at /30 (48.7 Hz).

### 4.3 Reversible live experiment (LPCDR only; requires the user's permission)

Standard Ingenic sequence followed by `ingenic_cgu_set_rate` [V for the logic]: write the divider with CE, wait for BUSY to clear and remove CE. **The bit positions** (CE = 29, BUSY = 28) follow the Ingenic convention and are not confirmed against the X1600 manual [I].

```sh
# 48.7 Hz (within spec): /30
devmem 0x10000064 32 0x6400001D
while [ $(( $(devmem 0x10000064 32) & 0x10000000 )) -ne 0 ]; do :; done
devmem 0x10000064 32 0x4400001D
# 60.9 Hz (OUT of spec): /24  → 0x64000017 … 0x44000017
# Restore: 0x64000020, wait for BUSY, 0x44000020  (or turn the screen off and on [I])
```

Measure with `jzfb` IRQs per second and `hibyctl fps`. Leave the panel for 1–2 minutes with a still image and a moving one, and check for a washed-out image, banding, flicker or missing lines.

**Risks:**
- Out of spec, the image may degrade or break; it recovers by restoring LPCDR or rebooting.
- Changing DOTCLK in the middle of a frame may produce a corrupt one: no consequences.
- Damage to the panel is unlikely: it is only scan frequency, not voltage [I].

**Do not touch MPLL:** the SD, DDR and AHB hang off it.

---

## 5. Concrete recommendation

1. **Now: make 44.3 Hz perfect.** That is what is most noticeable.
   - **A single owner of vsync during animations.** In 'P' or 'C' mode, hold back the player's presentations ('H') or merge them, so that every 22.57 ms period is used by the shell. Otherwise the two presentations within the same period get serialized and a frame is dropped.
   - **Pacing:** draw the next frame right when the pan returns (there is a ~22 ms budget) and sample the animation curve at the **instant it will be seen**: t_pan_return + 22.57 ms, not `now()`. That way there is no judder even with CPU jitter.
   - iOS durations at 44.3 Hz: 300 ms ≈ 13.3 frames, enough for a smooth cubic ease-out.
   - Fix the guide: `UI_GUIDE_FOR_AGENTS.md` says "each `FBIOPAN_DISPLAY` transfers the image". In reality it **waits for vsync** on a RAM-less panel; the practical rule (write, then present) still holds.
2. **Optional, with the user's permission:**
   - validate /30 (48.7 Hz) live, §4.3;
   - if it looks perfect, include the **50 Hz** patch (§4.2) in the next firmware;
   - adjust the shell's pacing to 20.0 ms.
   - Gain: +13 % smoothness, within the panel spec.
3. **60 Hz:**
   - **Not as the default.** It exceeds the ST77903's minimum line time by 16–18 %.
   - At most, a visual experiment with /24 or `refresh = 62` and trimmed porches.
   - It would only be adopted if, after minutes of testing, there are no artifacts at all. Even then it would be "works on this unit", with no guarantee on others or across temperature.

---

## 6. Reproduce

```sh
# WSL
cd ~/hiby/rootfs/module_driver
~/hiby/venv/bin/python ~/hiby/tools/kodis.py soc_fb.ko auto_calculate_pixel_clock jzfb_pan_display lcdc_pan_display jzfb_irq_handler jzfb_blank
~/hiby/venv/bin/python ~/hiby/tools/kodis.py lcd_st77903_8bit.ko st77903_8bit_power_on   # SPI init (bit 8 = D/C)
~/hiby/venv/bin/python ~/hiby/tools/korel.py lcd_st77903_8bit.ko .data                   # lcdc_data (file offset 0x14a0)

# Device (read-only)
cat /sys/class/graphics/fb0/modes /sys/module/soc_fb/parameters/*
devmem 0x10000064 32; for a in 0x13059000 0x13059004 0x13059008 0x1305900c 0x13059010 0x13051000; do devmem $a 32; done
grep jzfb /proc/interrupts; sleep 5; grep jzfb /proc/interrupts
/usr/data/hibymod/t/hibyctl pantime 40
```

`ingenic_cgu_set_rate` (0x803bf0b4) was read with `devmem` from physical memory 0x3bf0b4 and disassembled with capstone. It computes `div = ceil(parent/rate) − 1` and writes with CE, waiting on BUSY.

## 7. Tearing of `'P'`/`'R'`/`'O'` (fixed in library v6, not installed)

The "write, then present" rule applies to the buffer that is **not yet visible**. The shell commands write into the **visible** buffer, and for that one the correct rule is the reverse:

1. wait for the end of frame (pan of the same buffer);
2. copy immediately from top to bottom; the copy stays ahead of the scan.

Details and measurement plan: `research_play_list.md` §8 (`frame_edge()` and `copy_then_ov()` in `src/hibymod_disp.c`).
