# Folder sync, smart library and backups: design

Status: design, 12 Nov 2026. Requirements: `docs/SYNC_LIBRARY_SPEC.md`. This document combines five research passes (sync, DB, metadata, playlists, backup) and one review into one plan. It builds on branch `apps/desktop-lib`:

- abdff2e: core library index, partial reads, cover rewriting and `SyncPlanner`;
- 47dc109: the byos-cli library, playlist, music sync and cover commands;
- 6ba7e85: the desktop Library and Music tabs.

Where the research passes disagreed, this document decides. The decisions are listed in §0.3 so reviewers can check them.

---

## 0. Scope, ground rules and reconciliations

### 0.1 What the owner asked for (summary of the spec)

1. **Link folders.** For each kind (music, video, audiobooks, ebooks), link a folder on the PC or phone to the player.
2. **Sync buttons.** One per kind, plus **Sync all**.
3. **Only-on-player policy.** Choose whether files that exist only on the player are kept or deleted, always after a preview.
4. **Library database.** The sync **builds the player's library DB on the PC or phone** (the file the hiby-mods database script produces) and sends it, so the player never has to scan.
5. **Library on the computer.** View and rebuild it, make playlists, and fix covers, lyrics, albums, genres and artists.
6. **Smart metadata.** Genres, artists, albums and lyrics are merged and harmonised by similarity, with optional online checks.
7. **Playlists both ways**, plus better generation (smart rules, radio, mixes, most played).
8. **Backup and restore** of all player data, with a toggle per kind and per item.

Constraint that drives several decisions: libraries of **tens of thousands of files**, and a player with 57 MB of RAM and about 24 MB free on `/usr/data`.

### 0.2 Ground rules (from the spec and the project memory)

- **Heavy work runs on the PC or phone.**
  - One Kotlin `:core` serves byos-cli (JVM) and the Android app.
  - The Node desktop app has no npm dependencies. It only runs `byos-cli` and renders its JSON-lines events.
- **No new third-party code without the owner's approval (§13).**
  - GPL code is never copied. Mutagen (GPL-2.0) is reference only.
  - hiby-mods `Update_Database.py` (MIT) is already credited.
- **Online services:** only MusicBrainz, Cover Art Archive and LRCLIB.
  - MusicBrainz: at most 1 request/s, UA `ByOS/<ver> (https://github.com/Grohle/hibyos)`.
  - Only artist, album and title strings and durations are sent.
  - Online use is off until the user turns it on.
- **Never delete without a preview.**
  - Nothing is deleted from the player without the preview sheet, or `--yes` in a script.
  - The default only-on-player policy is **Keep**.
  - Deletions are blocked automatically when the computer-side source looks unavailable or incomplete (§3.3.3).
- **The live DB is never written while the player runs.** The one exception is the optional single-row M3U registration (§6.5), which follows the existing hibymix precedent.
- **Test off the device first.** Everything is tested with the fake device, the CLI mock and the VM harness (`docs/VM_SHELL.md`) before any device run. The owner runs device checks, under the device rules; the browser is never launched.
- **UI text** in EN/FR/ES, iOS/iTunes-like. Public docs in English.

### 0.3 Decisions

| Conflict or open point | Decision |
|---|---|
| Caps bit 5 is UPDATE; ops 0x60/0x61 are STATUS; two passes both claimed 0x46 | New numbers are allocated in §8.1 |
| Who applies the staged DB and the restore at boot | **One applier, `hibytag apply-staged`** (hibytag already links SQLite). The display shim constructor only checks for `/usr/data/hibymod/sync/pending` and, if it is there, execs the applier with a time budget before `main()`. When the `sys/hiby_player.sh` override ships (docs/OTA.md), the call moves there with no other change. |
| Card manifest location | Per kind: `ByOS/sync/manifest-v2/<kind>.json`. The branch's v1 `ByOS/sync/manifest.json` is migrated once. |
| Identity of a source track | **`(link_id, rel_path)`** everywhere: sync state, meta overlay, playlist `TrackRef`. |
| Album artist default | Stock behaviour (empty). Filling it is a metadata *proposal*, never automatic. |
| Article stripping in sort keys | No. Stock order, as `LibraryBuilder` does today. |
| Track ids: branch keeps 1..N (reuses removed ids); research says never reuse | **Never reuse**, with gaps allowed, if the golden test and a device check show that stock tolerates gaps. Otherwise fall back to 1..N, and **delete** (not re-point) the user-table rows of removed tracks, so a favourite never points at a different song. |
| Large libraries | **SD-DB mode is the supported path above the internal-memory limit**, scheduled in M4/M5, not "later". |
| Metadata fixes vs card files | **DB first.** Card copies are rewritten only when a file is being sent anyway, or by an explicit, costed "Rewrite tags on the HiBy". |
| Backup transport | A device-made **bundle on the SD** (BACKUP_MAKE), fetched with GET. Restore is a bundle PUT to the SD and staged with RESTORE_STAGE. No `/usr/data` namespaces. |
| Playlist commands | Extend the existing `playlist-*` family. No parallel `pl-*`. |

### 0.4 Deliberate differences from hiby-mods `Update_Database.py`

The PC-built DB has the **stock scan's format**, which the script also aims at. The ways it differs from the script on purpose are:

| Script | ByOS builder | Why |
|---|---|---|
| Rebuilds ids from 1 on every run | Keeps ids and `ctime` per path | History, favourites, queue and "Recently added" survive |
| Fills SEARCH_TABLE with every row | Leaves it empty, as the stock scan does | Matches stock; less space. If a device check shows stock search needs it, it gets filled |
| Embeds 360 px art into the files on the card | Does not change card files during the DB build. Cover optimisation happens on the copy at sync time (branch default) | No rewrite of the user's files as a side effect |
| All M3U rows get the same id | Distinct ids | Correctness |
| Writes the DB in place on the SD (R3 Pro II layout) | Stages it and the device applies it at the next start, with a check and a rollback | The R3 II's live DB is internal; it cannot be swapped under a running player |

---

## 1. Architecture

```
            ┌──────────── Desktop (Node 20, no deps) ────────────┐   ┌──── Android (Compose) ────┐
            │ ui/media.js: Library (computer) · device tabs       │   │ connected mode screens:   │
            │ Sync · Library (HiBy) · Backup → server.mjs (ids)   │   │ Sync, Library health,     │
            └──────────────┬──────────────────────────────────────┘   │ Playlists, Backup         │
                           │ spawn, JSON lines (BYOS_CLI.md §2)        └────────────┬──────────────┘
                     ┌─────▼─────┐                                                  │ in-process
                     │ byos-cli  │  (JVM, JDK 17)                                   │
                     └─────┬─────┘                                                  │
        ┌──────────────────▼──────────────────────────────────────────────────────▼───────────┐
        │ :core (Kotlin, no new deps)                                                          │
        │  library/  Tags, PictureWriter, Sqlite, LibraryBuilder(+CardView), LibraryVerifier,  │
        │            LibraryDiff, SyncPlanner                                                  │
        │  sync/     Links, LocalSource(File|SAF), FolderSync(scan/diff/apply), DeleteGuard,   │
        │            Journal, Lock                                                             │
        │  meta/     Fold, Similarity, Clusterer, MetaStore(overlay), Analysers, TagWriter,    │
        │            Online(MB/CAA/LRCLIB, cache, rate limit)                                  │
        │  playlists/ M3u, PlaylistRegistry, Resolver, Merge3, Rules, Radio(hibymix port), Stats│
        │  backup/   Manifest, BackupStore(CAS + .byosbackup zip), Bundle(ustar), RestorePlanner│
        │  btp/      existing BTP/1 client                                                     │
        └──────────────────────────────────┬───────────────────────────────────────────────────┘
                                           │ BTP/1 over USB (adb forward) or Wi-Fi (paired, encrypted)
        ┌──────────────────────────────────▼───────────────────────────────────────────────────┐
        │ Player: hibysync (daemon) · hibytag (dbsnapshot/dbcheck/dbapply/apply-staged/        │
        │ m3ureg/bundle) · hibymod_disp.so constructor → hibytag apply-staged · hibyshell       │
        └──────────────────────────────────────────────────────────────────────────────────────┘
```

- **Platform glue.** Only `LocalSource` differs between platforms: `File` on the JVM, SAF `DocumentsContract` on Android. Image work uses `java.awt` in the CLI (as `CoverArt` on the branch) and `BitmapFactory` on Android.
- **Desktop security rule (kept from desktop-lib).** The page never sends a file path. It sends only ids (link id, track index, proposal id, snapshot id). `server.mjs` maps them to CLI arguments; see `mediaParams` and the `LOCAL` command set.

---

## 2. Storage layout

### 2.1 On the PC or phone

`<config>` is the byos-cli `--config` folder. The desktop app passes the same folder, so the app and the CLI share state. On Android, `<config>` is the app's files directory, with the same JSON formats.

```
<config>/sync/<device_id>/links.json                 {format:1, client_id, client_name, links:[Link]}
<config>/sync/<device_id>/<link_id>/local.json       last local scan: rel → {size, mtime, qfp?, doc_id?, tags, sidecars}
<config>/sync/<device_id>/<link_id>/state.json       last good sync:  rel → {dest, src_size, src_mtime, sent_size,
                                                                        sent_sha, card_size, card_mtime, card_qfp?, covers, tags_rev}
<config>/sync/<device_id>/runs/<run_id>.jsonl        append-only journal, one line per finished op (fsync per line)
<config>/cache/sync/prepared/<sha(src_sha+opts)>     deterministic prepared copies (covers/tags rewritten)
<config>/cache/library/<device_id>/{snapshot.db,snapshot.json,card-list.json,tags.json,playlists.json}
<config>/cache/meta/{meta.json, proposals.json, covers/, lyrics/, net/, journal/, mb.lock}
<config>/playlists/registry.json
<config>/stats/<device_id>.json                      {path: {count, last, lowerBound}}
Documents/ByOS Backups/<name> (<device_id>)/{objects/, snapshots/, profiles.json}
```

`Link`:

```json
{"id":"a1b2c3d4","kind":"music|video|audiobooks|ebooks",
 "local":{"type":"path","path":"D:\\Music"},             // or {"type":"saf","tree_uri":"content://…","label":"Music"}
 "root_marker":{"volume_id":"…","entries_last":28440},   // used by DeleteGuard (§3.3.3)
 "card_root":"Music", "layout":"artist-album|keep",       // artist-album: music only
 "selection":{"mode":"all|chosen","artists":[],"albums":[],"genres":[]},   // from the branch's SyncSelection
 "remove_unselected":false,
 "player_only":"keep|delete|ask", "include_sidecars":true,
 "exclude":["**/*.tmp"], "enabled":true, "label":"Music"}
```

- **Default card roots:**

  | Kind | Card root | Notes |
  |---|---|---|
  | music | `Music` | |
  | audiobooks | `Audiobooks` | `AB_ROOT_NAME` in books_abcore |
  | ebooks | `Books` | one of the folder names the Books reader scans |
  | video | `Video` | |

- **Always excluded** under a card root:
  - `Music/Free/**` (shown as "Downloaded on the HiBy");
  - `ByOS/**`;
  - `Playlists/**` (owned by §6);
  - hidden files.

  Outside the card roots, nothing is ever planned for deletion. That includes `Mezclas/` and any root folders.
- **Several links for one kind** are allowed only if their card subtrees do not overlap. With layout `keep`, the second link goes to `card_root/<label>/`.
- **Migration:** desktop-lib `settings.sources[]` and the Music tab's selection options become music links. The branch's `ByOS/sync/manifest.json` becomes `manifest-v2/music.json`.

### 2.2 On the card (no daemon change needed)

```
ByOS/sync/card.json                     {card_id (random UUID), created}: written once; identifies this card (§4.3)
ByOS/sync/lock.json                     PUT CREATE (atomic: a second client gets E_EXISTS)
                                        {client_id, name, run_id, kinds, started, heartbeat}
ByOS/sync/manifest-v2/<kind>.json       {format:2, files:{dest:{link, rel, origin, src_size, src_mtime,
                                         sent_size, sent_sha, card_mtime, covers, time}}}
ByOS/sync/journal/<run_id>-<seq>.json   checkpoint files (PUT CREATE), folded into the manifest at the end
ByOS/sync/trash/<run_id>/…              move-aside area for a near-full REPLACE (§3.5)
ByOS/sync/playlists.json                playlist merge base (§6.2)
ByOS/backup/out/<id>.tar                device-made backup bundle, deleted after the client fetches it (§7.3)
ByOS/backup/in/<id>.tar                 restore bundle uploaded by the client (§7.3)
Playlists/<name>.m3u8                   playlists written by the apps
```

### 2.3 On the player's internal memory

```
/usr/data/hibymod/sync/pending          marker: something is staged (the shim checks only this)
/usr/data/hibymod/sync/db/              snap.db, incoming.db, staged.db, staged.json, apply.result, apply.boot
/usr/data/usrlocal_media.db.bak         previous live DB (or SD/.temp/usrlocal_media.db.bak in SD-DB mode)
/usr/data/hibymod/sync/restore/         staged/, prev/, apply.journal, restore.result
```

In SD-DB mode, `incoming.db` and `staged.db` live in `SD/ByOS/sync/db/` instead (§4.3).

---

## 3. Folder sync engine (`core/sync`)

### 3.1 Bugs in the branch that this design fixes

| # | Bug (branch `SyncPlanner` / `syncApply`) | Fix |
|---|---|---|
| 1 | The manifest is keyed by the absolute `src`. Moving `D:\Music` to `E:\Music` copies everything again as "X (2)" | Identity is `(link_id, rel)`. Relinking only changes `link.local` |
| 2 | The manifest is written only in `finally`. After a kill, the next plan creates " (2)" duplicates | Local journal plus card checkpoints, replayed before planning |
| 3 | `same_size` skips untracked files without checking their content | `adopt` by quick fingerprint (qfp) |
| 4 | Fixed album or artist tags never move the card file | A dest change emits `move` (BTP MOVE) |
| 5 | Removal only sees files in the manifest | Full card listing under `card_root`, with player-only sub-reasons |
| 6 | Optimised covers are not byte-stable, so resume restarts and leaks parts | Content-addressed prepared cache, kept until the file is recorded |

### 3.2 Identities

- **Source:** `(link_id, rel)`, with `rel` in NFC and `/`-separated.
- **Content, from cheapest to most expensive:**
  1. `sz_mt`: size + mtime.
  2. `qfp`: SHA-256 of (size ‖ first 16 KiB ‖ last 16 KiB).
     - On the card it is read with two GETs at an offset, each cancelled after 16 KiB. This needs no daemon change; optional daemon change D5 adds a length.
     - It is computed only when the sizes already match, and cached in `state.json` (`card_qfp`).
  3. `sha`: SHA-256 of the bytes as **sent**. This is the PUT hash, so it costs nothing extra.
- **Card mtimes** are only ever compared with earlier readings from the same device (±2 s). The FAT timezone and 2 s granularity are never compared with PC times.
- **Origin:** every card entry records the `client_id` that wrote it. By default the delete policy only applies to entries this client wrote.

### 3.3 Diff (`FolderSync.diff`)

**Inputs:**

- the local scan;
- `state.json`;
- the manifest v2 plus pending card journals;
- a LIST walk of `card_root` (cached per run);
- DF;
- the link options (selection, exclusions);
- the meta overlay (for dest names).

#### 3.3.1 Steps

1. **Local changes** (local scan vs state):
   - `unchanged` (same size and mtime), `changed`, `missing`, `new`;
   - `unreadable`: a folder or file the scan could not read. It is **never** treated as missing.
   - `renamed`: one missing and one new entry with the same `sz_mt` (on Android also the same `doc_id`). qfp breaks the tie.
   - `deselected` / `excluded`: present locally but outside the link's selection or exclusions.
2. **Card view** of each tracked entry (listing vs `card_size`/`card_mtime`):
   - `card_same`, `card_changed`, `card_missing`;
   - `card_renamed`: one untracked card file nearby with the same size and qfp.
3. **Dest:** `SyncPlanner.destFor(tags after overlay, layout)`. If it differs from the recorded dest, the item becomes a `move`.
4. **Classify:**

| action | when | card ops |
|---|---|---|
| `add` | new locally, card path free | MKDIR -p, PUT CREATE |
| `adopt` | new locally, untracked card file at dest with the same size | 2 short GETs. qfp equal → record only; not equal → `conflict` |
| `update` | changed locally, `card_same` | PUT REPLACE |
| `move` | dest changed, or a local rename (keep layout) | MOVE (then update if the content also changed) |
| `skip` | unchanged on both sides | – (counted, not listed in the plan) |
| `conflict` | changed on both sides; untracked file at dest with other content; edited on the player while unchanged locally | user picks: computer / player / both (" (2)") / import player edits (§5) |
| `pull` | the user picks "Copy to computer" on a player-only file | GET into `<local>.byos-part`, then rename |
| `player_only` | a card file under `card_root` with no current local source | per policy and sub-reason (below) |
| `blocked` | ≥4 GiB on vfat (`DF_R.sd_fs`); illegal name; path >1024 bytes or >32 levels; folder >60k FAT slots | – (reason shown) |

#### 3.3.2 Player-only sub-reasons (groups in the preview)

| Sub-reason | Meaning | With policy `delete` |
|---|---|---|
| `deleted_on_computer` | tracked, origin is this client, and the local file was **seen deleted** (its folder was read successfully) | **pre-ticked** (the only one), unless DeleteGuard trips |
| `excluded_on_computer` | tracked, but now deselected or excluded by the link options | not pre-ticked; ticked by "Remove unselected" (branch option) after its own confirmation |
| `from_other_device` | tracked, written by another client | needs "Select all" or `--delete-untracked` |
| `added_on_player` | untracked | needs "Select all" or `--delete-untracked` |
| `downloaded_on_player` | FreeMusic, audiobook downloads, Gutenberg EPUBs | needs "Select all" or `--delete-untracked` |

**Never deleted:**

- anything outside `card_root`;
- anything under the exclusions;
- the sidecars of a kept track;
- files under an `unreadable` local folder.

#### 3.3.3 DeleteGuard (mass-delete protection)

Before any delete is offered as pre-ticked, `DeleteGuard` checks the link. Any of these turns **every** delete of that link into an unticked item, with a red banner: "Your music folder looks unavailable. Nothing will be deleted."

- The local root is missing, empty, or no longer readable. This covers an unplugged drive, an offline network share and a lost SAF grant.
- `root_marker.volume_id` changed. On Windows this is the volume serial; on SAF it is the tree document id.
- More than **10 %** of the link's tracked files, or more than **200** files, would be `deleted_on_computer` in one run.
- The scan reported I/O errors.

Over the threshold, the user can still delete, but only by typing the count (desktop) or with a second confirmation (Android). Scripts need `--yes --allow-mass-delete`.

#### 3.3.4 Sidecars

- `.lrc`, `.cue`, `cover.jpg`/`folder.jpg`/`front.jpg` and album `.m3u` are child items of a track or a folder.
- `.lrc` files follow their track on a move.
- An album that uses a CUE sheet is forced to layout `keep` for that folder.

#### 3.3.5 FAT rules (in `SyncPlanner`)

- `cleanName` stays as it is today, plus:
  - a 255 UTF-16 unit cap alongside the 200-byte cap;
  - NFD names converted to NFC;
  - Windows reserved names (CON, PRN, AUX, NUL, COM1–9, LPT1–9) for pulls;
  - case-insensitive collisions folded on ASCII only.
- **FAT folder slot budget:** a long name uses 1 + ⌈UTF-16 length / 13⌉ slots. A folder above 60k slots is blocked.

### 3.4 Plan JSON

```json
{"format":2,"device_id":"…","run_id":"…","kinds":["music"],
 "summary":{"music":{"add":312,"update":4,"move":18,"adopt":40,"skip":28110,"conflict":2,
            "player_only":{"deleted_on_computer":12,"excluded_on_computer":0,"from_other_device":0,
                           "added_on_player":3,"downloaded_on_player":25},
            "guard":{"tripped":false,"reason":null},
            "blocked":1,"bytes_add":2254857830,"bytes_remove":91000000,"bytes_fingerprint":1310720,
            "sd_free":…,"fits":true,"eta_s":610}},
 "items":[{"id":"i42","kind":"music","link":"a1b2c3d4","action":"player_only","reason":"deleted_on_computer",
           "src_rel":null,"dest":"Music/ABBA/Gold/03 SOS.flac","old_dest":null,"size":31000000,
           "title":"SOS","origin":"client:9f…","default_choice":"delete","choices":["delete","keep","pull"]}],
 "playlists":{…},                                           // §6.4, when --playlists
 "library":{"mode":"internal|sd","tracks_after":28440,"db_estimate":24700000,"fits":true}
}
```

- `skip` items are only counted, so a 30k-track plan stays small.
- `DEC.json` (the user's choices, sent to `sync-apply`):

  ```json
  {"i42":"delete","i77":"computer","i90":"both", "policy":{"music":{"player_only":"ask"}}}
  ```

### 3.5 Apply order and interruption safety

1. **Lock.** PUT CREATE `lock.json`. If the lock is held and its heartbeat is less than 10 min old, the result is `locked_by` and the UI offers "Take over".
2. **Battery gate.** When caps STATUS is set: refuse below 15 % when not charging; warn below 30 %. Re-read DF.
3. **Replay** unfinished local runs and card journals.
4. **Moves** (swaps go through temporary names), then the **approved deletes**, then pruning of the folders left empty. This order frees space first.
5. **Puts, one album folder at a time** (smallest first when space is tight). For each file:
   1. prepare it into the content-addressed cache;
   2. hash it, then PUT (resumes on the part);
   3. STAT the card file and record `card_mtime`;
   4. append a line to the local journal.

   Every 200 files or 60 s: write a card checkpoint and refresh the lock heartbeat.
   - **REPLACE on a nearly full card**, until daemon change D2: MOVE the old file into `ByOS/sync/trash/<run>/`, PUT CREATE the new one, then delete the trash copy. The old copy is never deleted before the new one is complete.
6. **Pulls.**
7. **Fold** the journals into `manifest-v2/<kind>.json` (REPLACE), delete the journals, write `state.json` locally.
8. **Sync all:** steps 4–7 run per kind (music → audiobooks → books → video) under one lock.
9. **Then**, once for the whole run: the **playlists step** (§6), then the **library step** (§4), then the lock is released.

| Interruption | What is left | Next run |
|---|---|---|
| Cable or Wi-Fi drop, hibysync restarted | part file | 3 reconnects (2/4/8 s), then `paused`. Resume uses the same PUT_BEGIN at the PUT_READY offset |
| Card switched to USB mass storage (`E_SD_UNAVAILABLE`) | part file | `paused_usb_storage` |
| PC or phone dies | the journals | replayed: no re-copy, no " (2)" |
| Player power loss | the file is old or new (fsync + rename) | re-planned as changed or missing |
| `E_NO_SPACE` | – | the put phase stops; "N files did not fit"; the DB is still built for what is on the card |
| Android 6 h dataSync limit (`onTimeout`) | the journals | checkpoint, then `paused` |

---

## 4. Library database built on the PC or phone

### 4.1 Builder input: a whole-card view (`core/library/CardLibrary.kt`)

```kotlin
data class CardTrack(val cardPath: String /* SD-relative, NFC */, val size: Long, val mtime: Long,
                     val tags: AudioInfo?, val origin: Origin)    // PC_FILE | DEVICE_ROW | CARD_SCAN
data class CardView(val tracks: List<CardTrack>, val playlists: List<String>,
                    val rootEntries: List<RootEntry>, val cueSheets: List<CueSheet> = emptyList())
fun LibraryBuilder.build(base: File, out: File, view: CardView, opts: LibraryOptions)
// existing folder mode becomes CardView.fromFolder(musicRoot, sdRoot) so `db-build --music` keeps working
```

**The view covers the whole card, not only the linked roots.**

- **Inside the linked roots:**
  - Existence, size and mtime come from the post-copy LIST of each `card_root`.
  - Tags come from the local tag cache (through `(link, rel) → dest`), after the meta overlay (`LibraryOptions.overlay`).
  - Player-only files the user **kept** use their snapshot MEDIA row (`DEVICE_ROW`). Without a row, they use `lib-scan` partial-read tags (`CARD_SCAN`).
  - Deleted files are absent.
- **Outside the linked roots** (SD root folders, `Mezclas/`, `Music/Free` and so on):
  - Every snapshot MEDIA row is **carried over unchanged** (`DEVICE_ROW`). Its existence is checked against a LIST of its own folder; the folder list comes from the snapshot paths, so this is not a blind walk of the whole card.
  - New files there are not added: the stock scan or a later "Rebuild from card" (`lib-scan` over the whole card) adds them.
- `playlists`: every `.m3u` and `.m3u8` on the card.
- MEDIA2 is built from the root listing.

**Kept from the current `LibraryBuilder`:**

- the snapshot's schema;
- track ids and `ctime` kept per path (id policy: §0.3);
- the user tables (HISTORY, COLLECT, `list_tb_*`, MANAGER, PLAYLIST, BOOK*) re-pointed by path. Rows of removed tracks are dropped, never re-pointed to another track;
- MEDIA2 as the root cache;
- SEARCH and MEDIA3 empty (§0.4);
- MQA = "18";
- stock sort order;
- empty `album_pic_path`;
- the NUL-terminated `a:\` text format.

**Grouping:** artists, albums, genres and album artists are grouped by the **canonical value from the meta overlay** (§5). Without an overlay they are grouped by the ASCII-lowercase key, so a build without an overlay matches today's output.

**CUE** (cue_id, begin and end times) is deferred until its stock semantics have been researched.

### 4.2 Verification layers (no single layer is trusted)

1. **Builder invariants:** unique PKs, `a:\` + NUL paths, COUNT_TABLE row 0 = MEDIA count, CTIME and MTIME id sets equal to the MEDIA ids.
2. **`LibraryVerifier`** (Kotlin): re-read every B-tree; check that every index entry matches its row; check the schema is identical to the base; check kept-table counts.
3. **An independent engine** where one exists:
   - Android: platform `SQLiteDatabase` `PRAGMA integrity_check` plus sample NOCASE queries;
   - desktop: `node:sqlite` when Node ≥22.13 is present;
   - CI: Python sqlite3, plus a **golden test** on a scrubbed stock DB: rebuilding from the same files must reproduce the stock rows.
4. **`LibraryDiff` preview,** **per root and outside the roots.**
   - Removing more than 20 % of the tracks needs an extra confirmation.
   - Any removal **outside** the linked roots is listed by name.
5. **`hibytag dbcheck`** on the device at stage time.
6. **Crash-loop rollback** on the device.

### 4.3 Device side (blocks the end-to-end flow)

| Piece | Behaviour |
|---|---|
| `hibytag dbsnapshot LIVE OUT` | `sqlite3_backup` with busy_timeout 5000, written to tmp, fsync, rename. Unblocks DB_SNAPSHOT (today it returns E_IO) |
| `hibytag dbcheck FILE [--live LIVE]` | Read-only. Checks: full `integrity_check`=ok; VERSION_TABLE=20181015; `sqlite_master.sql` identical to live; MEDIA count 0..100000; paths `a:\…\0` with no `/`; COUNT row 0 = MEDIA count; CTIME/MTIME ids = MEDIA ids; the stock queries prepare. A distinct exit code per failure class. **Runs at DB_STAGE time (daemon), not at boot** |
| DB_STAGE additions | The client sends `card_id` (from `ByOS/sync/card.json`) and the build's `mode` (internal/sd). The daemon writes both into `staged.json`, then creates `sync/pending` |
| `hibytag dbapply` | 1. sha(staged) = `staged.json`. 2. **Card check:** if the SD is mounted, `ByOS/sync/card.json` must match `staged.json.card_id`; otherwise **postpone**: keep everything, `last_apply=6`, and the shell shows "Library update waiting for its card". 3. ATTACH live and **merge the user tables** (HISTORY, COLLECT, COLLECT_OPERATE, `list_tb_*` + MANAGER, PLAYLIST, BOOK*, BOOKMARK, and M3U rows not present in staged), re-pointed by path. 4. `quick_check`. 5. Rename live → `.bak`, staged → live; fsync the folder. 6. Write `apply.result` and the `apply.boot` marker. Any failure leaves live untouched (last_apply 2/3/4) |
| `hibytag apply-staged` (one-shot) | Run by the shim constructor only when `sync/pending` exists, with a **10 s budget**. Over budget → stop before the rename, leave live untouched, retry at the next start. Order: DB (`dbapply`, at the live path the current mode selects) → ByOS restore files (§7) → results → remove `pending`. Journalled, so it resumes after a power loss |
| Crash-loop rollback (shim) | "Healthy" = the first main `.view` seen by `note_view` plus 30 s; the shim then deletes `apply.boot`. **Two** starts with `apply.boot` still present → restore `.bak`, `last_apply=5`. This fires before ByOS's 3-failed-boots safe mode, and the safe-mode counter is not reset by the rollback |
| Restart | BTP never reboots. The shell shows *Restart Now* on `DBSTAGED`. The client says "Restart the HiBy to use the new library" |

#### SD-DB mode (`tf_music_db_enable=1`): the large-library path

- **Internal mode** fits while `data_free ≥ 3 × db_size + 1 MiB` (live + staged + `.bak`; about 9k tracks at 0.87 KB/track with 24 MB free). The plan computes `library.fits`.
- **Above that, the plan proposes SD-DB mode.** The live DB is at `SD/.temp/usrlocal_media.db`, staging in `SD/ByOS/sync/db/`, `.bak` next to the live DB, and the space check is against `sd_free`.
- **Prerequisites (M4):**
  - hibymix.c:40, hibytag.c:2254 and music.inc.c:34 read the mode instead of hard-coding `/usr/data/usrlocal_media.db`;
  - the daemon already resolves the path (§13);
  - **device checks:** the stock player in SD-DB mode (performance, RAM, behaviour when the card is absent at boot), and whether the SD is mounted when the shim constructor runs. If it is not, SD-mode apply happens on the first start where it is.
- Switching modes is a user action with a sheet ("Keep the library on the card"). It writes the setting through hibyset and stages the current DB at the new path. It is never automatic.

### 4.4 Flow for `sync-apply --then rebuild`

1. The copy (all kinds selected).
2. A LIST of each card root, plus the folders of snapshot rows outside the roots.
3. `db-pull` (after the copy, to shorten the stale window).
4. Build from the CardView.
5. Verify.
6. Diff.
7. Push: PUT NS_DB, then DB_STAGE (`card_id`, mode).
   - With caps DB_APPLY: flag `DEVICE_MERGE`. Plays and favourites change the live DB all the time, so a strict base check would almost always fail.
   - Without DB_APPLY: on `E_DB_STALE`, re-pull and rebuild once.

Result:

```json
{"library":{"action":"rebuild","ok":true,"staged":true,"mode":"internal","sha256":"…","tracks":28440,
            "removed_outside_roots":0,"checks_failed":[],"restart_needed":true}}
```

**Fallback without caps DB_APPLY (an old ByOS):** LIB_RESCAN mode 1 for the changed files, then `warn library_pending` ("Open Settings › Update library on the HiBy"). This is today's behaviour on the branch.

---

## 5. Smart metadata (`core/meta`)

### 5.1 Principle: overlay first, DB second, files only on request

- **Fixes live in `<config>/cache/meta/meta.json`** and are applied:
  1. to the PC catalogue the UI shows;
  2. in `LibraryBuilder`, so **the player DB is fixed with no file transfer**;
  3. to the **card copies only when a file is sent anyway** (add, update), so new copies carry the fixes at no extra cost.
- **"Rewrite tags on the HiBy"** (opt-in):
  - re-sends the affected files with fixed tags;
  - the plan shows the cost first ("Re-send 1,204 songs, 38 GB, about 2 h over USB").
  - Without it, a **stock on-device rescan** would bring the old tags back. The Library database panel therefore recommends turning the stock auto-rescan off and says why.
- **"Also fix my files"** (the user's own files): opt-in, with a journal and Undo.
- **Formats Kotlin cannot write** (WMA, APE, WAV, DSF, DFF) are fixed in the DB only, with a warning.

```json
{"format":"byos-meta/1","rev":12,
 "settings":{"applyTo":"player|player+files","rewriteCard":false,"genreLanguage":"en|majority",
             "genreDetail":"detailed|broad","featuring":"keep|title","online":false},
 "rules":{"genre":{"<fold key>":"Hip Hop"},"artist":{"<fold key>":"Beyoncé"},"albumArtist":{},"split":{"genre":";|/|,"}},
 "tracks":{"<link_id>/<rel>":{"size":…,"mtime":…,"set":{"ALBUMARTIST":"Various Artists","COMPILATION":"1"},"from":"p:4f1c…"}},
 "albums":{"<album key>":{"cover":{"file":"covers/<sha1>.jpg","source":"caa:<rg-mbid>","w":1200},"mbid":"…"}},
 "lyrics":{"<link_id>/<rel>":{"file":"lyrics/<sha1>.lrc","source":"lrclib:123","synced":true}},
 "decisions":{"<proposal id>":"accepted|rejected|edited"}}
```

- **Proposal id** = `sha1(kind + sorted member fold keys)`. A rejected proposal never comes back.
- **Per-track entries** are re-validated when the file's size or mtime has changed.
- **Covers and lyrics** are small sidecar files (`.lrc`, `folder.jpg`). They are sent as sidecars, which is cheap, not embedded by re-sending audio.

### 5.2 Algorithms (deterministic, no ML)

1. **`fold(s)`:**
   1. NFKC, casefold, strip accents;
   2. map `& / and / y / et / 'n'` to one token;
   3. turn `-_/.` into spaces, drop punctuation, collapse spaces;
   4. drop leading articles (the/el/la/les/le/der);
   5. turn "Beatles, The" into "beatles".

   `tokkey` = the fold tokens, sorted, with filler words removed (music/musique/música).
2. **Blocking.** Two values are only compared when they share one of: fold key, token key, the first 3 characters, or at least 2 trigrams. They are then scored with Damerau-Levenshtein, Jaro-Winkler (strings of 8 characters or fewer) and token Jaccard.
3. **Scores:**

   | Evidence | Score |
   |---|---|
   | Same fold key | 1.0 |
   | Same token key | 0.95 |
   | Alias table hit (e.g. Rap ↔ Hip Hop) | 0.95 |
   | Only a filler word differs | 0.9 |
   | Typo (edit distance ≤1 at length ≥5, or ≤2 at length ≥9), at least one side not a known genre | 0.8 |
   | Co-occurrence on the same artist or album | +0.1 |
   | MusicBrainz votes agree | +0.1 |

   - **Cannot-link:** two distinct known MusicBrainz genres, or two artists with different MBIDs, are never merged **by edit distance**. Only the alias table can join them. This keeps Electro/Electronic and Trap/Rap apart.
   - Clusters are built with union-find, never chaining through a cannot-link pair.
   - Thresholds: ≥0.9 pre-ticked; 0.7–0.9 shown unticked; below 0.7 hidden.
4. **Canonical label**, in this order:
   1. the MusicBrainz or alias name (in the `genreLanguage` setting);
   2. the value with the most tracks;
   3. the best form (mixed case, accents kept, fixed exceptions such as R&B, EDM, J-Pop).
5. **One genre per track in the DB:**
   - split multi-value tags, protecting forms such as "R&B" and "Drum & Bass";
   - the first mapped value goes to the DB;
   - **album consensus** = the genre carrying the most track duration (ByOS Music groups albums by genre);
   - "Broad" mode rolls each genre up to about 25 parent buckets.
6. **Albums:**
   - Album identity = folder + fold(album without disc/edition suffix) + album artist (or the majority primary artist).
   - **Compilation:** 3 or more artists and none above 50 % → album artist "Various Artists" and COMPILATION=1.
   - **Featuring** is parsed ("feat.", "ft.", "&", "x").
   - **Discs** from `CD\d` / `Disc \d` folders or duplicate track numbers.
   - **Track numbers** from "01 - " file names.
   - **Year:** the mode of the album's years.
   - **Mojibake:** only when re-decoding gives valid UTF-8 and the original contains Ã or Â.
7. **Lyrics:**
   - Sources: sidecar `.lrc`, embedded text, LRCLIB (`/api/get`, then `/api/search`).
   - Ranking is the same as `lrc_rank`.
   - A result is accepted with high confidence only on an exact `get` match, or a token Jaccard ≥0.6 against the existing text.
   - Timestamps must rise and stay below the track duration.
   - Written as `<track>.lrc`, UTF-8 with BOM.
8. **Covers:**
   - Sources: the folder image, the embedded picture, CAA `front-1200`/`front-500` by release-group MBID. iTunes is **not used**.
   - Checks: `cover-missing`, `cover-small` (<300 px), `cover-inconsistent`, `cover-bad` (the `art-fix` rules).
9. **Online client:**
   - one `HttpURLConnection` client;
   - MusicBrainz at most 1 request/s **across processes** (lock and timestamp file `cache/meta/mb.lock`, ported from `hibynet.c`);
   - disk cache `cache/meta/net/<sha1(url)>.json` for 30 days, including misses;
   - resumable.
10. **TagWriter** (Kotlin port of the project's own `hibytag.c`):
    - FLAC Vorbis comments and ID3v2.3/2.4 + v1: in place within the padding, otherwise temp + fsync + rename;
    - M4A text atoms, extending the `PictureWriter` atom rewrite;
    - an Ogg Vorbis/Opus reader and writer (re-paginate, CRC32, renumber the pages).

    Every write is read back with `TagReader`. Ogg/Opus also gets a **reader** in `Tags.kt`; today those files are invisible.

**Bundled data** (`core/src/main/resources/meta/`), approval in §13:

- `mb-genres.txt`: the MusicBrainz genre list, CC0;
- `genre-aliases.json`: EN/FR/ES synonyms;
- `genre-parents.json`: about 25 parent buckets;
- `genre-mood.json`: energy and valence, used by §6.6.

**Proposal kinds:**

- genres: `genre-merge`, `genre-split`, `genre-rollup`, `genre-fill`, `album-genre`;
- artists: `artist-merge`, `featuring`, `mojibake`;
- albums: `albumartist-set`, `compilation`, `album-merge`, `disc-merge`, `track-number`, `year`;
- covers: `cover-missing`, `cover-small`, `cover-inconsistent`, `cover-bad`;
- lyrics: `lyrics-missing`, `lyrics-misnamed`, `lyrics-upgrade`.

**Edits made on the player** (tag editor, setcover, lyrics) are detected by `sync-plan` as `card_changed` → `conflict`. The choice **"Import player edits"** reads the card tags and turns them into `meta-set` entries.

---

## 6. Playlists (`core/playlists`)

### 6.1 Player sources

| Source | Read | Write (this design) |
|---|---|---|
| `.m3u`/`.m3u8` (+ `M3U_TABLE` row) | yes | **yes**: the file; the row comes with the next DB build, or live through LIB_M3U (optional) |
| Stock `PLAYLIST_TABLE` + `list_tb_<id>` | yes (`playlist-state`) | pull only; write-back later (M14) |
| Favourites `COLLECT_TABLE` | yes | pull only; write-back later |
| `HISTORY_TABLE` | yes, for play counts | – |
| ByOS queues `list_tb_7701/7702`, queue `list_tb_1` | – | never synced |
| hibymix output `Mezclas/` | yes, imported as `mix` | never deleted by sync |

### 6.2 Model

- **Files the apps write:** `Playlists/<name>.m3u8`, UTF-8 without BOM, `#EXTM3U`, optional `#EXTINF`, relative paths (the branch's `playlist-add` format).
- **One core M3U implementation:**
  - keeps `#EXTINF`, the encoding and the line ends; falls back to CP1252;
  - replaces both the core `M3uWriter` and the app's `connected/Playlists.kt`.
- **Card merge base:** `ByOS/sync/playlists.json`, shared by the desktop and Android so they cannot overwrite each other.

  ```json
  {"v":1,"rev":7,"lists":[{"uid":"7f3c…","name":"Road trip","path":"Playlists/Road trip.m3u8",
    "kind":"manual|smart|mix|imported","sha256":"…","base":["Music/ABBA/Gold/01 Dancing Queen.flac"],
    "origin":"pc|android|player|stock:12|favorites","by":"desktop:<client_id>","time":…}],
   "favorites_sha":"…","stock":{"12":"<sha of name+paths>"},"history_rowid":394}
  ```

- **Local registry:** `<config>/playlists/registry.json`.

  ```json
  {"v":1,"lists":[{"uid","name","kind","rules":{…}|null,"seed":123,"tracks":[TrackRef],
    "file":"<music link folder>/Playlists/<name>.m3u8","local_sha":"…",
    "devices":{"<device_id>":{"rev":7,"base_sha":"…"}},"deleted":false,"mtime":…}]}
  ```

  `TrackRef = {"card":"Music/…"|null, "local":{"link":"a1b2","rel":"…"}|null, "key":"artist|title|album|durS"}`

- **Resolving a track**, in this order:
  1. manifest-v2 (`(link, rel)` ↔ dest);
  2. the same relative path under a link;
  3. the normalised tag key, with duration ±2 s and size as tie-breakers.

  Unresolved entries are kept and shown as "missing on HiBy" or "missing here".
- **Saved to the computer or phone:** `<music link>/Playlists/<name>.m3u8`, with paths relative to the local files. On Android this needs a WRITE grant on the SAF tree.

### 6.3 Merge (by uid)

| PC | Player | Action |
|---|---|---|
| unchanged | unchanged | none |
| changed | unchanged | push |
| unchanged | changed | pull |
| changed | changed | merge |
| new | – | create |
| – | new file, stock list, favourites or mix | import (`kind: imported`/`mix`) |
| deleted | unchanged | delete-device (default on, listed in the preview) |
| unchanged | deleted | delete-local (default on, listed in the preview) |
| deleted on one side | changed on the other | conflict (default keep) |

**The merge** is a diff3 over ordered lists:

- entries are keyed by (track key, occurrence number), so duplicates survive;
- removals from both sides are applied;
- inserts go after their nearest surviving base anchor, PC before player at the same anchor;
- if both sides reordered, the side with more moves wins, then the other side's adds and removes are applied.

**Other cases:**

- A smart playlist edited on the player → "Convert to normal playlist" or "Discard the HiBy's changes".
- Two uids with the same name → " (2)" on the card.

### 6.4 Steps in a sync

`sync-plan --playlists` adds a `playlists` section to the same plan. `sync-apply` runs the playlist step after the media copy, so the paths exist:

1. write the files (PUT REPLACE);
2. update `playlists.json` **last**;
3. the library step (§4) adds the M3U_TABLE rows. LIB_M3U is used only for a **playlist-only** sync (no DB rebuild) when caps LIB_M3U is set; otherwise `warn library_pending`.

### 6.5 Player side

- **`hibytag m3ureg <db> add|rm <a:\path>…`:** the same insert as hibymix `emit()` (NUL-terminated path, `name` = file name, `character` letter, id = MAX+1), busy_timeout 4000, one transaction.
- **BTP `LIB_M3U`** (§8, optional M6b):
  - only `.m3u`/`.m3u8` under the SD;
  - for `add`, the file must exist;
  - when a DB is staged, the row is also applied to `staged.db`.
- **Later (M14):**
  - a ByOS play journal `SD/ByOS/stats/plays.tsv` (`time\tpath\tplayed_ms\tdur_ms` at 50 % or 4 min; rotate at 256 KB);
  - "Add to playlist" in ByOS Music;
  - `LibraryBuilder` writing PLAYLIST_TABLE, `list_tb_N` and COLLECT from the registry.

### 6.6 Smart playlists and generation

- **Play counts:**
  - `plays-pull` counts HISTORY rows with `rowid > history_rowid` as one play each;
  - if all rows are new, the counts are marked `lowerBound`;
  - the play journal replaces this heuristic once it exists;
  - COLLECT is the "loved" flag.
- **Rules (iTunes-like):**
  - match all/any, with nested groups;
  - fields: artist, album artist, album, title, canonical genre, year/decade, date added (ctime), duration, plays, last played, format, lossless, hi-res, sample rate, favourite, in/not in playlist, has cover, has lyrics, mood/energy (estimated);
  - operators: is, is not, contains, starts with, in, between, in the last N days, >, <;
  - limit by N songs, minutes or MB, chosen by seeded random, most/least played, most recent, most recently played or album order;
  - option "only songs on the HiBy".
- **Radio and mixes:** a Kotlin port of `hibymix.c`.
  - Artist graph = hibymix edges + playlist co-occurrence (1/ln(2+len)) + play counts.
  - Track score = 2.0·graph + 1.4·language + 0.7·era + genre (canonical match 1.0, same family 0.5, unrelated −0.3) + 0.1 format + familiarity − recent penalty − version penalty + seeded jitter.
  - Greedy selection with caps: 5–7 per seed artist, 3 per other artist, 2 per album, no repeated title. Then the 6-track ordering pass.
  - Generators: radio (from a song, artist, album, genre or playlist); mixes daily, discover, favourites, new, decade; forgotten favourites (many plays, not played for 90+ days); era; genre mix.
  - The `seed` is stored, so a resync is stable; "Refresh" picks a new seed. `explain` gives the "Why this song?" text.

---

## 7. Backup and restore (`core/backup`)

### 7.1 What is backed up

Classes: **P** = portable; **F** = same firmware only (otherwise replayed or skipped); **X** = never.

| Kind (toggle, default) | Contents | Where it comes from | Class |
|---|---|---|---|
| `settings.byos` (on) | `hibymod/shell/launcher.conf`, `keyboard.conf`; `power.conf`, `audio.conf`, `vlim.state` (never restore ack=1 onto another unit), `listen.dat`, `store_repos.conf`, `ota.conf`, `mirror_on`, `no_hibykb`, `no_launcher`; `buds/` | bundle | P |
| `settings.hiby` (on) | `settings_kv`: the decoded `user.ini` values that hibyset can replay; region; time zone. Raw `user.ini` is stored for reference only and **never written back** in v1 | bundle | F (replay) |
| `sound.presets` (on) | `peq/*.peq`, `mseb_list`, `menu_cfg`, `harmonic.json` | bundle | F |
| `library` (on) | DB snapshot (DB_SNAPSHOT) | DB_SNAPSHOT | P (conditional) |
| `favourites.history` (on) | HISTORY, COLLECT and queue tables inside the DB; off means blanked in the restored DB | DB | P |
| `playlists` (on) | SD `.m3u`/`.m3u8`, `Mezclas/`, DB playlists exported to m3u8 | NS_SD + DB | P |
| `books.progress` (on) | `appdata/books` (settings, index, `*.pos`, `ab_*.pos`, `catalogs.tsv`, `abdl.q`) | bundle | P |
| `radio` (on) | `appdata/radio`, `a:\radio.txt`, `radio.ini` | bundle + NS_SD | P |
| `lyrics` (on) | SD `*.lrc` | NS_SD | P |
| `wallpapers` (on), `internal.files` (on) | `fondos/`, `archivos/` | bundle | P |
| `downloads`, `ebooks.files`, `audiobooks.files`, `audio`, `video`, `images`, `caches` (off) | SD media | NS_SD | P (`ref_only` when off: path, size and mtime only) |
| later | Wi-Fi networks (secrets), Bluetooth pairings | – | deferred (§12 M14) |
| never | `sync/device.key`, peers, `sync/db`, bin, preload, fast, sys, t, theme, mirror, apps (code), cache, logs, debug flags, `pin.conf`, `alsa.conf`, `wifi_*.txt`, `override/*.json` | – | X |

**Per-item control:** after the kind toggles, a "Choose items…" tree (from `backup-scan`) lets the user untick folders or files. The exclusions are saved as globs in a backup profile, so they also apply to later backups. Bundle items are always fetched (they are small) and filtered on the computer side.

### 7.2 Store

- **Location:** `Documents/ByOS Backups/<name> (<device_id>)/`. On Android, a SAF tree with a persisted grant.
- **Layout:**
  - `objects/<sha[0:2]>/<sha>`: content-addressed blobs shared between snapshots, so a second backup copies only new files;
  - `snapshots/<UTC>.json`: one manifest per backup.
- **Export and import:** `.byosbackup` = a ZIP with `manifest.json` first, then `files/<path>`. Media is stored uncompressed, text is deflated.

Manifest v1:

```json
{"format":"byos-backup","v":1,"id":"<uuid>","created":…,"client":"desktop|android","client_version":"…",
 "device":{"id":"…","name":"…","model":"R3II-2025","byos_version":"…","firmware":"…",
           "fw_fingerprint":"<sha256 /usr/bin/hiby_player>","card_id":"…"},
 "kinds":{"settings.byos":{"files":12,"bytes":…}},
 "items":[{"src":"data:hibymod/launcher.conf","kind":"settings.byos","class":"P","size":…,"mtime":…,"sha256":"…","mode":"replace"},
          {"src":"data:user.ini","kind":"settings.hiby","class":"F","sha256":"…","mode":"reference"},
          {"src":"db:library","kind":"library","sha256":"…","schema":20181015,"tracks":2488,"db_mode":"internal"},
          {"src":"sd:Music/A/B/01.flac","kind":"audio","ref_only":true,"size":…,"mtime":…}],
 "derived":{"settings_kv":{…},"playlists_m3u8":["derived/playlists/<name>.m3u8"],"apps":[{"id":"…","version":"…"}]},
 "excluded":[{"path":"…","reason":"user|kind-off|deny|too-large"}],"warnings":[]}
```

### 7.3 Device side: bundles on the SD

- **`BACKUP_MAKE`** (caps BACKUP):
  1. the daemon runs `hibytag bundle make OUT`;
  2. it writes a ustar of the **compiled-in allow-list** (the bundle items of §7.1) to `SD/ByOS/backup/out/<id>.tar`;
  3. it answers with path, size, sha256 and the item count.

  Rules:
  - A **hard deny-list wins over the allow-list:** `hibymod/sync/**`, `*.key`, `mnt/**`, preload, bin, fast, sys, t, theme, mirror, apps, cache, `override/**`, shell binaries, and non-regular files.
  - The walk uses openat/O_NOFOLLOW.
  - `user.ini` is read twice and included only if both reads match (3 retries).

  The client GETs the tar over NS_SD (resumable, hashed), then DELETEs it.
- **`RESTORE_STAGE`** (caps RESTORE):
  1. the client PUTs the restore tar to `SD/ByOS/backup/in/<id>.tar`, then sends RESTORE_STAGE (path, sha256);
  2. the device verifies the hash, checks **every entry against the allow-list** (otherwise `E_RESTORE_INVALID`), and checks that `data_free` covers the extracted size + `prev/` + 1 MiB;
  3. it prompts on the device: "Restore backup from <client>, <date>? The HiBy will restart" (Allow/Deny, 120 s; the pairing ALLOW/DENY flow over the control pipe);
  4. on Allow it extracts into `/usr/data/hibymod/sync/restore/staged/` (internal, so the boot applier does not depend on the SD) and creates `sync/pending`.
- **Applied by `hibytag apply-staged`** at the next start, journalled:
  1. copy the current targets to `prev/` (for undo);
  2. install the ByOS files (tmp + fsync + rename);
  3. the DB, if one was staged together with the restore (same `dbapply` path as §4.3);
  4. write `restore.result`.
- **Shell, on the first start after an apply:**
  - replay `settings_kv` through hibyset 'S' (`hs_set_send`) and report the values it could not set;
  - reapply `audio.conf`;
  - show the card "Restored from backup (date): N items, M skipped";
  - list the store apps to reinstall (signed, through the store).

### 7.4 Restore flow on a reset player

1. If ByOS is missing, the existing first-install flow runs first (DESKTOP_APP §5.8). Restore needs caps BACKUP and RESTORE.
2. Connect over USB (preferred) or pair again; the device key changed.
3. `restore-plan`: compare the backup with DF, STAT, the firmware fingerprint and the card.
4. SD files first (identical files are skipped), so book `.pos` hashes (path + size) match again.
5. DB strategy:
   - **as-is** (FORCE) if ≥98 % of the backup's MEDIA paths exist on the card with the same size;
   - otherwise **rebuild**: `db-build --base backup.db` against the card view;
   - with `favourites.history` off, those tables are blanked.

   Then DB_STAGE (with the current `card_id`).
6. The settings bundle goes up, then RESTORE_STAGE and the device prompt.
7. One restart. The applier runs, the shell replays, and the client runs `restore-status`. `restore-undo` re-stages `prev/`.

---

## 8. Protocol and daemon changes

### 8.1 Allocations

All of these are backward compatible: a request whose capability bit is not set gets E_UNSUPPORTED.

| Kind | Value | Name | Payload / meaning |
|---|---|---|---|
| caps | bit 9 | DB_APPLY | the device applies `staged.db` at start with the user-table merge, card check, `.bak` and rollback |
| caps | bit 10 | LIB_M3U | LIB_M3U op |
| caps | bit 11 | BACKUP | BACKUP_MAKE |
| caps | bit 12 | RESTORE | RESTORE_* ops |
| caps | bit 13 | PARTS_CLEAN | DELETE flag PART |
| caps | bit 14 | SD_DB | DB_SNAPSHOT/DB_STAGE/apply work in SD-DB mode |
| op | 0x46 | DB_RESTORE | empty → DB_STATUS_R (swap `.bak` back at the next start) |
| op | 0x47 | LIB_M3U | `mode u8 (1 add, 2 rm), count u16, count × path str16` (NS_SD) → OK |
| op | 0x62 | BACKUP_MAKE | `flags u8` → BACKUP_R |
| op | 0x63 | BACKUP_R | `path str16 (NS_SD), size u64, sha256 bytes[32], items u32` |
| op | 0x64 | RESTORE_STAGE | `flags u8, sha256 bytes[32], path str16 (NS_SD)` → RESTORE_STATUS_R |
| op | 0x65 | RESTORE_STATUS | empty → RESTORE_STATUS_R |
| op | 0x66 | RESTORE_STATUS_R | `staged u8, last_result u8, result_json str16` |
| op | 0x67 | RESTORE_UNSTAGE | → RESTORE_STATUS_R |
| op | 0x68 | RESTORE_UNDO | → RESTORE_STATUS_R |
| flag | DB_STAGE bit 1 | DEVICE_MERGE | skip the strict staleness check; the device merges the user tables at apply |
| field | DB_STAGE tail | `card_id bytes[16], mode u8 (0 internal, 1 sd)` | appended; an old device ignores it, and the client only sends it with DB_APPLY |
| flag | DELETE bit 1 | PART | leaf names must match `.btp-[0-9a-f]{16}.part` |
| field | DB_STATUS_R tail | `has_bak u8, update_setting u8, db_mode u8` | appended; old clients ignore it |
| last_apply | 5 | ROLLED_BACK | crash-loop rollback |
| last_apply | 6 | WAITING_CARD | postponed: the SD is missing or a different card |
| error | 82 | E_RESTORE_INVALID | the restore bundle failed its checks |
| error | 83 | E_DECLINED | the user tapped Deny on the device |

### 8.2 Other daemon and firmware items

- **D1:** remove the hard-coded DB paths (hibymix.c:40, hibytag.c:2254, music.inc.c:34); read `tf_music_db_enable` the way Settings does. Prerequisite for SD_DB.
- **D2:** the REPLACE space check credits the size of the replaced file.
- **D3:** a "sync in progress" hint to the shell (`sync_xfer_busy`). The shell shows "Syncing…" and postpones the auto-rescan and the FreeMusic/audiobook downloads into the linked roots.
- **Optional, only if measurements show a need:** D4 recursive LIST; D5 GET with a length (`HASH_RANGE`).
- All of this is documented in `docs/SYNC_PROTOCOL.md` §5, §7, §9, §13, a new §13.4 (LIB_M3U) and §22 (backup/restore).

---

## 9. byos-cli commands (JSON lines, BYOS_CLI.md §2)

The existing commands keep working: `music-scan`, `sync-plan`, `sync-apply`, `db-pull`, `db-build`, `db-push`, `playlist-ls/add/rm`, `lib-index`, `lib-scan`, `lib-art`, `lib-rescan`, `art-fix`, `cover-cache-reset`, `rm --prune-empty`.

```
# links and sync
link-ls | link-add --kind K LOCAL [--card-root R] [--layout artist-album|keep] [--player-only keep|delete|ask]
          [--select artists:…|albums:…|genres:…] [--remove-unselected] [--exclude G]…
link-set ID [opts] | link-rm ID
sync-scan  [--kind K|all] [--full]
sync-plan  [--kind K|all] [--playlists] [--out PLAN.json] [--delete-untracked]
sync-apply PLAN.json [--decisions DEC.json] [--rewrite-tags] [--then none|rescan|rebuild] [--yes] [--allow-mass-delete]
sync-resume | sync-status | sync-unlock [--force]
# library DB
db-build --base SNAP.db (--music DIR | --plan PLAN.json --card-list CARD.json) [--meta meta.json] [--verify] --out NEW.db
db-verify NEW.db --base SNAP.db | db-diff NEW.db --base SNAP.db | db-status | db-unstage | db-restore
db-mode internal|sd                                   # switch with a sheet in the UI; stages the DB at the new path
# metadata
meta-analyze --catalog C [--kinds genre,artist,album,cover,lyrics] [--online]
meta-lookup album KEY | track ID --source musicbrainz|caa|lrclib
meta-decide --accept ID… --reject ID… [--edit ID=JSON] | --decisions FILE
meta-set ID… KEY=VALUE… | meta-cover ALBUMKEY --file IMG|--candidate ID | meta-lyrics ID --file LRC|--candidate ID|--clear
meta-genres | meta-apply [--dry-run] [--only KIND] [--files] | meta-undo JOURNAL
# playlists (extends playlist-*)
playlist-state [--db SNAP.db] | playlist-local | playlist-save --file PL.json | playlist-delete UID | playlist-import FILE…
playlist-gen (--rules R.json | --radio song:ID|artist:N|album:A|genre:G|playlist:UID | --mix daily|discover|favorites|new|decade) [--n 50] [--seed N]
playlist-sync-plan [--policy ask|prefer-pc|prefer-player] | playlist-sync-apply PLAN.json [--decisions D.json] [--yes]
plays-pull
# backup
backup-scan [--kinds …] | backup-make --store DIR [--kinds …] [--exclude G]… [--profile N] [--export F.byosbackup]
backup-ls --store DIR | backup-info SNAP|FILE | backup-prune --store DIR --keep N
restore-plan SNAP [--kinds …] [--exclude G]… | restore-apply SNAP [--yes] | restore-status | restore-undo
```

**Events:** the existing ones plus:

- `phase` (`name`, `index`, `count`);
- `item` (backup/restore, with `class`, `action`, `compat`, `reason`);
- `backup`;
- `entry` (streamed plan items and proposals).

**Progress ops:** `scan`, `tags`, `list`, `fingerprint`, `prepare`, `hash`, `put`, `move`, `remove`, `pull`, `checkpoint`, `db_pull`, `db_scan`, `db_build`, `db_verify`, `db_push`, `analyze`, `lookup`, `cluster`, `write`, `playlists`, `bundle`, and `sync` overall (with `rate` and `eta_s`).

**Warning codes:** `resumed`, `reconnecting`, `paused_usb_storage`, `locked_by`, `manifest_migrated`, `journal_replayed`, `conflict`, `blocked`, `failed`, `library_pending`, `stale_parts`, `unsupported`, `locked`, `playlist_unresolved`, `large_for_internal`, `card_only_kept`, `missing_on_card`, `source_unavailable`, `mass_delete_blocked`, `removed_outside_roots`, `waiting_card`, `tags_not_on_card`.

**Prompts:** `confirm` (deletions: count and bytes), `confirm_mass_delete`, `take_over_lock`, `device-confirm`.

**New error codes:** `device_declined`, `unsupported`, `no_space_data`, `backup_corrupt`.

---

## 10. Desktop UI (on top of apps/desktop-lib)

The branch's structure is kept:

- the sidebar, with its Library entries;
- the device tabs: Summary · Music · Files · Library · Updates · Settings.

Changes:

| Place | On the branch | After this design |
|---|---|---|
| Sidebar **Library** (Music, Video, Audiobooks, Books, Playlists) | opens the HiBy's library | shows the **computer** (the linked folders): the owner's "top part" |
| Device **Music** tab | music folders + selection + plan | renamed **Sync**: one row per kind, the same selection UI per kind |
| Device **Library** tab | the HiBy's library (lib-index / lib-scan) | unchanged, plus the **Library database** panel (§10.3) |
| Device **Backup** tab | – | new, after Library |

### 10.1 Library (sidebar: the computer)

- **Views:** the branch's albums grid, artists, songs table and playlists, fed by `music-scan` of the links instead of `lib-index`.
- **Toolbar:**
  - **Rebuild** (re-scan the folders);
  - **Tidy up** (badge: the number of proposals);
  - **Sync this kind**.
- **Get Info sheet:** edit fields (`meta-set`), cover (find online, from file), lyrics (find, edit, synced badge).
- **Sidebar:** Genres (Genre manager tree).
- **Tidy up states:**
  1. never analysed: "Make your library consistent", the online switch (off by default, with one sentence on what is sent), Analyse;
  2. analysing (files, then lookups "1 per second"), Cancel;
  3. summary per category (Genres 143 → 31 …), "Accept all suggested" (≥0.9), "Apply to: The HiBy's library / Also my files", and the separate costed "Rewrite tags on the HiBy";
  4. genre clusters: editable canonical combo, member chips with counts (drag out to split), reason tags, confidence dot, Accept/Ignore, Detailed ↔ Broad with a live count;
  5. album diff cards with field-by-field accept and "Find on MusicBrainz" (CAA thumbnails);
  6. covers grid;
  7. lyrics list (synced/plain/none) and editor (nudge ±0.1 s, global offset);
  8. applying, then a toast with **Undo**.
- **Playlists sidebar:**
  - sections: On this computer, On the HiBy, Smart, Mixes, Favourites (HiBy), HiBy playlists (read-only);
  - badges: ✓, ↑, ↓, !, only here, only on HiBy, ⚙, "N missing";
  - drag-to-reorder editor;
  - smart rule editor (`[field][op][value]` ⊕/⊖, Limit to…, live count);
  - "Create Radio" in the context menus;
  - "Why this song?".

### 10.2 Device › Sync tab (was Music)

- **Setup:** one row per kind:
  - folder picker (`/api/pick-folder`) → "On the HiBy: Music/";
  - Entire library / chosen artists, albums, genres (the branch's chooser);
  - layout (music only);
  - "Files only on the HiBy: Keep / Delete / Ask me";
  - "Remove unselected";
  - enable toggle.

  The Video row adds the note "No ByOS app plays video yet".
- **Buttons:** Sync Music · Sync Video · Sync Audiobooks · Sync Books · **Sync All**.
- **States:**
  - `idle` (last sync, "3 changes on this computer");
  - `scanning`, `planning`, `preview`;
  - `syncing` (overall bar, current album, ETA, Pause, Cancel);
  - `paused` (cable, Wi-Fi, USB storage, battery; Resume);
  - `building library`;
  - `done` ("Restart the HiBy to use the new library");
  - `done_with_problems` (Retry failed);
  - `locked` ("Syncing from NAME. Take over");
  - `source_unavailable` ("Your music folder isn't available. Nothing will be deleted.");
  - `blocked_db` (`library_pending` text);
  - "Faster over USB" when on Wi-Fi and the ETA is over 30 min.
- **Preview sheet:**
  - header line, e.g. "Add 312 songs (2.1 GB) · Update 4 · Move 18 · Remove 12 · 1.2 GB free after";
  - sections: To copy, To update, To move/rename, **Only on the HiBy** (grouped by sub-reason, Keep/Delete per group and per row, "Copy to computer"), **Conflicts** (Computer / HiBy / Both / Import player edits), **Can't copy** (with the reason), **Playlists** (§6), **Library** (the db-diff summary: +added / −removed, removals outside the synced folders listed by name, "N favourites re-linked", "Library too large for internal memory: keep it on the card?");
  - the Apply button states deletions in plain words: "Sync and delete 12 files from the HiBy". Over the mass-delete threshold, the user must type the count.

### 10.3 Device › Library tab: Library database panel

**States:**

- Up to date;
- Building;
- Checking;
- Preview (with the 20 % gate);
- Sending;
- Ready, restart (Discard);
- Applied (Restore previous);
- Kept old library (with the reason);
- Rolled back;
- Waiting for its card;
- Too large for internal memory (Keep the library on the card…);
- Changed on the HiBy (Sync again);
- Auto-rescan on (Turn off, with why: it would undo the fixed tags);
- Not available ("Update ByOS on the HiBy").

### 10.4 Device › Backup tab

**States:**

1. empty;
2. idle (last backup, Back Up Now, Restore…, snapshot list with Export/Delete, and "Back up automatically when connected", off, at most once a day);
3. configure sheet (grouped toggles with live sizes, "Choose items…" tree);
4. running (phase checklist, Cancel);
5. done / done with warnings;
6. restore picker with a compatibility report (green restored / yellow replayed / grey skipped / orange "N songs not on the card: sync them from Music");
7. "Confirm on the HiBy";
8. "Restart the HiBy to finish";
9. result list with Undo restore;
10. errors (old ByOS, no internal space, SD unavailable).

**Server routes** (all wrap CLI jobs; ids only, never paths):

- `/api/links`, `/api/sync/{scan,plan,apply,status,unlock}`
- `/api/meta/{proposals,genres,decide,lookup,apply,undo}`
- `/api/playlists/{state,local,save,gen,sync-plan,sync-apply}`
- `/api/db/{status,diff,unstage,restore,mode}`
- `/api/backups`, `/api/backup/{scan,make}`, `/api/restore/{plan,apply,status,undo}`

Mock scenarios for every state go into `byos-cli-mock.mjs`. Strings go into `ui/i18n.js` (EN/FR/ES).

## 11. Android UI (connected mode)

The same states and texts as the desktop, as iOS grouped-list Compose screens. Everything runs in-process through `:core`; long runs go through `TransferService`.

- **Sync screen.**
  - Link setup uses `OpenDocumentTree` + `takePersistableUriPermission(READ|WRITE)`. A lost grant shows "Choose the folder again" and trips DeleteGuard.
  - `SafLocalSource` uses `DocumentsContract` child queries with a projection (not `DocumentFile`), and `openFileDescriptor` for random access.
  - During apply: a partial wake lock and a high-performance Wi-Fi lock, plus the "Keep the phone charging" hint.
  - On `onTimeout`, the run checkpoints and goes to `paused`.
  - The preview is a full-screen sheet; conflicts open in bottom sheets.
- **Library health.** The same summary, genre cluster cards and album diff bottom sheet. Covers use `BitmapFactory`. "My files" writes through SAF with the journal; that write is not atomic, so the journal is required.
- **Playlists.** The core M3U replaces `connected/Playlists.kt`. Sections, badges and a smart editor (rule cards).
- **Backup & Restore.** A row on the device page; the store is a persisted SAF tree.
- **Library DB check.** Platform `SQLiteDatabase` `integrity_check` before sending.

---

## 12. Milestones (in order), with acceptance tests

The desktop and CLI land first for each feature; Android follows from the same core (A1–A3). Device-side steps are verified first in tests and the VM harness, then by an owner-run device check.

**M1 — Core sync identity and safety** (core + CLI, no UI)

- **Scope:**
  - `(link, rel)` identity;
  - manifest v2 per kind with v1 migration;
  - `state.json`;
  - local journal, card checkpoints, lock;
  - deterministic prepared cache;
  - `adopt` by qfp (2 × 16 KiB, cached);
  - move on dest change;
  - MOVE-aside REPLACE;
  - **DeleteGuard**;
  - `sync-resume`, `sync-status`, `sync-unlock`.
- **Accept:**
  - unit tests reproduce bugs 1–6 and pass: relink `D:\`→`E:\` gives 0 adds;
  - a kill after 50 % followed by a re-plan gives no " (2)" and no re-copy;
  - a same-size retag is detected;
  - an album-name fix produces `move`;
  - **an empty or missing source root, a changed volume id, or 11 % missing all give 0 pre-ticked deletes**;
  - an unreadable sub-folder is never "deleted";
  - fake-device fault tests: kill, drop, `E_SD_UNAVAILABLE`, `E_NO_SPACE`, and a second client sees `locked_by`;
  - the branch's v1 manifest is migrated without copying anything.

**M2 — Links, kinds, selection and player-only policy**

- **Scope:**
  - `links.json` and the `link-*` commands (with selection and remove-unselected);
  - `sync-scan/plan/apply --kind`;
  - Sync all in sequence;
  - sidecars and exclusions;
  - the five player-only sub-reasons;
  - conflicts;
  - the blocked rules;
  - `DEC.json`.
- **Accept:**
  - fixture card trees: policy `delete` pre-ticks only `deleted_on_computer`;
  - adding an exclude produces `excluded_on_computer`, not ticked;
  - `Music/Free` and anything outside `card_root` are never listed;
  - a 4.5 GB video on vfat is `blocked: too_big_fat32`;
  - a 61k-slot folder is blocked;
  - `.lrc` follows its track on a move;
  - a CUE album keeps its layout;
  - `sync-plan` on a synthetic 30k-track tree finishes in under 60 s against the fake device, and the plan file is under 2 MB.

**M3 — Desktop Sync UI**

- **Scope:** §10.2 by turning the branch's Music tab into Sync; migration from `settings.sources` and its selection; the sidebar Library switched to the computer (§10.1 views only).
- **Accept:**
  - every state reachable in `--mock` scenarios, including `source_unavailable` and the typed mass-delete confirmation;
  - the page never posts a path (server test);
  - EN/FR/ES strings present;
  - frame-by-frame screenshot review of the preview and sync animations (the project's quality bar).

**M4 — Device DB pieces, both modes**

- **Scope:**
  - `hibytag dbsnapshot/dbcheck/dbapply/apply-staged`;
  - the shim's `pending` check and exec with a time budget;
  - crash-loop rollback (2 starts, before safe mode);
  - card check (`card.json`, `WAITING_CARD`);
  - DEVICE_MERGE, DB_RESTORE, `has_bak`, caps DB_APPLY;
  - **D1 (DB paths) and caps SD_DB**.
- **Accept:**
  - PC tests with the stub (`tests/`);
  - in the VM: stage → restart → applied, in internal and SD modes;
  - a corrupt DB is refused at stage time;
  - a different card → postponed, live DB untouched;
  - power cut at each apply step → resumes or keeps the live DB;
  - a forced crash loop → rollback (last_apply 5) without entering safe mode;
  - favourites added after staging survive the merge;
  - over the budget → live untouched;
  - owner-run device test plan (back up the DB first): timing of `quick_check` and the merge on the X1600; whether the SD is mounted at constructor time; the stock player in SD-DB mode with a large library.

**M5 — PC-built library DB end to end**

- **Scope:**
  - whole-card `CardView` and the `LibraryBuilder` refactor;
  - `LibraryVerifier`, `LibraryDiff` (per root and outside the roots);
  - `db-build --plan/--card-list`, `db-verify`, `db-diff`, `db-status/unstage/restore`, `db-mode`;
  - the real `sync-apply --then rebuild` with the stale retry and the `library_pending` fallback;
  - the id policy decision (§0.3);
  - desktop §10.3.
- **Accept:**
  - golden test: rebuilding a scrubbed stock DB from its files reproduces the rows field by field (ids, ctime, COUNT, MEDIA2);
  - Python sqlite3 `integrity_check` = ok in CI;
  - ids are kept across rebuilds; a removed track's favourite is dropped, never moved to another song;
  - **rows outside the linked roots survive a sync unchanged**;
  - the 20 % removal gate triggers;
  - a 30k-track library is built and staged in SD mode;
  - on the device, after a sync and a restart, the stock library shows the new tracks with no scan, and history and favourites are intact.

**M6 — Playlists both ways**

- **Scope:**
  - core M3U unification;
  - `playlist-state` reading all player kinds;
  - `playlists.json`, the registry, the resolver, Merge3;
  - `playlist-sync-plan/apply`, `sync-plan --playlists`;
  - rows through the DB build;
  - the desktop playlists UI.
  - **M6b (optional):** `hibytag m3ureg` + LIB_M3U for playlist-only syncs.
- **Accept:**
  - Merge3 fixture suite (duplicates, both-reorder, delete vs edit);
  - an app playlist appears in the stock player after the sync's restart (owner device check, including the `.m3u8` / relative-path / `#EXTINF` questions);
  - a playlist made on the player lands in `<music link>/Playlists/`;
  - desktop and Android against the same card converge.

**M7 — Smart metadata, offline**

- **Scope:**
  - fold, similarity, blocking, union-find with cannot-link;
  - the `meta.json` overlay wired into `music-scan`, `LibraryBuilder` and newly sent copies;
  - the genre, artist and album analysers;
  - `meta-analyze/decide/genres/set`;
  - Tidy up states 1–5.
- **Accept:**
  - EN/FR/ES unit corpus: Hip-Hop / Hip Hop / hiphop merge at ≥0.9; Rap is proposed through the alias table; Electro ≠ Electronic; Prog Rock ≠ Pop Rock; Beyoncé = Beyonce; "BeyoncÃ©" is fixed;
  - 10k artist values analysed in under 10 s;
  - a rejected proposal never returns;
  - the player DB built with the overlay shows the merged genre count **with zero bytes re-sent**.

**M8 — Tag writer**

- **Scope:**
  - Kotlin FLAC/ID3 port, M4A text, Ogg reader and writer;
  - `meta-apply --files`, `meta-undo`;
  - "Import player edits";
  - the costed `--rewrite-tags`.
- **Accept:**
  - a corpus covering ID3 unsync, ID3 before FLAC, M4A with moov after mdat, multi-page Ogg comments and locked Windows files;
  - audio bytes unchanged (hash of the audio frames);
  - read-back equal;
  - Undo restores the files byte for byte.

**M9 — Online lookups**

- **Scope:**
  - the rate-limited, cached client;
  - MusicBrainz genre votes and release match;
  - CAA covers;
  - LRCLIB lyrics;
  - `meta-lookup`;
  - the covers and lyrics panels.
- **Accept:**
  - a recorded-response test suite (no live calls in CI);
  - two concurrent processes never exceed 1 MusicBrainz request/s;
  - a second analysis is served from the cache;
  - only artist, album and title strings and durations go out (request log test).

**M10 — Playlist generation**

- **Scope:**
  - `plays-pull`;
  - the rules engine;
  - `playlist-gen --rules/--radio/--mix` (hibymix port);
  - regenerating smart playlists on sync;
  - the smart editor and Create Radio.
- **Accept:**
  - on the same catalogue, the Kotlin port reproduces hibymix's output with the same seed;
  - artist and album caps are respected;
  - the same seed gives the same list;
  - rule-engine table tests.

**M11 — Backup**

- **Scope:**
  - core Manifest, BackupStore, the `user.ini` decoder (to `settings_kv`);
  - `hibytag bundle make`, BACKUP_MAKE, caps BACKUP;
  - `backup-scan/make/ls/info/prune`;
  - the desktop Backup tab (backup half).
- **Accept:**
  - path test vectors: deny-list beats allow-list, symlinks refused, `device.key` and `override/` never in a bundle;
  - a second backup copies only new blobs;
  - `.byosbackup` round-trips;
  - `user.ini` double-read under concurrent writes in the VM.

**M12 — Restore**

- **Scope:**
  - RESTORE_* ops, the device prompt, extraction to internal staging;
  - the applier steps and journal;
  - shell replay, result card, Restart now;
  - `restore-plan/apply/status/undo`;
  - the restore UI.
- **Accept:**
  - in the VM: reset the 24 MB tmpfs `/usr/data` → restore → all P items equal;
  - F settings on another firmware fingerprint are replayed, never written raw;
  - a bundle with an off-list path → `E_RESTORE_INVALID`;
  - power cut at every step;
  - Deny on the device → `device_declined`;
  - Undo works;
  - owner device test on a factory-reset unit.

**A1–A3 — Android parity** (A1 can start after M3; A2 after M6; A3 after M12)

- **A1:** `SafLocalSource` and the Sync screen (with DeleteGuard on grant loss).
- **A2:** Library health, playlists, DB check.
- **A3:** Backup & Restore.
- **Accept:**
  - the same fixture plans as the CLI give identical results;
  - 10k-file SAF scan in under 60 s;
  - grants survive a restart;
  - `onTimeout` pauses cleanly.

**M14 — Later**

- D2/D3, then D4/D5 if the measurements need them;
- the on-device play journal and "Add to playlist";
- stock playlist and favourites write-back;
- CUE;
- Wi-Fi networks in backups (needs an encrypted path that never leaves plaintext on the SD);
- Bluetooth pairings (same device only).

---

## 13. Dependency approvals needed from the owner

**No new code libraries are needed.** Everything uses:

- the JDK (`java.util.zip`, `HttpURLConnection`, `java.awt`);
- the Android platform (`android.database.sqlite`, `DocumentsContract`, `BitmapFactory`);
- the existing pure-Kotlin SQLite reader and writer;
- the player's existing libsqlite3 through hibytag.

Items to approve:

1. **Bundle MusicBrainz genre data** (`mb-genres.txt`, plus the genre-relationship parents if they are easy to extract). The data is CC0; it gets an entry in THIRD_PARTY_NOTICES as data.
2. **Use the online services** MusicBrainz, Cover Art Archive and LRCLIB from the PC and phone (already named in the spec). Opt-in and rate-limited.
3. **Optional:** raise `desktop/package.json` `engines` to `>=22.13` to use the built-in `node:sqlite` as an extra DB check. Still no npm dependencies. Otherwise it is used only when present.
4. **Optional, not recommended for v1:** AcoustID with `fpcalc` (Chromaprint, LGPL-2.1; static builds include FFmpeg). It would use the user's own `fpcalc`, never bundled, and needs a registered ByOS API key; the data is CC BY-SA. Desktop only.
5. **Hand-curated data files** written by us: genre aliases EN/FR/ES, the parents map, the genre → mood table. No approval needed beyond review.

Explicitly rejected:

- xerial sqlite-jdbc;
- Mutagen or any GPL code;
- iTunes Search, Discogs and Last.fm (terms or keys).

---

## 14. Risks

1. **The device DB pieces are missing** (`dbsnapshot`, `dbcheck`, the apply step). "The sync builds the DB" is not end to end until M4. Mitigation: the `library_pending` fallback; M4 is scheduled early.
2. **Large libraries depend on SD-DB mode** (D1 plus the stock behaviour in that mode). If the stock player misbehaves in SD-DB mode, libraries over about 9k tracks stay on `library_pending`. **See Q1.**
3. **Mass deletion from a missing source.** Mitigation: DeleteGuard, "unreadable ≠ missing", default Keep, typed confirmation.
4. **A staged DB meets a different card.** Mitigation: `card_id` check, postponed apply, `.bak`.
5. **Metadata fixes undone by a stock rescan** (fixes live in the DB, not in the card files). Mitigation: the auto-rescan warning, D3, the costed "Rewrite tags on the HiBy", fixes applied to every newly sent copy.
6. **The player rewrites synced files** (tag editor, setcover, lyrics) and downloads into linked roots. Mitigation: `card_changed` conflicts, the sub-reasons, default Keep, D3.
7. **Several clients on one card.** Mitigation: the lock, origin tracking and the shared `playlists.json`.
8. **False metadata merges.** Mitigation: cannot-link lists, review before apply, remembered rejections.
9. **File damage from tag writing.** Mitigation: off by default, temp + rename, read-back, journal and Undo.
10. **Boot-path code.** The shim exec and the applier run before `main()`. Mitigation: marker-only fast path, a time budget, journalling, rollback before safe mode, and the existing recovery partition.
11. **Restore writes to `/usr/data`.** Mitigation: compiled-in allow-list and deny-list, checked at stage time and again at apply; on-device confirmation; no code paths, no `override/`, no raw `user.ini`. Wi-Fi ADB already bypasses all of this (SYNC_PROTOCOL §20).
12. **Correlated bugs:** the Kotlin SQLite writer and reader share an author. Mitigation: the independent-engine checks plus the device `dbcheck` are mandatory.
13. **Wi-Fi throughput and encryption cost on the X1600 are unmeasured;** first-adopt fingerprinting and tag rewrites scale with it. Mitigation: the plan shows the bytes; large jobs are steered to USB.
14. **Unverified FAT behaviour:** mtime and timezone round-trip, MOVE on vfat, case folding, large folders.
15. **Unknown stock semantics:** how the stock player reads `.m3u8` and `#EXTINF`; whether stock tolerates id gaps; whether stock search needs SEARCH_TABLE; the PLAYLIST_TABLE path format; CUE rows; the image cache keyed by id.

## 15. Open questions (for the owner, or for a device check)

1. **Large libraries.** OK to schedule SD-DB mode (D1 + device checks) in M4 as the path above about 9k tracks? How many tracks is the owner's library?
2. **Video.** Ship video sync in v1 as a file mirror only, even though no ByOS app plays video (and it is unknown whether the stock player does)?
3. **Default layout** for new music links: `artist-album` (tidy card) or `keep` (mirrors the PC folders)?
4. **Metadata default.** Is "fix the HiBy's library only" right, with "Also my files" and "Rewrite tags on the HiBy" as opt-ins?
5. **Genre language** for canonical names: English MusicBrainz names, or the library's majority form (e.g. "Clásica")?
6. **Automatic backup on connect** (at most daily): off by default, as proposed?
7. **Mass-delete threshold:** 10 % or 200 files per link. Acceptable?
8. **Device checks to schedule** (owner-run, after the VM tests; never launch the browser):
   - whether the SD is mounted when the shim constructor runs;
   - the stock player in SD-DB mode;
   - stock `.m3u8` handling;
   - id gaps and an empty SEARCH_TABLE in the stock UI;
   - the PLAYLIST_TABLE format (snapshot after creating one stock playlist);
   - FAT mtime and timezone;
   - MOVE on vfat;
   - `quick_check` and merge time;
   - USB and Wi-Fi throughput;
   - whether the stock factory reset wipes `/usr/data/hibymod`;
   - whether stock rescans at boot when `update`=1.
9. **AcoustID.** Is it wanted at all? It is not in the plan unless approved (§13.4).

---

# Changes made to the draft

1. **Whole-card DB.** Rows outside the linked roots are carried over from the snapshot. LibraryDiff reports per root and lists removals outside the roots. Added `removed_outside_roots`.
2. **SD-DB mode** moved from "later" to M4/M5 as the large-library path. Added D1 (hard-coded DB paths), caps bit 14 SD_DB, `db-mode`, `library.mode/fits` in the plan, and staging on the SD.
3. **DeleteGuard:** source availability, volume id, a 10 % / 200-file threshold, and "unreadable ≠ missing". Added `--allow-mass-delete`, the typed confirmation and the `source_unavailable` state. Added the `excluded_on_computer` sub-reason (never pre-ticked).
4. **Kept the branch's selective sync** (`selection`, `remove_unselected`) in `Link`, the CLI and the UI.
5. **Card identity:** `ByOS/sync/card.json`, `card_id` in DB_STAGE and `staged.json`, apply postponed (`last_apply 6 WAITING_CARD`).
6. **Applier moved** from `hibysync --apply-staged` to `hibytag apply-staged`. The shim checks only `sync/pending`. 10 s budget; full `integrity_check` moved to stage time. Rollback before ByOS safe mode.
7. **Metadata is DB-first.** No silent re-upload of card files. Fixes go into newly sent copies; an opt-in, costed `--rewrite-tags`; auto-rescan warning; covers and lyrics as sidecars.
8. **qfp reduced** to 2 × 16 KiB, computed only on size match and cached; the cost is shown in the plan.
9. **Backup and restore redesigned** around device-made ustar bundles on the SD (BACKUP_MAKE / RESTORE_STAGE with an SD path). NS_DATA, NS_RESTORE and the GET SECRET flag removed. Wi-Fi and Bluetooth deferred. No raw `user.ini` write and no `override/config.json` regeneration in v1. Op numbers re-allocated (0x62–0x68).
10. **Id policy decided:** never reuse ids if stock tolerates gaps (verified by test and device check); otherwise 1..N with the user rows of removed tracks deleted, never re-pointed.
11. **Added §0.4**, the explicit differences from hiby-mods `Update_Database.py`.
12. **Desktop mapping fixed to the real branch.** Sidebar Library = computer; the Music tab becomes Sync; the device Library tab gets the DB panel; a Backup tab is added. The branch reference now includes 6ba7e85.
13. **CLI:** the full list of existing commands; `playlist-*` extended instead of a parallel `pl-*`; new warnings, prompts and the `db-mode` command.
14. **Sync all:** kinds run in sequence under one lock, with one playlist step and one DB build at the end. `skip` items are counted, not listed.
15. **LIB_M3U** is optional (M6b): playlist rows come with the DB build.
16. **Milestones, risks and questions updated:** DeleteGuard in M1, SD-DB/card check in M4, whole-card acceptance in M5, "zero bytes re-sent" in M7, the threshold question, the SD-at-constructor device check.
