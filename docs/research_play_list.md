# Playing a file or a list with the stock engine (hiby_player)

Date: 2026-11-10. **[V]** = verified (in the binary, or on the PC against a copy of the database); **[I]** = inferred, not tested on the device.

The device was not connected over adb during this work (`adb devices` empty). So everything comes from analysis of the binary `rootfs/usr/bin/hiby_player` (md5 `d1dea413a91ca3c6759c9029744bc52c`) and from copies of the database (`~/hiby/big.db`, `mixtest.db`, `rt2.db`). **Nothing has been tested on the real player**: §7 is the test plan.

Continues `research_playback.md` §5.

## 0. Summary

| Question | Answer |
|---|---|
| Is there an internal function "load this list and start at index i"? | **Yes [V].** The queue is a *list path* (UTF‑16 string) stored in the player state, together with an index. When a song is tapped, the stock UI does `set_param(0x17,0,0)`, `set_param(0x15, path)`, `set_param(8, n)`, `set_param(0x10, current)` and `play_index(i, -1, 0x10002)` (`0x43e8a0`). |
| Can it be called from the library? | **Yes [V/I].** The binary is `EXEC` (not PIE, base `0x400000`), so the addresses are fixed. Those functions take the player lock themselves. The **HiBy Link** server (the phone app) already calls them **from its own thread**, not from the UI thread (`playUuid` → `0x4577a0`). We do the same from a thread of our own. |
| Where does the list content come from? | The list path is the key of `MANAGER_TABLE(id, path, count…)`. The songs are in `list_tb_<id>`, which is read with `SELECT * FROM list_tb_%d limit ? offset ?` (`0x71de60`) **every time a track is needed** [V]. Example: `list_tb_1` + `'album_artist\ABBA'` = the current queue in the database copy. |
| Own queue (b + a) | The library creates `list_tb_7701` or `list_tb_7702` and its row in `MANAGER_TABLE`, with path `hibymod\q1`/`hibymod\q2`. To do so it uses **the player's own SQLite connection and lock**, with no second process writing. It then calls the stock sequence. Next, previous, shuffle, the stock Now Playing and resume at power-on read that table like any other list. |
| Is there an IPC that does this (c)? | **No local one.** The player's message queue (`0x462b20`, messages 4/5/6/8) only does play/pause. HiBy Link (BT/Wi‑Fi) does have `playUuid`/`playTrackUuid`/`setPlayMode`, but with its own binary protocol and "uuids" using the internal list grammar. It is a possible external route, but much more expensive (§5). |
| Shuffle / repeat | `setting_set(0x1a, mode)` + `set_param(0xa, mode)`, the same as HiBy Link's `setPlayMode`. Modes: **0** in order, **1** repeat track, **2** shuffle, **3** repeat list [V, from the name table]. It is a persistent setting. |
| Status | Code written and compiled: `bin/hibymod_disp_v6test.so` (**not installed**), `pc_play_list()` in `playctl.inc.c` and `hibyplay list…`. The SQL logic is tested on the PC against a copy of the database [V]. The on-device test is still pending (§7). |

## 1. Binary facts

- `ELF 32-bit LSB executable, MIPS32 rel2`, `Type: EXEC`, `LOAD 0x400000 R E` and `0x8fa000 RW`. Text and `.data/.bss` addresses are absolute on every boot [V].
- Wide strings: 2-byte UTF‑16LE (`hw16`), not glibc `wchar_t`. Own functions: `0x426040`/`0x426080` (copy and bounded copy), `0x426180` (compare), `0x4261e0` (compare n characters) and `0x426300` (length).
- **Player state `P = 0x969ac8`** (`0x1e68` bytes). `P+0x48`/`P+0x4c` are pointers to the player's lock/unlock functions.

### 1.1 Parameters (`set_param` `0x4366a0(id, ptr, val)`, `get_param` `0x4368e0(id, out, out2)`)

Jump tables at `0x7f1c34` (set) and `0x7f1cb0` (get). Both functions take the `P+0x48`/`0x4c` lock.

| id | field | meaning |
|---|---|---|
| 0x08 | `P+0x2c` | number of tracks in the list. `get(8, &n, 1)` recomputes it (`0x433ca0`); for a normal path it returns the value that was set |
| 0x0a | `P+0x24` | playback mode; also notifies HiBy Link (`0x459e00`) |
| 0x0b | `P+0x34` | current index in the list (`play` writes it as `i - off - n`, see 0x17) |
| 0x10 | `P+0x38` | previous index (the UI puts the current one here before changing) |
| 0x15 | `P+0x1654` (`0x96b11c`) | **list path** (UTF‑16, max. 0x104) |
| 0x17 | `P+0x3c`, `P+0x40` | offset and header of the view (folders before the songs). 0,0 for a plain list |
| 0x1b | `0x96a684` | list title (UTF‑8, 64 bytes) |

### 1.2 Starting track i

```
0043e8a0 play_index(i, pos, flags):
  get_param(0x15, path)            ; the current list
  0x43e7a0(path, i, item)          ; track i: first an open view with that path (0x44b620/0x50a9e0),
                                   ;   otherwise 0x71de60(item, i, 1, 1, path) = database query
  0x43dfc0(item, i, pos, flags)    ; set 0xb = i-off-n; set 0x13/0x1a = 0; get 8 (n) -> 0x435ce0 (shuffle order);
                                   ;   0x4371c0 -> 0x433900(flags, item, pos) under the player lock
```

Observed calls (`callers.py 43e8a0`):

| Address | Caller | Flags |
|---|---|---|
| `0x4b4634`/`0x4b48a4` | UI, tapping a song in a list view | `0x10002`, pos = -1 |
| `0x457834` | HiBy Link `playUuid` | `0x10002`, pos = -1 |
| `0x457f4c` | HiBy Link `playTrackUuid` | `0x10002`, pos = -1 |
| `0x57031c` | "play all" (`a:\*`) | `0x10002`, pos = -1 |
| `0x439aec`/`0x509fe8` | resume at startup | `0x40000`, pos = setting `0x1f`; the path comes from setting `0x20` |

### 1.3 What the stock UI does when a song is tapped (`0x4b4420`)

```
004b45c8  lw s4,0x3b70(s0); lw s1,0x3b84(s0)      ; (only views with a header, view->0x40==2)
004b45d8  jal 0x4366a0  (0x17, s4, s1)             ; in all others: (0x17, 0, 0)  [0x4b4814]
004b45e8  jal 0x4366a0  (0x15, view+0x3de0, 0)     ; list path of the view
004b45f8  jal 0x4366a0  (8, view->0x70, 0)         ; number of items
004b4610  jal 0x4368e0  (0xb, &cur, 0)
004b4620  jal 0x4366a0  (0x10, cur, 0)
004b4634  jal 0x43e8a0  (i, -1, 0x10002)
```

### 1.4 HiBy Link (the same path, from another thread)

- Method table in `.data.rel.ro`, `{name[32], id, handler, id}`. For example `setPlayState 0x1201`, `setPlayMode 0x1202`, `playUuid 0x1204`, `playTrackUuid 0x120a`… The handler for the 0x12xx ones is `0x744678`, which calls the callbacks stored at `0x9b46c0`.
- Registration: `0x45a64c jal 0x744904(struct)`. Callbacks: `+0x00 setPlayState 0x457a80`, `+0x04 setPlayMode 0x4578e0`, `+0x0c playUuid 0x458d80`, `+0x10 playPrevious 0x456ce0`, `+0x14 playNext 0x456ca0`, `+0x18 setProgress 0x457760`, `+0x24 playTrackUuid 0x457e00`…
- `playUuid(list, track)` → `0x457d60` (UTF‑8→UTF‑16) → `0x4577a0`:

```
004577d4  jal 0x455c20 (uuid, &type, path)   ; uuid -> internal list path
004577e4  jal 0x456380 (type, path, track)   ; index of the track in the list
00457810  jal 0x4366a0 (0x17, 0, 0)
00457820  jal 0x4366a0 (0x15, path, 0)
00457834  jal 0x43e8a0 (idx, -1, 0x10002)
```

- `setPlayState(1|2|…)` → `0x436d40(5|6|4, …)`, which posts a message to the player queue (`0x462b20`, queue at `*(P+0)`).
- `setPlayMode(v)` → `0x42baa0(0x1a, m)` (setting) + `0x4366a0(0xa, m)`. Mapping: HiBy Link 1→0, 2→3, 3→1, 4→2. `0x459e00` does the reverse.
- Internal mode names: at `0x4f8c00` the array `{"order","single","random","loop"}` = 0..3 is built, i.e. *Play playlist*, *Loop track*, *Shuffle* and *Loop list* (`str/english/play_settings.ini`) [V].

Thread: the server runs in its own threads (`hl_thread_begin`, BT/Wi‑Fi transport), so the player already accepts these calls from outside the UI thread. This is the main reason to call them from a library thread instead of waiting for an `FBIOPAN`, which never arrives when the screen is off or static.

### 1.5 Lists in the database

- Database context: `ctx = *(void**)0x991364`. `ctx+0` is the `sqlite3*`; `ctx+0x564`/`+0x568` are the pointers to `0x70eee0`/`0x70ef00`, the lock/unlock (a semaphore, `0x47b460(sem,-1)`) that wraps every database access. They are set at `0x71a6e8`.
- `0x71de60(out, from, count, flag, path)`: list path dispatcher.
  - It compares the path against `all`, `album`, `format`, `m3u`, `book*`, `bookmark=`, `data\`, `collect`, `search`, `step`, `history`, `recent`, `info_table`, `music_search_`, `qobuz`, `sony`…
  - If none match: `id = 0x719520(path)` (`SELECT id FROM MANAGER_TABLE WHERE path = ?`, with `bind_text16` of `(wcslen+1)*2` bytes, which is why stored paths end in `\0`).
  - Then: `SELECT * FROM list_tb_%d limit ? offset ?` (`0x71ecd4`), with no ORDER BY, so the order is the `rowid`. There is special ordering only if the path starts with `album` or is a drive path (`a:` …) [V].
- `list_tb_N` has **the first 27 columns of MEDIA_TABLE** and `PRIMARY KEY(id, path, cue_id)`. The exact schema is in the string `CREATE TABLE IF NOT EXISTS %s%d(...)` [V].
- The tables are created by `0x7221a0` (folder scan/query → `INSERT INTO %s%d VALUES …`) when a view is opened. If nobody has created it, `play_index` on a folder or .m3u path does not find the track: `0x719520` returns -1. That is why the library creates it itself.
- Statically linked SQLite API (3.12.2, `THREADSAFE=1`) [V by usage]:

  | Function | Address | Evidence |
  |---|---|---|
  | `prepare_v2` | `0x62e740` | `(db, sql, -1, &st, 0)` |
  | `step` | `0x618480` | returns 100/101 |
  | `finalize` | `0x5fbae0` | `beqz a0` |
  | `reset` | `0x5fbbc0` | |
  | `bind_text16` | `0x5db5a0` | `lb SQLITE_UTF16NATIVE` |
  | `bind_int` | `0x5db700` | `j bind_int64; sra a3,a2,31` |
  | `column_int` | `0x5c9840` | |

- Drives: `a:` = `/data/mnt/sd_0` [V, from data]; `b:` = `sd_1` [I].

## 2. Options evaluated

| | Option | Verdict |
|---|---|---|
| a | Call the internal function | **Chosen**, with the exact sequence used by the UI and HiBy Link. No "load list event" is needed: the state is changed with `set_param` (under the lock) and `play_index` is called. |
| b | Write the queue to the database and force a reload | There is no "reload" event. But track i is **always** read from `list_tb_<id>` on the spot, so writing the table + (a) is enough. It is written from inside the player, with its connection and its lock: no inter-process locking and no other process's statement cache. `list_tb_1` (the user's) is not touched. |
| c | Internal IPC | The message queue `0x462b20` only carries play/pause/stop. `/data/dmr_streamer` (DLNA) is ruled out (`research_playback.md` §1). HiBy Link: §5. |

## 3. Implemented design (library v6, command `'L'`)

```
shell: pc_play_list(paths,n,i,title,mode)
   writes /tmp/hibymod_playreq (atomic rename) and sends 'L' to /tmp/hibymod_disp
library (control thread): 'L' -> pl_kick(): own thread (512 KB stack), without the display mutex
   pl_check(): 13 signatures of 16 bytes + mappings (/proc/self/maps) + ctx->lock==0x70eee0 and unlock==0x70ef00 + P+0x48 != 0
   slot = the one NOT playing (get_param 0x15 == "hibymod\q1" ? 2 : 1)
   db_lock(); BEGIN
     TEMP hm_req(pos, path, cue) <- the paths (a:\..., with trailing \0, bind_text16)
     DROP/CREATE list_tb_770x (stock schema)
     INSERT OR IGNORE ... SELECT from MEDIA_TABLE LEFT JOIN hm_req ORDER BY pos   (unscanned=1: missing ones are included too)
     real index = rows before the first row of the requested path
     MANAGER_TABLE: DELETE id/path; INSERT (770x, 'hibymod\qS\0', n, n, 0, 0)
   COMMIT (ROLLBACK if anything fails); db_unlock()
   [mode] setting_set(0x1a,m); set_param(0xa,m)
   [title] set_param(0x1b,title)
   set_param(0x17,0,0); set_param(0x15,path); set_param(8,n); get(0xb)->set(0x10); play_index(i, pos|-1, 0x10002)
   -> /tmp/hibymod_playres  "id=N rc=R rows=M index=I slot=S ms=T msg" (+ one line in /usr/data/hibymod/disp.log)
```

- **Inert** if no `'L'` arrives: no thread is created and nothing is read. The other commands are unchanged, except that `'L'` no longer goes through the display mutex.
- Different firmware: `rc=-ENOEXEC` and nothing is called. Before reading a fixed address it is checked in `/proc/self/maps`, so that a different binary cannot crash the player.
- One request at a time: if another arrives in the meantime, it is ignored and noted in the log.
- Expert requests:
  - `list=PATH` plays a list that already exists, without writing anything; it is the safest test. `n` (param 8) comes from its `MANAGER_TABLE` row; if it has none (folders `a:\…`), the previous `n` is kept [I].
  - `check=1` checks signatures and returns the current mode, list, index and total.
  - `drop=1` deletes `list_tb_7701/7702` and their rows; it refuses if one of them is playing.
- **Tested on the PC [V]** (`tests/pl_sql_test.py`, Python sqlite against a copy of `big.db`). The statements are extracted from `hibymod_disp.c` itself:
  - order is preserved;
  - a duplicate is discarded;
  - a nonexistent path is discarded or, with `unscanned=1`, inserted in its place;
  - the index is recomputed correctly (4th requested with one discarded before it: `index 2`);
  - `MANAGER_TABLE` ends up correct and `integrity_check ok`.
  - Plan: `SCAN r` + `AUTOMATIC COVERING INDEX (path=?)`, i.e. O(n + m), not O(n·m). 1000 tracks: 58 ms on the PC (on the device it will be tens or hundreds of ms [I]).

### Shell API (`src/playctl.inc.c`)

```c
int pc_play_list(const char *const *paths, int n, int index, const char *title, int mode); // blocks <=5 s
int pc_play_file(const char *path);                                    // list of 1
int pc_list_send(paths, n, index, title, mode);  int pc_list_poll(id, &res);  // non-blocking, for the UI
int pc_listpath_send(const char *listpath, int index);                 // already existing list
int pc_list_check_send(void);  int pc_list_drop_send(void);
// mode: -1/0xff leave unchanged | 0 in order | 1 repeat track | 2 shuffle | 3 repeat list ; | PC_UNSCANNED
// paths: /data/mnt/sd_0/... or a:\... ; "CUE|path" picks a track from a .cue (cue_id)
// returns rows (>=0) or <0: -ENXIO no library, -ETIMEDOUT, -ENOEXEC, -EBUSY, < -1000 = SQLite error
```

Tool: `bin/hibyplay`:

```
hibyplay list [-i N] [-t T] [-m M] [-u] F...
hibyplay listpath PATH N
hibyplay listpath check
hibyplay listpath drop
```

**Music app (recommended)**:

- **Album**: the album's paths in track order (from the database copy the app already reads) + the tapped index, with `pc_list_send` and `pc_list_poll` in the loop.
- **Playlist**: same.
- **Single song**: a list of 1, or better the album with that song's index, as the stock UI does.

## 4. Known limits and risks

- **Duplicates**: the key `(id, path, cue_id)` prevents repeating the same track in a queue. They are discarded, and `rows`/`index` reflect it.
- **Unscanned tracks**: discarded by default; with `-u` they are inserted with minimal data (name = path, `format=0`). I don't know whether the player then derives the format from the extension [I].
- **Transaction on the player's connection**: it lasts as long as the INSERT (with `db_lock` held, so the UI waits and nothing interleaves). If another of its threads used the database without `db_lock` and opened its own `BEGIN` right then, its transaction would fail [I, unlikely: all the code seen uses `ctx+0x564`]. If ours collides (transaction already open), we return `-EBUSY` without touching anything.
- **Stock cleanup**: there is code that drops `list_tb_*` tables (`0x714800`, `DROP TABLE IF EXISTS %s%d`) [I: I have not seen when it runs, probably when rebuilding the database]. If it dropped ours, the next track would fail as if the file had been deleted; the fix is to send the queue again.
- **Stock "current list" view** with path `hibymod\q1`: I don't know which title or view type it picks [I]. Tested in §7.
- **`pos=`** (start at a given point): `play_index` accepts it, but the unit of setting `0x1f` (ms?) is not verified [I]. Default -1.
- **`mode`** changes a persistent user setting, just like its menu. It is only touched when requested.
- `b:`/`c:` (sd_1, USB) have not been verified; `a:\…` paths have.

## 5. Library-free alternative: HiBy Link client (not implemented)

`hl_wifi_server_init` (`0x748f30`) and the BR/EDR and BLE servers expose `playUuid(list, "<path>…")`, `playTrackUuid`, `setPlayMode`, `getPlaylistItems`… The list "uuid" is the same internal path (`0x455c20`/`0x45b780`: types 1–0x15, `d`+`data\playlist\X.m3u`, `album`, `artist`…). It would require:

- reimplementing the transport (`link_*`, `msg_*`, CRC16 `hl_crc16_calc`);
- having the Wi‑Fi server enabled (a setting);
- and even then the list would have to exist in the database.

More work and more fragile than v6. Only useful if LD_PRELOAD ever becomes unusable.

## 6. Files

| File | What |
|---|---|
| `src/hibymod_disp.c` | v6: final section "play a list" + `'L'` in `control()` (without the display mutex) |
| `src/hm_shared.h` | only the comment for command `'L'` (the struct does not change) |
| `src/playctl.inc.c` | `pc_play_list`, `pc_play_file`, `pc_list_send/poll/wait`, `pc_listpath_send`, check/drop |
| `src/hibyplay.c` | `list`, `listpath` (including `check`/`drop`) |
| `bin/hibymod_disp.so` = `bin/hibymod_disp_v6test.so` | compiled v6 (md5 `dd921b08c6d0a750b937bc8f27becd5a`), **not installed**: v5 + command `'L'` + tearing fix (§8) |
| `bin/hibyplay` | test tool (static musl) |

## 7. Test plan (with the user's permission; requires a reboot)

`adb` = `%LOCALAPPDATA%\platform-tools\adb.exe -s "HiBy R3II_2025"`. Before starting, the user should pause the music.

**P0. Backups (no risk)**

```
adb shell "mkdir -p /usr/data/hibymod/t; cp /usr/data/usrlocal_media.db /usr/data/hibymod/t/media_antes_v6.db"
adb shell "ls /usr/data/hibymod/preload/; md5sum /usr/data/hibymod/preload/*.so"
adb shell "cp /usr/data/hibymod/preload/hibymod_disp.so /usr/data/hibymod/t/hibymod_disp.so.bak"
adb push bin/hibymod_disp_v6test.so /usr/data/hibymod/t/
adb push bin/hibyplay /usr/data/hibymod/t/ ; adb shell chmod 755 /usr/data/hibymod/t/hibyplay
```

**P1. Install and reboot (with permission)**

```
adb shell "cp /usr/data/hibymod/t/hibymod_disp_v6test.so /usr/data/hibymod/preload/hibymod_disp.so && sync && reboot"
```

**Caution:** v6 includes v5, which has not been tested on the device yet either (the device runs v4).

- What v5 brings: file and PCM logging, and the PCM thread at `SCHED_FIFO`; the latter is disabled with `touch /usr/data/hibymod/no_rt`.
- `bin/hibymod_disp.so` is already this same v6; there is no separate v5 binary left. To separate the risks, v5 has to be rebuilt from an earlier copy of the source.
- v6 startup is identical to v5's. The new code only runs when `'L'` is received, and if it failed, the player would crash and the device would restart (`hiby_player.sh`) with nothing sending `'L'` again.

After boot, check that everything from before still works the same: shell, gestures, Now Playing (`hibyplay status`) and `cat /usr/data/hibymod/disp.log`.

**P2. Side-effect-free check**

```
adb shell /usr/data/hibymod/t/hibyplay listpath check
```

- Expected: `rc=0 … firmas ok; modo=M lista='album_artist\ABBA' indice=K total=18` (literal output, Spanish: "signatures ok; mode=… list=… index=…"). Note down `M`, the list and the index to restore them.
- If it prints `rc=-8` (ENOEXEC), the firmware is different: stop here.

**P3. Existing stock list, writing nothing**

```
adb shell "/usr/data/hibymod/t/hibyplay listpath 'album_artist\ABBA' 3"   # (the path reported by P2)
```

- Expected: track 4 of that list plays.
- Check `hibyplay status` (path = 4th row of `list_tb_1`), the stock Now Playing and the player screen (capture with `C`).
- Then `hibyplay next` → the 5th and `hibyplay prev` → the 4th.
- If nothing plays: `cat /tmp/hibymod_playres` and `disp.log`.

**P4. Own queue (writes only to `list_tb_7701` + `MANAGER_TABLE`)**

Pick 3 paths from two different albums taken from `MEDIA_TABLE` (from the P0 copy):

```
adb shell "/usr/data/hibymod/t/hibyplay list -i 1 -t Test /data/mnt/sd_0/A/B/01.flac /data/mnt/sd_0/A/B/02.flac /data/mnt/sd_0/C/D/05.flac"
```

- Expected: `rc=0 rows=3 index=1 slot=1 ms=<…>` and `02.flac` plays.
- `next` → `05.flac`; `next` at the end: depends on the mode (in order it stops, repeat list goes back to the 1st).
- `prev` → `02.flac`.
- Open the stock "current list" from its Now Playing: does it show 3 items, and with what title? Capture it.
- Verify on a copy:

  ```
  adb shell cp /usr/data/usrlocal_media.db /usr/data/hibymod/t/m_p4.db
  adb pull /usr/data/hibymod/t/m_p4.db
  ```

  On the PC: `SELECT * FROM MANAGER_TABLE`, `SELECT rowid,name FROM list_tb_7701` and `pragma integrity_check`.

**P5. Alternation and repetition**

- Send another queue → `slot=2`.
- One more → `slot=1`.
- Both times the requested track should play with no odd glitches.

**P6. Performance**

- A queue of about 500 tracks (for example, all the tracks of a big artist).
- Note down `ms=`.
- Meanwhile, scroll a stock list: it should not freeze for longer than `ms`.

**P7. Errors (must not change anything)**

- Nonexistent path → `rc=-5 … lista vacia` (literal output, "empty list").
- Without `HMPL1` → `rc=-22`.
- Two requests in a row without waiting → the second is ignored (`pl: ocupado`, i.e. "busy", in `disp.log`) and its `hibyplay` ends with `-ETIMEDOUT`. The shell must wait for the response before sending another.

**P8. (Optional, changes a user setting) Shuffle**

- `hibyplay list -m 2 …` → `next` jumps randomly.
- Afterwards restore mode `M` from P2 with `hibyplay list -m M …`, or from its menu.

**P9. (Optional) Resume**

With one of our queues playing, reboot. On startup, does it resume our queue (`check` → `lista='hibymod\q1'`)?

**Rollback**

1. Play any stock list.
2. `hibyplay listpath drop` (deletes 7701/7702 and their rows).
3. `adb shell "cp /usr/data/hibymod/t/hibymod_disp.so.bak /usr/data/hibymod/preload/hibymod_disp.so && sync && reboot"`.

The original database stays in `t/media_antes_v6.db`. **Do not restore it live**: only with the player stopped, and only if really needed.

**What to record at each step**:

- the output of `hibyplay` and `/tmp/hibymod_playres`;
- the `pl:` lines in `disp.log`;
- `hibyplay status`;
- captures of the stock Now Playing.

## 8. Extra in the same build: tearing when presenting the shell

There is tearing in transitions in normal mode.

**Cause.** The ST77903 panel has no GRAM: the DPU scans the buffer in DDR continuously (`research_60fps.md` §1.4). Commands `'P'`, `'R'` and `'O'` wrote to the **visible** buffer (`buf_at(last_var.yoffset)`) with the scan at any point, and then panned, which only waits for end of frame. The tear line ended up wherever the scan happened to be.

**Fix** (`frame_edge()` in `hibymod_disp.c`):

- **Order.** First `frame_edge()`: a pan of the same buffer, which sleeps until the end-of-frame interrupt. Then the copy is done top to bottom, with no second pan.
  - The full copy is 300 KB, on the order of 1 ms; the dirty-rows copy, less.
  - It starts in the vblank (27 lines, about 0.8 ms) and is much faster than the scan (22.6 ms per screen), so it always stays ahead of it [I: wake-up latency still to be measured].
  - The pan was used instead of `FBIO_WAITFORVSYNC` because its wait is already verified in the driver (`lcdc_pan_display`) and no extra ioctl is needed.
- **`'P'`.** The sequence lock `fr0_seq` is checked after the edge; it is almost always even, so it does not delay the copy.
- **`'R'`** (capture exit). `copy_then_ov()` copies the rows down to the end of the overlay, applies the overlay and then copies the rest. No overlay row is scanned without it.
- **`'O'`**. After the edge, `ov_restore` + `ov_apply` back to back (tens of µs). No frame is shown with the overlay half removed.
- **Dead shell** (`input_thread`). Same rule.
- **Unchanged**: the player's pan hook: there it writes to the buffer that *is about to* be presented, which is not visible yet. That is correct.

**Latency.** Before: copy, then wait for end of frame (shown in the scan in progress, with a tear). Now: wait, then copy (shown whole in the next scan). Each `'P'` still blocks at most one period, so the cadence does not change [I].

**Why this does not repeat the old failure.** The reverted "vsync then write" attempt assumed the panel had GRAM: it wrote after a pan that had already started the transfer. Here there is no transfer: the write happens in the vblank, just before the scan that will display it.

**Measure on the device** (once installed):

- `hibyctl pantime 40` and `hibyctl fps` during an animation: they should stay at about 44 fps, with no extra dropped frames.
- Recording (`rec`) of a transition, frame by frame: no tear line.
- Open and close the volume popup (`'O'`): no overlay flicker.
