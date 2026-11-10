# Clocks, overclocking, power saving and charge control (HiBy R3 II 2025)

Date: 2026-11-10.

Tags: **[V]** verified (read-only reads on the device, disassembly of the `.ko`/`hiby_player`, or the rootfs); **[I]** inferred, not tested on the hardware.

Only reads were performed on the device: `cat` of sysfs/proc, `devmem <addr> 32` **without a value** (read) of CPM registers and of the kernel's PLL table, `ps`/`top`/`wpa_cli status`/`rfkill list`. debugfs was not mounted and nothing was written.

---

## 0. Summary

1. **The CPU already runs at ~1104 MHz**, 10 % above the X1600 datasheet's 1.0 GHz [V]. There is no cpufreq, no governor, no cpuidle and no `jz_clk`; the CPU clock is registered in the kernel as **read-only** (`ingenic_cpccr_rate_ro_ops`) [V].
2. **Overclocking: not recommended.** It is only possible by writing the CPM registers through `/dev/mem` (it exists, without `STRICT_DEVMEM`, and the `devmem` applet is present). Zero gain for our UI, which is limited by the SLCD transfer at 44 fps and by memory (DDR/AHB hang off the MPLL, which cannot be touched without breaking SD, LCD and possibly audio). Risk: high instability, low chance of a brick.
3. **Core voltage:** `dcdc2` = fixed 1.10 V (min = max in the *constraints*); there is no user interface to change it. Doing it through raw I²C to the AXP2101 is the **only real soft-brick risk** (the PMIC keeps the value as long as the battery is connected). **Do not do it.**
4. **Real savings:** brightness, not redrawing when nothing changes (and never with the screen off), 30 fps in Saver mode, WiFi and BT off when not in use. *Underclocking* (dividing the CPU down to 552 MHz) is possible and relatively safe, but saves little because idle already uses `wait` + NO_HZ; it stays experimental.
5. **There is no current sensor.** `current_now` of `ac`, `usb` and `axp_battery` are configured or constant values, not measurements [V]. Consumption can only be measured from the slope of `battery/voltage_now` and `battery/capacity` (CW2015).
6. **"USB power only, do not charge the battery": `echo 0 > /sys/class/power_supply/axp_battery/charger`** (sets the AXP2101's ICC charge current to 0 mA). `echo 1` restores it [V by disassembly]. It must be **re-asserted after every plug-in**: the driver thread re-applies the charge current 5 s after every VBUS change [V].
7. HiBy's `dac_charge_disable` option ("USB current limited") **is not that**: in DAC mode it limits USB input to 100 mA and disables charging, so the device **runs off its own battery** to avoid draining the phone's [V].

---

## 1. Clock tree

### 1.1 What is there (and what is not) [V]

| Item | Status |
|---|---|
| `/sys/devices/system/cpu/cpu0/cpufreq`, `cpuidle` | Do not exist |
| `/proc/jz/` | Only `dmmu/` and `efuse/` (no clocks) |
| debugfs | Supported (`nodev debugfs`) and with `clk_summary` in the kernel (`clk_summary_show` in kallsyms), but **not mounted**. Mounting it (`mount -t debugfs none /sys/kernel/debug`) would give the exact clock tree; it was not done because of this task's rules |
| CPU clock in the clk framework | `ingenic_cpccr_rate_ro_ops`, `ingenic_cpccr_mux_ro_ops`: **read-only**, the kernel never changes the CPU frequency |
| Kernel PLL table (`x1600_pll_rate_table`, 0x80798dc4) | 1000, 900, 800, 700, 600, 500, 400 MHz. Formula `24 MHz × M / N / OD1 / OD0` (e.g. 1000 = 125/3/1/1; 900 = 75/1/2/1) |
| `/dev/mem` | `crw-r----- 1,1`; no `devmem_is_allowed` in kallsyms ⇒ **no STRICT_DEVMEM**: full access to physical memory |
| `devmem` | busybox applet at `/sbin/devmem` |
| Idle | `wait instruction: yes`, `nohz_mode: 2` (tickless), `CPM.LCR = 0x1F00` (low-power mode = IDLE) |
| `/sys/power/state` | `freeze standby mem` (the player uses `echo mem` for its standby) |

### 1.2 CPM registers read (base 0x10000000) [V read; decoding I unless noted]

| Reg. | Value | Decoding |
|---|---|---|
| CPAPCR (0x10) | `0x02E049CD` | M=46, N=1, OD1=1, OD0=1 ⇒ **APLL = 24×46 = 1104 MHz** |
| CPMPCR (0x14) | `0x0AF0C9CD` | M=175, N=3, OD=1/1 ⇒ **MPLL = 1400 MHz** |
| CPCCR (0x00) | `0x9A094410` | CPU ← APLL, AHB0/AHB2 ← MPLL; CDIV=0 (/1), L2DIV=1 (/2), H0DIV=4 (/5), H2DIV=4 (/5), PDIV=9 (/10) |
| DDRCDR (0x2C) | `0xA0000003` | DDR ← MPLL /4 |
| MSC0CDR (0x68) | `0x4000000D` | MPLL/(2×14) = **exactly 50.0 MHz** (SD "high speed", which confirms MPLL = 1400 and the formula) |
| LPCDR (0x64) | `0x44000020` | LCD pixel ← MPLL/33 ≈ 42 MHz |
| I2S CDR (0x60, 0x70, 0x7C, 0x80) | `0x68000C35`, `0x61A`… | Fractional M/N divider; **source not confirmed** (APLL, MPLL or EXCLK) |
| CPCSR (0xD4) | `0xF8000000` | PLLs stable, no divider changes in progress |

**Current frequencies:**

| Clock | Frequency | Note |
|---|---|---|
| CPU | **1104 MHz** | BogoMIPS 1099.36 corroborates it [V] |
| L2 | 552 MHz | |
| AHB0 / AHB2 | 280 MHz | |
| APB | 140 MHz | |
| DDR | 350 MHz | DDR2-700, 64 MB (`mem=64M`) |

**Nominal frequency.** The X1600/X1600E datasheet (Lumissil/Ingenic) gives **1.0 GHz** for the XBurst1 core ([datasheet at Mouser](https://www.mouser.lt/datasheet/2/198/X1600_X1600E_DS-3196268.pdf); DigiKey says the same, "1 Core 1GHz", [product page](https://www.digikey.com/en/products/detail/lumissil-microsystems/X1600/17753858)). HiBy's bootloader [I] already sets the APLL to 1104 MHz, above the kernel's own table (max. 1000).

### 1.3 AXP2101 regulators (`/sys/module/axp2101/parameters/*`, `/sys/class/regulator/*`) [V]

| Rail | Voltage | Probable use [I] |
|---|---|---|
| dcdc1 | 3.3 V | I/O |
| **dcdc2** | **1.10 V** (`min_microvolts = max_microvolts = 1100000`) | **Core VDD** |
| dcdc3 | 1.8 V | DDR2 / I/O |
| dcdc4 | 1.2 V | |
| aldo2, aldo4, bldo1, bldo2 | 3.3 V, on | |
| aldo3 | 3.3 V | Audio (the player switches it off and on) |

- `/sys/class/regulator/*` is read-only.
- The `reg-virt-consumer.*` devices have no driver bound: there is no writable `microvolts`.
- The only way to change dcdc2 would be raw I²C (bus 0, address 0x34, `I2C_SLAVE_FORCE`, AXP2101 DCDC2 register) underneath the driver and its regmap.

---

## 2. Overclock? Analysis and recommendation

### 2.1 What it would take [I]

All through `/dev/mem`, without the kernel noticing:

1. Move the CPU to another source or to a high divider (CPCCR with `CE_CPU`, bit 22, and wait for CPCSR.bit0 to clear).
2. Reprogram CPAPCR with a larger M (e.g. M=50 ⇒ 1200 MHz) and wait for the PLL-stable bit.
3. Select the APLL again.

**The MPLL (DDR, AHB, APB, SD, LCD) cannot be touched:** changing it throws off the SD clock (50 MHz), the LCD pixel clock, the UART and probably I²S. Therefore an overclock only affects the CPU and the L2, not memory.

### 2.2 Risks

| Risk | Level | Reason |
|---|---|---|
| Brick | Low | The APLL is reprogrammed on every boot, so a reboot restores it. But if the system hangs *during* a NAND write (UBIFS on `/usr/data`), `user.ini` or the DB can get corrupted |
| Instability | **High** | It already runs +10 % over nominal at a fixed 1.10 V. There is no known headroom, and without raising the voltage random hangs are to be expected |
| Heat | Low-medium | 9×9 BGA package with no heatsink, inside a small enclosure |
| Audio dropouts | Medium | The I²S clock source is not confirmed. If it hangs off the APLL, the pitch changes or there are clicks when switching |
| Kernel delays | Medium | `loops_per_jiffy` is calibrated at 1104 MHz: at a higher frequency `udelay()` comes up short (GPIO I²C, SDIO, panel) |

### 2.3 Gain

- The SLCD panel tops out at 44 fps (22.7 ms per frame).
- The shell draws in 5–10 ms and the rest is dominated by memory copies and the `FBIOPAN_DISPLAY` transfer.
- All of that hangs off the MPLL, the DDR and the AHB, which do not change.
- A +9 % CPU bump (1200 MHz) would save less than 1 ms per frame and **does not raise the fps**.
- Audio decoding does not need it either, except perhaps DSD256 with DSP, which is still to be measured.

### 2.4 Recommendation

**Do not overclock.** **Medium** risk (mostly instability) for **zero** UI gain.

- **dcdc2 voltage: NEVER.** The AXP2101 keeps the register as long as the battery is connected, not just across a warm reboot. If the boot does not reprogram it, an unstable voltage would cause a crash loop until the battery runs flat (soft brick). **High** risk.

---

## 3. Power saving

### 3.1 Measuring consumption from user space [V]

| Source | What it really is |
|---|---|
| `/sys/class/power_supply/battery/` (CW2015, i2c-0 0x62) | **The real battery**: `voltage_now` in µV (4362000 when full), `capacity` in % (1 % resolution), `status`. `time_to_empty_now = 8191` while charging |
| `/sys/class/power_supply/axp_battery/` | AXP fuel gauge almost disabled: `voltage_now = 0`, `capacity = 0`, `current_now` = constant 200000 (in `axp210x_bat_get_property`). `status` does seem real ("Full") [I] |
| `/sys/class/power_supply/usb/current_now` | **Not a measurement**: returns the configured charge current (driver field +0x28; currently 500) |
| `usb/voltage_now` (4400) and `ac/*` | Configured values, not measured |
| `usb/online` | VBUS present (PMIC register) [V] |

**Method.** With the device unplugged:
1. Every 60 s, log `date`, `battery/capacity` and `battery/voltage_now` to `/usr/data/hibymod/bat.log`, never to `/tmp`. A `sleep 60` in a shell loop is enough, and cheap.
2. Compare modes with the same track, the same volume and the same output, for 30–60 min each.
3. Result: %/h and mV/h. From ~3.7 V upward the curve is almost linear.

The firmware also has its own log: create `/data/mnt/sd_0/battery_log.txt` (see `research_hidden_firmware.md` §3.4).

### 3.2 Levers, by estimated impact [I unless noted]

| Lever | Path and values | Saving | Risk |
|---|---|---|---|
| **Brightness** | `/sys/class/backlight/backlight_pwm0/brightness`: 0–101 (`max_brightness = 101`, currently 100) [V]. `bl_power`: 0 on, 4 off. The player also writes it (`backlight_set`), so it must be re-applied after the screen wakes up | **The largest** with the screen on | None |
| **Do not draw with the screen off** | If `bl_power != 0` or `brightness == 0`, the shell does no `FBIOPAN` and no compositing. Every `FBIOPAN` is a full SLCD transfer | Large | None |
| **Frames** | Draw only when something has changed (dirty → present). 44 fps only during animations; 30 fps in Saver; `present_rows` for the clock and volume; status bar every 60 s | Medium | None |
| **WiFi off** | HiBy's scripts: `/usr/bin/wifi_off.sh` (kills `wpa_supplicant`/`udhcpc`, `ifconfig wlan0 down`), `/usr/bin/wifi_down.sh` (also `rfkill block wifi`) and `/usr/bin/wifi_on.sh` / `wifi_up.sh` [V]. Better to use the player's setting so its UI does not get out of sync | Medium-high | None |
| **WiFi power save** | The `cywdhd` driver implements `wl_cfg80211_set_power_mgmt` [V], but **there is no `iw`**, only `wpa_cli`. It would need a static `iw` or a minimal nl80211 tool (`NL80211_CMD_SET_POWER_SAVE`). DHD usually enables PM2 on association [I], so it may already be active; it can be queried with `NL80211_CMD_GET_POWER_SAVE` | Low-medium | Low (streaming latency) |
| **Bluetooth off** | `/usr/bin/bt_disable` (`bt-adapter --set Powered Off`) [V], or `rfkill block bluetooth`. Currently `bluetoothd`, `bluealsa`, `bt-agent` and `brcm_patchram_plus` are running. Better to use the player's setting | Medium | None |
| **NAND write-back** | `/proc/sys/vm/dirty_expire_centisecs` = 3000 and `dirty_writeback_centisecs` = 300 [V]. The player adjusts `dirty_*` | Low | Low (more data lost on a hard power-off) |
| **Underclock** | §3.3 | Low | Medium (experimental) |
| **Services** | `adbd` (idle, uses almost nothing), `sys_server`. No clearly useless daemons | Very low | Medium (do not kill HiBy processes) |
| **Regulators** (`dcdc3_always_pwmmode = 0`) | Already in automatic PFM/PWM [V] | — | — |

### 3.3 Experimental underclock (Saver mode only, off by default) [I, untested]

- **What changes.** Only the CPU and L2 dividers in CPCCR; the PLL is not touched.
  - CPU = APLL/2 = 552 MHz.
  - L2 = APLL/4 = 276 MHz.
- **Why it is safer than overclocking.**
  - `udelay()` comes out *longer*, which is safe.
  - The MPLL (DDR, SD, LCD) does not change.
  - The timers (OST) run from the 24 MHz EXCLK.
- **Procedure.** CPCCR semantics as on the X1000/X1830; this needs to be verified:

  ```sh
  v=$(devmem 0x10000000 32)                    # 0x9A094410
  # lower: CDIV=1, L2DIV=3, CE_CPU (bit22)=1; all other bits untouched
  devmem 0x10000000 32 $(( (v & ~0xFF) | 0x31 | 0x400000 ))
  while [ $(( $(devmem 0x100000D4 32) & 1 )) -ne 0 ]; do :; done  # CDIV_BUSY
  devmem 0x10000000 32 $(( ($(devmem 0x10000000 32)) & ~0x400000 ))
  # restore: (v & ~0xFF) | 0x10, with the same CE/BUSY sequence
  ```

- **Risks.**
  - Race with the kernel: minimal, because the kernel never writes CPCCR at runtime.
  - If the X1600's CE/BUSY bits do not behave as on the X1000, the CPU may hang. A reboot fixes it.
  - DSD or the equalizer could run out of CPU and cause audio dropouts.
- **Expected saving.** Small: without lowering the voltage, dynamic power only drops during active time, and the CPU sits in `wait` more than 80 % of the time during playback.
- **Measure it first with §3.1.** If it does not show ≥3–5 % more battery life, drop it.

---

## 4. Charging: "use USB only, do not charge the battery"

### 4.1 `axp2101.ko` driver (disassembled with `kodis.py`) [V]

**`/sys/class/power_supply/axp_battery/charger` (rw):**
- `charger_store`:
  - Writing `0` ⇒ flag +0x280 = 1 and `axp2101_icchg_set(0)`, which sets **register 0x62 (ICC, charge current) to 0 mA**.
  - Writing anything other than 0 ⇒ flag = 0 and ICC = value stored at +0x28 (the last `constant_charge_current_max`).
- `charger_show`: prints `!flag`, i.e. **1 = charging allowed, 0 = disabled**. Currently `1`.

**`/sys/class/power_supply/usb/constant_charge_current_max` (rw), prop 0x1e:**
- Stores the value at +0x28.
- If ≤ 500: applies ICC immediately and sets counter +0x289 = 5.
- If > 500: applies ICC with a delay, from the driver thread.

**`/sys/class/power_supply/usb/charge_control_limit_max` (rw), prop 0x22:**
- Sets the VBUS input current limit (0 ⇒ 2000 mA).
- Sets register 0x18 bit 1 (battery charge enable): **it is cleared only if the value is exactly 100**.

**`thread_dosomthing` thread:** runs every ~1 s.
- Every VBUS change resets the counter to 0.
- With VBUS present, when the counter reaches 5 (about 5 s), it **re-applies `axp210x_set_charge_current(+0x28)` ignoring the `charger` flag**.
- Consequence: `charger = 0` **is lost on every re-plug**, and `cat charger` will still say 0 even though it is charging again.

### 4.2 What `hiby_player` does [V]

**Function `0x481de0(MaxPower, limit_max, current_max)`.** Writes, only if the value changes:
- `/sys/kernel/config/usb_gadget/android0/configs/c.1/MaxPower`;
- `usb/charge_control_limit_max`;
- `usb/constant_charge_current_max`.

It is called on USB events:

| Situation | Values (MaxPower, limit, ICC) |
|---|---|
| Enumerated by a host (`/sys/class/udc/13500000.otg_new/state` = `configured`) and USB mode ≠ DAC | (500, 500, 500) |
| Host, **DAC** mode (param 8 = 0) **and** `dac_charge_disable` on (param 0x5f = 1) | **(10, 100, 0)**: input ≤ 100 mA, charging disabled, ICC 0. The R3 lives off its battery |
| Host, DAC mode and `dac_charge_disable` = 0 | (500, 500, 500) |
| Wall charger (VBUS without a host) | (500, 0, 1000) or (1500, 0, 1500): input 2000 mA, charging 1–1.5 A |
| Unplugged (`CHARGE_REMOVE_MSG`) | (10, 100, 0 if param 0x5f = 1, otherwise 100) |

**Where the parameters are stored.** `get_param(n)` (`0x42a500`) reads from the `user.ini` image:

| Parameter | Offset in `user.ini` | Meaning | Current value |
|---|---|---|---|
| param 8 | **+0x274** | USB mode: 0 = DAC (UAC), 1 = mass storage (`system_if_usb_select_device`) | 1 |
| param 0x5f | **+0x6f8** | "USB current limited" (`dac_charge_disable`) [I the label; V the usage] | 0 |

**Conclusion.** HiBy's option is for not draining the phone, not for "power from USB without charging". For what is being asked, `axp_battery/charger` must be used.

### 4.3 Design for the shell

```sh
# Enable "USB only" (does not charge the battery; the system runs from VBUS through the AXP2101's power path)
echo 0 > /sys/class/power_supply/axp_battery/charger
# Disable (the ICC configured by the player comes back)
echo 1 > /sys/class/power_supply/axp_battery/charger
```

**Rules:**
1. **The shell stores the desired state** in `/usr/data/hibymod/power.conf` (`nocharge=1`). `cat charger` cannot be trusted, per §4.1.
2. **Watchdog** (in the shell loop, cheap):
   - read `usb/online` every 1–2 s;
   - on the 0→1 transition, wait **7 s** (longer than the thread's 5 s and the player's events) and write `0` again;
   - additionally, re-assert it every 30 s while `online = 1`;
   - nothing is needed when going to `online = 0`.
3. **Battery protection:** if `battery/capacity` < 20 %, re-enable charging automatically and notify.
   - "Battery care" variant: charge up to 80 % (`charger = 0`) and resume below 75 % (`charger = 1`).
4. **Reading the real state:**
   - `axp_battery/status` (Charging / Not charging / Full) [I: still to verify that it changes after `charger = 0`];
   - the trend of `battery/voltage_now`: with ICC 0, the resting voltage drops from ~4.36 V and stops rising.
5. **Pending test** (with the user's permission):
   - with the battery below 90 % and plugged into the PC, `echo 0 > …/charger`;
   - for 10 min, `capacity` and `voltage_now` must not rise and `status` should change;
   - then `echo 1`.
6. **Input limit.** On a PC the player leaves the input at 500 mA, which is enough for the system. If a peak exceeds it, the AXP makes up the difference from the battery; this is not harmful.

**Detecting the connection and "DAC mode":**

| What | How |
|---|---|
| VBUS | `/sys/class/power_supply/usb/online` (1/0) [V] |
| Host or charger | `/sys/class/udc/13500000.otg_new/state`: `configured` with a PC or phone [V, today]; with a charger, some other state (e.g. `not attached`) [I] |
| DAC mode selected | `user.ini` +0x274 == 0 [V] |
| DAC mode active | UAC function linked in `/sys/kernel/config/usb_gadget/android0/configs/c.1/` (currently there are `ffs.adb` and `mass_storage.0`) and existence of `/dev/uac_sa` [I] |
| HiBy's "USB current limited" | `user.ini` +0x6f8 [V read] |

**Side note, completing §3.6 of the previous report** [V]:
- `charge_voltage_limit = 4400` and `charge_term_current = 100`. The battery's design voltage is 4350 mV and it reads 4.362 V when full.
- Lowering it to 4350 in `module_driver/axp2101.sh` is still the recommendation.
- The parameter is writable (`rw-r--r--`), but only applied when the module is loaded [I].

---

## 5. Design of the three modes

Implemented in `hibyshell` with a `perf_mode` setting in `/usr/data/hibymod/power.conf`. The levers are applied when the mode changes or at startup, and re-applied when the screen wakes up and on USB events.

| Lever | **Saver** | **Normal** (default) | **Performance** |
|---|---|---|---|
| Shell frames | Capped at 30 fps; only during animation | 44 fps during animation, 0 at rest | 44 fps (the panel cannot do more), 0 at rest |
| Animations | Shorter (≤200 ms), no recomputed blur or glass (pre-composited backgrounds) | Those from the guide (250–350 ms) | Those from the guide, with cover art or thumbnail preloading |
| Status bar / clock | `present_rows` every 60 s | Every 30 s | Every 30 s |
| Screen off (`bl_power != 0`) | Shell fully paused (no `FBIOPAN`, polling ≥1 s) | Same | Same |
| Brightness (`/sys/class/backlight/backlight_pwm0/brightness`, 0–101) | `min(user, 40)` | User | User |
| Screen-off timeout | 15 s (via the player's setting, `bkl_time`) | User | User |
| WiFi | Off if there is no streaming or relay (`wifi_off.sh`, or better the player's setting); if in use, power save on (nl80211) | As the user leaves it; driver's default PS | On; **PS disabled** (nl80211 `set_power_save off`) for lower latency in the YouTube relay |
| Bluetooth | Off if no A2DP sink is connected (`bt_disable`, or the player's setting) | User | User |
| CPU | 1104 MHz; **optional, experimental**, CDIV/2 = 552 MHz (§3.3), off by default and never with DSD | 1104 MHz (stock) | 1104 MHz (**no overclock**) |
| Priority | Shell background tasks (thumbnails, scanning) at `nice 10` and in batches | `nice 5` | `nice 0`; **never above `hiby_player`** (audio dropouts) |
| NAND (`/proc/sys/vm/dirty_expire_centisecs`) | 6000 | 3000 (stock) | 3000 |
| Charging (`axp_battery/charger`) | Independent of the mode: its own "USB only (do not charge)" and "Battery care 80 %" switches (§4.3) | ← | ← |

**Implementation notes:**
- Save the original values (brightness, `dirty_*`, CPCCR) to restore them when leaving the mode or the shell.
- If the player rewrites the brightness on wake-up, re-apply the Saver cap after the screen-on event.
- Validate each mode with the §3.1 log (%/h) and with the *gauntlet* loop from `UI_GUIDE_FOR_AGENTS.md` (30 fps in Saver must not show half-drawn frames).

---

## 6. Main evidence (read-only commands)

```
/proc/cpuinfo: "ingenic,x1600_halley6_module_base", BogoMIPS 1099.36, wait instruction: yes
devmem 0x10000000 → 0x9A094410 (CPCCR); 0x10000010 → 0x02E049CD (APLL); 0x10000014 → 0x0AF0C9CD (MPLL)
devmem 0x1000002C → 0xA0000003 (DDR); 0x10000068 → 0x4000000D (MSC0); 0x10000004 → 0x00001F00 (LCR)
devmem 0x00798dc4.. → PLL table {1000,125,3,1,1},{900,75,1,2,1},{800,100,1,3,1},{700,175,2,3,1},{600,25,1,1,1},…
/proc/kallsyms: ingenic_cpccr_rate_ro_ops, clk_summary_show; no devmem_is_allowed and no cpufreq
/sys/class/regulator/regulator.2 (dcdc2): 1100000, min=max=1100000
/sys/class/power_supply/axp_battery/charger = 1 ; usb/constant_charge_current_max = 500 ; usb/charge_control_limit_max = 500
/sys/class/udc/13500000.otg_new/state = configured ; user.ini+0x274 = 1 ; user.ini+0x6f8 = 0
/sys/module/axp2101/parameters/charge_voltage_limit = 4400 ; battery/voltage_now = 4362000 (Full)
/proc/sys/vm: dirty_background_ratio 10, dirty_ratio 20, dirty_expire 3000, dirty_writeback 300 ; nohz_mode 2
```

Disassembly: `~/hiby/venv/bin/python ~/hiby/tools/kodis.py rootfs/module_driver/axp2101.ko charger_store charger_show axp210x_usb_set_property axp210x_usb_get_property thread_dosomthing axp2101_icchg_set.isra.1`. In `hiby_player`: `hpdis.py 0x481de0 0x481f10`, `0x482040 0x4820e4`, `0x42a500` (jump table 0x7f09c0) and `0x47dba0`.
