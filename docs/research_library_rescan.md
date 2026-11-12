# Music library rescan on demand (HiBy R3 II 2025, `hiby_player`)

Date: 2026-11-12. Binary: `rootfs/usr/bin/hiby_player`, md5 `d1dea413a91ca3c6759c9029744bc52c` (same build as the other research docs).

**Tags:** **[V]** verified statically (disassembly, tables, strings). **[I]** inferred. **Nothing here has been run on the device.**

Tools: the existing `~/hiby/tools` scripts (`xref.py`, `hpdis.py`, `callers.py`, `callargs.py`, `memref.py`) with `~/hiby/venv`. I also used four small helper scripts, kept outside the repo: a data-pointer search, a `lui`/`addiu` code-reference search across all sections, a function-start finder and a table dumper.

---

## 0. Summary

| Question | Answer |
|---|---|
| What starts a scan? | `music_update_show(parent, owner, type)` at **`0x507f00`** [V]. It opens the `music_update` dialog. The dialog's `oncreate` (`0x507d20`) calls `music_scan_start(type)` (`0x507320`), which starts the worker thread `0x507240`. `type` 1 = full library scan; 2 = playlist/file variant. |
| What does the stock "Update database" item call? | Play settings (`vg_listview_play_settings`, click handler `0x4f84c0`), item index 3 (`music_update`, "Update database"). If `access("a:")` fails, it shows the toast `no_tf` ("SD-card not found"). Otherwise it calls `0x507f00(listview, view->0xd8, 1)` (call at `0x4f871c`) [V]. |
| Is there a UI message for it? | **Yes: UI message `0x125`** [V]. UI handler table `0x7f8818` entry `{0x125, 0x464c40}` at `0x7f88c0`. The handler calls `0x507f00(NULL, mainwin, 1)` unless DAC, USB, AirPlay or BT-input mode is active. HiBy Link's `startScanMusic` posts exactly this message: `0x457f80` → `ui_post(0x125)`. |
| SD insert? | `lg_gui_sdcard_message(1)` (`0x48aba0`) posts `0x111`. The handler `0x464620` defers `0x4638e0`, which scans **only if setting 7 (`update`) == 1 (auto)**. It waits up to 1.5 s for `a:`, then calls `0x507f00(NULL, win->0x48, 1)` [V]. |
| Recommended for ByOS | From the preload control thread: if `scan_idle()` (`*(int*)0x93bed8 == 1`) is true, call **`ui_post(0x125)`** (`0x48a900`; ByOS already has a signature for it). Then poll `0x93bed8` until it goes 0 → 1. This is the same path HiBy Link uses. The scan runs on the UI thread and the stock dialog's own worker thread. |
| Big caveat | A full scan **stops playback and clears the current list path**: the worker calls `set_param(0x15, "")` and then `av_close` (`0x437620`) [V]. ByOS must expect playback to stop and must reload its queue (`hibymod\q1/q2`) after the scan. It must also not read `usrlocal_media.db` while the scan runs. |
| No-code alternative | Set `update` = 1 (auto) through the existing 'S' settings request, then make the SD card go away and come back (USB mass-storage round trip, or remount). The SD-insert path (`0x111` → `0x4638e0`) then rescans. The phone HiBy Link app's "scan music" also works (external). |

---

## 1. The scan code

### 1.1 Dialog opener `0x507f00` [V]

```
void *music_update_show(void *parent /*may be NULL*/, void *owner, int type)
```
- `malloc(0xc)` ctx: `+4 = type`; `+8 = (parent == NULL)`.
- Dialog: `0x494b80("music_update", parent, owner, 0, &cbs)`. Its callbacks are `oncreate 0x507d20`, `ondestroy 0x507c00` and timer/refresh `0x5077a0`.
- The layout is `music_update.dlg` (`0x922250`, used at `0x4963b4`).
- Signature: `3c020050 27bdffa8 24427d20 afa20020`.

Callers (all [V]):

| Call site | Context | Args |
|---|---|---|
| `0x464c64` | UI msg `0x125` handler `0x464c40` | `(NULL, mainwin, 1)` |
| `0x4639c0` | SD-insert deferred `0x4638e0` (auto mode only) | `(NULL, win->0x48, 1)` |
| `0x4f871c` | Play settings → "Update database" (item 3) | `(listview, view->0xd8, 1)`, after the `access("a:")` check |
| `0x529238` | `where_songs_vg_update` ("Scan" button on the empty library) | `(a0, a1->0x50, 1)`, after the `a:` check |
| `0x5554a0` | `launcher_apps_vg_scan` | `(a0, a1->0x50, 1)`, after the `a:` check |
| `0x4e2d7c`, `0x4e30f0` | System settings: `update_type` change and `db_storage_change` (DB moved to/from SD; resets `/data/usrlocal_media.db`) | `(win->0x40, listview, 1)` |
| `0x55b088` | m3u view, `msg_not_found_music` path | `(…, …, 2)` |

`a:` is the litegui drive for the SD card (UTF-16 `"a:"` at `0x81a904`). `0x483ba0(path, mode)` is an `access()` wrapper.

### 1.2 Start and worker [V]

- `0x507d20` (oncreate):
  - wires the `vg_update_cancel` click to `0x507e40`;
  - for type 2, shows `updating_file`;
  - calls `0x4293a0()`, then **`0x507320(type)`**.
- `0x507320` (`music_scan_start`):
  - creates 2 mutexes;
  - resets the state block at `0x93bec4`;
  - loads the `exception.ini` names (`unknow_artist`, `unknow_album`, `unknow_genre`, `unknow_text`);
  - builds the scanner config (`0x723cc0` with callbacks `0x507300/0x507220/0x507200`);
  - stores `type` at `+0x28`;
  - starts the thread **`0x507240`** (`0x47b360`).
  - It has exactly one caller (`0x507da8`), so it is only reached through the dialog.
- Thread `0x507240` (named `lg_music_update`):
  - `+0x14 = 0` (busy);
  - `0x7442b8(1, 0, "")`: HiBy Link scan-state notification (`hls_service_action_notify`);
  - if type != 2: **`set_param(0x15, "", 0)`** (clears the current list path), **`0x437620()` = `av_close`** (stops playback), then `0x724100(0)` (the full scan);
  - type 2: `0x7242a0()` instead, with no stop and no clear;
  - `+0x18 = 1` if the scan returned nonzero (error);
  - `+0x14 = 1` (idle).

### 1.3 State block `0x93bec4` [V]

| Offset | Address | Meaning |
|---|---|---|
| +0x04 | `0x93bec8` | worker thread handle |
| +0x08 | `0x93becc` | cancel flag (`0x5076e0(v)` sets it; HiBy Link `stopScanMusic` → `0x456648`) |
| +0x0c/+0x10 | | mutexes |
| **+0x14** | **`0x93bed8`** | **idle flag: 1 = idle (initial value 1 in `.data`), 0 = scanning** |
| +0x18 | `0x93bedc` | 1 = scan returned an error |
| +0x24 | `0x93bee8` | aborted by `0x507720` (cancel-and-wait, used at `0x44b2e4` and `0x463df8`) |
| +0x28 | `0x93beec` | type |

- `scan_idle()` = **`0x507620`**: `lw v0, 0x93bed8`. Signature `3c020094 03e00008 8c42bed8 00000000`.
- Callers that check it before touching the library: `0x444bf0`, `0x44aa58`, `0x4593ec`, and others.
- HiBy Link `getScanState` (`0x456660`) uses it as well (`0x456678`).

### 1.4 Completion UI [V]

- The timer `0x5077a0` shows the live count (`0x71d6e0`) with `update_handle`.
- When `scan_idle()` == 1, it shows `update_finish` and `update_done_msg` (N tracks found) and closes the dialog after 2000 ms (`0x494aa0(dlg, 3, 2000)`).
- `ondestroy` `0x507c00`:
  - if `parent` was NULL (ctx+8), the scan was not aborted and type != 2: it switches the stock UI to `lg_activity_main` (`0x42f960(0xc, "launcher")`, then `0x44b1c0("lg_activity_main")`);
  - otherwise it refreshes the `vg_main_category_hiby` counts.
- So the `0x125` path ends with the stock UI on its main activity. This is the same as a HiBy Link scan.

---

## 2. Triggers

### 2.1 UI message `0x125` (recommended) [V]
- UI handler table at `0x7f8818` holds `{msg, fn}` pairs, 0x80 entries.
- The dispatcher at `0x467060..0x4670a8` runs `fn(window, &msg)` **on the UI thread**.
- Entry `0x7f88c0` = `{0x00000125, 0x00464c40}`.
- `0x464c40`:
  ```
  if (!in_special_mode(win) /*0x4631a0: vg_airplay/dac/usb/bt_input_hiby present*/)
      music_update_show(NULL, win, 1);
  ```
  Signature `27bdffe0 afb00018 afbf001c 0c118c68`.
- It does **not** check `scan_idle()` or the `a:` drive itself.
- Only poster: HiBy Link `startScanMusic` handler `0x457f80` (`ui_post(0x125)`, then sleep 1 s).
  - The HiBy Link op table is set up at `0x45a580..`: `setScanMusic 0x455660`, `startScanMusic 0x457f80`, `stopScanMusic 0x456640`, `getScanState 0x456660`.
  - The names are the strings at `0x8fcc70..0x8fccf4`.
  - The order matches, and stop/get call `0x5076e0` and `0x507620` [V].

### 2.2 SD card events [V]
- `lg_message_thread` handles `SD_INSERT_MSG` and `SD_REMOVE_MSG` (`0x48ff30..`) → `0x48aba0(state)`:
  - 1 → `0x48a7e0(0x111)`;
  - 0 → `0x112`;
  - 2 → `0x113`.
- Handler `0x111` = `0x464620`. It registers `0x4638e0` as a deferred callback through `0x4494c0(win, "vg_screensavers_hiby", …)`.
- `0x4638e0`:
  ```
  if (setting_get(7) == 1 && !in_special_mode(win->0x48)) {
      wait up to 15×100 ms for access("a:") == 0;
      music_update_show(NULL, win->0x48, 1);
  }
  ```
- So an SD re-mount only rescans when **auto update** is on. On this device the setting is apparently manual, which explains the observations.

### 2.3 Settings
- Setting `update` (id 7, `research_settings_api.md`) only stores the value.
- Changing the update mode in stock system settings (`0x4e2ce0`) and changing the DB storage location (`0x4e3020`) both open the dialog themselves.
- The stock Home "long-press Music" hint string (`update_notify`) exists. That path was not traced and is not needed.

---

## 3. Recommended ByOS implementation

New control request (e.g. `'R'`), handled on the preload control thread:

1. Verify the signatures, and refuse if any of them differs:
   - `0x48a900` (`ui_post`, already in the ByOS table, entry 3);
   - `0x507620`;
   - the table word pair at `0x7f88c0` = `{0x125, 0x464c40}`;
   - optionally `0x464c40` and `0x507f00`.
2. If `*(volatile int *)0x93bed8 != 1`, the scan is already running: return busy.
   - Do not post again: `0x464c40` would open a second dialog and start a second worker over the same state block.
3. Optionally check that `/usr/data/mnt/sd_0` is mounted. The stock settings item refuses without the SD card; `0x125` does not check, and the scanner then finds 0 tracks.
4. Call `ui_post(0x125)`. It only enqueues `{0, 0x125, 0, 0, 0}` with `0x462b20`. The dialog, the thread and the DB writes then happen in stock code, on the stock threads. This is exactly what HiBy Link does.
5. Completion: poll `0x93bed8`.
   - It goes to 0 within about 1 frame after the UI loop runs the message; time out if it never does (special mode, UI blocked).
   - It returns to 1 when the scan has finished.
   - `0x93bedc == 1` means the scan failed.
   - The track count can then be read from the DB.
6. After completion:
   - reload or rebuild ByOS's own queue (the scan cleared list path `0x15` and closed the audio);
   - re-open any SQLite handles or cached queries on `usrlocal_media.db`.
   - During the scan, do not read or write the DB from ByOS.
7. Cancel (optional): `0x5076e0(1)` sets the cancel flag, like HiBy Link `stopScanMusic` (signature `3c020094 2442bec4 ac440008 03e00008`). It is a plain store, so it is safe from any thread.

**Why not call `0x507f00` or `0x507320` directly:**
- `0x507f00` builds a litegui dialog. Like all view code, it must run on the UI thread, and the control thread is not the UI thread.
- `0x507320` without the dialog skips the UI, but it was only ever called from oncreate. The `ondestroy` cleanup (freeing ctx, the refresh) would never run.
- It also races with any dialog the user opens.
- `ui_post` is the only cross-thread entry point that is already used for this purpose.

**Side effects to show in the ByOS UI** [V]:
- playback stops;
- the stock dialog "Please wait… N" appears on the stock layer and closes after the "Update complete" message;
- the stock UI then switches to `lg_activity_main`.
- If ByOS hides the stock layer, only the stop and the navigation matter.

---

## 4. Alternatives without code calls

1. **Auto mode and SD re-mount** [V path, I end-to-end]:
   - set `update` = 1 with the existing 'S' request (`setting_set(7, 1)`), or once in stock settings;
   - after USB mass storage ends (or any SD remove/insert), `SD_INSERT_MSG` → `0x111` → `0x4638e0` scans automatically.
   - Drawback: it scans on every insert or USB session, not on demand after a download.
2. **HiBy Link app** `startScanMusic` (BT or Wi-Fi). This is external and uses the same `0x125` path.
3. **Manual:** stock Play settings → "Update database" (requires the SD card).

---

## 5. Test plan (device; not run)

| Step | Action | Expected |
|---|---|---|
| P0 | Back up `/usr/data/usrlocal_media.db` | |
| P1 | Read `0x93bed8` with music paused | 1 |
| P2 | `'R'` request | `0x93bed8` → 0; the stock dialog appears; playback stops |
| P3 | Wait | `0x93bed8` → 1; "Update complete"; the DB mtime changes; `0x93bedc` = 0 |
| P4 | `'R'` twice quickly | The second one returns busy |
| P5 | `'R'` without the SD card | The scan finishes quickly with 0 tracks. ByOS should refuse beforehand. |
| P6 | Check that ByOS reloads its queue after P3 | |
