# Controlling the stock player's settings without its UI (hiby_player)

Date: 2026-11-10. **[V]** = verified statically in the binary (disassembly, jump tables, strings); **[I]** = inferred, not yet observed on the device. **Nothing here has been run on the device**: §7 is the test plan.

Binary: `rootfs/usr/bin/hiby_player`, md5 `d1dea413a91ca3c6759c9029744bc52c` (MIPS32r2 `EXEC`, base `0x400000`, fixed addresses). Continues `research_play_list.md` (command `'L'`), whose guard design is reused here.

## 0. Summary

| Question | Answer |
|---|---|
| Is there one entry point that *applies* a setting (not just stores it)? | **Yes [V].** `setting_set(id, v)` at `0x42baa0` is a 0x8f-way jump table (`0x7f0bfc`). Each case stores the field in the settings struct, **calls the apply function** (sys_server message, ALSA mixer control, output-device driver, audio-engine parameter, timer...), then rewrites the whole settings file `/data/user.ini` (`0x42a2a0`, 0xad0 bytes). |
| What does the stock Control Centre add on top? | Almost nothing. Its tap handlers (`0x54cfe0`..`0x54dd40`) do `v = setting_get(id)`, toggle, `setting_set(id, v')`, then only redraw their own icons. Exceptions: **DLNA/AirPlay** also post a UI-queue message that starts/stops the service; **gain** first checks "greyed out"; **MSEB** checks playback speed and MQA; **line out = 1** asks for confirmation first. |
| How to read a value? | `setting_get(id)` at `0x42a500`: a pure load from the settings struct (jump table `0x7f09c0`), no lock, no side effect [V]. |
| Volume (absolute)? | **Yes [V]**, the exact HiBy Link `setVolume` handler (`0x4578a0`): `volume_apply(v)` (`0x47d0c0`) then `setting_set(0x11, v)`. Read: `setting_get(0x11)`, max `setting_get(0x10)`. |
| Thread | No path we use touches a widget. Volume and play mode are already called by the player **from HiBy Link / DLNA threads** [V]; the rest go to sockets, ALSA mixer, `ioctl`, `system()` or a mutex-protected timer [V], so calling them from our own thread is classified "safe [I]". UI refreshes are deliberately **not** called (they touch widgets). |
| Implemented | Library v7, FIFO command **`'S'`** (inert until used), `src/hibyset.inc.c` (shell API), `src/hibysetctl.c` (CLI). Built in `out/agent_set/`. Different firmware → `rc=-ENOEXEC`, nothing called. |

## 1. Core functions

| Address | Name (ours) | Role | Signature checked |
|---|---|---|---|
| `0x42a500` | `setting_get(id)` | `if (!*(void**)0x969590) return 0;` switch on id (<0x8f) → `lw v0, off(struct)` | yes |
| `0x42baa0` | `setting_set(id, v)` | switch → store + apply → `0x42a2a0` (save `/data/user.ini`) | yes |
| `0x47d0c0` | `volume_apply(v)` | BT abs-volume (`bt-control --op=notify_absvol`) if a BT sink is up; `0x735aa0(v)` output-device volume (`0x733640`) | yes |
| `0x48a900` | `ui_post(msg)` | if the UI queue `*(0x96e020)` exists, push `{0,msg,0,0,0}` with `0x462b20` (same queue primitive HiBy Link uses through `0x436d40`) | yes |
| `0x4f62e0` | `item_locked(id)` | "greyed out" test used by the UI for gain/balance/filter/line out, from the output type `0x47cf60()` and line-out state | yes |
| `0x439ea0` | `mqa_playing()` | takes the player lock (`P+0x48`) and reads `get_param(0x1c)` | yes |

- Settings struct: `*(void**)0x969590`, 0xad0 bytes, persisted verbatim in `/data/user.ini` (`/usr/data/user.ini` on the device). **The struct offset of a field = its offset in `user.ini`**, so a copy of that file gives every value for a later restore [V].
- `setting_set` is **not locked**. It already runs from non-UI threads for ids 0x1a (HiBy Link `setPlayMode`, `0x4578e0`) and 0x11 (HiBy Link `setVolume` `0x4578a0`, DLNA `set_volume@%d` `0x453c60`) [V].
- We do **not** take the player lock around it: `setting_set(0x1a)` itself calls `set_param(0xa)`, which takes that lock (deadlock risk if it is not recursive) [V].

## 2. Per option

`off` = offset in the settings struct / `user.ini`. Sequence = what the library does (identical to the stock handler unless noted). "Thread": **S** = safe from a non-UI thread (with precedent [V] or by inspection [I]); **U** = would need the UI thread; **X** = not feasible.

| Key | id | off | Values | Sequence (set) | Apply function (inside `setting_set`) | Evidence | Thread |
|---|---|---|---|---|---|---|---|
| `bt` | 0x23 | 0x394 | 0/1 | `setting_set(0x23,v)` (CC `0x54d0a0`) | on `0x47e120` → BT-on datagram to sys_server (`0x727660`, string not checked); off `0x47e180` → `BT:OFF` (`0x7276a0`). Re-entry guard `0x96958c`: a call while busy is ignored → we return `-EAGAIN` | [V] | S [I] |
| `wifi` | 0x33 | 0x3c8 | 0/1 | `setting_set(0x33,v)` (CC `0x54d1e0`) | on `0x47f1c0` → Wi-Fi-on datagram for `wlan0` (`0x727420`; skipped if `0x48e560()` says so); off `0x47f240` → `WIFI:OFF:wlan0` (`0x727460`) | [V] | S [I] |
| `dlna` | 0x35 | 0x3d0 | 0/1 | `setting_set(0x35,v)` + `ui_post(v ? 0x12e : 0x12f)` (CC `0x54d2e0`, net settings `0x5556e4` do the same) | the message handler on the UI loop runs `system_if_dlna_turn_on/off` (`0x480d40`/`0x480ea0`: `DLNA:TURN_ON:<bt_name>` / `killall dmrd`, `DLNA:TURN_OFF`). Off while a DLNA stream plays: `setting_set` itself stops it (`0x4392a0`, list path `dlna…`) | [V] sequence, [I] handler identity (`0x465508/0x465528`) | S (work done on UI thread via the queue) |
| `airplay` | 0x34 | 0x3cc | 0/1 | `setting_set(0x34,v)` + `ui_post(v ? 0x130 : 0x131)` (CC `0x54d3c0`, net settings `0x555608`) | `setting_set` only stores; the posted message starts/stops shairport | [V] sequence, [I] handler | S |
| `hibylink` | 0x2f | 0x3b4 | 0/1 | `setting_set(0x2f,v)` (CC `0x54dc20`) | `0x45aa20` (`/dev/sa-config` ioctl / HL server), on: `hcitool … 0x000A 01` BLE adverts + `0x48e380`; off: adverts off + `0x45a860` which **waits up to 3.1 s** for the HL server to stop | [V] | S [I] (blocks ≤3.1 s) |
| `gain` | 0x15 | 0x24c | 0 low ("MDB") / 1 high ("HDB") | refuse if `item_locked(0x15)`; `setting_set(0x15,v)` (CC `0x54d4a0`) | `0x47d060` → `0x735be0("HDB"/"MDB")` → output device re-applies the current volume with the new gain (`0x733640`) | [V] | S [I] (same `0x733640` path as HL volume) |
| `mseb` | 0x3f | 0x420 | 0/1 | refuse if `setting_get(0x65) != 100` (speed) or `mqa_playing()`; `setting_set(0x3f,v)` (CC `0x54d600`) | `0x481180` → `0x485000`: audio-engine plugin params `enable`=v, `max_samplerate`=96000 | [V] on/off only; the MSEB tuning values (ids ~0x40, 0x6x) not covered | S [I] |
| `lineout` | 0x2d | 0x280 | 0/1 | refuse if `item_locked(0x2d)`; **1 needs `force=1`** (stock UI shows `line_out_warning` dialog `output_select`, then `setting_set(0x2d,1)` at `0x4fa624`); 0: `setting_set(0x2d,0)` | `0x47dae0` → `Output Port Switch` mixer / `0x7363a0`; then volume: 1 → `volume_apply(100)` (**fixed maximum**), 0 → `volume_apply(setting_get(0x11))` | [V] | S [I] |
| `usbmode` | 0x08 | 0x274 | 0 audio (USB DAC, `uac_device_config.sh`) / 1 storage (`usb_storage_init.sh`) | **any change needs `force=1`**; `setting_set(8,v)` (CC `0x54dcc0`, toggles 0↔1) | `0x47dba0`: `system()` scripts that rebuild the USB gadget (configfs) | [V] | S [I] (blocking, **drops USB/adb**) |
| `balance` | 0x14 | 0x254 | −10…10 (L/R, 0 = centre) | refuse if `item_locked(0x14)`; `setting_set(0x14,v)` | `0x47d040` → `0x735b20` (signed byte at output-device +0x35, `0x733640`) | [V] (range from the play-settings slider `0x4f32c0`: min −10, max 10) | S [I] |
| `gapless` | 0x1b | 0x234 | 0/1 | `setting_set(0x1b,v)` (CC `0x54da00`) | `0x43dee0` → engine param 0x20c; if gapless=1 and fade=1, fade is switched off (`0x43df00(0,0)`) | [V] | S [I] |
| `fade` | 0x32 | 0x3c4 | 0/1 | `setting_set(0x32,v)` (CC `0x54d940`) | `0x43df00(3000,3000)` / `(0,0)` → engine params 0x211–0x213; fade=1 switches gapless off | [V] | S [I] |
| `filter` | 0x21 | 0x3c0 | 0 "Apodizing fast roll-off" / 1 "Linear phase fast roll-off" | refuse if `item_locked(0x21)`; `setting_set(0x21,v)` (list view `0x4cedc8`) | `0x480a80` → ALSA mixer `Digital Filter`. **Values ≥ 2 are coerced to 0 by `setting_set`** (`slti a1,2`), so "minimum phase" cannot be selected on this build | [V] | S [I] |
| `sleep` | 0x09 | 0x270 | 0/1 | `setting_set(9,v)` (CC `0x54dac0`) | `0x48ddc0(0xa, v)` → `system_lister` param, under its own mutex (`0x47b520`/`0x47b540`) | [V] | S [V by lock] |
| `sleepmin` | 0x0a | 0x0c | 5…120 min (UI step 5) | `setting_set(0xa,m)` | `0x48ddc0(7, m*60)` (seconds) | [V] (range from the list item `0x4f2e40`) | S |
| `playmode` | 0x1a | 0x250 | 0 order, 1 loop track, 2 shuffle, 3 loop list | `setting_set(0x1a,m)` | stores + `set_param(0xa,m)` (player lock) | [V] (also HiBy Link) | S [V] |
| `volume` | 0x11 | 0x23c | 0…`volmax` | refuse if line out = 1; `volume_apply(v&0xff)`; `setting_set(0x11,v)` | `setting_set(0x11)` rejects `v > max`, stores, notifies HiBy Link (`0x459e80`), per-output memories | [V] (HiBy Link `0x4578a0`, DLNA `0x453cc0`) | S [V] |
| `volmax` | 0x10 | 0x238 | read only | — | — | [V] | — |
| `speed` | 0x65 | 0x710 | read only (%, 50…200) | — | — | [V] | — |

Not in the list but seen: NOS 0x6a, SPDIF 0x3e, brightness 0xd, idle shutdown 4/0xc, BT sink type 0x24/0x25. Same pattern; not exposed (not requested, not reviewed in depth).

### Things the library deliberately does NOT do

- **UI refreshes** of the stock views (`0x549500`, `0x54c940`, `0x4bd020`, `0x4c0020`, `0x552560`, `0x46cc80`): they walk widgets and are UI-thread only (**U**). Skipping them is harmless while HiBy's UI is hidden; the stock views read the settings again when they are built. If the stock CC happens to be open, its icons stay stale until it is reopened [I].
- **High-volume (EU) warning**: the physical volume key path calls `0x535de0` (dialog `volume_long_time_high`) before changing the volume. HiBy Link does not, and neither do we. The shell must enforce its own limit if wanted.
- **DLNA/AirPlay without Wi-Fi**: the CC posts the message regardless; the network-settings page refuses when Wi-Fi is not connected (`0x47f7e0`, "warn3"). We mirror the CC. Recommendation for the shell: only offer DLNA/AirPlay when `wifi=1`.

### Not feasible / not covered

| Item | Why |
|---|---|
| Sleep timer *remaining time* | The CC computes it from another source (`0x54b9bc`, `%02d:%02d:%02d`); not traced. Workaround: the shell can count down from `sleepmin`. |
| Digital filter "minimum phase" (2) | `setting_set(0x21)` coerces ≥2 to 0 on this build [V]. |
| MSEB tuning sliders | Only on/off was mapped. |
| BT pairing / Wi-Fi network choice | Separate flows (scan lists, passwords); not settings. |

## 3. Implementation (library v7, command `'S'`)

```
shell: hs_set("gain", 1)
   writes /tmp/hibymod_setreq (rename) "HMSET1 id= op=set key=gain val=1 [force=1]" and sends 'S' to /tmp/hibymod_disp
library control thread: 'S' -> st_kick(): own detached thread (512 KB stack), no display mutex
   st_check(): 6 signatures of 16 bytes + every jump-table entry used (get 0x7f09c0 / set 0x7f0bfc)
               + /proc/self/maps for every fixed address + settings struct present (else -EAGAIN)
   st_set(): range -> volume rules -> unchanged? (rc 0, nothing called) -> item_locked -> force
             -> MSEB preconditions -> sequence of §2 -> read back; value differs -> -EAGAIN
   -> /tmp/hibymod_setres "id=N rc=R key=K val=<value now> ms=T msg" (+ "k=v" lines for list)
   -> one line "st: ..." in /usr/data/hibymod/disp.log
```

- **Inert** until an `'S'` arrives: no thread, no reads. Other commands unchanged (`'S'` bypasses the display mutex like `'L'`).
- Requests are served one at a time. Extra `'S'` while busy are coalesced: the thread re-reads the request file after each one and skips a file it already served (same inode/mtime/size), so a new request is never lost and none is served twice.
- Setting a value equal to the current one calls nothing (no flash write, no `BT:OFF` resend).
- `force=1` is required for `lineout=1` (fixed max output level) and any `usbmode` change.
- Errors: `-ENOEXEC` other firmware (nothing called), `-EAGAIN` settings not loaded / refused or busy (e.g. BT guard), `-ENOENT` unknown key, `-ERANGE`, `-EPERM` (greyed out, line-out fixed volume, MSEB preconditions), `-EACCES` (needs force), `-EROFS`.

### Files

| File | What |
|---|---|
| `src/hibymod_disp.c` | v7: last section "ajustes del reproductor ('S')" + `'S'` in `control()` |
| `src/hm_shared.h` | comment for `'S'` only (struct unchanged) |
| `src/hibyset.inc.c` | shell API: `hs_get`, `hs_set`, `hs_set_force`, `hs_list`, `hs_check`; non-blocking `hs_set_send`/`hs_get_send` + `hs_poll` |
| `src/hibysetctl.c`, `tools/build_hibysetctl.sh` | CLI: `check`, `list`, `get K`, `set K V [force]`, `raw` |
| `out/agent_set/hibymod_disp.so` | v7 build (v6 + `'S'`), **not installed** |
| `out/agent_set/hibysetctl` | static musl CLI |

Build (WSL):
```
bash tools/wsl/zbuild_so.sh src/hibymod_disp.c out/agent_set/hibymod_disp.so   # from Git Bash: MSYS_NO_PATHCONV=1 wsl bash ...
bash tools/build_hibysetctl.sh            # -> out/agent_set/hibysetctl ;  "pc" -> /tmp/hibysetctl_pc
```
Warnings: `-Wall -Wextra` on the library gives only the pre-existing `fclose` nonnull warning (line ~749); the CLI builds clean.

**Tested on the PC [V]**: library built for x86 and preloaded into a process named `hiby_player`: `check`/`list`/`get`/`set` all answer `rc=-8 (ENOEXEC)` without calling anything, a malformed request gives `rc=-22`, a burst of 8 `'S'` does not crash nor double-serve, and with no library the CLI returns `-ENXIO`. The player-side logic itself can only be tested on the device.

### Shell usage

```c
#include "hibyset.inc.c"                     // after hm_shared.h
int v; if (hs_get("bt", &v) == 0) ...        // blocking (≤2 s)
hs_set("gain", 1);                           // blocking (≤6 s); returns 0 only if really applied
// UI loop (non-blocking): one request in flight at a time
int id = hs_set_send("wifi", 0, 0);  ...  struct hs_res r; if (hs_poll(id, &r)) { r.rc, r.val }
struct hs_kv kv[24]; int n = hs_list(kv, 24); // every value in one round trip (CC opening)
```
Recommendations: call `hs_list` when the CC opens; for the volume slider send at most ~10 requests/s and only the last value (each `setting_set` rewrites `user.ini` on flash); grey out DLNA/AirPlay when `wifi=0`; ask for confirmation before `lineout=1`.

## 4. Risks

- **Runs inside the audio player.** A wrong call crashes playback (then `hiby_player.sh` restarts it). Mitigations: every function and every jump-table slot used is checked; values are range-checked before the call; nothing runs unless asked.
- **No lock around `setting_set`**: a simultaneous change of the *same* option from the stock UI (hidden) or HiBy Link could interleave. Same exposure as HiBy Link today [I, low].
- **`user.ini` write per change** (0xad0 bytes on UBIFS): rate-limit sliders.
- `usbmode` restarts the USB gadget: **adb over USB drops**. Test it last, or not at all over adb.
- `lineout=1` sets the output to fixed maximum (`volume_apply(100)`): **unplug headphones** before testing.
- `hibylink=0` blocks our thread up to ~3.1 s (fine: it is our thread), and `bt`/`wifi` changes are asynchronous in sys_server: the value reads back immediately, the radio follows a moment later.
- DLNA off while a DLNA stream plays stops it (stock behaviour).

## 5. Evidence pointers

- CC handlers: strings `pull_down_menu_iv_*` → `0x54cfe0` play mode, `0x54d0a0` bt, `0x54d1e0` wifi, `0x54d2e0` dlna, `0x54d3c0` airplay, `0x54d4a0` gain, `0x54d560` NOS, `0x54d600` mseb, `0x54d800` line out, `0x54d8c0` balance, `0x54d940` fade, `0x54da00` gapless, `0x54dac0` sleep, `0x54db60` spdif, `0x54dc20` hibylink, `0x54dcc0` usb mode.
- Full get/set table dump (all 0x8f ids, offsets and first instructions of each case) was produced with a script over `~/hiby/hp_dis.pkl`; the ids/offsets used are in §2 and hard-coded as expected values in `st_opt[]`.
- Labels: `str/english/play_settings.ini` (`sharp`, `slow`, `minimum_phase`, `balance`…), `sys_set.ini` (`usb_mode`: Storage/Audio, `sleep_time`), `line_out.ini` (warning text).

## 6. Classification summary

| Class | Options |
|---|---|
| Safe from a non-UI thread, precedent in the player [V] | `volume`, `playmode`, `sleep`, `sleepmin` (mutex) |
| Safe from a non-UI thread by inspection [I] | `bt`, `wifi`, `hibylink`, `gain`, `mseb`, `lineout`, `balance`, `gapless`, `fade`, `filter`, `usbmode` |
| Safe: real work deferred to the UI thread through its queue | `dlna`, `airplay` |
| Needs the UI thread (not done) | stock icon/list refreshes only |
| Not feasible | filter "minimum phase", sleep remaining time (not traced) |

## 7. Device test plan (with the user's permission; safest first)

`adb` = `%LOCALAPPDATA%\platform-tools\adb.exe -s "HiBy R3II_2025"`. `T=/usr/data/hibymod/t`. Music paused. Never open the browser on the device during these tests.

**P0. Backups (no risk)**
```
adb shell "mkdir -p $T; cp /usr/data/user.ini $T/user_before_v7.ini; md5sum /usr/data/hibymod/preload/*.so"
adb shell "cp /usr/data/hibymod/preload/hibymod_disp.so $T/hibymod_disp.so.bak"
adb push out/agent_set/hibymod_disp.so $T/hibymod_disp_v7.so
adb push out/agent_set/hibysetctl $T/ ; adb shell chmod 755 $T/hibysetctl
adb pull /usr/data/user.ini user_before_v7.ini
```
`user_before_v7.ini` holds every original value at the offsets of §2 (little-endian int32), e.g. `bt` at 0x394.

**P1. Install and reboot** (v7 = v6 + `'S'`; v6/v5 are also still untested on the device, see `research_play_list.md` §7)
```
adb shell "cp $T/hibymod_disp_v7.so /usr/data/hibymod/preload/hibymod_disp.so && sync && reboot"
```
Check that everything else still works (shell, gestures, `hibyplay status`, `disp.log`).

**P2. No side effects**
```
adb shell $T/hibysetctl check      # expect rc=0 "firmas ok"; rc=-8 = other firmware: STOP
adb shell $T/hibysetctl list       # write every value down (compare with the P0 copy)
adb shell $T/hibysetctl get volume
adb shell $T/hibysetctl set bt <current value>   # "sin cambios": proves the no-op path, calls nothing
```

**P3. Pure audio-engine / mixer toggles** (audible, instantly reversible; play music at low volume)
1. `set gapless X` then back. 2. `set fade X` (check gapless turns 0 with `list`), back to original values.
3. `set playmode 2` → `hibyplay next` shuffles; restore the P2 value.
4. `set volume <cur-5>` then `<cur>` — hear the change, check the stock volume popup/value with `get`.
5. `set balance -3` → left heavier; `set balance <orig>`.
6. `set gain 1` / `set gain 0` (audible level step; `-EPERM` means the item is greyed out for this output — expected with some outputs); restore.
7. `set filter 1` / `set filter <orig>` (subtle).
8. `set mseb 1` / `set mseb <orig>` (check `speed=100` first; `-EPERM` is the CC rule).
Record `ms=` and `disp.log` (`st:` lines) at each step.

**P4. Timer**: `set sleepmin 5`, `set sleep 1` → the device should power off after 5 min (or cancel with `set sleep 0` after 1 min and make sure it does NOT power off); restore `sleepmin`.

**P5. Radios** (asynchronous; give each 5–10 s)
1. `set wifi 0` / `set wifi 1` (if it was on): `ip link`, Wi-Fi icon in the shell. 2. `set bt 1` / `set bt 0` (`hciconfig`). 3. `set hibylink 1` / `0` (expect `ms` up to ~3100 when turning off).
4. With Wi-Fi connected: `set dlna 1` → `ps | grep dmrd` appears; `set dlna 0` → gone. `set airplay 1` → shairport running; `0` → stopped.

**P6. Output routing (headphones unplugged!)**: `set lineout 1` → `-EACCES` without force; `set lineout 1 force` → line out at fixed level, `set volume` → `-EPERM`; `set lineout 0` → volume back to the stored value.

**P7. USB mode (last; not over adb-over-USB)**: from a shell on the device (or via Wi-Fi adb), `set usbmode <other> force`, check what the PC sees, then restore `set usbmode <orig> force`.

**P8. Errors**: `get nosuch` → `-ENOENT`; `set balance 50` → `-ERANGE`; `set volmax 3` → `-EROFS`; 8 quick `get` calls in a loop (each waits for its answer) → all answered. Two clients at the same time are NOT supported: the second request file can replace the first before it is read, and the first client then gets `-ETIMEDOUT` (the shell must serialise its requests).

**Restore**: `hibysetctl set K <value from P2>` for each changed key (preferred: it applies the value). Library rollback: `adb shell "cp $T/hibymod_disp.so.bak /usr/data/hibymod/preload/hibymod_disp.so && sync && reboot"`. Do **not** copy `user_before_v7.ini` back while the player runs (it rewrites the file on every change and does not re-read it); only with the player stopped, and only if really needed.

## 8. Shell integration: the Settings app (2026-11-10)

HiBy's system Settings app and its "USB DAC" app are replaced by our Settings app (`src/settings_app.inc.c` page framework + `src/sysset.inc.c` system pages). Entry points: `settings_app(ix, iy)` (root page) and `usbdac_app(void)` / `usbdac_app_at(ix, iy)` (opens straight on the USB page, as its own root titled "DAC USB").

How it uses this API (`sysset.inc.c`, prefix `ps_`):
- On opening: values from `/usr/data/user.ini` at the offsets of §2 (instant), then one `hs_list` in a worker thread. Again (max 1/s) every time a sub-page is closed.
- Changes: shown at once; a single worker thread sends them one at a time (`hs_set_send` + `hs_wait_ex`), at most 4 writes/s, coalescing repeated changes of the same key (balance slider). On failure the value goes back to the last confirmed one and a toast explains (`-ENXIO/-ETIMEDOUT` not responding, `-EPERM` greyed out / MSEB rule, `-ENOENT` needs a newer library...). After `gapless`, `fade`, `lineout`, `wifi` it re-lists (the player changes other keys by itself).
- `lineout=1` and `usbmode` ask for confirmation in an iOS alert, then use `HS_FORCE`.
- Not in the library yet: the keys of §9. The UI already uses them; when `hs_list` succeeds without them they are shown greyed out with the saved value.
- Only one client may talk to the library at a time: other shell code should not send `'S'` requests while `ps_busy()` is true.

## 9. Proposal: library v8 keys (shim change NOT made)

Found statically with the same method (jump tables `0x7f09c0`/`0x7f0bfc` + the system-settings list builders `0x4f2c00`..`0x4f2f80` and the value renderer `0x4e1b10`..`0x4e1e40`, strings in `str/*/sys_set.ini`). All [V] for id/offset/case address, [I] for ranges marked so.

| Key | id | off | Values | Evidence | Set case | Get case |
|---|---|---|---|---|---|---|
| `bright` | 0x0d | 0x18 | 1..100 (%; others become 50 inside `setting_set`) | renderer `"%d%%"`; set does `v-1 < 100 ? v : 50`, then `0x47d1c0(v)` applies it | `0x42c748` | `0x42a974` |
| `bklon` | 0x0e | 0x3d4 | 1 = screen turns off after `bkltime`, 0 = "Stay on" | renderer: `get(0xe)==1 ? "%dS" of get(0xf) : "always_on"`; set → `0x48ddc0(1, v)` (system_lister param, own mutex) | `0x42c728` | `0x42a96c` |
| `bkltime` | 0x0f | 0x10 | seconds, 10..120 step 10 [I: list item max 0x78, step 10] | renderer `"%dS"`; set → `0x48ddc0(2, v)`; **no range check inside `setting_set`**: the library must check | `0x42c184` | `0x42a964` |
| `idle` | 0x0b | 0x26c | 0/1 (idle shutdown) | renderer: `get(0xb)==1 ? "%dMin" of get(0xc) : "off"`; set → `0x48ddc0(6, v)` | `0x42c6c8` | `0x42a984` |
| `idlemin` | 0x0c | 0x14 | minutes, 1..10 [I: list item max 10, step 1] | set stores, then `0x48ddc0(4, v*60)` | `0x42c6a8` | `0x42a97c` |
| `update` | 0x07 | 0x20 | 0 manual, 1 auto (library scan mode) | renderer: `get(7)==1 ? "auto" : "manual"`; set only stores (`sltiu 2`) | `0x42c540` | `0x42a9a4` |

Proposed `st_opt[]` rows (same layout as v7, `flags` 0, no UI posts):
```c
    { "bright",   0x0d,  1, 100, 0, 0, 0, 0x42a974, 0x42c748 },
    { "bklon",    0x0e,  0,   1, 0, 0, 0, 0x42a96c, 0x42c728 },
    { "bkltime",  0x0f, 10, 120, 0, 0, 0, 0x42a964, 0x42c184 },
    { "idle",     0x0b,  0,   1, 0, 0, 0, 0x42a984, 0x42c6c8 },
    { "idlemin",  0x0c,  1,  10, 0, 0, 0, 0x42a97c, 0x42c6a8 },
    { "update",   0x07,  0,   1, 0, 0, 0, 0x42a9a4, 0x42c540 },
```
Thread: `bklon`/`bkltime`/`idle`/`idlemin` go to `0x48ddc0` (same mutex-protected system_lister path as `sleep`/`sleepmin`, classified safe [V by lock]); `bright` calls `0x47d1c0` (not reviewed: check it touches only sysfs/ioctl before enabling); `update` only stores. The shell (`hibyset.inc.c`) needs no change: it already sends any key name, and `hs_ini_get()` reads all of them from `user.ini`.

Also seen, not proposed: usbmode value **2 = "Dock"** (`"adb"` label) exists in the renderer (the v7 library allows only 0/1, which is right); id 0x56 (clock/rotation, 3 values, posts UI message 0x153); id 4 (theme). Date/time: the player sets the clock with `date -s` + `hwclock -w --utc` and keeps the zone in `/usr/data/localtime` → the shell does the same itself (`settimeofday` + `hwclock -w --utc`), no library needed. "Auto set time" and 12/24 h ids were not traced. Factory reset (`restore.dlg`) and manual library rescan ("long-press Music" in the stock home) were not traced; not offered.

### Device verification order for the Settings app (after §7 P0–P2)
1. Open Settings with the v7 library: values must match `hibysetctl list`; Display/Library rows with §9 keys greyed out ("next library version").
2. P3 items from the UI (gapless/fade exclusion: the other switch slides off; play mode; balance slider — at most ~4 `st:` lines/s in `disp.log` while dragging; gain; MSEB greyed at speed ≠ 1x).
3. P4 sleep timer from General > Auto power-off (footer shows the off time).
4. P5 Wi-Fi/BT toggles (fallback pages) and Streaming (AirPlay/DLNA only with Wi-Fi on; HiBy Link off can take ~3 s — switch stays on, no freeze).
5. P6 line out (headphones unplugged): alert → on; then off.
6. P7 USB mode last, from Wi-Fi adb or no adb: "DAC USB" icon → alert → mode changes; the PC re-enumerates.
7. Error path: with the library removed (or v6), toggles revert after the timeout and the toast says the player is not responding; Sound shows the "not responding" footer.
8. Date & time: set a time ±1 min, check the status bar clock and `hwclock -r` after a reboot.
