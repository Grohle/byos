# Now playing and playback control (HiBy R3 II, hiby_player)

Date: 2026-11-10. **[V]** = verified on the device or in the binary; **[I]** = inferred, not tested.

## Summary

| Need | Recommended solution | Status |
|---|---|---|
| Current track (path, title, artist, album, duration) | `hibymod_disp.so` v5 records the audio files the player opens; `np_get()` interprets them and reads the tags with hibytag | Code done and tested on the PC; on the device it activates on reboot |
| Playing or paused? Position | v5 counts the frames written to the PCM (tinyalsa or libasound) | Same |
| The same **without** v5 (the one installed now) | `/proc/<pid>/fd` + `fdinfo` (decoder read position) | **Already works** [V] |
| 128×128 RGB565 cover art | `np_cover()`: embedded image (FLAC/ID3/M4A) or the folder one → `hibywall` → cache | **Already works** [V] (0.28 s uncached, <5 ms cached) |
| Play/pause, next, previous | Inject the key into the evdev of the physical button (`playctl.inc.c`) | Play/pause **[V]**; next and previous [I], with the same mechanism |
| Play a file, album or playlist from an index | There is no external interface. Internal solution: library v6, command `'L'` → `research_play_list.md` | RE done; code ready, still to be tested on the device |
| Jump to a position (seek) | There is no key. Long press of NEXT/PREV [I], or RE (§5) | Pending |

## 1. What the player exposes (and what it doesn't)

- **D-Bus [V]**: on the system bus there are only `org.bluez`, `org.bluealsa`, `sys_server` (:1.0) and `bt-agent`. hiby_player is **not** on D-Bus. The bus policy rejects even `Introspect` towards sys_server and bt-agent.
- **MPRIS [V]**: sys_server registers an `org.mpris.MediaPlayer2.Player` with bluez at `/org/bluez/hiby/player`, but **only with properties** (`PlaybackStatus`, `Position`, `Metadata`) and only for AVRCP with BT headphones. The data is sent by the player (`BT:METADATA`, `BT:PLAYER_STATUS`, `BT:POSITION`). It cannot be used for control and only exists while BT is connected.
- **Unix sockets [V]**:
  - `/var/run/sys_server` and `/var/run/sys_client` (datagrams): system commands (`WIFI:*`, `BT:*`, `MOUNT:*`, `DLNA:*`, `INPUT:ADD`…). There are no playback commands. `BT:CONTROL:%s %s %d` goes from the player to sys_server, to be sent to a BT *sink*.
  - `/var/run/airplay_socket_server` and `airplay_meta_server`: audio and metadata from shairport to the player.
  - **`/data/dmr_streamer`** (player-side server, for `dmrd`): accepts `play`, `pause`, `stop`, `seek`, `next`, `previous`, `set_uri`, `set_meta`, `get_position_info`, `set_volume`… (table at `0x920998`, handlers at `0x453a60`–`0x4551e0`).
    - Every command is ignored unless setting no. 0x35 (`0x42a500(0x35)`, DLNA enabled) equals 1.
    - `set_uri` enters DLNA mode (view `lg_dlna`): there is no queue and no normal screen, and next/prev are forwarded to dmrd.
    - **Not recommended** (it requires changing a setting and leaves the player in a different mode), but it is the only "play this URI" route that exists. Not tested.
- **Files [V]**:
  - `/usr/data/user.ini` (binary, rewritten on every track or state change) stores at `0x28` the UTF‑16 path of the last track (`?:\Artist\Album\track.flac`, with the drive letter set to 0).
  - `momery_list.lst` stores the playlist position.
  - The player does not re-read either of them at runtime: they are only useful for reading.
- **Keys [V]**:

  | Device | Keys |
  |---|---|
  | `md-gpio-keys` (event0) | 116 (power) and 165 (`PREVIOUSSONG`) |
  | `jz adc keyboard` (event2) | 163 (`NEXTSONG`) and 164 (`PLAYPAUSE`) |
  | `sa-ring-keys` (event3) | volume wheel, 105/106 |
  | `earpods_adc` (event4) | 163/164/165/168/208/114/115 |

  The player keeps event0/2/3/4 open (and listens for `INPUT:ADD` for new devices).
- **strace [V]** (available at `/usr/bin/strace`). On pressing play:
  1. `lg_music_player` (tid 1021) opens the track twice: a probe of about 0.4 s and then **the decoder fd, which stays open while the track is loaded, also when paused**.
  2. `album_cover_par` (1072) opens it once more for the cover art.
  3. `ot device` (993) opens `/dev/snd/pcmC0D0p`.
  4. **`HiBy_Main_Thread` (1022) writes the audio with `ioctl(SNDRV_PCM_IOCTL_WRITEI_FRAMES)`** (its own tinyalsa; it is libc's ioctl, which the library already intercepts).

  **When paused [V]**: the PCM and the file remain open, but there are no further writes. Before the first play after boot, nothing is open.

## 2. Control: key injection (`src/playctl.inc.c`)

A `write()` of a `struct input_event` to `/dev/input/eventN` injects the event into that device (`evdev_write` → `input_inject_event`), and the kernel delivers it to **everyone** that has it open: the player receives it as if the button had been pressed.

- No uinput or library is needed.
- It works with the screen off and in any view.
- It is the same thing `player_pause()` in `store.inc.c` already does.

**[V] 2026-11-10**: `KEY_PLAYPAUSE` on "jz adc keyboard" started playback and, when repeated, paused it. Checked with strace: 0 writes to the PCM in 2 s. It was left paused, as it was before.

API:

- `pc_toggle()`, `pc_next()` and `pc_prev()`. They look for the physical device that has the key (`EVIOCGBIT`), skipping virtual ones.
- `pc_play()` and `pc_pause()`: only press if needed, by checking `np_get()`.
- `pc_key(code, hold_ms)`: long press, to test fast-forward [I].
- `pc_play_file()` returns `-ENOSYS` (see §5).

Limitation: it is a "button" control. The track or position cannot be chosen.

## 3. Now playing: library v5 + `src/nowplaying.inc.c`

### Library side (`src/hibymod_disp.c` v5; `bin/hibymod_disp.so` built, **not deployed**)

- **New fields at the end of `struct hm_state`** (`hm_shared.h`). `HM_MAGIC` does not change, and there is also a dedicated `np_magic = HM_NP_MAGIC`: an old shell does not see the new fields, and an old library leaves `np_magic = 0`. A `_Static_assert` checks that it fits in 64 KB.
- **`np_open[8]`**: every opening of an audio file by the player, whether it comes from `open`/`open64`/`openat`/`openat64`/`fopen`/`fopen64`:
  - what is stored: path, fd, tid and open and close ms (from `close`/`fclose`), PCM frames at open time, and the last `lseek`/`lseek64` (offset, ms and frames);
  - when reusing a slot, the oldest closed one is chosen, so quick cover-art reads while browsing do not evict the decoder;
  - everything is protected by the `np_seq` sequence lock and its own mutex, **never the display one**.
- **PCM**:
  - on the `/dev/snd/pcmC*p` fd: `WRITEI`/`WRITEN_FRAMES` (frames, `pcm_last_ms`), `HW_PARAMS` (rate, bits, channels), `DROP`, `PREPARE` and `PAUSE`;
  - in addition, libasound's `snd_pcm_writei`, `snd_pcm_hw_params` and `snd_pcm_drop` are wrapped (Bluetooth via bluealsa and USB DAC). A per-thread flag avoids double-counting libasound's internal ioctls.
- **Cost**: a few fd comparisons per call, with no locking on the normal path.

### Shell side (`np_get`)

- **Track** = the oldest open audio file that has been open for more than 600 ms and whose fd is still alive in `/proc/<pid>/fd` (this protects against closes that go unseen).
  - With gapless, the next track is opened before the current one ends; it switches to it when the decoder closes the previous one, a few seconds before it is heard [I].
  - If none is open: the last one that was open for more than 1.5 s (`stopped = 1`).
- **Playing** = PCM open, not paused, and written to less than 400 ms ago.
- **Position** = frames written since the track was opened ÷ sample rate. It runs ahead by at most the size of the PCM buffer.
  - A seek is detected as a decoder `lseek` within ±1.5 s of a `DROP`. From that point, position = bytes → time, using the audio region read from the header, and then frames again.
  - It is approximate for VBR [I, not tested on the device].
- **Without v5** (`NP_SRC_PROC`, what is installed now):
  - the track comes from `/proc/<pid>/fd` (the audio file with the lowest fd);
  - "playing" if `fdinfo pos` advanced within the last 2.5 s (the decoder reads in bursts);
  - the position comes from bytes → time, minus 2 s.
- **Just powered on** (`NP_SRC_LAST`): the path from `user.ini`, with `playing = 0`.
- **Tags**:
  - hibytag's `ht_read`, if it was included earlier (in the shell it is included by `tageditor.inc.c`);
  - otherwise, the file name and the folders.
- **Duration**: FLAC STREAMINFO, MP3 Xing/Info/VBRI or CBR, M4A `mvhd`, WAV and DSF. Cached per track.
- **Cost**: with v5 it is a 3.5 KB `memcpy` plus a `readlink`. Measured on the device with the `/proc` source: 20 ms in total, including process startup.
- **Recommendation**: call every 500 ms while the Control Center is open and every 1–2 s for the widgets; request the cover art when `track_id` changes.

### Cover art (`np_cover(path, out[128*128])`)

1. Looks for the embedded cover (FLAC PICTURE type 3, ID3v2.2/2.3/2.4 APIC/PIC, M4A `covr`). If there is none, the folder one (`cover`, `folder`, `front`… as `.jpg`/`.png`).
2. Copies the bytes to `cache/np/tmp.img` in 16 KB blocks.
3. Runs `hibywall IMG RAW 128 128`. libjpeg downscales while decoding and runs in another process, so the shell's memory does not grow.
4. Saves the result to `/usr/data/hibymod/cache/np/<hash(folder, size)>.raw` (32 KB): the next track from the same album comes from the cache.

hibytag did not read images (only `ht_set_cover` writes them); the locator lives in `nowplaying.inc.c`.

**[V]**: the cover art of "La ley innata" comes out correct. 0.28 s the first time, 0.00 s from the cache. Since it can take up to 0.5 s uncached, it must be called from a thread or on track change, never inside an animation.

### Test tool: `bin/hibyplay` (`src/hibyplay.c`, `tools/build_hibyplay.sh`)

```
hibyplay status [-w MS]    # source, PLAYING/paused, position/duration, path, tags
hibyplay raw               # dump of hm_state.np_* (v5), including rt_tid
hibyplay toggle|play|pause|next|prev
hibyplay cover [PATH] OUT.raw
```

Tested on the PC with a fake `hiby_player` and the library built for x86: probe + decoder + `fopen`/`fclose`, track change, stop and FLAC/MP3 cover art. On the device, with v4 installed: `status` and `cover` [V].

## 4. Audio protection (extra request): PCM thread at SCHED_FIFO

- **What it does [I until rebooting with v5]**: the first time a thread writes to the PCM (tinyalsa's WRITEI ioctl or `snd_pcm_writei`), the library switches it to `SCHED_FIFO` with priority 10 (`pthread_setschedparam`).
  - In this firmware that thread is `HiBy_Main_Thread` (tid 1022 in this session) [V].
  - It writes a line to `/usr/data/hibymod/disp.log` and sets `rt_tid`/`rt_prio`/`rt_err` in `hm_state`.
  - It is disabled by creating `/usr/data/hibymod/no_rt`.
- **Why it is safe**: the thread spends almost all its time blocked waiting for DMA. FIFO only gives it priority on wake-up; it does not give it more CPU.
  - The kernel has the real-time throttle enabled: `sched_rt_runtime_us = 950000` / `period = 1000000` [V]. Even if the thread ran away, the UI would keep at least 5 %.
  - Today all the player's threads are `SCHED_OTHER`, nice 0 [V].
- **The decoder (`lg_music_player`) is left alone**: without measurements there is no evidence it is needed, and giving real-time priority to a CPU-consuming thread could indeed steal CPU from the UI. If measurements showed it were needed, the prudent option is `nice -5` (`setpriority`), not FIFO.
- **How to check it after rebooting**:
  1. `cat /usr/data/hibymod/disp.log`, which must say `rt: hilo N (HiBy_Main_Threa) SCHED_FIFO 10: ok` (the literal log line; "hilo" = thread).
  2. `hibyplay raw`.
  3. `chrt -p N`, which must report `SCHED_FIFO`, priority 10.
  4. Or field 41 of `/proc/<pid>/task/N/stat`, which must be 1. Careful: names with spaces shift the fields, so `chrt` is better.
  5. Test for dropouts: a UI recording at 44 Hz with 24/192 music playing.

## 5. Playing a file, an album or a playlist: options

> **Update**: option 1 has now been researched and implemented (library v6, command `'L'`, `pc_play_list()`). See `research_play_list.md`.

There is no external command for this (§1). Options, from most to least recommended:

1. **Internal call from the library, on the UI thread** [I, recommended for the Music app].
   - The library already runs inside the player and has an entry point on its presentation thread (`ioctl FBIOPAN`), where a command queued in `hm_state` can be executed.
   - What remains is to locate, through RE, the function that the file browser or the song list uses when a track is tapped, i.e. "load this list and start at index i". Leads for finding it:
     - the message dispatcher `0x4368e0(id, buf, 0)`, which the DLNA `play` handler uses with id 0x15;
     - the strings `lg_music_player`, `hl_media_list_get_playlist`, `SELECT rowid FROM list_tb_%d WHERE path = ?`, `<path><cue_id>` and `open Y:\music_list.lst fail`.
   - The queue lives in `list_tb_1` of `usrlocal_media.db`. Writing it directly is not enough: the player keeps it cached.
   - Estimated cost: 2–4 days of RE + testing, with a reboot per iteration.
2. **Automate the stock UI under the capture** [I]: the shell sets `C` (the panel shows its image while the player keeps drawing underneath) and navigates File browser → folder → track with injected taps. It works without RE, but it is fragile (positions, language, timing) and takes 1–2 s.
3. **DLNA `set_uri`** over `/data/dmr_streamer`: requires enabling DLNA (a setting) and leaves the player in DLNA mode, with no queue. Discarded.
4. **Own engine (MPD)**: see `research_mpd_player.md`. Discarded for now.

## 6. Recommended architecture

```
hiby_player ── hibymod_disp.so v5 ──► /dev/shm/hibymod_fbstate (np_open[], pcm_*, rt_*)
                                              │ mmap (read-only)
hibyshell ── nowplaying.inc.c: np_get() every 0.5–1 s ──► CC "Now Playing", widgets, mini-player
          ├─ np_cover() on track_id change (thread) ──► 128×128 cache in /usr/data/hibymod/cache/np
          └─ playctl.inc.c: evdev keys ──► player (play/pause/next/prev)
Music app (album grid): own index reading a COPY of usrlocal_media.db;
          "play album" = option 1 of §5 once available (meanwhile, option 2)
```

**Integration into the shell (without touching other files in this task)**: `#include "nowplaying.inc.c"` and `#include "playctl.inc.c"` after `tageditor.inc.c`. Names use the `np_`/`pc_`/`NP_` prefixes.

**Library deployment**: copy `bin/hibymod_disp.so` to `/usr/data/hibymod/preload/` and reboot. Until then `np_get` uses the `/proc` source, which already works.
