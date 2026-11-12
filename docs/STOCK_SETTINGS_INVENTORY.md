# Stock HiBy settings vs ByOS: full inventory

Date: 2026-11-12. Firmware: HiBy R3 II (2025), `hiby_player` md5 `d1dea413a91ca3c6759c9029744bc52c`.

**Goal.** List every option that HiBy's own interface offers, say where ByOS has it today, and say
how to add the missing ones to the ByOS interface. The user's complaint: Equalizer, PEQ, MSEB, music
update mode, the Play settings and per-app options are lost under ByOS.

**Tags.** **[V]** verified statically (rootfs, string tables, disassembly of the setting jump
tables); **[I]** inferred, not checked on the device. Nothing in this document was run on the device.

**Sources.**
- Stock rootfs `~/hiby/rootfs`: `usr/resource/set_functions.json`, `config.json`,
  `str/english/*.ini` (labels), `layout/theme3/*.view` and `dialog/*.dlg`.
- `hiby_player` disassembly (`~/hiby/hp_dis.pkl`, tools in `~/hiby/tools/`):
  - Play settings list builder `0x4f8360`: 25 label keys at `0x81d7a4`.
  - Its click handler `0x4f84c0` and its on/off helper `0x4f7680`.
  - System list: item-name table `0x939930` (`{int idx; char name[64]}`), click handler around
    `0x4e2020`, on/off helper `0x4e1820` with the table `{item, setting id}` at `0x81c020`.
  - HiBy Link remote-settings tables (`hl_*_d`, `0x920b00`..`0x921970`): they give the setting id
    and the value list of most Play items.
  - `setting_get`/`setting_set` jump tables `0x7f09c0`/`0x7f0bfc`, all 0x8f ids. The case
    addresses are listed below so they can go straight into `st_opt[]`.
- ByOS: `src/sysset.inc.c`, `src/settings_app.inc.c`, `src/wireless.inc.c`, `src/cc*.inc.c`,
  `src/music.inc.c`, `src/nowui.inc.c`, `src/hibymod_disp.c` (`st_opt[]`),
  `docs/research_settings_api.md`, `docs/research_hidden_firmware.md`.

**What the "proposed implementation" column means.**
- **(a)** Simple toggle or choice. The key already exists in the library's `'S'` API (`st_opt[]`):
  only UI work.
- **(b)** Needs a new `'S'` key. The table gives the setting id, the `user.ini` offset and the
  get/set case addresses, ready for `st_opt[]`. The "set" behaviour shown is what that case does.
- **(c)** Complex screen. Two options:
  - **(c1)** A native ByOS editor. The parameters to edit are described.
  - **(c2)** Open HiBy's own screen from ByOS by driving its interface. ByOS already does this for
    "HiBy Library" in Music (`mu_open_stock()`: `nav_start(NAV_APP)` + injected taps + `cmd('R')`).
- **(d)** Not applicable under ByOS: stock UI cosmetics, or features with no hardware on this model.

**Priorities.** **P1** audible or used often: users miss it. **P2** useful. **P3** rare or cosmetic.

---

## 0. The stock settings structure

```
Settings (stock app)                     settings.ini
 ├─ Wireless  ── Bluetooth › · Wi-Fi › · HiBy Link · Import Music via Wi-Fi › · DLNA · AirPlay
 ├─ Play      ── 25 rows (§1)            play_settings.ini   (also the ⚙ of the stock Music app)
 ├─ System    ── 29 rows (§2)            sys_set.ini + set_functions.json
 └─ (USB DAC app) ── its own list (§5)
Control Centre (pull-down menu)  ── tiles (§7)
Now Playing ── play mode, favourite, "more" menu, A-B repeat, speed (§8)
Music app   ── categories, lists, sort, item menu (§9)
```

---

## 1. Play settings (`play_settings.ini`, builder `0x4f8360`)

There are 25 rows, in this order [V]. The first five are confirmed on screen
(`docs/capturas/11_ajustes_musica.png`: MSEB, Equalizer, PEQ, Update database, Music update mode).

Four rows have no hardware behind them on the R3 II 2025: Soundfield, DSD gain compensation,
Max digital volume output and SPDIF. `research_hidden_firmware.md` §3.5 covers this. The list
builder has no per-model filter that I could find, so whether those rows are shown is **[I]**.

| # | Item (stock label) | Values / range | Setting id · `user.ini` off · get / set case | ByOS today | Proposed ByOS implementation | Prio |
|---|---|---|---|---|---|---|
| 0 | **MSEB** › | Off / on, plus the MSEB editor (§1.1) | on/off `0x3f` · `0x420` (key `mseb`) | Settings › Sound › MSEB (on/off only) | on/off **(a)** done; editor **(c)**, see §1.1 | **P1** |
| 1 | **Equalizer** › | Off / on, 10 presets, 10-band custom (§1.2) | on/off `0x39` · `0x3e4` · `0x42a824` / `0x42c474` (store + `0x503620(get(0x18))` applies the curve); preset `0x18` · `0x268` · `0x42a91c` / `0x42c034` (0..9); custom bands `0x3a` → 10 × int32 at `0x3e8` (**pointer argument**, `memcpy` 0x28) | **MISSING** | **(b)** keys `eq` (0x39) and `eqpreset` (0x18); bands **(c1)** need a new "array" variant of `'S'`, or **(c2)** | **P1** |
| 2 | **PEQ** › | Parametric EQ (§1.3) | PEQ state `0x8c` · `0x8c0` (0..2, applies), `0x8d` · `0x8c4`, preset name `0x8e` → string at `0x8c8` (**pointer**); files `/data/peq/<name>.peq` [V strings] | **MISSING** | **(c)** (§1.3); the on/off **(b)** once `0x8c`/`0x8d` are confirmed [I] | **P1** |
| 3 | **Update database** | action | UI message `0x125` (library `'U'`) | Settings › Music Library › *Update library now* | done | — |
| 4 | **Music update mode** › | Manual / Auto · MQA decoder detection · Distinguishing English from other languages · Song DB on the TF card (only if `tf_music_db_enable`) | Auto `0x07` · `0x20` (key `update`); MQA detection `0x6e` · `0x734` · `0x42a650` / `0x42bfd4`; English sort `0x76` · `0x754` · `0x42a610` / `0x42bfe4` (→ `0x493700`, asks for a rescan: `scan_music_gain`); TF DB `0x86` · `0x8a8` | Music Library › *Find new music automatically* (`update`), *Library on the SD card* (via `config.json`) | Auto: done **(a)**. MQA detection and English sort **(b)** (`mqadetect`, `sorteng`); after `sorteng` offer *Update library now* | P2 |
| 5 | **Play mode** | Play playlist (0) · Loop track (1) · Shuffle (2) · Loop list (3) | `0x1a` (key `playmode`) | Sound › Play Mode | done | — |
| 6 | **Output selection** | Headset / Line out (warning dialog `output_select`) | `0x2d` (key `lineout`) | Sound › Line Out (confirmation) | done | — |
| 7 | **DSD output mode** | PCM (`d2p`, 0) · DoP (1) · Native (2) | `0x2c` · `0x3ac` · `0x42a88c` / `0x42c3d8` (store + `0x480a00` reapplies the output) | **MISSING** | **(b)** key `dsdout` 0..2. Note: `ot_devices_2025.json` forces `AnalogDsdD2p:"dop"`, so check on the device what each value really does [I] | P2 |
| 8 | DSD gain compensation | slider | `0x4b` · `0x474` (→ `0x480a60`) | — | **(d)** probably not shown (no DSD gain path on ES9219) [I] | P3 |
| 9 | **Resume play from last** | (none) 0 · Track 1 · Position 2 | `0x1e` · `0x24` · `0x42a8fc` / `0x42c294` (store, `<3`) | **MISSING** | **(b)** key `resume` 0..2 → Sound › Playback | **P1** |
| 10 | **Gapless playback** | on/off | `0x1b` (key `gapless`) | Sound › Gapless | done | — |
| 11 | Soundfield | — | — | — | **(d)** no hardware | — |
| 12 | Maximum digital volume output | on/off | `0x5d` · `0x6f0` per the HiBy Link table; its set case calls `0x481dc0` (USB MaxPower / charge current), so the label is doubtful [I] | — | **(d)** probably hidden on this model [I] | P3 |
| 13 | **Max volume** | 1..100 (slider) [I range] | `0x10` · `0x238` · `0x42a95c` / `0x42c14c` (stores, clamps the current and the power-on volume) | read only (`volmax`, `SF_RO`) | **(b)** make `volmax` writable, `lo` 1 `hi` 100 | P2 |
| 14 | **Volume on power-on mode** › | Memory (0) / Fixed (1) + *Fixed power-on volume* 0..Max volume | mode `0x13` · `0x244` · `0x42a944` / `0x42c08c` (`<2`); fixed volume `0x12` · `0x240` · `0x42a94c` / `0x42c0a0` (rejects > `0x10`) | **MISSING** | **(b)** keys `bootvolmode`, `bootvol` (range checked against `volmax`) | P2 |
| 15 | **Crossfade** | on/off (excludes gapless) | `0x32` (key `fade`) | Sound › Crossfade | done | — |
| 16 | **Gain** | Low (MDB) / High (HDB) | `0x15` (key `gain`) | Sound › Gain | done | — |
| 17 | **ReplayGain** | (none) 0 · by Track 1 · by Album 2 | `0x30` · `0x3b8` · `0x42a86c` / `0x42c400` (stores + player message 7; **no range check**) | **MISSING** | **(b)** key `replaygain` 0..2 (the library must check the range) | **P1** |
| 18 | **Balance** | L10..0..R10 | `0x14` (key `balance`) | Sound › Balance | done | — |
| 19 | **Digital filter** | Apodizing fast / Linear phase fast / Minimum phase (only 0 and 6 reach the DAC) | `0x21` (key `filter`) | Sound › Digital filter: 8 ES9219 filters via `amixer` | done (better than stock) | — |
| 20 | **Play through folders** | on/off | `0x22` · `0x390` · `0x42a8dc` / `0x42c280` | **MISSING** | **(b)** key `folderskip` | P2 |
| 21 | **Play through albums** | on/off | `0x3b` · `0x410` · `0x42a814` / `0x42be34` (plain store, **no range check**) | **MISSING** | **(b)** key `albumskip` 0..1 | P2 |
| 22 | **Repeat function** (A-B repeat) | on/off; the A/B points are set on Now Playing (`playing_plane_iv_repeater_*`, ± buttons) | `0x66` · `0x714` · `0x42a690` / `0x42bebc` | **MISSING** | **(b)** key `abrepeat` + Now Playing controls; setting A/B needs the player's position calls [I] | P3 |
| 23 | **Speed play** | on/off; speed 0.5×, 0.75×, 1×, 1.25×, 1.5×, 2× (`hiby_play_speed_menu.view`) or 50..200 % (`set_playspeed.dlg`) | on `0x64` · `0x70c` · `0x42a6b8` / `0x42bedc` (0 resets the speed to 100 through `setting_set(0x63,100)`); speed `0x63` · `0x708` · `0x42a6c0` / `0x42bf34` (50..200, otherwise 100); current speed `0x65` (key `speed`, read only) | **MISSING** (speed read only) | **(b)** keys `speedon`, `speedval`; a control on Now Playing. MSEB requires speed 100 (the CC rule already in `st_set`) | P2 |
| 24 | Automatically slide to the playback interface | on/off | not traced | — | **(d)** stock Now Playing behaviour; ByOS has its own Now Playing | — |

Labels in `play_settings.ini` that are **not** in this model's 25-row list (other models): NOS,
DRE, SPDIF output, Soundfield value, Lock volume by default, Volume indicator, Play key function,
Lyrics colour. Do not add them.

### 1.1 MSEB editor (`hiby_tune.view`, `mseb.ini`, opened by `0x550ba0`)

| Element | Values | Mechanism [V unless marked] |
|---|---|---|
| MSEB on/off | 0/1 | `0x3f` (key `mseb`) |
| 10 sliders: Overall temperature (Cool/Bright ↔ Warm/Dark), Bass extension (Light ↔ Deep), Bass texture (Fast ↔ Thumpy), Note thickness (Crisp ↔ Thick), Vocals (Recessed ↔ Forward), Female overtones (Detox ↔ Vivid), Sibilance LF (Soft ↔ Crisp), Sibilance HF (Soft ↔ Crisp), Impulse response (Slow ↔ Fast), Air (Soft ↔ Crisp) | ± the chosen range | audio-engine plugin params, written by `0x484f80` (`enable`, `width`, `max_samplerate`, `master_temp`, ...). The values sit in the settings struct around `0x47c`–`0x4bc` (`0x4d` int, `0x4e` float pointer, `0x50` array of 13 int32 copied to `0x488`) [I: exact slider ↔ index mapping] |
| Adjustment range | Fine-tuning ±20 · Middling ±40 · Excessive ±100 | `0x40` · `0x424`, accepts only 40 / 80 / 200 · `0x42a7ec` / `0x42bdd4` |
| Presets | Hiby seeds, Knight, OFF, custom presets (New, Save, Load, Delete, rename) | preset index `0x42` · `0x450` (`vg_listview_mseb_preset`); list file `/data/mseb_list` [V string] |
| Reset all changes | action | — |

- **(c1) native editor:** an iOS list with 10 sliders showing the min/max captions, a segmented
  range control and a presets list. The library needs an array or float variant of `'S'`,
  because ids `0x4e` and `0x50` take pointers. Each slider move rewrites `user.ini`: rate-limit it
  (≤4/s, like balance).
- **(c2) open HiBy's screen:** Settings › Play › MSEB.

### 1.2 Equalizer (`hiby_equalizer.view` + `hiby_eq_title.view`, `eq.ini`, opened by `0x505020`)

| Element | Values | Mechanism |
|---|---|---|
| On/off switch (`eq_title_iv_switch`) | 0/1 | `0x39` |
| Style | Custom, Off, Rock, Classic, Jazz, Pop, Dance, Voice, Blues, Metal (10) | `0x18` (0..9; order from the table at `0x81c8f0` [I]) |
| 10 band gains (graphic EQ, `geq_band_gain`) | dB range not traced; HiBy uses ±12 dB [I] | `0x3a`: 10 × int32 at `0x3e8`, pointer argument |
| Reset equalizer? | action | `lg_reset_equalizer_gain` |

- **(b)+(c1):** keys `eq` and `eqpreset` give on/off and presets at once (a choice list:
  quick win). The custom curve needs an array request (`HMSET1 ... key=eqbands val=v0,v1,...,v9`)
  handled by a new library path that fills a local `int[10]` and calls `setting_set(0x3a, ptr)`.
  - Native editor: 10 vertical sliders (31 Hz…16 kHz [I]) plus a preset picker.

### 1.3 PEQ (`peq/hiby_peq.view`, `dialog/peq_filter.dlg`, opened by `0x4fe7c0`)

| Element | Values [V from the embedded "PEQ Combined" plugin description] |
|---|---|
| PEQ enable (`iv_peq_enable`) | on/off |
| Preset name, presets (`iv_peq_set`), reset (`iv_peq_reset`) | files `/data/peq/<name>.peq` |
| Pre-amp | −9…+2 dB, step 0.1 |
| Bands | the engine accepts up to 32 (Band 0..31); how many the stock UI shows is not traced [I] |
| Per band: enable, filter type | Low shelf / Peaking / High shelf |
| Frequency | 30…20 000 Hz (log), step 0.1 |
| Q | 0.2…20 (log), step 0.01 |
| Gain | −9…+9 dB, step 0.1 |

- **(c1) native:**
  - A list of bands (type, Fc, Q, gain) with an editing sheet, plus a curve drawn from the
    biquads.
  - Presets are read and written straight as `/usr/data/peq/*.peq` files. The format is not
    reverse-engineered: dump one file saved by the stock UI first.
  - Activating a preset still needs the player: ids `0x8c`/`0x8d`/`0x8e` [I].
- **(c2)** is the cheap first step: Settings › Play › PEQ.

---

## 2. System settings (`sys_set.ini`, `set_functions.json`)

The stock list is the `set_functions.json` order (the keys set to 0 are hidden on the stock 2025
config). Ids come from the system on/off table `0x81c020` and the click handler [V].

| Item | Values / range | Setting id · off · get / set case | ByOS today | Proposed | Prio |
|---|---|---|---|---|---|
| Language | 13 languages | `0x05` · `0x08` (read as `lang` by `hibyset.inc.c`) | General › Language (ByOS language; does not change HiBy's) | Optional **(b)** `lang`, to keep HiBy's own screens (c2 targets) in the same language | P3 |
| Backlight settings › Brightness | 1..100 % | `0x0d` · `0x18` | Display & Brightness › Brightness (sysfs) | done | — |
| Backlight settings › Backlight time / Stay on | 10..120 s, Stay on | `0x0f`/`0x0e` (keys `bkltime`, `bklon`) | Display › Auto-Lock | done | — |
| Theme color (`settings_color.dlg`) | 5 colours + hue/brightness | `0x36`/`0x37`/`0x38` · `0x3d8`/`0x3dc`/`0x3e0` | — | **(d)** stock UI only | — |
| UI themes | Theme 1 / Theme 2, light/dark | `0x04` · `0x1c` (writes `theme:%d` to mtd5, reboot) | — | **(d)** (dangerous: writes flash) | — |
| Font size | Default / Middle / Big | not traced | — | **(d)** for stock; a ByOS text size option would be its own setting | P3 |
| USB working mode | Device / Host (OTG) | `0x71` · `0x740` · `0x42a638` / `0x42bf90` (→ `0x4825a0`: Type-C role Sink/Source/DRP, gadget re-bind) | **MISSING** | **(b)** key `usbrole` with confirmation (drops USB/adb). Host mode = external USB DAC/storage | P2 |
| USB device mode | Storage (1) / Audio (0) / Dock (2) | `0x08` (key `usbmode`) | Settings › USB (DAC/Storage); Dock = Developer › ADB | done | — |
| USB current limited (hidden on stock 2025) | on/off | on/off table: `0x5f` · `0x6f8` · `0x42a7ac` / `0x42bf18` (plain store; applied in DAC mode through `0x5d` → `0x481dc0`) [I] | USB › *USB only (don't charge)* (ByOS's own sysfs) | covered differently; optionally **(b)** `dacnocharge` | P3 |
| USB DAC feedback | on/off | `0x75` · `0x750` · `0x42a618` / `0x42bf64` (→ `0x482640`, sysfs on/off) | **MISSING** | **(b)** key `dacfeedback` under Settings › USB | P3 |
| Car mode (hidden) | on/off | `0x72` · `0x744` · `0x42a630` / `0x42bf88` | **MISSING** | **(b)** `carmode` (also needs `set_functions.json` `car_mode:1`?) [I] | P3 |
| Auto play in car mode (hidden) | on/off | `0x73` · `0x748` · `0x42a628` / `0x42bf80` | **MISSING** | **(b)** `carplay` | P3 |
| Time setting › | Adjust date, adjust clock time, Auto set time, Auto set time zone, Time zone, Region, Clock 12h/24h | date via `date -s` + `hwclock -w --utc`; clock format `0x7c` · `0x784` · `0x42a5d0` / `0x42bbfc`; region/zone `0x81`/`0x82` · `0x810`/`0x814` (strings) | General › Date & Time (date, time, zone shown; manual picker) | add a 12/24 h choice **(b)** `clock24`, or ByOS's own; NTP "auto set time" ByOS-side (needs Wi-Fi); time zone picker ByOS-side (`/usr/data/localtime`) | P2 |
| Idle shutdown | Off / 1..10 min | `0x0b`/`0x0c` (keys `idle`, `idlemin`) | General › Auto Power-Off › When Idle | done | — |
| Sleep shutdown | Off / 5..120 min (step 5) | `0x09`/`0x0a` (keys `sleep`, `sleepmin`) | General › Auto Power-Off › Sleep Timer (15..120) | done (finer steps optional) | — |
| Battery percentage display | on/off | `0x3d` · `0x418` · `0x42a804` / `0x42be24` | ByOS draws its own status bar | ByOS-side option in Display, no API needed | P3 |
| Standby (hidden) | on/off | `0x45` · `0x45c` · `0x42a750` / `0x42bd90` (→ `0x48ddc0(0x12)`, system_lister param under its mutex) | **MISSING** | **(b)** `standby` (same safe path as `sleep`) → Battery & Power | P2 |
| In-line remote | on/off | `0x27` · `0x388` · `0x42a8b4` / `0x42c1bc` (→ `0x4816c0` `system_if_set_line_control`, earpods ADC) | **MISSING** | **(b)** `inline` → Sound › Headphones | P2 |
| LED indicators | on/off | `0x44` · `0x458` · `0x42a758` / `0x42bda4` (store; the LED driver path is `0x47d1a0`, used by ids `0x5e`/`0x88`) [I how it is applied] | **MISSING** | **(b)** `led` → Display or Battery & Power | P2 |
| Double tap to wake up (hidden) | on/off | `0x6f` · `0x738` · `0x42a648` / `0x42bfc0` (→ `0x482660`, `echo on/off >` touch sysfs) | **MISSING** | **(b)** `dtwake` (battery cost; needs gt9xx support) [I] | P3 |
| Button operation when screen off | on/off (the case accepts 0..3) | `0x06` · `0x264` · `0x42a9ac` / `0x42c554` | **MISSING** | **(b)** `lockkeys` → Settings › Buttons | **P1** (pocket use) |
| Volume operation when screen off | on/off | `0x5b` · `0x6e8` · `0x42a768` / `0x42bc88` (**stores `!v`**: `sltiu s0,a1,1`) | **MISSING** | **(b)** `lockvol`; the UI must invert the value | **P1** |
| Shortcut menu (drop down / pull up) + Function selection | tiles | `0x74` (function list) and others | Settings › Control Center (ByOS CC) | **(d)** for stock; ByOS CC tiles: §7 | — |
| Screensaver setting | Album cover / Dynamic cover / clock | `0x4a` · `0x470` | — | **(d)** stock; ByOS lock screen is its own feature | P3 |
| Screen rotation | 0° / 180° | `0x56` · `0x6d4` (posts UI message `0x153`) | — | ByOS-side (it draws the framebuffer) + touch flip | P3 |
| Restore factory settings | confirm + "Also delete playlist data" | `restore.dlg`, handler `0x4eb320` (not traced) | General › Reset: sound + home only | **(c2)** open the stock dialog, or ByOS: delete `user.ini` with the player stopped [I] | P2 |
| Firmware update | Via SD-card / Via OTA | `vg_listview_upgrade` | General › Software Update (ByOS OTA) | done for ByOS; stock "via SD" not needed | — |
| Certification information | image page | `hiby_certificate` | — | **(d)** | — |
| About | Model, SD capacity, BT MAC, Wi-Fi MAC, Serial number, Machine code; 7 taps → Developer mode | `hiby_about_dev`, dev mode `0x7b` | General › Info (model, version, counts, storage, Wi-Fi MAC) | add BT MAC, serial number, HiBy firmware version (read-only, files/sysfs) | P3 |
| Developer options | Volume lock (EU 50/40 cap), Short-press Play = screenshot | `0x7a` · `0x77c`; `0x80` · `0x810` | Sound › *EU high-volume warning* (config.json) | Volume lock **(b)** `vollock` (P3); screenshot **(d)** | P3 |
| Stream media (hidden key) | Tidal / Qobuz / Sony | `0x62` | — | **(d)** | — |

---

## 3. Wireless app (stock `hiby_wireless.view`)

| Item | Values | Mechanism | ByOS today | Proposed | Prio |
|---|---|---|---|---|---|
| Bluetooth on/off | on/off | `0x23` (key `bt`) | Settings › Bluetooth | done | — |
| Search devices / Paired / Available / Unpair / Disconnect | lists | sys_server + bluez | Bluetooth page (bluez via dbus) | done | — |
| Device name (`bt_name`) | text | `/usr/resource/bt_name` [I] | **MISSING** | ByOS-side: `bluetoothctl system-alias` or the stock file [I] | P3 |
| **Codec** | SBC, AAC, aptX, LDAC; UAT 1.2M/900K/600K, UAT TWS (other models) | aptX on/off `0x2e` · `0x3b0` · `0x42a87c` / `0x42c384` (`bt_aptx on/off`) | **MISSING** | **(b)** `aptx` | P2 |
| **LDAC** | LDAC (audio quality) / LDAC Standard (connection) / Prefer LDAC connection (Auto) | `0x43` · `0x454` · `0x42a760` / `0x42bdac` (→ `0x47e8c0`, writes `codec LDAC…` to `/usr/data/alsa.conf`) | **MISSING** | **(b)** `ldac` 0..2 [I mapping] → Bluetooth › device page "Codec" | **P1** (LDAC users) |
| Bluetooth volume adjust | Increase / Decrease (`bt_vol_adjust.dlg`) | AVRCP via sys_server [I] | **MISSING** | **(c1)** two buttons; or rely on the volume keys | P3 |
| Bluetooth absolute volume | on/off | not traced (candidate `0x61`, set from `vg_listview_wireless`) [I] | **MISSING** | **(b)** once the id is confirmed | P2 |
| Bluetooth Volume Sync | on/off | not traced | **MISSING** | **(b)** after tracing | P3 |
| Bluetooth audio input (receiver/sink mode, `hiby_bt_input.view`) | on/off + "waiting for connection" page | `0x25` · `0x39c` (same busy guard as `bt`); `0x24` · `0x398` related | **MISSING** | **(b)** `btsink` + a simple "receiving" page | P3 |
| Wi-Fi on/off, networks, hidden SSID, forget, info | — | `0x33` + wpa_supplicant | Settings › Wi-Fi (complete) | done | — |
| DNS settings (`hiby_dns_set.view`) | manual DNS | sys_server [I] | read-only DNS shown | ByOS-side (`resolv.conf` / udhcpc hook) | P3 |
| Airkiss | — | — | — | **(d)** | — |
| HiBy Link | on/off | `0x2f` (key `hibylink`) | Streaming | done | — |
| Import Music via Wi-Fi (`hiby_wifi_song.view`) | built-in web upload server | stock web server [I] | not in ByOS (ByOS has the desktop sync app) | **(c2)** open the stock page, or a ByOS web uploader | P2 |
| DLNA / AirPlay | on/off | `0x35` / `0x34` | Streaming | done | — |

---

## 4. Music update mode page (sub-page of Play)

See row 4 of §1. Proposed ByOS page: Settings › Music Library, adding *MQA decoder detection*
(`0x6e`) and *Sort English separately from other languages* (`0x76`, then offer a rescan).

---

## 5. Stock USB DAC app (`hiby_dac.view`; item table at `0x81c914`)

The items, with the ids from the parallel table at `0x81c974` [V]: Output selection (`0x2d`),
Gain (`0x15`), MSEB, EQ, PEQ, Digital filter (`0x21`), DAC feedback (`0x75`), DSD output
(`0x2c`), DSD gain, Balance (`0x14`), SPDIF.

ByOS's DAC USB page (`usbdac_app`) shows the USB mode and charging. Proposal: add shortcuts to the
same Sound rows (gain, filter, balance, DSD output, DAC feedback) on that page. All are covered by
§1 and §2 keys.

---

## 6. Summary of new `'S'` keys (proposed `st_opt[]` rows)

Same format as `hibymod_disp.c` (key, id, lo, hi, flags, post_on, post_off, get case, set case).
Ranges marked * must be enforced by the library because `setting_set` does not check them.

```c
    /* Play */
    { "eq",          0x39, 0,   1, 0,       0, 0, 0x42a824, 0x42c474 },
    { "eqpreset",    0x18, 0,   9, 0,       0, 0, 0x42a91c, 0x42c034 },
    { "resume",      0x1e, 0,   2, 0,       0, 0, 0x42a8fc, 0x42c294 },
    { "replaygain",  0x30, 0,   2, 0,       0, 0, 0x42a86c, 0x42c400 },   /* * */
    { "dsdout",      0x2c, 0,   2, 0,       0, 0, 0x42a88c, 0x42c3d8 },
    { "folderskip",  0x22, 0,   1, 0,       0, 0, 0x42a8dc, 0x42c280 },
    { "albumskip",   0x3b, 0,   1, 0,       0, 0, 0x42a814, 0x42be34 },   /* * */
    { "abrepeat",    0x66, 0,   1, 0,       0, 0, 0x42a690, 0x42bebc },   /* * */
    { "speedon",     0x64, 0,   1, 0,       0, 0, 0x42a6b8, 0x42bedc },
    { "speedval",    0x63, 50, 200, 0,      0, 0, 0x42a6c0, 0x42bf34 },
    { "bootvolmode", 0x13, 0,   1, 0,       0, 0, 0x42a944, 0x42c08c },
    { "bootvol",     0x12, 0, 100, 0,       0, 0, 0x42a94c, 0x42c0a0 },   /* <= volmax */
    /* volmax: drop SF_RO, lo 1 hi 100, set case 0x42c14c */
    { "msebrange",   0x40, 40, 200, 0,      0, 0, 0x42a7ec, 0x42bdd4 },   /* only 40/80/200 */
    { "msebpreset",  0x42, 0,  63, 0,       0, 0, 0x42a7dc, 0x42bdcc },   /* * range [I] */
    { "mqadetect",   0x6e, 0,   1, 0,       0, 0, 0x42a650, 0x42bfd4 },
    { "sorteng",     0x76, 0,   1, 0,       0, 0, 0x42a610, 0x42bfe4 },
    /* System */
    { "lockkeys",    0x06, 0,   1, 0,       0, 0, 0x42a9ac, 0x42c554 },
    { "lockvol",     0x5b, 0,   1, 0,       0, 0, 0x42a768, 0x42bc88 },   /* stored inverted */
    { "inline",      0x27, 0,   1, 0,       0, 0, 0x42a8b4, 0x42c1bc },
    { "led",         0x44, 0,   1, 0,       0, 0, 0x42a758, 0x42bda4 },
    { "standby",     0x45, 0,   1, 0,       0, 0, 0x42a750, 0x42bd90 },
    { "usbrole",     0x71, 0,   1, SF_CONFX,0, 0, 0x42a638, 0x42bf90 },   /* drops USB */
    { "dacfeedback", 0x75, 0,   1, 0,       0, 0, 0x42a618, 0x42bf64 },
    { "clock24",     0x7c, 0,   1, 0,       0, 0, 0x42a5d0, 0x42bbfc },
    { "carmode",     0x72, 0,   1, 0,       0, 0, 0x42a630, 0x42bf88 },
    { "carplay",     0x73, 0,   1, 0,       0, 0, 0x42a628, 0x42bf80 },
    { "dtwake",      0x6f, 0,   1, 0,       0, 0, 0x42a648, 0x42bfc0 },
    { "vollock",     0x7a, 0,   1, 0,       0, 0, 0x42a5e0, 0x42bc0c },
    /* Bluetooth */
    { "aptx",        0x2e, 0,   1, 0,       0, 0, 0x42a87c, 0x42c384 },
    { "ldac",        0x43, 0,   2, 0,       0, 0, 0x42a760, 0x42bdac },   /* mapping [I] */
```

Before enabling each row:
- Verify the apply function only touches sysfs, ALSA, sockets or files, as was done for v7.
  Not yet reviewed: `0x480a00` (DSD), `0x4825a0` (USB role), `0x47e8c0` (LDAC), `0x503620` (EQ),
  `0x493700` (sort).
- Check that the value round-trips through `setting_get`.
- Keys whose set case takes a **pointer** (`0x3a` EQ bands, `0x4e`/`0x50` MSEB values, `0x85`,
  `0x8e` PEQ name) cannot go through the integer `'S'` request. They need a new request form
  (values in the request file, copied into a buffer owned by the library thread).

## 6.1 How to open HiBy's own screens from ByOS (c2)

- **Proven path in ByOS today:** `mu_open_stock()` in `music.inc.c`.
  1. Copy the frame.
  2. `nav_start(NAV_APP)` → `open_app_nav()` taps the stock launcher icon (`apps[].ax/ay`).
  3. Fade to HiBy's frame and `cmd('R')` (release the screen to the stock UI).
  4. The user comes back with the ByOS gestures.
- **For EQ / PEQ / MSEB:** open the stock Settings icon (stock coordinates `202, 425`, the `settings`
  entry), tap "Play", then tap the row.
  - From the screenshot, the cards on the Play page are ~80 px tall: MSEB ≈ y 110,
    Equalizer ≈ y 190, PEQ ≈ y 270.
  - Each step should wait with `settle()` and check a frame signature, as `is_home()` and
    `is_plane()` do. The y positions are [I] until checked on the device.
- **Shorter alternative [I]:** the library could post a UI-thread job that calls the stock
  view-open functions directly: `0x550ba0` (MSEB `hiby_tune`), `0x505020` (`hiby_equalizer`),
  `0x4fe7c0` (`hiby_peq`). Their argument is the current activity context from the Play list
  handler (`0x4f84c0`). This needs a new UI-queue message, which is not traced.

---

## 7. Control Centre tiles (stock `hiby_pull_down_menu.view` vs ByOS CC)

| Stock tile | Key | ByOS CC | Proposed |
|---|---|---|---|
| Bluetooth, Wi-Fi | `bt`, `wifi` | yes | — |
| DLNA, AirPlay, HiBy Link | `dlna`, `airplay`, `hibylink` | no (Settings › Streaming only) | optional CC buttons **(a)** — P3 |
| Gain | `gain` | no | CC button **(a)** — P2 |
| MSEB | `mseb` | no | CC button **(a)** — P2 |
| Line out | `lineout` | no | CC button with confirmation **(a)** — P3 |
| USB mode | `usbmode` | yes ("DAC USB") | — |
| Balance | `balance` | no | — (Settings is enough) |
| Gapless, Crossfade | `gapless`, `fade` | no | P3 |
| Digital filter | `filter` | yes | — |
| Sleep timer (+ remaining time) | `sleep`, `sleepmin` | no | CC button cycling Off/15/30/60/90 **(a)** — P2 |
| Play mode | `playmode` | no | CC / Now Playing **(a)** — P1 (§8) |
| Brightness slider | sysfs | yes | — |
| EQ (not a stock tile) | — | — | an EQ on/off CC button once `eq` exists **(b)** — P2 |

---

## 8. Now Playing (stock `hiby_playing_plane.view`, `hiby_play_menu.view`, `play_menu.dlg`)

| Stock control | Mechanism | ByOS Now Playing (`nowui.inc.c`) | Proposed | Prio |
|---|---|---|---|---|
| Play mode button (order / loop track / shuffle / loop list) | `playmode` | **MISSING** (only Settings › Sound) | **(a)** shuffle + repeat buttons as in iOS Music (map to the 4 modes) | **P1** |
| Favourite (heart, `playing_plane_iv_collect`) | stock favourites DB | **MISSING** | needs a "favourite" command in the library or a write to HiBy's DB [I] | P2 |
| "More" › List (current queue) | — | **MISSING** | ByOS queue sheet (`research_seek_queue_volume.md`) | P2 |
| "More" › Add to playlist | stock DB | **MISSING** (also in Music) | P2 |
| "More" › Equalizer | opens EQ | **MISSING** | link to Settings › Sound › Equalizer | P2 |
| "More" › Album | — | "Go to Album" | done | — |
| "More" › Properties (`playmenu_song_info.dlg`: title, artist, album, year, genre, size, duration, format, sample rate, bit depth, bitrate, channels, path) | file tags | partial (tag editor) | read-only "Song info" sheet **(c1)** | P2 |
| "More" › Remove / delete (`remove_song.dlg`) | — | **MISSING** | P3 |
| A-B repeat (`repeater_switch`, left/right markers, ±) | `0x66` + position | **MISSING** | after `abrepeat` **(b)** | P3 |
| Playback speed button (0.5×..2×) | `0x63`/`0x64` | **MISSING** | after `speedon`/`speedval` **(b)** | P2 |
| Audio quality text (`playing_plane_tv_bit_info`) | player info | check: format line [I] | show format/sample rate/bit depth under the title | P2 |
| Lyrics | — | yes (better) | — | — |

---

## 9. Stock Music app vs ByOS Music

| Stock | ByOS Music | Proposed | Prio |
|---|---|---|---|
| Categories: All, Files, Albums, Artists, Genres, Album Artist, Format | Songs, Albums, Artists, Genres, Mixes, Playlists, "HiBy Library" (stock fallback); Files is the separate Files app | Album Artist and Format views (ByOS DB) | P3 |
| Lists: Favorites, Recents, Playlists, Recently added, Most played | Playlists, Recently added (home), mixes ("Your favourites" is a mix) | real Favorites / Recents / Most played lists from HiBy's DB | P2 |
| Sort order: Alphabetical / Date added asc / desc | — | sort choice on Songs and Albums | P2 |
| Search + recent searches | search | recent searches | P3 |
| Item menu: Add to playlist, Add/Remove favourites, Track info, Delete file/folder, Remove from playlist, Select all | Play, Create radio, Go to album/artist, Edit song/album, Find lyrics, Find cover | add Add to playlist, Favourite, Song info, Delete | P2 |
| ⚙ → Play settings | — | ⚙ in Music → Settings › Sound › Playback | **P1** (that is how users look for EQ) |

---

## 10. Summary: missing items grouped into ByOS pages

**Settings › Sound** (existing page, regrouped)
- **Equalizer** › (new page). On/off and 10 presets: **(b)** `eq`, `eqpreset`, P1. 10-band custom
  curve: **(c)** array request or HiBy's screen.
- **PEQ** › (new page). Pre-amp, bands (type, Fc 30–20 kHz, Q 0.2–20, gain ±9 dB), presets in
  `/data/peq`: **(c)**, first step (c2) HiBy's screen. P1.
- **MSEB** › (turn the toggle into a page). On/off (done), range ±20/±40/±100 `msebrange`,
  presets `msebpreset`, 10 sliders **(c)**. P1.
- **Playback**:
  - ReplayGain Off/Track/Album `replaygain` P1.
  - Resume Off/Track/Position `resume` P1.
  - Play through folders `folderskip` P2.
  - Play through albums `albumskip` P2.
  - Speed `speedon`/`speedval` P2.
  - A-B repeat `abrepeat` P3.
  - (Play mode, Gapless and Crossfade are already there.)
- **Volume**:
  - Max volume (`volmax` writable) P2.
  - Power-on volume Memory/Fixed + level (`bootvolmode`, `bootvol`) P2.
  - Volume lock (EU cap) `vollock` P3.
- **Output**:
  - DSD output PCM/DoP/Native `dsdout` P2.
  - In-line remote `inline` P2.

**Settings › Buttons** (new small page, or inside General)
- Buttons work with the screen off `lockkeys` P1.
- Volume keys work with the screen off `lockvol` (inverted) P1.

**Settings › Bluetooth** (device page "Codec")
- LDAC quality/standard/auto `ldac` P1.
- aptX `aptx` P2.
- Absolute volume P2 (id to trace).
- Volume sync P3, device name P3, receiver mode `btsink` P3.

**Settings › USB**
- USB role Device/Host `usbrole` P2 (confirmation).
- DAC feedback `dacfeedback` P3.
- Sound shortcuts on the DAC USB page P3.

**Settings › Battery & Power**
- Standby `standby` P2.
- LED indicators `led` P2.
- Car mode `carmode` + Auto play `carplay` P3.
- Double tap to wake `dtwake` P3.

**Settings › General**
- Date & Time: 12/24 h `clock24`, automatic time (NTP) and time zone picker (ByOS-side) P2.
- Factory reset (stock dialog via c2, or ByOS) P2.
- Info: BT MAC, serial, HiBy firmware version P3.

**Settings › Music Library**
- MQA decoder detection `mqadetect` P2.
- Sort English separately `sorteng` (+ rescan) P2.

**Control Centre** (optional buttons)
- Gain, MSEB, EQ, Sleep timer P2.
- DLNA/AirPlay/HiBy Link, Line out, Gapless/Crossfade P3.

**Now Playing**
- Shuffle/Repeat (play mode) P1.
- Speed P2, Song info P2, Queue P2, Favourite P2, Add to playlist P2.
- A-B repeat P3.

**Music**
- ⚙ → Sound/Playback P1.
- Sort order P2, Favorites/Recents/Most played P2.
- Add to playlist/favourite, Delete, Song info P2.
- Album Artist/Format P3.

**Not to port (d):** Theme colour, UI themes, Font size (stock), Screensaver, Shortcut-menu type,
Automatic slide to Now Playing, Certification page, Airkiss, Soundfield, DSD gain, Max digital
volume output, SPDIF/NOS/DRE (other models). Screen rotation and battery-percent display only as
ByOS-side options (P3).

**Quick wins:** UI-only work over existing keys, or one `st_opt[]` row each with no pointer
arguments.
1. Play-mode buttons on Now Playing.
2. ⚙ in Music.
3. CC buttons for gain/MSEB/sleep.
4. `eq` + `eqpreset`, `replaygain`, `resume`, `lockkeys`/`lockvol`, `ldac`.
5. A (c2) "Open HiBy Equalizer / PEQ / MSEB editor" row in Sound, until the native editors exist.
