# Seek, "play next" / queue, and absolute volume in the stock player (hiby_player)

Date: 2026-11-10. **[V]** = verified statically in the binary (disassembly, jump tables, data flow) or on the PC; **[I]** = inferred, not observed on the device. **Nothing here has been run on the device**: §7 is the test plan.

Binary: `rootfs/usr/bin/hiby_player`, md5 `d1dea413a91ca3c6759c9029744bc52c` (MIPS32r2 `EXEC`, base `0x400000`, fixed addresses). Continues `research_play_list.md` (command `'L'`) and `research_settings_api.md` (command `'S'`), whose guard design is reused. `P = 0x969ac8` is the player state.

## 0. Summary

| Question | Answer |
|---|---|
| Seek to an absolute position? | **Yes [V].** `seek_locked(ms)` `0x437780` = player lock + `0x4376a0(ms)` + unlock. `0x4376a0` is what the stock progress-bar drag (`0x50c924`), the stock fast-forward keys (`0x510004`/`0x510224`) and HiBy Link `setProgress` (`0x457778`, from its own thread) call. The DLNA seek (player message 3) calls `0x437780`. |
| Unit | **Milliseconds, absolute in the file [V]**: DLNA multiplies seconds by 1000 (`0x4541dc`: ×125×8); the stock UI adds the track start `get_param(0xd)` (non-zero only for `.cue` tracks); the position event handler throttles at 1000 (`0x434908`). |
| Read position / duration? | **Yes [V]**, exactly like the stock progress bar (`0x510040`): `pos = get_param(6) − get_param(0xd)`, `dur = get_param(0xe) − get_param(0xd)`. `get_param(6)` = `P+0x20`, updated by the engine position events (`0x434914`); `get_param(0xe)` = `P+0xe8` = the item's `end_time`, or the decoder duration (`P+0x5c`) when `end_time` is −1 (every non-cue track in the library) (`0x4388e0..0x438900`). State: `0x437260()` → 0 paused, 1 playing, 2 stopped. |
| Insert "play next" / append without restarting? | **Yes, for our own queues [V static + V PC SQL, I on device].** The player reads track *i* from `list_tb_<id>` **each time** it needs one (`0x4341c0` → `0x71de60`, no cache) and the next index is `cur+1` over `n = P+0x2c` (`0x435fc0`). For `hibymod\q1/q2` (rowid order) the library inserts the row after the current one (shifting the rest) and then `set_param(8, n+1)`. Nothing is interrupted. |
| Shuffle | The order is a vector of `n` `{index, played}` pairs at `P+0xec` (size `P+0xf4`). Changing `n` without rebuilding it would make the next-track walk read past the vector, so **in shuffle mode insertion is refused unless `reshuffle=1`**, which rebuilds it with `0x435ce0(n+1, 0)` under the player lock (the stock net-radio code does `set(8)` + `0x435ce0` too, `0x4ce534..0x4ce554`) and puts the new track right after the current one. Play history of the shuffle cycle is lost. |
| Current list is a stock list (album, playlist…) | Not modified in place (`rc=-ENOTSUP`). With `rebuild=1`: the list is copied to `hibymod\q1` + the new track, verified against the player's own reader at up to 6 positions, and playback continues at the same index and position (`play_index(i, pos_ms)` + a fallback seek). **Short audible gap.** |
| Absolute volume | **Yes [V]**, the HiBy Link `setVolume` sequence already used by `'S'` (`volume_apply(v)` `0x47d0c0` + `setting_set(0x11, v)`), max `setting_get(0x10)`, refused while line out is on. Rate-limited: ≥50 ms between writes in the library, ≥100 ms in the shell helper (each write rewrites `user.ini`). |
| Implemented | Library **v8**, FIFO command **`'Q'`** (inert until used), shell API in `src/playctl.inc.c`, CLI `hibyplay pos|seek|next-add|append|vol|qcheck`. Built in `out/agent_seek/`. |

## 1. Seek

### 1.1 Functions [V]

```
00437780 seek_locked(ms):  lock = P+0x48; 0x4376a0(ms); unlock = P+0x4c
004376a0 seek(ms):         if (!P+0x18) return          ; no decoder open
                           BT AVRCP "stopped"/"playing" + position (0x491aa0/0x491ae0)
                           0x969a7c = ms ; P+0xf8 = 0 ; if list item is http: P+0xba8 = 1
                           0x43dda0(ms)                  ; engine command 0x205 (seek) via 0x675564
                           0x47b5a0(10)                  ; 10 ms sleep
                           P+0x20 = ms                   ; position seen by get_param(6)
00437420 track_active():   lock; P+0x18 && (engine state == playing || P+0xfc == 0); unlock
00437260 play_state():     lock; !P+0x18 -> 2; engine state 1 -> 1; P+0xfc ? 2 : 0; unlock
```

Callers of `0x4376a0`/`0x437780` [V]:

| Address | Who | Notes |
|---|---|---|
| `0x50c924` | stock Now Playing progress-bar drag | gated by `track_active()`; `seek(rel + get_param(0xd))`; unlocked, UI thread |
| `0x510004`, `0x510224` | stock fast-forward / rewind keys | steps of 3000 ms, clamps to `[0, dur]` |
| `0x457778` | HiBy Link `setProgress(v)` | `seek(v + P+0x51c)` from the HiBy Link thread, unlocked |
| `0x439a68` | player message 3 (posted by DLNA `seek@%d`, `0x4541f4`) | `set_param(0x13,0,0)`; `seek_locked(ms)`; `0x437340()` = resume play |
| `0x5060c8`, `0x4399c4` | "previous" restarts the track when > 10 s in; A-B / bookmark paths | `seek_locked(begin)` |

### 1.2 Choice

The library calls **`seek_locked(begin + ms)` from its own thread**: same callee as the UI and HiBy Link, plus the player lock (which the player itself takes around it for message 3). It does **not** post message 3, because that one also resumes playback (DLNA semantics). Whether a seek while paused stays paused is [I] (the stock drag works while paused, gated by the same `track_active()`).

Guards in the library: nothing loaded (`track_active()==0`) → `-ENODATA`; list path `dlna…` → `-ENOTSUP` (dmrd drives the engine); unknown duration (radio/stream, `dur ≤ 0`) → `-ENOTSUP`; `ms < 0` or `> dur` → `-ERANGE`; `ms` within the last second is clipped to `dur − 1000` (avoids an immediate end-of-track).

### 1.3 Position

`op=pos` returns `pos`, `dur`, `state`, `index` (`get_param(0xb)`), `count` (`get_param(8)` without recompute). `P+0x20` is pushed by the engine; HiBy Link is notified at most once per second (`0x434900`), but `P+0x20` itself is written on every position event [V], so polling every 250–500 ms from the UI is fine. The existing `np_get()` (PCM frame counting, `nowplaying.inc.c`) stays the cheap no-IPC source; `pc_position()` is the authoritative one (cue offsets, true duration).

## 2. "Play next" / add to queue

### 2.1 How the player picks the next track [V]

- Next / natural end → player message 2 → `0x439340`: under the player lock `idx = 0x435fc0(flags, +1, &ok)`, then `0x4341c0(idx, &item)` reads the item, then `0x437ba0` plays it.
- `0x435fc0`: `n = P+0x2c`, `cur = P+0x34`, `mode = P+0x24`. Order (0): `cur+1` (end of list → stop / folder logic `0x4338a0`/`0x433da0`). Loop list (3): `(cur+1) % n`. Loop track (1): `cur`. Shuffle (2): walks the `{index, played}` vector (`P+0xec`, `P+0xf0` = position of the current one) to the next unplayed entry; when all are played it rebuilds.
- `0x4341c0` → for an ordinary list path `0x71de60(buf, idx, 1, 1, P+0x1654)` → `SELECT * FROM list_tb_<id> limit ? offset ?`. For a path that is not a drive path, not `album…` and not `music_search_…` there is **no ORDER BY** (`0x71ec60..0x71ecd4`), so the order is the rowid order. `hibymod\q1/q2` fall in that case [V]. Stock `album…` lists may use one of four `ORDER BY ctime/mtime` variants depending on a view sort setting (`ctx+0x500`, `0x71ec9c`).
- No list cache in this path: the row is read from SQLite every time [V]. The only "peek" of the next track is `get_param(0x1f)` (`0x436940`), used by the stock Now Playing UI to show "next song" (`0x50f0dc`, `0x511d94`).

### 2.2 In-place insertion (our queues) — implemented

```
db_lock; BEGIN
  count(*) of list_tb_770x must equal n (else abort: table and player disagree)
  R = rowid at offset at-1 (ORDER BY rowid)              ; at = cur+1 ("next") or n ("append")
  UPDATE ... SET rowid = -rowid WHERE rowid > R ; UPDATE ... SET rowid = 1 - rowid WHERE rowid < 0
  INSERT INTO list_tb_770x(rowid, cols) SELECT R+1, <MEDIA_TABLE row of the path>   (no OR IGNORE)
  count must be n+1 (else: not in the library -> abort)
  MANAGER_TABLE count = music_count = n+1
COMMIT; db_unlock
[shuffle + reshuffle=1]  player lock; 0x435ce0(n+1, 0); move the new index right after P+0xf0; unlock
set_param(8, n+1)        ; after the table, so the player never sees n+1 rows that do not exist
```

- The current index `P+0x34` does not move (the row goes after it). Between `COMMIT` and `set_param(8)` the player may still use `n` — harmless: the new row is already at `cur+1`.
- Shuffle: `0x435ce0` is called before `set_param(8)`, so the vector is never smaller than `n` (the walk uses `n` entries). It frees and reallocates the vector, hence the player lock around it (the player calls it under that lock too). Lock order is player → db everywhere (the player itself does the same in `0x4393ac` → `0x4341c0`); we never hold the db lock while taking the player lock.
- **Duplicates**: the table key is `(id, path, cue_id)`, so a track already in the queue gives `SQLITE_CONSTRAINT` → `-EEXIST`. With `dup=1` it is inserted with a synthetic id `−1000000−rowid` [I: whether anything else keys on `id` — play counts, favourites — is unverified; the 'L' unscanned rows already use negative ids].
- `unscanned=1`: a file not in `MEDIA_TABLE` goes in with minimal data (same as `'L'`).
- `.cue`: `path=N|a:\…\x.cue` picks `cue_id = N`; without `N|` the first cue track.

### 2.3 Stock lists — `-ENOTSUP`, or `rebuild=1`

Writing into the user's stock tables (`list_tb_1`, …) was ruled out: their order may come from an `ORDER BY` we cannot see, the stock UI owns them, and folder lists are re-created by the stock code. With `rebuild=1`:

1. Before touching the database, read up to 6 positions (`cur`, `cur±1`, `0`, `n/2`, `n−1`) **with the player's own reader** `0x4341c0` (under the player lock, outside the db lock).
2. In one transaction: copy `list_tb_<src>` (`src` from `MANAGER_TABLE`) to `list_tb_7701` `ORDER BY rowid`; require `n` rows; require each sampled position to have the same path in the copy (else abort: `"orden distinto…"`, nothing changes); insert the new track (§2.2); `MANAGER_TABLE` row for `hibymod\q1`.
3. Switch like `'L'`: `set_param 0x17/0x15/8/0x10` + `play_index(cur, pos, 0x10002)` with `pos = get_param(6)` (absolute ms). **`play_index`'s `pos` is the start position in absolute ms [V]**: at track open `P+0x20 = pos` (`0x4388dc`), and the power-on resume passes setting `0x1f`, which is saved from `P+0x20` (`0x436fc0`). Whether the `0x10002` flags honour it like the resume flags `0x40000` do is [I], so the library waits up to 2 s for the track and seeks if the position is more than 3 s behind.
4. The resulting queue is ours: further inserts are in place.

Virtual lists without a `MANAGER_TABLE` row (`all`, `format…`, drive/folder views that were never materialised) → `-ENOTSUP` "lista virtual". Shell fallback in that case: build the list itself and use `pc_list_send_pos(paths, n, index, title, mode, pos_ms)` (`'L'`, same mechanism).

### 2.4 What remains [I]

- **Gapless pre-open**: if the decoder of the next track is opened ahead of time near the end of the current one (not traced), an insertion in the last seconds may only take effect one track later.
- **Stock "current playlist" view** open in the hidden stock UI caches its own rows (`0x44b620`/`0x50a9e0`, see `research_play_list.md`): it would show stale content until reopened; playback is unaffected (the next-track path does not use it).
- **Loop-track mode (1)**: natural end repeats the current track, so "next" only matters for a manual next.

## 3. Volume

Same sequence as `'S'` (`research_settings_api.md` §2, `st_set`): refuse if `v > setting_get(0x10)` or line out (`setting_get(0x2d) == 1`, fixed level); no-op if unchanged; `volume_apply(v & 0xff)` (BT absolute volume / output device) then `setting_set(0x11, v)` (stores, notifies HiBy Link, per-output memory, **rewrites `/usr/data/user.ini`**). `op=volget` returns `val` and `max`. Rate limiting: the library sleeps so that two writes are ≥50 ms apart; `pc_volume_set_async()` + `pc_q_pump()` send only the latest value, at most one every 100 ms, and never while a request is in flight. The EU high-volume warning (`0x535de0`) is not shown (same as HiBy Link); the shell must enforce its own limit if wanted.

## 4. Implementation (library v8, command `'Q'`)

```
shell: pc_seek(ms) / pc_queue_next(path) / pc_volume_set(v) ...
   writes /tmp/hibymod_qreq (tmp + rename)   HMQ1 / id=N / op=pos|seek|next|append|vol|volget|check
                                             [ms=] [val=] [path=] [dup=1] [unscanned=1] [reshuffle=1] [rebuild=1]
   sends 'Q' to /tmp/hibymod_disp
library control thread: 'Q' -> qc_kick(): own detached thread (512 KB stack), no display mutex
   qc_check(): pl_check() (13 signatures of 'L' + /proc/self/maps + P lock + db context)
             + 7 functions x 2 windows of 16 bytes (entry + a distinctive window: several of these
               functions share the same first 32 bytes) + 12 get/set_param jump-table slots
             full check once; afterwards only the runtime state (P lock, db context) is re-checked
   vol/volget: also st_check() of 'S' (once; then the settings pointer must be unchanged)
   next/append also take the 'L' busy flag (never concurrent with an 'L' request)
   -> /tmp/hibymod_qres  "id=N rc=R op=OP pos= dur= state= index= count= val= max= ms=T msg"
   -> one line "qc: ..." in /usr/data/hibymod/disp.log
```

- **Inert** until a `'Q'` arrives. Other commands unchanged (diff vs v7 = additions only: the `'Q'` line in `control()`, a forward declaration, a header comment, and the final section).
- **Other binary → `rc=-8` (`-ENOEXEC`)**, nothing called; every fixed address is checked in `/proc/self/maps` before being read.
- **One request at a time**; extra `'Q'` while busy are coalesced (the thread re-reads the request file and skips one it already served — same inode/mtime/size), so the latest request is always served. Two *clients* at once are not supported (the second file can replace the first; the first then times out) — same as `'S'`.
- Errors: `-ENOEXEC`, `-EAGAIN` (player not initialised), `-ENODATA` (nothing loaded / no list), `-ENOTSUP` (DLNA, unknown duration, stock list without `rebuild`, order mismatch, virtual list), `-ERANGE`, `-EPERM` (shuffle without `reshuffle`; line out for volume), `-EEXIST` (already queued), `-ENOENT` (not in the library), `-EBUSY` (`'L'` running or open transaction), `-ENOSPC` (< 6 MB free on `/usr/data`), `< −1000` SQLite error.

### Shell API (`src/playctl.inc.c`)

```c
int pc_position(int *pos_ms, int *dur_ms, int *state);   // state 0 paused, 1 playing, 2 stopped
int pc_seek(int ms);                                     // 0 or <0
int pc_queue_next(const char *path);                     // new index (>=0) or <0
int pc_queue_append(const char *path);
int pc_queue_add(const char *path, int append, int flags);   // PC_Q_DUP | PC_Q_UNSCANNED | PC_Q_RESHUFFLE | PC_Q_REBUILD
int pc_volume_set(int v);  int pc_volume_get(int *v, int *max);
// non-blocking (UI loop): one request in flight
int pc_position_send(void), pc_seek_send(int ms), pc_queue_send(const char *path, int append, int flags),
    pc_volume_send(int v), pc_volume_get_send(void);            // -> id, or -EBUSY
void pc_seek_async(int ms), pc_volume_set_async(int v);         // latest value wins (slider drag)
int pc_q_pump(struct pc_q_res *r);   // call every frame: sends pending seek/volume, returns 1 on a response
int pc_q_poll(int id, struct pc_q_res *r);
```

Blocking calls wait ≤4 s (≤6 s for the reply itself). Paths: `/data/mnt/sd_0/…` or `a:\…`. Request ids include the pid, so two processes in the same second never accept each other's stale reply (found and fixed during the PC test).

UI recommendations: progress bar — `pc_position_send()` every 250–500 ms while visible (or `np_get()` between polls); on drag, update the knob locally and `pc_seek_async()` on release (or throttled while dragging); volume slider — `pc_volume_set_async()` on every move + `pc_q_pump()` each frame; "Play next" on our queues — `pc_queue_send(path, 0, 0)`; on `-EPERM` offer "reshuffle", on `-ENOTSUP` offer the rebuild (`PC_Q_REBUILD`, warn about the short gap).

### CLI (`hibyplay`)

```
hibyplay pos                         hibyplay seek 90000 | +10000 | -10000
hibyplay next-add [-d] [-u] [-r] [-b] PATH      hibyplay append [-d] [-u] [-r] [-b] PATH
hibyplay vol [N]                     hibyplay qcheck
```

### Files and builds

| File | What |
|---|---|
| `src/hibymod_disp.c` | v8: last section "posicion, cola y volumen ('Q')" + `'Q'` in `control()` |
| `src/playctl.inc.c` | appended: `pc_position`, `pc_seek`, `pc_queue_*`, `pc_volume_*`, async + `pc_q_pump` |
| `src/hibyplay.c` | `pos`, `seek`, `next-add`, `append`, `vol`, `qcheck` |
| `out/agent_seek/hibymod_disp.so` | v8 = v7 (`'S'`) + `'Q'`, md5 `4c2e4a9feafcb703f84d98e4d96c299a`, **not installed** |
| `out/agent_seek/hibyplay` | static musl CLI, md5 `3a1caa2678215eecd8c24147e2addca6` |

Build (WSL): `bash tools/wsl/zbuild_so.sh src/hibymod_disp.c out/agent_seek/hibymod_disp.so`; CLI with the `tools/build_hibyplay.sh` command line but `-o out/agent_seek/hibyplay`. Warnings: library with `-Wall -Wextra` (zig/clang and gcc) — identical set to v7 (only the pre-existing `fclose` nonnull one); CLI clean. `src/hm_shared.h` (not edited here) could get a one-line comment for `'Q'` like `'L'`/`'S'`.

## 5. Off-device tests done [V on PC]

- **Guard**: x86 build of the library preloaded into a process named `hiby_player`: `qcheck`, `pos`, `seek`, `vol`, `vol N`, `next-add`, `append` all answer `rc=-8 firmware distinto` without calling anything; malformed request → `rc=-22`; a burst of 8 concurrent clients does not crash or double-serve (6 answered, 2 time out — the documented single-client rule); `'L'` still answers; with no library the CLI returns `-ENXIO`.
- **SQL** (`qc_sql_test.py`, Python sqlite on a copy of `~/hiby/big.db`, statements extracted from `hibymod_disp.c` itself): next after index 1 lands at index 2 with the rest shifted; append; duplicate rejected with a constraint and the rollback restores the order; `dup=1` repeat; path not in the library → error + rollback; `unscanned=1`; rowids stay contiguous; `MANAGER_TABLE` count; rebuild copy of `list_tb_1` keeps the order, the order check finds the current track and detects a wrong index; `integrity_check ok`. (Script kept in the session scratchpad, not in the repo.)

## 6. Risks

- **Runs inside the audio player.** A wrong call stops playback (then `hiby_player.sh` restarts it). Mitigations: every called function and every jump-table slot is checked; nothing runs unless asked; all values are range-checked before any call.
- **Seek from a non-UI thread with the player lock**: same callee as HiBy Link (its own thread, unlocked) and the player's message loop (locked) [V precedent]. Engine-side races with a track change in progress are possible in theory [I]; the lock serialises with the player's own state changes.
- **Shuffle rebuild** resets the shuffle cycle (already-played tracks can come again). Only with `reshuffle=1`.
- **`dup=1` synthetic ids** [I] — see §2.2.
- **rebuild**: audible gap; resumes playback even if it was paused [I]; leaves the user's stock list untouched; uses slot 1 (`list_tb_7701`).
- **Database writes** (queue ops only) use the player's connection and lock in one transaction, like `'L'`; refused below 6 MB free (UBIFS GC stalls).
- **Volume** writes `user.ini` per change — rate-limited; `setting_set` is unlocked (same exposure as HiBy Link/`'S'`). Do not drive the same key from `'S'` and `'Q'` at the same time.

## 7. Device test plan (with the user's permission; safest first)

`adb` = `%LOCALAPPDATA%\platform-tools\adb.exe -s "HiBy R3II_2025"`, `T=/usr/data/hibymod/t`, `H=$T/hibyplay`. **Never open the browser on the device during these tests.** Music at low volume, headphones on (not line out). v8 includes v5–v7, also untested on the device: run `research_play_list.md` §7 P0–P3 and `research_settings_api.md` §7 P0–P2 first, or at least their backups.

**P0. Backups (no risk)**
```
adb shell "mkdir -p $T; cp /usr/data/usrlocal_media.db $T/media_before_v8.db; cp /usr/data/user.ini $T/user_before_v8.ini"
adb shell "md5sum /usr/data/hibymod/preload/*.so; cp /usr/data/hibymod/preload/hibymod_disp.so $T/hibymod_disp.so.bak"
adb push out/agent_seek/hibymod_disp.so $T/hibymod_disp_v8.so
adb push out/agent_seek/hibyplay $T/ ; adb shell chmod 755 $T/hibyplay
```

**P1. Install and reboot**
```
adb shell "cp $T/hibymod_disp_v8.so /usr/data/hibymod/preload/hibymod_disp.so && sync && reboot"
```
Check the shell, gestures, `$H status`, `cat /usr/data/hibymod/disp.log`.

**P2. No side effects**
```
adb shell $H qcheck        # expect rc=0 "firmas ok; modo=M lista='…' activa=…"; rc=-8: STOP (other firmware)
adb shell $H pos           # with music playing: pos increases between calls, dur = track length, state=1
adb shell $H vol           # val / max; compare with the stock volume display
```
Note `M` (play mode), the list and the volume to restore them. Compare `pos`/`dur` with `$H status` (PCM-based) — they should agree within ~1 s.

**P3. Volume**
`$H vol <cur−5>` → audibly lower, `$H vol` reads it back; `$H vol <cur>`. `$H vol 999` → `-ERANGE`. Loop 20 × `$H vol <cur>`/`<cur−1>` quickly: no stutter, ≤20 `qc:` lines/s in `disp.log`.

**P4. Seek (audible, harmless)**
1. Playing: `$H seek 60000` → jumps to 1:00; `$H pos` ≈ 60000. `$H seek +10000`, `$H seek -20000`.
2. `$H seek <dur>` → clipped to `dur−1000` (msg "recortado"); `$H seek <dur+5000>` → `-ERANGE`.
3. Paused (`$H pause`): `$H seek 30000` → does it stay paused? (record: [I] in §1.2). Then `$H play`.
4. A `.cue` track (if any): `pos` starts at 0 for each cue track; `seek 30000` lands 30 s into *that* track.
5. Stopped (after the end of a list): `$H seek 1000` → `-ENODATA`.
Record `ms=` and the `qc:` lines.

**P5. Queue on our own list (writes only `list_tb_7701/7702` + `MANAGER_TABLE`)**
1. `$H list -i 0 -t QTest A.flac B.flac C.flac` (three library tracks, `'L'`) → plays A, `qcheck` shows `lista='hibymod\q1'` (or q2).
2. `$H next-add D.flac` → `rc=0 index=1 count=4`, A keeps playing without a gap. `$H next` → D. `$H next` → B.
3. `$H append E.flac` → `index=4 count=5`. Let the list run to the end (or `next` ×3): E plays last.
4. `$H next-add B.flac` → `-EEXIST`; `$H next-add -d B.flac` → accepted; it plays next.
5. Not in the library: `$H next-add /data/mnt/sd_0/nope.flac` → `-ENOENT`.
6. Natural transition: insert during the last ~10 s of a track with gapless on and off; note whether the inserted track or the old next plays (§2.4).
7. Pull a db copy and check: `adb shell cp /usr/data/usrlocal_media.db $T/m_p5.db; adb pull $T/m_p5.db` → `SELECT rowid,name FROM list_tb_7701`, `SELECT * FROM MANAGER_TABLE WHERE id>7700`, `pragma integrity_check`.

**P6. Shuffle (changes nothing persistent by itself)**
1. `$H list -m 2 A B C D E` (shuffle, `'L'`). `$H next-add F.flac` → `-EPERM`.
2. `$H next-add -r F.flac` → `rc=0`; `$H next` → F; then a few `next`: no crash, no repeats beyond the documented reset.
3. Restore the play mode `M` from P2 (`hibysetctl set playmode M`, or `$H list -m M …`).

**P7. Stock list rebuild (audible gap; user's stock tables are only read)**
1. Play an album from the **stock** UI (or `$H listpath 'album_artist\ABBA' 3`). `$H next-add X.flac` → `-ENOTSUP "…no es una cola de hibymod"`.
2. `$H next-add -b X.flac` at ~1:00 into the track → short gap, same track continues near 1:00; `qcheck` → `lista='hibymod\q1'`; `$H next` → X; `$H next` → the album's next track.
3. Folder view (stock file browser) → expect `-ENOTSUP` "lista virtual" or "orden distinto" and **no change** to playback.

**P8. Errors / robustness**: `$H seek` with no list loaded; `$H next-add` while an `$H list` with 500 tracks is running (→ `-EBUSY` or timeout, no crash); 10 rapid `$H pos` in a loop (each waits) → all answered.

**Restore**
1. Play any stock list, then `$H listpath drop` (deletes `list_tb_7701/7702` and their rows; refuses while one of ours plays).
2. Restore volume / play mode with `$H vol <P2 value>` and `hibysetctl set playmode <M>`.
3. Library rollback: `adb shell "cp $T/hibymod_disp.so.bak /usr/data/hibymod/preload/hibymod_disp.so && sync && reboot"`.
4. Do **not** copy `media_before_v8.db` or `user_before_v8.ini` back while the player runs; only with it stopped, and only if really needed.

What to record at each step: the `hibyplay` output, `/tmp/hibymod_qres`, the `qc:` lines in `disp.log`, `$H status`, and screen captures of the stock Now Playing when relevant.

## 8. Side findings

- `play_index(i, pos, flags)`: `pos` = start position in **absolute ms** [V by data flow, §2.3] — this settles the "unit unverified" note on `pc_list_send_pos()` / `'L'` `pos=` (`research_play_list.md` §4); only the interaction with the `0x10002` flags remains [I].
- Duration of non-cue tracks comes from the decoder (`P+0x5c`), not from the library: `MEDIA_TABLE.end_time` is −1 for every non-cue track in the db copy (4976/4976) [V].
- `get_param(5)` returns `(P+0x20, P+0x1c)` = (position, decoder duration) in one call [V]; `get_param(0x1f)` peeks the next item without advancing [V].
