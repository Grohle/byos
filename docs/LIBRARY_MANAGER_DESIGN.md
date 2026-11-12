# ByOS library manager and player: design

Status: design, 12 Nov 2026. Final version, merging three working parts (Windows app, Android app, edit pipeline) and the completeness review of those parts. Where the parts disagreed, this document states one decision. Anything not decided here is listed in §10.

Read with:

- `docs/SYNC_LIBRARY_DESIGN.md` (called **SLD** below): links and kinds (§2), the sync engine with DeleteGuard, manifests and plan/apply (§3), the PC-built library DB (§4), smart metadata `core/meta` (§5), playlists `core/playlists` (§6), protocol allocations (§8.1), CLI (§9), desktop UI (§10), Android UI (§11), milestones M1–M12 (§12) and the **Owner decisions** of 12 Nov 2026.
- `docs/SYNC_PROTOCOL.md` (BTP/1), `docs/BYOS_CLI.md` (JSON-lines CLI), `docs/DESKTOP_APP.md`, `docs/LYRICS_AND_COVERS.md`.

---

## 0. Scope and the owner's request

### 0.1 The request (12 Nov 2026)

> The Windows app must play music with an iTunes-style player and manage the libraries like iTunes: edit data in bulk on a selection, an album, a genre or an artist; change covers; search lyrics for any selection or artist. The phone app gets a library manager too. Changes must be updated live on the HiBy through sync, or, as they are made, ask whether to sync them to the device. Basically a library manager for audiobooks, ebooks and music.

### 0.2 What this document covers

| Area | In v1 | Later (v1.1+) |
|---|---|---|
| Windows player | Play, pause, queue (Up Next), shuffle, repeat, LCD, Now Playing with lyrics, mini player, keyboard and media keys, ReplayGain, audiobook speed and positions | Gapless engine (WebCodecs), DSD → PCM, crossfade, output device picker |
| Windows library manager | Songs/Albums/Artists/Genres/Audiobooks/Books views at 50,000 tracks, Get Info for 1 or N items, bulk edit of a selection, album, artist or genre, rename and merge, helpers (number tracks, from file name, case), artwork, lyrics, Tidy Up, History with Undo, Add to Library, playlists, Remove from Library | Find duplicates, "keep my folder organised" |
| Android library manager | Browse the HiBy and the phone library, search, the same edits, covers, lyrics, playlists, pending changes, offline queue | Background delivery while the app is closed |
| Player (HiBy) | Tag edit jobs run on the device (no audio re-sent), live DB refresh, edits made on the player reported to the PC/phone, book positions read | Writing "finished"/progress from the PC/phone |
| Sync of edits | Three modes: **Update the HiBy right away**, **Ask me** (default), **At the next sync** | – |
| Kinds | Music, audiobooks, ebooks (EPUB, TXT) | – |

### 0.3 Owner decisions that shape this design

1. **More than 30,000 tracks**: everything is sized and tested for **at least 50,000**.
2. **Fixes go to the HiBy library and to the PC/phone files by default.** With the new request, this document reads decision 2 as follows (to be confirmed, §10 Q1):
   - **Retagging on the device** (hibytag rewrites tags in the card file; no audio is sent) may happen live, in "Update the HiBy right away" mode.
   - **Re-sending audio is never automatic.** Any path that re-sends file bytes (M4A text edits on the card, EPUB rewrites, a device without the EDIT capability) always goes through a prompt that shows the cost, in every mode.
3. **MusicBrainz, Cover Art Archive, LRCLIB and the CC0 genre list** are approved, opt-in and rate-limited.
4. **Artist/Album layout** for new music links.

### 0.4 Hard constraints

- No new third-party dependencies (§8).
- The HiBy is MIPS with 57 MB RAM: device work runs one file at a time, with a peak RSS under 4 MB.
- Edits are safe: undo, backups of every changed tag region, never lose a tag, never change audio bytes.
- The existing sync engine is respected: DeleteGuard, manifest-v2, plan → preview → apply, the card lock, and the PC-built DB flow (SLD §4).
- Public text is in English, with French and Spanish translations shipped at the same time.

### 0.5 Section numbering inside SLD

This document is self-contained. When it is merged into SLD, it becomes three sections: **§16 Edit pipeline** (sections 2, 5, 6 here), **§17 Windows library manager and player** (section 3), **§18 Android library manager** (section 4). The protocol rows of §5.1 are added to SLD §8.1 and SYNC_PROTOCOL.md in the same change.

---

## 1. Architecture and where code runs

### 1.1 One pipeline, three front ends

```
 Windows page (desktop/ui)          Android Compose UI                  HiBy shell (tag editor, lyrics, covers)
   media.js, player.js,               ui/library/*                         writes EDIT_LOG records (§5.4)
   getinfo.js, history.js                 │                                          │
        │ ids only                        │ in-process                               │
 server.mjs (Node runner)                 │                                          │
   /api/play Range, uploads,              │                                          │
   live-mode agent, SSE                   │                                          │
        │ JSON lines                      │                                          │
 byos-cli serve (JVM)  ───────── :core (Kotlin, shared) ─────────                    │
   EditStore · PcWriter · TagWriter · Delivery · Online · CoverImage · EpubMeta      │
        │                                                                            │
        └──────────── BTP/1 over USB (adb forward) or Wi-Fi ─────────── hibysync ── hibytag edit-apply
                                                                                    (async child, nice 10)
```

- **Every change** (a Get Info field, a renumber, a cover, lyrics, a Tidy Up proposal, an undo, an import of edits made elsewhere) becomes one **Edit** in the Kotlin `EditStore` (§2). There is no second path in Node, in the page or in the Android UI.
- **The page and the phone UI only build edit requests and show results.**

### 1.2 Where each kind of work runs

| Work | Runs in | Why |
|---|---|---|
| Views, sort, filter, selection, "Mixed" computation, helper previews, playback, queue | Windows page (`desktop/ui`) / Compose UI | Interactive, in memory |
| Audio and artwork serving (HTTP Range), id → file mapping, upload temp files, `player.json`, Recycle Bin, Show in Explorer, live-mode agent | `desktop/server.mjs` | Already owns the token, files and processes |
| Tag read/write, edit log, overlay, backups, read-back, EditGuard, Tidy Up, online lookups, image processing, EPUB rewrite, catalogue patches, delivery to the device | `:core` (via `byos-cli serve` on Windows; in-process on Android) | One implementation for both clients. Node keeps no music logic and no npm dependencies |
| Phone catalogue (`catalog.db`, FTS) | Android app (`android.database.sqlite`) | Paged lists at 50k without holding the library in memory |
| Tag edit jobs on card files, DB row refresh, card backups | HiBy: `hibytag edit-apply`, started by `hibysync` | No audio over the wire; the device already has safe tag writers |

### 1.3 Security rules (unchanged, restated)

- The Windows page sends **ids only**: track ids, album keys, edit ids, upload tokens. `server.mjs` and byos-cli turn them into files.
- `/api/play` uses the existing `byos_<port>` cookie, which `server.mjs` already accepts, so `<audio src>` works unchanged. **Never add `?token=` to a media URL**: it would leak through Media Session metadata and logs.
- CSP gains `media-src 'self'; worker-src 'self'`. `img-src` stays `'self' data:`: online thumbnails are fetched by the CLI and served from `/api/art`, so the page never contacts a foreign host.
- On Android, files are reached only through SAF grants; covers from the gallery use the Photo Picker. No new permissions.

### 1.4 Long-lived CLI session

`byos-cli serve --config DIR` reads one JSON request per stdin line (`{"id":7,"cmd":"edit-preview","args":{…}}`) and writes the usual JSON-lines events tagged `"req":7`.

- It keeps the catalogue, overlay and fold indexes in memory (about 60 MB of JVM heap at 50k tracks) and saves the 0.4–0.8 s JVM start per edit, which matters for inline edits and live previews.
- `server.mjs` starts it lazily for LOCAL library commands and restarts it if it dies (the request in flight ends as `internal`, as today).
- **`serve` is the only writer of the edit store.** Device commands (`edit-push`, sync) still run as separate processes on the device queue so a sync never blocks editing. They take `edits.lock` and re-read the index before they write delivery state, and `serve` re-reads after they release it (it watches the index mtime). This resolves the two-writer problem.
- The existing watchdog, cancel and "exactly one `result`" rules apply per request.

---

## 2. Edit model and pipeline (shared by Windows, Android and the player)

### 2.1 Identity keys

| Key | Meaning |
|---|---|
| `t:<link_id>/<rel>` | A track with a source file in a linked folder (SLD §2.1). Primary key |
| `c:<card path>` | A track that exists only on the HiBy (player-only, `Music/Free`, outside the roots), or a HiBy track edited from the phone without a local copy. SD-relative, NFC, the DB path without `a:\` |
| `b:<link_id>/<rel>` or `b:c:<card path>` | An audiobook folder or an ebook file. An audiobook edit fans out to its chapter files (`t:`/`c:` keys) |

- `t:` ↔ card path goes through `state.json` and manifest-v2.
- **Re-keying:** when the sync engine detects a local rename (SLD §3.3.1) or a card move, the edit store re-keys pending entries in the same transaction that writes `state.json`.
- **Page alias:** the Windows page uses `tid` = the first 16 hex digits of `sha1(link_id + "/" + rel)` as a short alias of a `t:` key. It is never used as a storage key.
- `(size, mtime)` of the PC file and the card file are recorded as **hints**, not identity (§2.5).

### 2.2 The edit log

Storage, the same layout on Windows (`<config>/library/edits/`) and Android (`<files>/library/edits/`):

```
edits/log-<seg>.jsonl        append-only; one line per committed edit; fsync per line; 8 MB segments
edits/index.json             eid → {segment, offset}; per-destination pending sets; watermarks; post-write ledger
                             (rebuilt from the log if lost)
edits/backup/<eid>/…         tag-region backups of PC/phone files (§2.7)
edits/cas/<sha256>           de-duplicated blobs: pictures, cover images, .lrc texts
edits/edits.lock             taken by any process that writes the index besides serve
sync/<device_id>/edits.json  per-device delivery state: {applied_rev, jobs:{job_id: …}, imported:{segment watermarks}}
```

One edit:

```json
{"eid":"e_01J…","rev":4123,"time":1762950000,"by":"desktop:<client_id>","kind":"music|audiobook|ebook",
 "label":"Genre: Hip-Hop → Hip Hop (812 songs)",
 "target":{"how":"selection|album|artist|albumartist|genre|query|book","desc":"genre=Hip-Hop",
           "keys":["t:a1b2/Kanye West/…/01.flac","…"]},
 "set":{"GENRE":"Hip Hop"},
 "per":{"t:a1b2/…/01.flac":{"TRACKNUMBER":"3","TRACKTOTAL":"12"}},
 "base":{"GENRE":{"Hip-Hop":["#0-#790"],"hip-hop":["#791-#811"]}},
 "cover":{"blob":"cas:<sha>","w":1200,"h":1200,"embed":true,"albumImage":false},
 "lyrics":{"t:a1b2/…/01.flac":{"blob":"cas:<sha>","synced":true,"source":"lrclib:123"}},
 "book":{"title":"…","author":"…","narrator":"…","series":"…","series_index":"2","description":"…"},
 "move":{"t:…":"new rel"},
 "dests":{"pc":"pending|done|partial|failed|n/a","card":"pending|…","db":"pending|…"},
 "undo_of":null}
```

Rules:

- **Targets are resolved at commit.** "All songs by X" becomes an explicit key list; songs added later are never touched by surprise. A rule that keeps applying is a Tidy Up `rules.*` entry or a smart playlist, not an edit.
- **Values, not deltas.** `set`/`per` hold final values; `""` deletes a field. Replaying a half-delivered edit writes the same bytes again, and the device answers `ALREADY` for done files.
- **`base`** holds the value each target had when the edit was made, grouped by value with key index ranges. It is used for undo and for the field-level conflict check (§2.6). A 50k-track genre rename costs about 3–6 MB of log.
- **The overlay (`meta.json`, SLD §5.1) is a derived cache**: it holds values that no file holds yet (PC write pending or impossible, `c:` keys, DB-only formats, Tidy Up rules not yet materialised). When a PC file is written and read back equal, its overlay entry is dropped. The PC-built DB (SLD §4) still reads tags + overlay, so it always sees the newest values.
- **Revisions:** `rev` is a per-client monotonic counter. The catalogue, the DB builder and each device record the highest `rev` they include (watermarks).
- **Compaction:** an edit that is done everywhere and is older than 90 days, or beyond the newest 500 edits, loses its `base` and backups. History then shows "Undo no longer available".
- **The Android "outbox" is not a separate store.** It is the per-device pending view of this log (`sync/<device>/edits.json` watermarks + job states). It survives app kills and phone restarts because the log does.

### 2.3 Edit operations

| Operation | Fields / effect | Where it can be written |
|---|---|---|
| Set fields | TITLE, ARTIST, ALBUM, ALBUMARTIST, GENRE, DATE, TRACKNUMBER/TRACKTOTAL, DISCNUMBER/DISCTOTAL, COMPOSER, GROUPING, COMMENT, COMPILATION, BPM, sort fields, any other text key | See the format table §2.4 |
| Per-item values (helpers) | Number tracks in order, title from file name, change case, swap artist/title, move "feat. X" to the title, find & replace (regex, previewed), album artist = most common artist | Computed in the UI preview, stored as `per` values |
| Clear field | `""` via the field's Clear action (typing nothing is not clearing) | as Set |
| Cover | New front image (file, paste, drag, Photo Picker, CAA candidate, another song's art) or remove | Embedded front cover (default on); album image `cover.jpg` (default off) |
| Lyrics | Set synced or plain lyrics, clear | Synced → `<track>.lrc`; plain → embedded (§2.8) |
| Book fields | Audiobook: title, author, narrator, series, series index, year, genre, description, cover. Ebook (EPUB): title, author, language, series, series index, cover | §2.9 |
| Media Kind | Music ↔ Audiobook: a file move between links on the PC; on the card only through the sync plan | PC immediately; card at next sync (move item) |
| Rename/merge artist, album artist, album, genre | A Tidy Up `rules.*` entry (so later files with the old spelling map too) **plus** per-track `set` for every matching track | as Set |

**Command names** (the `edit-*` family; replaces the conflicting names in the working parts):

```
edit-preview --target sel:IDS.json|album:KEY|artist:NAME|albumartist:NAME|genre:NAME|book:ID|query:JSON
             [--set K=V]… [--per PER.json] [--clear K]… [--cover TOKEN|--cover-candidate ID|--cover-from TID|--cover-remove]
             [--album-image] [--lyrics FILE|--lyrics-candidate ID|--lyrics-clear] [--book K=V]… [--kind music|audiobooks]
             → result {preview_id, files, by_field:[{key,old,count}], guard:{level,reason,count}, cost:{pc,hiby,backup_bytes},
                       unsupported:[{key,reason}], sample}
edit-commit PREVIEW_ID [--confirm-count N]                 → events `edit`, `progress op "write"`; result {eid, written, failed, pending}
edit-push [--device D] [--eids …|--pending] [--yes] [--mass-ok]   → `progress op "retag"`, `conflict`; result per eid
edit-status [--job ID] | edit-cancel [--job ID]
edit-ls [--pending] [--device D] | edit-show EID | edit-undo EID [--force] | edit-redo EID
edit-revert-files EID [--hiby] | edit-import [--device D] [--yes] | edit-backups [--prune]
lyrics-find --target … | cover-find --target …             → `entry` candidates (online, rate-limited)
library-import --kind K [--link ID] FILES… | --from-list LIST.json   → copy into the link with its layout (§3.8)
book-info ID
sync-track-status --device D
play-prepare --device D PATH
serve
```

- `meta-set ID KEY=VALUE…` stays as an alias that runs preview + commit. `meta-undo JOURNAL` stays only for old Tidy Up journals. BYOS_CLI.md is updated in one pass.
- New progress ops: `write`, `verify`, `retag`, `lookup`, `import`. New warnings: `edit_conflict`, `edit_deferred_now_playing`, `edit_pc_in_use`, `edit_source_unavailable`, `edit_db_only`, `edit_replay_pending`, `edit_waiting_lock`, `edit_resend_needed`. New prompts: `confirm_mass_edit`, `deliver_now`. New errors: `mass_edit_blocked`, `edit_invalid`, `unsupported_format`, `edits_not_imported`, `volume_readonly`.

### 2.4 Formats: what can be read and written

The catalogue must list every audio file. `TagReader` today throws on Ogg, Opus, APE, WMA, DFF, AIFF and ISO although `AUDIO_EXT` lists them, so those files are missing from the PC catalogue. **Decision:** milestone LM7 adds own-code readers for Ogg/Opus Vorbis comments, APEv2 (APE, WV), ASF (WMA), DFF/DIIN and the AIFF ID3 chunk. Until a reader exists, a file appears as a filename-only row ("Tags not read") so the library count matches the disk.

| Format | PC/phone file (Kotlin TagWriter, SLD M8) | Card file (hibytag on the device) | Cover | Plain lyrics |
|---|---|---|---|---|
| FLAC | write | write | embed (PICTURE type 3) | embedded `LYRICS` |
| MP3 ID3v2.3/2.4 | write | write (except ID3v2.3 with global unsync → `UNSUP`) | embed (APIC type 3) | `USLT` |
| MP3 ID3v2.2, APEv2/Lyrics3 footers | write, footers preserved byte for byte | `UNSUP` → DB-only | embed on PC | `USLT` on PC |
| M4A/M4B (AAC, ALAC) | write (`ilst`) | **cover: yes** (hibytag already writes `covr`, including `moov` growth and `stco` shift). **Text: not in v1** → costed re-send prompt or DB-only. An `ilst` text writer in hibytag is optional work (§9 LM-opt, §10 Q6) | `covr` | `©lyr` |
| Ogg Vorbis/Opus | write (after M8 Ogg writer) | `UNSUP` → costed re-send or DB-only | `METADATA_BLOCK_PICTURE` | `LYRICS` |
| WAV, DSF, DFF, APE, WMA, AIFF | **DB-only** (overlay + library rows) | DB-only (`dbset`) | overlay only | `.lrc` without timestamps, labelled "ByOS only" |

- **DB-only** rows show the badge "Library only: a full rescan on the HiBy would undo it" (EN), and the next PC-built DB re-applies the overlay. Live device refreshes for these files use `dbset`, never `dbupdate` (which re-reads the file and would bring the old value back).
- **Corpus:** the M8 test corpus must include FLAC with a leading ID3v2, MP3 with APEv2 and Lyrics3 footers, ID3v2.3 with unsync, ID3v2.2, M4A with `moov` after `mdat`, Ogg with multi-page comment headers. A file the writer cannot handle safely is `UNSUP`, never a best-effort write.

### 2.5 From one edit to four destinations

```
 UI → edit-preview (resolve target, EditGuard, cost) → user presses Apply → edit-commit
   │
 EditStore: append log line (fsync) → overlay update → catalogue patch → `edit` event to every open view
   │
   ├─ (a) PC/phone files: PcWriter per file (precondition → tag-region backup → write → read-back → ledger)
   │
   └─ per known HiBy, according to the delivery mode (§6):
        ├─ sidecars: PUT <track>.lrc / cover.jpg / book.json (prepared, hashed)
        ├─ (b) card files: job file PUT → EDIT_APPLY → hibytag edit-apply on the device
        ├─ (c) HiBy DB: rows refreshed by the job (≤ 200-row transactions) or by the PC-built DB (large jobs)
        └─ results GET → card checkpoint journal → (d) patch the client's view of the HiBy library
```

#### (a) PC and phone files: `core/meta/PcWriter`

1. **Precondition:** the field values currently in the file must equal the edit's `base` (or the new value). `(size, mtime)` differing from the catalogue only triggers a re-read of that file; it is not a conflict by itself.
2. **Back up** the tag region (§2.7).
3. **Write:** in place inside the padding when it fits; otherwise `<name>.byos-tmp` in the same folder (8 KiB new padding), fsync, then `Files.move(ATOMIC_MOVE, REPLACE_EXISTING)` over the original.
4. **Read back** with `TagReader`: changed fields equal the new values, every other key and picture equal the old ones, and the audio-region hash is unchanged (SHA-256 of the audio region for files ≤ 64 MB; first + last 1 MiB of audio above that).
5. **Ledger:** record the post-write `(size, mtime, afp)` in `index.json › post[key]` and in `local.json`, so the next scan treats the file as unchanged and the planner can explain the change (§6.5).

Platform rules:

- **Windows locked files:** retry 3 times (0.5/1/2 s), then mark the key `pc: blocked(in_use)`; the overlay keeps the value and the UI shows "1 song is open in another app · Retry".
- **The track playing in the desktop player** is never rewritten. Its write is queued until the player moves to another track (the same rule as the device's NOWPLAYING, §5.3). We do not rely on libuv share flags plus a JVM atomic move over an open handle.
- **Android SAF** writes are not atomic: write `name.byos-tmp` as a sibling document, journal `{key, tmp_doc, backup}`, delete the original, rename tmp to the name, journal "done". On start, the journal is replayed: original gone + tmp present → finish the rename; both present → delete tmp. Providers that refuse `rw` or rename get this same path; if rename is impossible, the write is done in place under the journal and recovery uses the tag-region backup.
- **Source unavailable** (drive unplugged, lost SAF grant, DeleteGuard root checks fail): `pc` stays pending; nothing fails; the card and DB go ahead.
- **Parallelism:** 4 threads on SSD, 1 on HDD/network volumes, 1 on Android. Cancel stops at a file boundary; the edit becomes `partial` with the exact done set.

#### (b) Card files: cheapest safe method per file

| Case | Method | Cost shown |
|---|---|---|
| FLAC, MP3 (ID3v2.3 without unsync, v2.4); device has caps EDIT | Device retag by `hibytag edit-apply` | "Update 812 songs on the HiBy: about 4 min, nothing re-sent" |
| Embedded cover (any format hibytag writes, M4A included) | `C` record, after PUT of the 500 px baseline JPEG to `ByOS/sync/edits/in/` | About 30 MB of SD writes per song when the tag outgrows its padding; ETA shown |
| M4A text, Ogg/Opus, or a device without caps EDIT | Re-send of the prepared copy (PUT REPLACE) **only after a costed prompt**, never automatic; or "Library only" | Bytes and ETA |
| WMA/APE/WAV/DSF/DFF/AIFF, `UNSUP` files | `D` record → `dbset` (DB-only) | Badge "Library only" |
| Lyrics | PUT `<track>.lrc` (synced) or `S LYRICS=…` record (plain); `L` record refreshes `lrc_path` | KB |
| Album image (only when the option is on) | PUT `cover.jpg` next to the tracks; existing `folder.jpg`/`front.jpg` are left alone | KB |
| Audiobook `book.json` / `cover.jpg` | PUT REPLACE | KB |
| EPUB | Rewritten on the PC/phone, re-sent after a prompt (EPUBs are small), with the position-key record (§5.5) | MB |
| Card path change (layout Artist/Album and the artist/album changed) | **Never live in v1.** The next sync shows "Move 212 songs to match their new artist" as SLD §3.3.1 `move` items (carrying `.lrc` and sidecars), unticked when there are more than 200 | – |

- A device job **never copies the PC tag region**. It sets fields only; PC and card files differ on purpose (cover size, padding).

#### (c) HiBy library DB

| Situation | Path |
|---|---|
| Caps EDIT, no staged DB waiting, job ≤ 2,000 files | In place: `ht_db_update` (rows re-read from the rewritten file) or `dbset` (DB-only values), in transactions of **≤ 200 paths**, `busy_timeout 5000`, 50 ms pause between transactions. Drops to 64 if device check DC-E2 or DC-E7 fails |
| Job > 2,000 files, or a sync with a DB rebuild in the same run | Job flag **NODB** (tags only); the DB comes from the PC-built flow (SLD §4: build with overlay, stage, "Restart the HiBy to use the new library") |
| A staged DB is waiting (DB_STATUS staged = 1) and is older than the edit | The job appends touched paths to `replay.list`; `hibytag apply-staged` replays `dbupdate`/`dbset` for them after the swap (within its 10 s budget; the rest at the next start, journalled). The client also offers "Rebuild the waiting library". **Every push checks DB_STATUS first** |
| Device without caps EDIT | Next sync: DB-only fixes via the PC-built DB; LIB_RESCAN mode 1 for ≤ 256 files that were re-sent |

- `ht_db_update` already refreshes ARTIST/ALBUM/GENRE/ALBUM_ARTIST and COUNT_TABLE for the values it touches, so **album and artist lists update live** too. Its `SELECT COUNT(*) … WHERE col=?` has no index; DC-E7 measures it on a 50k DB (internal and SD-DB). Above 2 s per transaction, the aggregate refresh runs once per job instead of once per transaction.
- This extends the ground rule "the live DB is never written while the player runs" with a second listed exception (after hibymix M3U): `hibytag dbupdate/dbset` in ≤ 200-row transactions. The on-device tag editor already does exactly this.

#### (d) Catalogues

- **Computer library** (Windows sidebar Library, phone "On this phone"): patched from the commit. `edit` events `{eid, rev, keys, fields}` update rows and regroup only the album/artist/genre groups whose values changed. No rescan.
- **View of the HiBy library** (Windows device Library tab via `lib-index`; phone `catalog.db`): patched from the **job result** (new values, size, mtime). A full `DB_SNAPSHOT` pull happens only when the live DB changed in a way our own jobs do not explain (the client compares the DB's change with job ids and `rev` recorded in `sync/<device>/edits.json`). Each live edit therefore does **not** trigger a snapshot pull or a catalogue rebuild on the phone.
- **ByOS Music on the player** rebuilds `lib.tsv` when the DB changes. During a job it holds the rebuild and rebuilds once on `EDITDONE` (§5.3).

### 2.6 Conflicts: field-level three-way check

Every written value carries its base. A destination is written only when its current value is the base or already the new value.

| Base | Current at destination | New | Result |
|---|---|---|---|
| A | A | B | write B |
| A | B | B | already done (`ALREADY`) |
| A | C | B | **conflict**: no field of that file is written |
| any (after "Use this computer's") | * | B | write B |

- `(size, mtime)` are hints only (FAT has 2 s mtime granularity and local-time skew). A size/mtime mismatch whose fields pass becomes `card_touched`: written, and the manifest entry refreshed.
- **Merging pending edits** (several edits to the same file before delivery): the delivered job keeps the **latest value** and the **earliest base** per (key, field).
- **Conflict sheet** (Windows and phone; also in the sync preview's Conflicts section): "3 songs were changed on the HiBy since you last synced", per field *On this computer: B* / *On the HiBy: C*, with **Use this computer's**, **Use the HiBy's** (a new PC edit sets C so the PC file follows), **Edit…**, and bulk buttons.

### 2.7 Safety: backups, undo, invariants

**PC/phone backups.**

- Before the first write of a file in an edit, its **tag region** is saved to `edits/backup/<eid>/<sha1(key)>.tagbak`: FLAC metadata blocks with exact layout and padding; ID3v2 + ID3v1/APE/Lyrics3 footers; M4A `moov` without sample tables when they do not move; Ogg comment-header pages; plus `audio_start`, `audio_len` and the audio-region hash. Pictures go to `cas/`, de-duplicated (an album's 15 identical covers cost one copy).
- Typical: 2–20 KB per file plus one cover per album. Budget **2 GB** by default (Settings › Library › "Undo history: 1.8 GB · Empty"), oldest compacted edits pruned first. If an edit's backups would exceed the budget, the preview says so and asks to raise the limit or continue without byte-exact restore (field undo still works).
- Optional **full copies** for edits of ≤ 50 files ("Keep full copies of changed files", off).
- Nothing is ever written into the user's music folders as a backup (no `.byos-undo` folders in links: OneDrive/NAS sync, other players' indexing and read-only shares make that unsafe).

**Card backups.** `hibytag edit-apply` writes `tagbak` files to `SD/ByOS/sync/edits/bak/<job_id>/`, capped at **256 MB and 30 days**, oldest first; skipped when the job has NO_BACKUP or SD free space is under 512 MB (the result says `nobackup`). They matter most for `c:` (player-only) files.

**Undo.**

- **Undo** (toast after each commit, Ctrl+Z on Windows, History list on both) creates a **new edit** (`undo_of: eid`) that sets each field back to its base, with base = the undone edit's new value. Fields changed again since are listed ("4 songs were changed again afterwards: keep those changes / undo anyway").
- It is delivered like any edit (live, ask, next sync), so undo is consistent on PC, card and DB, and is itself undoable (Redo = undo of the undo).
- Undo of an edit that was never delivered still creates the inverse edit; the device then answers `ALREADY`. No special case.
- Cover undo restores the previous picture blob from CAS; lyrics undo restores or deletes the `.lrc`.
- **Revert files** (`edit-revert-files EID [--hiby]`) is a separate action for byte-exact restore from tagbak, only when the audio hash still matches. On the card it runs a job with `B <tagbak>` records.

**Invariants (each one has a test).**

1. A file is never half-written: in place = one prepared region `pwrite` + fsync; otherwise temp + fsync + rename + fsync(dir).
2. Every write is read back; a mismatch restores the tagbak and marks the file `failed: verify`.
3. Audio bytes never change (audio-region hash before and after).
4. Fields not in the edit never change; unknown frames, atoms and blocks are carried over byte for byte.
5. No destination is written when its current value is neither the base nor the new value.
6. The log line is fsynced before any file is touched.
7. The live DB changes only in ≤ 200-row transactions or through the staged, checked flow.

### 2.8 Covers and lyrics: one default for every client

| | Default | Notes |
|---|---|---|
| Embedded front cover | **On** for PC/phone files and card files | The stock player reads only embedded art. Only the front picture (type 3) is replaced; other pictures are kept |
| Album image `cover.jpg` | **Off** ("Also save cover.jpg in album folders") | Written only when the folder holds one album. `folder.jpg`/`front.jpg` are never moved or overwritten |
| Image format | **Baseline JPEG** always (`CoverImage`): PC/phone ≤ 1200 px and ≤ 1 MB; card copy 500 px (SLD art-fix rules); EXIF orientation applied, colour profile dropped | Progressive JPEGs may fail in `hibywall` and the ByOS cover cache |
| Embedding on the card above 200 files | "At the next sync" by default, with the cost line | Each rewrite is ~30 MB of SD writes |
| Synced lyrics | `<track>.lrc` next to the audio, UTF-8 with BOM, `[ar:][ti:][al:][length:]` | Stock player reads it at playback; ByOS via `lrc_core` |
| Plain lyrics | Embedded (`USLT` / `LYRICS` / `©lyr`) | The stock player shows embedded unsynced text; an untimed `.lrc` is unreliable. On DB-only formats: `.lrc` without timestamps, labelled "ByOS only" |

- Sidecars written next to synced tracks are tracked: each manifest-v2 entry gets `sidecars:[{name, sha, size}]`. Sync sends, moves and deletes sidecars with their track, DeleteGuard counts them with it, and the planner never reports them as `card_only`/`downloaded_on_player`. `.lrc` files written into PC link folders are sent by the next sync like any sidecar.
- The overlay records `albums[key].cover` (source `caa:<mbid>` or `file:<sha>`), so a rescan keeps the choice and the DB build can flag `cover-inconsistent`.

### 2.9 Audiobooks and ebooks

**Player facts (from `books.inc.c`, `books_ab.inc.c`):**

- An audiobook is a folder with `book.json` (`id`, `title`, `author`), `cover.jpg` and `NN - chapter.mp3`. The chapter index comes from the **leading number of the file name**. The position is `appdata/books/ab_<hash(id)>.pos`.
- The ebook reader opens **EPUB and TXT only** and lists **at most 600 books** (`BK_MAXB`). Ebook position, "finished" and cover cache are keyed by `bk_hash(basename, size)` (`<hash>.pos`, `<hash>.cov`) in `/usr/data/hibymod/appdata/books`, which is not on the SD.

**Decisions.**

- **`book.json` is the source of truth** for audiobooks on the player. It gains `narrator`, `series`, `series_index`, `description`; unknown keys are kept; **`id` is never changed**. Tags mirror it:

  | Field | `book.json` | Tags |
  |---|---|---|
  | Book title | `title` | ALBUM (`©alb`, TALB) |
  | Author | `author` | ARTIST + ALBUMARTIST |
  | Narrator | `narrator` | COMPOSER, plus `©nrt` (MP4), TXXX:NARRATOR (ID3), NARRATOR (Vorbis) |
  | Series / index | `series`, `series_index` | GROUPING (`©grp`/TIT1) plus SERIES / SERIES-PART |
  | Description | `description` | COMMENT (`desc` in MP4) |
  | Year, genre | – | DATE, GENRE (default "Audiobook") |

- **Chapter files are never renamed or renumbered by file name** from the library manager: "Number Tracks…" and "Fill from File Name" are tags-only for audiobooks. Chapter titles inside M4B (`chpl`/QuickTime chapters) are read (M8 adds a read-only chapter reader) but not edited in v1.
- **Media Kind music → audiobook** generates `book.json` (new `id`) and `cover.jpg` in the audiobook folder, so ByOS Audiobooks shows the book.
- **Single-file M4B** appears as an audiobook in the HiBy views only after device check **DC-B1** confirms ByOS Audiobooks plays it with chapters.
- **EPUB metadata** (`core/meta/EpubMeta`, `java.util.zip`): rewrite only `content.opf` and the cover item; `mimetype` stays the first, stored entry; update EPUB3 `meta refines` for series and author roles and `dcterms:modified`; a new cover is a manifest item with `properties="cover-image"` **and** the EPUB2 `<meta name="cover">` is updated; **DRM/encrypted EPUBs (`META-INF/encryption.xml`) are refused**; validation reopens the file with the player's own `bk_zip` code in the VM harness. The old file is the undo copy (kept in `edits/backup`, counted in the budget).
- **Ebook positions survive a rewrite**: the edit job carries a `P <oldkey> <newkey>` record and the **device** renames `<old>.pos/.cov` (the client cannot reach `/usr/data`). Until LB3 ships, EPUB edits for books on the card are offered only for books with no reading position.
- **TXT:** title and author come from the file name (`Author - Title.txt`); editing them is a rename, which reaches the card only through the sync plan (with the `P` record).
- **Other ebook formats** (PDF etc.): listed read-only with "Not readable on the HiBy". When the ebooks link exceeds 600 books, the sync preview warns "The HiBy's Books app shows the first 600" (raising or paging `BK_MAXB` in the shell is a separate shell task, §10 Q8).
- **Positions and progress:** v1 adds a read-only `POS_LIST` op (§5.1) so desktop and phone show progress and "Finished" for HiBy books. Writing "Mark as finished / Reset progress" from the PC/phone is v1.1 (an edit-job record). Desktop playback positions are separate, labelled "On this computer".

### 2.10 EditGuard (mass-edit protection)

The DeleteGuard idea applied to tags. It runs in `edit-preview` (both clients) and again on the device (`E_MASS_EDIT`).

| Trigger | Response |
|---|---|
| ≤ 50 files | Applied at once; Undo toast |
| 51–1,000 files | Confirmation sheet with the preview |
| > 1,000 files or > 5 % of the library; or a field **cleared** on > 50 files | Sheet + **type the number of songs** (Windows) / a second confirmation (phone); the device needs MASS_OK |
| Same TITLE on > 1 file, or same TRACKNUMBER twice in one album | Warning row: "All 230 songs would be called *Intro*" |
| Target from an artist/genre whose name fold-matches several spellings | The sheet lists them ("Hip-Hop (790), hip-hop (21)"), each can be unticked |
| Cover embed on > 200 card files | Cost line; HiBy default "At the next sync" |
| Some source roots unavailable | Listed as "will be updated when the folder is back"; never treated as gone |

The preview shows: per-field before → after grouped by old value with counts, a sample of 20 rows, the destinations ("Your files · The HiBy (tags + library)"), the cost per destination, files that will be Library only, and the backup size. **Nothing is written before Apply.**

### 2.11 Online lookups (shared by both clients)

- **MusicBrainz:** at most 1 request/s across processes (`mb.lock`). **Cover Art Archive:** through MB release groups. **LRCLIB:** `get` then `search`, **2 requests/s, serial**, through one cross-process `net.lock` on every client. All cached 30 days, misses included.
- Off by default; each switch names the service and exactly what is sent ("Album and artist names go to MusicBrainz and the Cover Art Archive"; "Artist, title, album and duration go to LRCLIB"). On Android, paused on metered networks (default) and in battery saver.
- **Batch defaults** are "albums missing artwork" / "songs without lyrics" in the current selection. A whole-library run (about 14 h at 1 req/s for 50k albums) shows the ETA, can be stopped and resumed, and reuses the cache.
- Results are always **proposals**: pre-ticked only for exact matches (a single release group with score ≥ 95 and matching track count; an exact LRCLIB `get`, or Jaccard ≥ 0.6 against existing text); lyrics timestamps must rise and stay inside the duration or the row cannot be ticked. Accepted proposals become one edit.
- iTunes Search API, Discogs and Last.fm are not used.

---

## 3. Windows library manager and player

### 3.0 Starting point (code read for this design)

| Piece | Today | Gap |
|---|---|---|
| `desktop/ui/media.js` | One set of views for the HiBy (`devLib` via `lib-index`) and the computer (`pcLib` via `music-scan`): albums grid opening in place, artists, a virtual Songs table (6 columns), audiobooks, playlists. HiBy row menu: Information, Add to playlist ›, Re-analyse on the HiBy, Delete from the HiBy. Computer rows: Information, read-only | No playback, no editing, no Genres/Books views. `songRows()` re-sorts with `compareNames` every render; row DOM rebuilt every scroll frame |
| `desktop/server.mjs` | Token/Host/Origin checks, cookie `byos_<port>`, CSP, job queue (LOCAL set), `/api/catalog`, `/api/art/:key`, incoming uploads | No audio route, no `media-src` |
| `core/library/Tags.kt` | TagReader for FLAC, MP4, WAV, DSF, MP3 | No Ogg/APE/ASF/DFF/AIFF readers; no text writer (M8) |

### 3.1 Information architecture

```
┌ Sidebar ─────────────────────┐┌ Toolbar ───────────────────────────────────────────────────────────┐
│ LIBRARY            (this PC) ││ ◀◀  ▶  ▶▶  ──●── 🔊   [ LCD: cover · song · artist — album ══●══ ]  ☰  🔍 │
│  Music          All ▾        │└────────────────────────────────────────────────────────────────────┘
│    Recently Added            │┌ "On This Computer" · Songs | Albums | Artists | Genres  [Tidy Up ③] ⏱ │
│    Artists · Albums · Songs  ││  ↑ 61 changes not on the HiBy · Update the HiBy · Review             │
│    Genres                    │├────────────────────────────────────────────────────────────────────┤
│  Audiobooks     All ▾        ││ content (virtualised)                                               │
│  Books          All ▾        ││                                                                     │
│ DEVICES                      ││                                                                     │
│ ▾ HiBy R3 II  ⏏ ● ↑12        ││                                                                     │
│     Music · Audiobooks ·     │├────────────────────────────────────────────────────────────────────┤
│     Books · Playlists        ││ 48,212 songs · 131.4 days · 1.62 TB · 3 selected                     │
│ PLAYLISTS                    │└────────────────────────────────────────────────────────────────────┘
│   On this computer · Smart · │
│   Mixes · On the HiBy        │
└──────────────────────────────┘
```

- **LIBRARY** is this computer: the linked folders grouped by kind. Each kind has a sub-source selector: **All** (default) or one link ("Music – D:\Music", "Music – NAS"), which answers "manage the libraries".
- **DEVICES › HiBy** opens the existing device tabs (Summary · Sync · Library · Files · Backup · Updates · Settings). Its disclosure shows the HiBy library (Music, Audiobooks, Books, Playlists) using the **same views** fed by `lib-index`; header "On HiBy R3 II". The **↑12** badge is the pending-edit count; clicking it opens History filtered to pending.
- **Status columns both ways:** computer rows show *On HiBy*; HiBy rows show *On Computer*.
- **Routes** (hash): `#computer/<kind>/<view>[/<id>]`, `#device/<id>/library/<view>`, `#nowplaying`. The `demo=` and `&frame=` review hooks cover every new state.

### 3.2 The player

**Engine (v1): element engine, no new dependency.**

- Two `HTMLAudioElement`s A and B → `MediaElementAudioSourceNode` → `GainNode` (ReplayGain, fades) → destination. The next track preloads into the idle element 15 s before the end; on `ended` the other starts in the same task (gap about 10–40 ms).
- **Format probe** at start: `canPlayType` per MIME/codec, confirmed by a tiny decode of fixtures shipped in `ui/probe/`; cached per Edge version in `player.json`. MP3, AAC, FLAC, Opus/Vorbis, WAV play natively. ALAC and WMA depend on Edge's Media Foundation path: **the probe decides**. APE, DSD and anything that fails the probe show "Can't play on this computer · Plays on the HiBy"; the file stays editable and syncable.
- **MIME map** in `/api/play`: `.m4a/.m4b → audio/mp4`, `.opus → audio/ogg; codecs=opus`, `.flac → audio/flac`, etc.
- **ReplayGain / Sound Check:** Off · Track · Album, from `REPLAYGAIN_*`, `TXXX:REPLAYGAIN_*`, `iTunNORM` (catalogue field `rg`); clipping prevented from peaks.
- **Audiobooks:** speed 0.75–2× (`preservesPitch`), skip ±15/30 s, remembered position (saved every 10 s and on pause), "Mark as finished".
- **Media Session API:** title, artist, album, art; Windows media overlay and hardware keys.
- **Honest limit** in the info popover: Windows' shared mixer may resample; bit-perfect output is not promised.

**Serving audio: `GET /api/play/:tid`.** The runner maps `tid` → `(link_id, rel)` → local path, checks the path is inside the link root after `realpath` (no symlink escape) and is an audio type, and answers `206` with Range, `Accept-Ranges: bytes`, `Cache-Control: no-store`. **HiBy-only tracks** ("Play on This Computer"): `play-prepare` GETs the file into `cache/stream/` (LRU, 2 GB) on the device queue; playback starts after 512 KB; never during a sync ("Available after the sync").

**Queue, shuffle, repeat.**

- **Up Next** popover (☰): "Playing from: *Album X*", reorderable upcoming list, Clear, History (last 100). Every track menu has **Play Next** and **Add to Up Next**; dragging rows onto the LCD adds them.
- Double-click/Enter plays from that row; the rest of the view in its current sort and filter becomes the queue.
- **Shuffle** Off · Songs · Albums (seeded; turning it off returns to the source order at the same song). **Repeat** Off · All · One.
- **Persistence** in `<settings>/player.json` via `/api/player/state` (debounced 2 s). Not `localStorage`: the runner's port, and so the origin, changes at every start.
- **Plays:** counted at 50 % or 4 min into `<config>/stats/local.json` (Plays, Last Played columns), separate from the HiBy's counts.

**LCD, Now Playing, mini player.**

- **LCD:** cover thumbnail, title, a line alternating "artist — album" and the format ("FLAC 24/96"), scrubber, elapsed / −remaining, shuffle/repeat glyphs, a progress ring when a device job runs, buttons **Lyrics** and **Up Next**.
- **Now Playing** (`#nowplaying`, Ctrl+Shift+F): large art on its dominant colour (`tintDetail`), lyrics panel (synced lines highlighted, click to seek; plain lyrics scroll; "Find Lyrics…" when none), queue on the right.
- **Mini player** (Ctrl+Shift+M): Document Picture-in-Picture (Edge 116+), 300×300 art or 320×88 strip, driven by the main document; fallback `window.open('/mini')` over `BroadcastChannel`.

**Keyboard (Windows iTunes layout).** Space play/pause (not in text fields) · Enter play selection · Ctrl+→/← next/previous · Ctrl+Alt+→/← seek ±10 s (±30 s audiobooks) · Ctrl+↑/↓ volume · Ctrl+Alt+↓ mute · Ctrl+L show current song · Ctrl+I Get Info (Ctrl+Shift+I edit album) · Ctrl+F search · Ctrl+B column browser · Ctrl+A select all · Ctrl+N / Ctrl+Shift+N new playlist / from selection · Ctrl+Alt+N smart playlist · Ctrl+Z / Ctrl+Y undo/redo · Ctrl+Shift+M mini player · Ctrl+Shift+F Now Playing · Ctrl+Shift+L lyrics · Ctrl+R Show in File Explorer · Ctrl+S shuffle · Ctrl+Shift+R repeat · Ctrl+1…6 views · Delete remove (computer) / delete from the HiBy (device) · typing letters = type-ahead. Listed translated in Help › Keyboard Shortcuts.

**After v1 (milestone LM-later):** gapless engine (own demuxers + WebCodecs `AudioDecoder` + `AudioWorklet` ring buffer, sample-exact trimming; used for album-consecutive tracks), DSD → PCM in an AudioWorklet, crossfade 0–12 s, output device picker (`setSinkId`). The owner asked for an iTunes-style player, not bit-perfect gapless, so these do not block v1.

### 3.3 Views

**Songs table.**

- Columns can be shown/hidden (header right-click), resized and reordered; saved per view in `player.json › columns`. Default: ✓ (sync tick) · Title · Time · Artist · Album · Genre · Plays · On HiBy. Optional: Album Artist, Year, Track, Disc, Composer, Grouping, Comments, Kind, Bit Rate, Sample Rate, Bit Depth, Size, Date Added, Date Modified, Last Played, Loved (HiBy favourite, read-only), Lyrics (♪ synced / ♪ plain), Artwork, Sort fields, BPM, Location, Pending (↑).
- Sort by header (second click reverses); ties fall back to album artist → album → disc → track; sort names when present, otherwise "The " ignored.
- Selection: click, Shift-range, Ctrl-toggle; right-click keeps the selection. Drag to a playlist, to the LCD, or to the HiBy row (Send to HiBy Now).
- **Inline edit:** slow second click or F2 on Title, Artist, Album, Genre, Year, Track; Tab to the next column; with several rows selected, the value goes to all after an "Apply to 12 songs?" bubble (EditGuard applies).
- **Column browser** (Ctrl+B): Genres | Artists | Albums panes with counts, narrowing the table. This is how a whole genre or artist is selected for bulk work.
- **Search** (Ctrl+F): title, artist, album, album artist, composer, genre; field tokens (`genre:rock year:199*`); accent and case folding with the same `fold()` as Kotlin, tested against the same vectors.

**Sync status column.** `sync-track-status --device D` writes one byte per track to `cache/sync/<device>/track-status.json`: ● on the HiBy and identical · ↑ changed here, not on the HiBy yet · ○ selected, not copied yet · (blank) not selected · ! conflict · ⨯ can't be copied (tooltip). The ✓ column is iTunes' sync tick bound to the link's selection; unticking writes `link-set --select …` and never deletes on its own.

**Albums, Artists, Genres.**

- Albums grid virtualised by tile rows (180 px tiles); menu: Play, Shuffle, Play Next, Add to Up Next, **Get Info (album)**, **Get Album Artwork**, **Find Lyrics**, Add to Playlist ›, Tidy Up This Album, Show in File Explorer, Send to HiBy / Don't Sync. Dropping an image on a tile sets the cover.
- Artists: list with the first album cover as portrait; right pane albums then "Appears on". Menu: Play, Shuffle, **Edit Artist…**, **Rename Artist…**, **Find Lyrics for All Songs**, **Get Missing Artwork**, Tidy Up. Dragging one artist onto another: "Merge *Beatles* into *The Beatles*?".
- Genres: canonical genres with counts and parent bucket (Detailed ↔ Broad); right pane the genre's albums; inline Tidy Up banners; rename, merge by drag, "Set parent…".

**Audiobooks and Books.** Audiobooks: grid by book; detail with chapters, remembered position (here and, via `POS_LIST`, on the HiBy), Mark as finished, speed; Get Info relabelled (Book, Author, Narrator, Series, Year, Description) per §2.9. Books: cover grid for EPUB (OPF cover) and TXT (generated cover); Get Info for EPUB per §2.9; "Open" opens the book in Windows' default app (by id). No in-app reader in v1.

**Context menus.** HiBy rows keep the existing menu with its existing labels (Information, Add to playlist ›, Re-analyse on the HiBy, Delete from the HiBy) and add **Play on This Computer**, **Copy to Computer**, and, with caps EDIT, an editable Information sheet that edits `c:` keys. Computer rows:

```
Play · Play Next · Add to Up Next
──────────
Get Info                                      Ctrl+I
Edit Album… / Edit Artist… / Edit Genre…      (scope of the clicked row)
Number Tracks…
Artwork ›  Get Album Artwork · Choose File… · Paste · Remove
Lyrics ›   Find Lyrics… · Edit · Remove
Tidy Up These Songs
──────────
Add to Playlist ›   New Playlist… / list
Create Radio
──────────
Send to HiBy Now                              (HiBy connected)
Don't Sync to HiBy                            (unticks; never deletes)
Show in File Explorer                         Ctrl+R
──────────
Undo "Genre → Rock (24 songs)"                Ctrl+Z
Remove from Library…
```

**Remove from Library…** offers **Move to Recycle Bin** (runner spawns PowerShell `Microsoft.VisualBasic.FileIO.FileSystem.DeleteFile(…, SendToRecycleBin)` with an argument array; part of Windows) and **Keep the files, just hide them** (added to the link's `exclude`). The HiBy copy is untouched until the next sync preview lists it under "Deleted on this computer" with Keep pre-selected.

### 3.4 Get Info and bulk edit sheet

One sheet for 1 song, N songs, an album, an artist, a genre or an audiobook (the existing `C.sheet`, given tabs).

```
┌──────────────────────────────────────────────────────────────────────┐
│ [cover mosaic]  Album "Kind of Blue" · Miles Davis · 5 songs          │
│                 Details | Artwork | Lyrics | Sorting | Options | File │
├──────────────────────────────────────────────────────────────────────┤
│ Title          [ Mixed                    ]  (hidden for > 1)         │
│ Artist       ● [ Miles Davis              ] ↺                         │
│ Album Artist   [ Miles Davis              ]                           │
│ Genre        ● [ Jazz ▾ ]  Use "Jazz" (412 songs)?                    │
│ Year [1959]  Track [Mixed] of [5]  Disc [1] of [1]                    │
│ Composer [Mixed]  Grouping [ ]  BPM [ ]   Compilation [–]             │
│ Comments [Mixed]                                                      │
├──────────────────────────────────────────────────────────────────────┤
│ 2 fields will change on 5 songs · 5 files here · 5 on the HiBy        │
│                                              [Cancel]  [Apply]        │
└──────────────────────────────────────────────────────────────────────┘
```

- **Mixed** (grey italic placeholder) is not a value: leaving the field alone keeps each song's own value. Editing a field marks it ● and only dotted fields are written; ↺ reverts; **Clear** (field menu) writes an empty value shown as "(empty)".
- **Footer** = the live `edit-preview` result (debounced 300 ms up to 2,000 tracks; at Apply above that), including files that will be **Library only** and EditGuard warnings.
- **Scope header** names the scope: *Edit Album…* = every track with the album key; *Edit Artist…* = artist **or** album artist fold-matches; *Edit Genre…* = canonical genre matches. The whole album, artist or genre changes, not only the visible rows.
- **Combo boxes** (Artist, Album Artist, Album, Genre, Composer) suggest library values ranked by count with folded matching; Genre also lists the bundled MusicBrainz genres. **Harmonise chip:** when a typed value folds to an existing different spelling, "Use 'Hip Hop' (412 songs)?".
- **Single-song mode** adds ◀ ▶ (pending changes applied first).
- **Tabs:** Details · Artwork (§3.5) · Lyrics (§3.6) · Sorting (written as ID3 `TSOT/TSOP/TSO2/TSOA/TSOC`, Vorbis `*SORT`, MP4 `sonm/soar/soaa/soal/soco`; note "Used on this computer. The HiBy uses its own order." until §10 Q7 is answered) · Options (Media Kind, Remember position, Skip when shuffling; desktop-only, kept in the overlay `opts`) · File (read-only: format, rates, size, location, dates, plays here / on the HiBy, card path, last synced).
- **Helpers** (previewed old → new, applied as one edit): **Number Tracks…** (drag order or by file name / current track / title; start at; set "of"; discs from folders), **Fill from File Name…** (`%n - %t`, `%a - %t`, `%n. %a - %t`; non-matching rows skipped), **Change Case…** (Title/Sentence/UPPER/lower, exceptions table from SLD §5.2), **Swap Artist ↔ Title**, **Move "feat. X" to the title**, **Find & Replace** (regex), **Compilation** (offers Album Artist = *Various Artists*, pre-ticked).
- **Apply** runs `edit-commit`. The sheet closes on success; toast "Changed 24 songs · Undo"; then the delivery step (§6). Jobs above 500 files show progress in the LCD; Cancel stops after the current file.

### 3.5 Artwork (Windows)

- In: drag and drop (Artwork tab, album tile, Now Playing cover), **Choose File…**, **Paste**, **Use the cover from another song**, **Find Online…**, **Remove Artwork**. Uploads go to `/api/meta/cover-upload` (private temp, ≤ 20 MB, type sniffed by `imageType`) and return a token.
- **Find Online…** (online switch on): MusicBrainz release-group candidates and their CAA front images; 250 px thumbnails cached by the CLI and served via `/api/art`; grid with size badge, release, year, country, track count (✓ when it matches); a refine field; preview, then **Use for This Album** (default) or **Use for Selected Songs**.
- **Batch "Get Album Artwork"** (selection, artist; whole library with ETA): pre-accepts only exact matches; the rest go to the review grid; nothing written before Apply.
- Written per §2.8.

### 3.6 Lyrics (Windows)

- **Lyrics tab:** Synced / Plain / None badge; synced table `[mm:ss.xx] line` with ±0.1 s nudges, global offset and **Tap to sync** (play, press Space at each line); plain text area with "Convert to synced"; **Search LRCLIB** (get, then search; synced/plain, duration Δ, first lines, preview).
- **Batch "Find Lyrics…"** (selection, album, artist, genre): options Prefer synced (on), Replace existing (off), Only songs without lyrics (on). Review sheet: Song · Result (Exact synced ✓ / Exact plain / Close match / Not found) · Δ duration · first line, preview pane; **Save N Lyrics**.
- Written per §2.8 (synced `.lrc`, plain embedded).

### 3.7 History and delivery UI

- **History** (Edit › Change History, or ⏱ in the header): virtual list of edits with time, label, songs, destinations (✓ done · ↑ pending · ! conflict · "Library only"), and **Undo**, **Show Songs**, **Update the HiBy**. Filter "Not on the HiBy yet".
- **Pending bar** under the header while anything is pending: "↑ 61 changes not on the HiBy · Update the HiBy · Review".
- **Ask banner** (non-blocking, §6.1) and **Conflict sheet** (§2.6), **Bring N changes made on the HiBy** sheet (§5.4).

### 3.8 Add to Library and playlists

- **Add to Library** (drag files or folders onto the window, or File › Add to Library…): `library-import --kind music|audiobooks|ebooks` copies them into the chosen link using its layout (Artist/Album for music; `Author/Book` for audiobooks), applies the overlay rules (Tidy Up `rules.*`), and reports duplicates (same tag key + duration ±2 s, or same audio hash) with **Skip**/**Add anyway**. The originals are never moved.
- **Playlists on this computer** use SLD §6 `core/playlists`: New, New from selection (Ctrl+Shift+N), reorder by drag, remove, rename, delete, smart playlists (rule cards with a live count), Create Radio, and export to the HiBy through the same delivery as edits (`playlist-save` → PUT `Playlists/<name>.m3u8`, then `playlists.json`; with caps LIB_M3U registered live). The Android spec in §4.6 is the shared behaviour.

### 3.9 Server routes (ids only)

`/api/play/:tid` (Range) · `/api/player/state` · `/api/edits/{preview,commit,push,status,cancel,list,undo,redo,revert,import}` · `/api/edits/events` (SSE: `edit` events, pending count, job progress) · `/api/meta/cover-upload` · `/api/lyrics/:tid` · `/api/lyrics/find` · `/api/covers/find` · `/api/library/import` · `/api/books/:id/open` · `/api/reveal/:tid` · `/api/track-status/:device` · `/mini`.

---

## 4. Android library manager

### 4.1 Starting point and constraints

- App: one activity, `Screen` enum, Compose **foundation** with the project's iOS-style components (`ui/Components.kt`), no Material3/Navigation. `connected/ConnectedModel.kt` holds the session, worker thread and today's library (as in-memory lists: fine at 5k, not at 50k). `connected/SyncApi.kt` is the `SyncSession` interface; `TransferService` is the `dataSync` foreground service.
- Core: TagReader, PictureWriter, LibraryIndex, Sqlite, LibraryBuilder, `sync/`, `btp/`. `core/meta` (M7–M9) and `core/playlists` (M6) are prerequisites.
- **No new permissions.** Phone folders via SAF (`OpenDocumentTree`); gallery covers via the Photo Picker.

### 4.2 Architecture on the phone

```
ui/library/*  (Compose foundation)               LibState (immutable), LibActions
        │
connected/manage/LibraryManagerModel.kt ── worker (SyncEnv.execute) ── TransferService (EDIT, LOOKUP kinds)
        │                │                      │                         │
   Catalog (app)     EditStore (core)       Delivery (core)            Online (core/meta)
   catalog.db FTS    log + overlay          → SyncSession (BTP)        MB/CAA/LRCLIB, cache
        │                │
   snapshot.db     SafLocalSource + PcWriter (SAF journal)
```

- `LibraryManagerModel` is a new model next to `ConnectedModel` and **shares its session** (`ConnectedModel.withSession { s -> … }`); reconnect logic stays in one place.
- `SyncSession` gains `editApply`, `editStatus`, `editCancel`, `posList`, `dbStage`, `dbStatus`.
- `connected/Library.kt` helpers (`fold`, `cardPath`, sort keys) move into `:core`; `connected/Playlists.kt` is replaced by `core/playlists`.

**Catalogue** `<files>/catalog/<device_id>.db` (platform `SQLiteDatabase`):

```sql
item(id INTEGER PRIMARY KEY, kind TEXT, src INTEGER /*1 hiby, 2 phone, 3 both*/, ekey TEXT /*t:/c:/b: key*/,
     card TEXT, link TEXT, rel TEXT, title TEXT, artist TEXT, album_artist TEXT, album TEXT, genre TEXT,
     year INT, track INT, disc INT, duration_ms INT, size INT, mtime INT, fmt TEXT, cover_key TEXT,
     lyrics INT /*0 none, 1 plain, 2 synced*/, pending INT, sort_title TEXT, sort_artist TEXT, sort_album TEXT,
     f_title TEXT, f_artist TEXT, f_album TEXT /*folded copies for API 26-29*/)
album(id, key, title, artist, year, genre, tracks, duration_ms, cover_key)
artist(id, key, name, albums, tracks) ; genre(id, key, name, tracks)
item_fts USING fts4(title, artist, album, album_artist, genre, tokenize=unicode61 "remove_diacritics=1")
```

- Built in one transaction from the snapshot (`LibraryIndex.fromSnapshot`, streamed) and/or the phone scan (`local.json`), then the overlay. **Patched** from edit commits and job results; rebuilt only when the snapshot changed in a way our own jobs do not explain (§2.5 (d)).
- `remove_diacritics=1` needs SQLite 3.27 (Android 10+); on API 26–29 the app searches the folded `f_*` columns.
- Lists page over the cursor (ids first, rows in pages of 200). Covers come from a 256 px thumbnail disk cache (`<cache>/covers/<cover_key>.jpg`) with an LRU sized to 1/8 of the heap.

### 4.3 Sources, kinds and screens

- **Source** segmented control at the top of Library: **On the HiBy** (the DB snapshot; without a connection the last snapshot with "Last seen 2 h ago", edits still allowed and queued) · **On this phone** (the phone links, per kind, each link as a sub-source with "All" default; needs SafLocalSource, A1) · **Both** (badges ✓ on both / phone only / HiBy only; matched via manifest-v2, tag key as fallback).
- **Kinds:** Music (Albums · Artists · Songs · Genres · Playlists), Audiobooks (Books · Authors), Ebooks (Books · Authors · Series).
- **Entry points:** the device page's Library row opens `Screen.LIB` (replaces `SYNC_LIBRARY`; its delete, move, reveal and add-to-playlist actions move here). Home gets a Library row that works without a HiBy (phone source). A pill "3 changes for the HiBy" under the title opens Pending changes.
- **Browsing:** 2-column album grid (3 on tablets) with a dot when edits are pending; alphabetical lists with the stock fast-scroll letters; Songs list with title, artist · album, duration.
- **Selection mode** (long press, as in iOS Photos): checkmarks, Select all, count in the title; bottom bar **Edit**, **Cover**, **Lyrics**, **Add to playlist**, **More** (Re-analyse on the HiBy, Delete from the HiBy, Reveal in Files).
- **Context menu** (long press outside selection, or "⋯" on an album/artist/genre header): the same actions applied to the **whole** album, artist or genre.

### 4.4 Edit sheet (`LIB_EDIT`)

- Full-screen sheet, iOS grouped list, tabs **Details · Sorting · Options · Lyrics · Cover**. Same fields as Windows (§3.4); genre combo with canonical genres and library values.
- Many items: "Mixed" placeholders; a field changes only when typed in (blue check); title hidden; **Number in order**; "⋯" helpers: Capitalise (EN/FR/ES small words), Fix encoding (SLD §5.2 mojibake rule), Find & replace, Take from file names, Album artist = most common artist, Mark as compilation.
- **Preview** (always for more than one item): "Change 2 fields on 312 songs · HiBy 312, phone 298 · 3 Library only", EditGuard warnings, and a second confirmation above 1,000 files or 5 %.
- **Look up this album** (online): MusicBrainz candidates with a field-by-field diff; accepting fills the checked fields.
- Editing "On the HiBy" items without phone copies produces `c:` keys: only the card and the DB change.
- Save → `edit-commit` in-process → delivery per the setting (§6).

### 4.5 Covers and lyrics (`LIB_COVER`, `LIB_LYRICS`)

- **Cover sources:** Photo Picker (`PickVisualMedia(ImageOnly)`), Files (`OpenDocument("image/*")`), Online (CAA via MusicBrainz), From another song, Remove. Square crop (pinch and drag) with size readout and warnings (< 300 px, not square). A selection spanning several albums steps through them with "Next album". Output per §2.8 via `CoverImage` (Android implementation: `BitmapFactory`/`Bitmap.compress`, baseline JPEG).
- **Lyrics:** one song: current lyrics, Find (LRCLIB candidates, synced first, closest duration), preview, Use. Many: **Find missing lyrics** (option "Also replace plain lyrics with synced ones"), under the foreground service ("Lyrics: 84 of 312"); results grouped Found (exact, pre-ticked) · Found (check) · Not found; **Apply ticked**.
- **Editor:** plain text plus "Sync" timing mode playing the phone copy with `MediaPlayer`; HiBy-only songs offer ±0.1 s nudges and a global offset instead.

### 4.6 Playlists (`LIB_PLAYLISTS`, `LIB_PLAYLIST`)

Shared spec with Windows §3.8, on `core/playlists`: sections On the HiBy · On this phone · Smart · Mixes · Favourites (read-only) · HiBy playlists (stock, read-only); badges ✓ ↑ ↓ ! "N missing"; New from selection, Add to playlist (any selection size), drag to reorder, swipe to remove, Rename, Delete (prompt names both sides), New smart playlist (rule cards, Match all/any, Limit, live count), Create radio. Delivered through the same pending view as edits (`playlist-save`).

### 4.7 Pending changes (`LIB_OUTBOX`)

- Header "7 changes for HiBy R3 II · Not connected" with **Update the HiBy** and **Discard all** (discard = undo of each undelivered edit).
- One row per edit: label ("Genre → Hip Hop · 312 songs", "Cover · Kind of Blue", "Lyrics · 84 songs", "Playlist · Road trip"), state (waiting, sending n/N, done: live / at next library update, conflict, failed with reason, "Waiting for its card" when the HiBy has another card), actions **Undo** and **Details**.
- Conflicts open the shared conflict sheet (§2.6). A History section lists delivered edits for 30 days with **Undo**.

### 4.8 Search

FTS4 `MATCH` with folded prefix words (`word*`, any order), results grouped Artists · Albums · Songs (· Books), 5 per group with "See all". Target < 50 ms per keystroke at 50k, debounced 150 ms, previous query cancelled. Inside an artist/album/genre/playlist page the field filters that page. Field tokens (`genre:jazz year:1950-1969`) use the smart-playlist field names.

### 4.9 Android specifics

- **SAF scanning:** `DocumentsContract.buildChildDocumentsUriUsingTree` with a projection (never `DocumentFile.listFiles`); tags read through positional reads on `openFileDescriptor("r")` (only tag regions). First scan of 50k files under 8 min on a mid-range phone; later scans by size + mtime.
- **Long work** (bulk lookups, bulk phone writes, delivery over 50 files) runs under `TransferService` with new kinds EDIT and LOOKUP, a partial wake lock and a high-performance Wi-Fi lock while sending; `onTimeout` (Android 15 dataSync limit) checkpoints and shows "Paused. Tap to continue".
- **Memory:** no screen holds more than a page of rows; covers decoded with `inSampleSize` to ≤ 512 px for the editor; bulk jobs stream one file at a time.
- **Process death:** current screen, selection (as an id file above 1,000 ids) and the edit draft are saved to `<files>/manage/state.json`.
- **Delivery triggers:** connection to the device, **Update the HiBy**, the live debounce, and (Live mode, app in foreground) a retry of the paired device's last host every 60 s. No background delivery while the app is closed in v1 (§10 Q5).
- **Network disclosure:** the README section "What the app sends over the network" gets MusicBrainz, Cover Art Archive and LRCLIB, only when online lookups are on, sending artist, album, title and duration.

### 4.10 Settings (device page, group "Library")

- **When I change songs, books or covers:** Update the HiBy right away · Ask me *(default)* · At the next sync.
- **Also change my files on this phone:** on (owner decision 2); shown when a phone link exists.
- **Update files on the HiBy, not only the library:** on. "Library only" is faster, but a full rescan on the HiBy would undo it.
- **Online lookups:** off; **Only on Wi-Fi without data limits:** on.
- **Keep backups of changed files:** Tags only *(default)* · Whole files (≤ 50 files per edit, uses space).

---

## 5. Player side (device changes)

### 5.1 Protocol allocations (added to SLD §8.1 and SYNC_PROTOCOL.md)

All backward compatible: a request without the capability bit gets `E_UNSUPPORTED`.

| Kind | Value | Name | Payload / meaning |
|---|---|---|---|
| caps | bit 15 | EDIT | EDIT_* ops and the job format below |
| caps | bit 16 | EDIT_LOG | the shell writes `ByOS/library/edits/device-*.json` records for edits made on the player |
| caps | bit 17 | BOOK_POS | POS_LIST op |
| op | 0x48 | EDIT_APPLY | `flags u8, sha256 bytes[32], job str16 (NS_SD, under ByOS/sync/edits/in/)`. Starts the job as a detached helper; streams EDIT_PROGRESS; ends with EDIT_STATUS_R |
| op | 0x49 | EDIT_PROGRESS | `done u32, total u32, ok u32, conflict u32, failed u32, current str16`, at most 4 per second |
| op | 0x4A | EDIT_STATUS | `job_id str16` (empty = running or last job) → EDIT_STATUS_R; re-attaches after a drop |
| op | 0x4B | EDIT_STATUS_R | `state u8 (0 none, 1 running, 2 done, 3 failed, 4 cancelled, 5 paused_sd), done u32, total u32, result str16 (NS_SD path)` |
| op | 0x4C | EDIT_CANCEL | `job_id str16` → EDIT_STATUS_R; stops at a file boundary |
| op | 0x4D | POS_LIST | empty → `count u16, count × (key str16, kind u8 (1 ebook, 2 audiobook), percent u16 (0–10000), finished u8, mtime i64)`; reads `appdata/books/*.pos` only |
| flag | EDIT_APPLY bit 0 | NODB | tags/files only; a DB build follows |
| flag | EDIT_APPLY bit 1 | MASS_OK | required above 2,000 files; set only after the user confirmed (§2.10) |
| flag | EDIT_APPLY bit 2 | NO_BACKUP | skip card tag backups (only when the PC/phone holds the source of every file); default off |
| error | 84 | E_MASS_EDIT | job over the limits without MASS_OK |
| error | 85 | E_EDIT_INVALID | the job failed its checks (format, sha, a path outside the SD rules or under `ByOS/`, a non-audio target, a different card) |

Reused errors: E_BUSY (a job is running, or the card lock is held), E_BATTERY (< 15 % and not charging), E_SD_UNAVAILABLE, E_NO_SPACE.

The working drafts' inline TAG_EDIT/TAG_COVER ops and the JSON job file are **dropped**: an inline 256-file op would block the single-threaded daemon, and a JSON parser is not wanted on the device.

### 5.2 `hibytag` additions

```
hibytag edit-apply JOB RESULT   run a job file; resumable; prints "P done total ok conflict failed" lines
hibytag dbset LIST              DB-only values: lines "path<TAB>FIELD=value…"; same aggregate/COUNT upkeep as ht_db_update
hibytag tagbak FILE OUT         save the tag region (FLAC metadata, ID3v2 + v1/APE footers, M4A moov/udta) to OUT
hibytag tagrestore FILE BAK     put a tag region back (verifies the audio-region hash stored in BAK)
```

**Job file** `ByOS/sync/edits/in/<job_id>.tsv`: UTF-8, one record per line, TAB-separated, `\t \n \\` escaped, streamed (never loaded whole); ≤ 64 MB and ≤ 60,000 `F` records; the client sends ≤ 500 files per job for live pushes.

```
#byos-edit/1	job=<id>	client=<client_id>	rev=<rev>	card=<card_id>	files=<n>
F	<card path>	<hint_size|0>	<hint_mtime|0>
S	<KEY>	<base value, or \x1f = any>	<new value, empty = delete>
C	<card path of a prepared JPEG under ByOS/sync/edits/in/>	embed|remove
L	refresh                       (an .lrc was PUT next to the track; refresh lrc_path)
D	                              (DB-only: S values go to dbset; no tag write)
B	<tagbak path>                 (revert: restore this tag region; used by "Revert files on the HiBy")
P	<old book key>	<new book key>   (rename appdata/books/<old>.pos/.cov to <new>; ebook rewrites)
E
```

There is **no `R` (rename/move) record in v1**: card moves go through the sync plan (§2.5 (b)).

**Result file** `ByOS/sync/edits/out/<job_id>.tsv`, one line per `F`, fsynced every 50 files (the resume point):

```
<status>	<card path>	<new size>	<new mtime>	<how: inplace|rewrite|db|none>	<detail>
status: OK | ALREADY | CONFLICT (detail: KEY=current…) | MISSING | UNSUP | DEFERRED | ERR (detail: code) ; detail may add card_touched, nobackup
```

**Per file:** stat → read current values of the `S` keys → per field: current == new → nothing; current == base or base is "any" → write; otherwise `CONFLICT` and **no field of that file is written** → size/mtime hint mismatch with passing fields adds `card_touched` → tagbak (§2.7) → write with the existing safe paths (in place ≤ 128 KiB region, else temp + fsync + rename) → `C` → read back → add to the DB batch (every 200 paths one `ht_db_update`/`dbset` transaction, unless NODB) → append to `replay.list` when a staged DB waits.

**Now playing:** a file equal to the shell's `NOWPLAYING` path is moved to the end of the job and retried once; if still playing, `DEFERRED`, and the client re-queues it. (In-place writes are harmless, but a rewrite changes the inode under the decoder.)

**Resume:** after a kill, power loss or OOM, `edit-apply` reads the result file and skips finished `F` records; base checks make a re-run idempotent (`ALREADY`).

**Budgets on the 57 MB player:**

| Item | Budget |
|---|---|
| Tag write buffers | 64 KiB copy + ≤ 128 KiB in-place region (existing constants) |
| Job parsing | one line at a time, ≤ 16 KiB per value (HT_MAXVAL) |
| SQLite | `PRAGMA cache_size=-1024`, ≤ 200 rows per transaction (journal ≲ 1 MiB) |
| Peak RSS | **< 4 MB** (`VmHWM`, acceptance test) |
| OOM | `oom_score_adj` −300 (below the player, above helpers); resumable anyway |
| Speed (estimate, DC-E1) | in place ≈ 30–60 ms/file; rewrite ≈ size / 10 MB/s; 812 songs ≈ 40 s; 50k ≈ 40 min (only with MASS_OK) |
| SD space | per file: free ≥ largest file + 1 MiB, else `ERR no_space` for that file and the job continues |

### 5.3 `hibysync` and shell changes

- **D6 asynchronous helper.** EDIT_APPLY does `fork` + `setsid` + nice 10 + I/O class idle for `hibytag edit-apply JOB RESULT`; its stdout pipe joins the daemon's `poll()` set and progress lines become EDIT_PROGRESS frames. The job survives a session drop and daemon idle-exit; pid and state in `ByOS/sync/edits/running.json`. One job at a time (E_BUSY). The daemon does not idle-exit while a job runs.
- **Lock:** the job takes `ByOS/sync/lock.json` (kind `edit`, with the client's name) for its duration. While another client's sync holds the lock: E_BUSY, and the client retries every 60 s ("Waiting: Pixel 8 is syncing"). It is also refused while the shell reports `sync_xfer_busy` (D3).
- **Card check:** the job header's `card` must match the inserted card, else E_EDIT_INVALID (reason card); edits stay pending for the right card.
- **Control pipe:** daemon → shell `EDITJOB <done> <total> <client>` ("Updating songs from ALBERTO-PC…" in the status bar / Sync page) and `EDITDONE <ok> <conflict> <failed>` (the shell re-reads the now-playing tags and cover and refreshes the visible list). Shell → daemon `NOWPLAYING <path%>`.
- **ByOS Music `lib.tsv`:** the rebuild is **held while `EDITJOB` is active** and done once at `EDITDONE` (otherwise a 2,000-file job would rebuild the 50k-row file 10 times). Device check DC-E8.
- **apply-staged** replays `replay.list` after a DB swap (§2.5 (c)).
- **Pruning:** card backups per §2.7; `in/` and `out/` files older than 30 days are deleted.

### 5.4 Edits made on the player (EDIT_LOG)

- With caps EDIT_LOG, the player's tag editor, lyrics search, cover search and rename write append-only records `ByOS/library/edits/device-<boot>-<seq>.json` (`{time, path, set:{K:[old,new]}, cover, lyrics, rename}`), batched per editor save, tmp + rename on the SD.
- At connect and before each push, clients GET new records (a few KB) and turn them into edits `by:"hibyos-device"` (`c:` keys mapped to `t:` where possible). Both UIs show **"Bring 5 changes made on the HiBy to your files"** (default on in Ask mode; automatic in Live mode unless they conflict with pending local edits, which then go to the conflict sheet).
- Without EDIT_LOG (old ByOS): the existing path (`card_changed` → conflict → "Import player edits", partial GETs).

### 5.5 Book positions and keys

- **POS_LIST** (§5.1) feeds progress and "Finished" in both UIs.
- **`P` records** keep ebook positions and cover caches across an EPUB rewrite or TXT rename (the device renames `.pos/.cov` in `/usr/data/hibymod/appdata/books`). The more general fix (the reader keys by `dc:identifier` with a fallback and migrates on its own) is a shell option listed in §10 Q8.

### 5.6 Device checks (owner-run, after VM tests; the browser is never launched)

| Check | What |
|---|---|
| DC-E1 | `edit-apply` speed in place and with rewrites, 100 files each |
| DC-E2 | Stock UI shows rows changed by a live `ht_db_update` after re-entering a list; no crash with a 200-row commit while browsing |
| DC-E3 | Stock player and ByOS Music keep playing, no audible stutter, while a job rewrites a file in the same folder |
| DC-E4 | `setcover` shows in stock and ByOS views without "Update library" |
| DC-E5 | ByOS Books shows a re-sent EPUB's new title and cover, with its position kept (`P` record) |
| DC-E6 | `hibyguard` does not kill `edit-apply` at oom_score_adj −300 during playback |
| DC-E7 | Aggregate refresh cost of one 200-row transaction touching 1 and 50 distinct values on a 50k DB, internal and SD-DB |
| DC-E8 | `lib.tsv` rebuild held during a job and done once at `EDITDONE` |
| DC-B1 | ByOS Audiobooks plays a single-file M4B with chapters |
| DC-B2 | `POS_LIST` values match the books app's progress |

---

## 6. Sync of edits: live, ask, batched, and safety

### 6.1 Delivery modes (per device, same on Windows and Android)

Setting **"When I change songs, books or covers"**:

| Mode | HiBy connected | HiBy not connected |
|---|---|---|
| **Update the HiBy right away** | Pending edits are pushed **3 s after the last edit** (debounced; edits made during a push join the next one). Small edits show on the HiBy in about 2–10 s | Queued; pushed automatically at the next connection; badge "12 changes waiting for the HiBy" |
| **Ask me** *(default)* | After a batch closes (Get Info closed, a bulk action ended, or 10 s without edits): Windows shows a non-blocking banner under the toolbar, Android a bottom sheet: "**Update the HiBy?** 3 changes to 61 songs · about 20 s" with **Update the HiBy** · **Later** · "Don't ask again" (switches to Right away). Edits made while it is up merge into it | Windows: the pending bar. Android: toast "Saved. It will go to the HiBy when it's connected", then at the next connection "7 changes are waiting. Update the HiBy now?" |
| **At the next sync** | Nothing is pushed. The next Sync's preview has a **Library edits** section and its plan includes them | same |

- "Later" leaves edits pending (pending bar / pill; History filter). **"Never for this change" is not in v1**: Undo covers "I didn't want this".
- **Never automatic, in every mode:** pushes above the live limits (> 2,000 files for tags, > 200 embedded covers), and anything that re-sends audio or an EPUB. Those become a prompt with the cost: "Update 8,412 songs on the HiBy: about 35 min · Do it now / At the next sync".
- If this client is running a sync, pending edits are folded into that sync's plan instead of a separate push.

### 6.2 What a push does (`edit-push`, core `Delivery`)

1. Take `edits.lock` (Windows) / the model's worker (Android); check DB_STATUS (staged DB → `replay.list` path) and caps.
2. Merge pending edits per (key, field): latest value, earliest base (§2.6).
3. Map keys to card paths (manifest-v2 + pending card journals). Split into: sidecars, device job records, re-send candidates (prompt), DB-only, deferred moves (next sync).
4. PUT sidecars and prepared cover JPEGs (`ByOS/sync/edits/in/`), then the job file (PUT CREATE), then EDIT_APPLY; follow EDIT_PROGRESS; on a drop, re-attach with EDIT_STATUS.
5. GET the result file. For every written file, PUT CREATE a **card checkpoint journal** `ByOS/sync/journal/edit-<job>-<seq>.json` with `card_size`, `card_mtime`, `card_tags_rev`, sidecars and `origin: edit:<eid>`. **Nobody rewrites manifest-v2 outside a sync**: the next sync folds these journals like any run journal, and readers already merge pending journals (SLD §3.2).
6. Write a **card edit segment** `ByOS/library/edits/<client_id>-<seq>.json` (PUT CREATE, never rewritten) listing the delivered edits by card path, for the other client (§6.4).
7. Update `sync/<device>/edits.json` and the edit's `dests`; patch the HiBy view (§2.5 (d)); `CONFLICT` and `DEFERRED` rows return to pending (conflicts to the sheet).
8. Large sets (> 2,000 files): default "**Update the library now, files in the background**": stage a PC/phone-built DB with the overlay first (zero audio sent; "Restart the HiBy to use the new library"), then NODB jobs of 500 files while connected (on the phone, while charging). Alternative: "Library only". The client never restarts the HiBy.
9. Device without caps EDIT: sidecars still go live (PUT + LIB_RESCAN mode 1); tag edits go to the next sync's DB build as DB-only; re-sends only via the costed prompt.

### 6.3 The sync engine with edits

- **Manifest:** `card_tags_rev` added; `sent_sha` now means "audio as sent". `sidecars:[…]` added (§2.8). The prepared-copy cache key includes the overlay rev for that key, so a stale 500 px cover is never re-used.
- **Plan actions** (SLD §3.4) gain `retag` (device job; bytes 0, eta estimated), `sidecar`, `dbfix`, and the preview header "Update tags of 412 songs (no re-copy)". `move` items for layout changes (unticked above 200).
- **Explained changes:** the planner's local diff asks the ledger first. If a source file's `(size, mtime)` equals the ledger's post-write values, the change is explained: emit `retag` (or nothing if delivered), not `update`. This holds for both scanners (`music-scan` and the Android SAF scanner): phone writes record post values in the same ledger, so the phone's next sync does not re-send edited files.
- **Foreign retags** (another tagger): when `afp` (first + last 64 KiB after the tag) is unchanged but size/mtime changed, the change is tags-only: read the tags, diff against the `local.json` tag snapshot, synthesise an edit (`by:"external"`) and emit `retag`. A 30 GB foobar2000 retag costs 0 bytes of transfer.
- **The PC-built DB must not revert edits made elsewhere.** `sync-plan` and `db-build` first run `edit-import`: card edit segments from the other client and device EDIT_LOG records go into the overlay (conflicts to the sheet). `db-build` refuses with `edits_not_imported` while unread segments exist. This is an acceptance test (LM29).
- **Send to HiBy Now** (Windows ○ rows, or drag onto the HiBy row): `sync-plan --kind music --only IDS.json` (adds/updates only; no deletions possible), the standard preview, then `sync-apply`. DeleteGuard, lock, manifests and DB step unchanged.

### 6.4 Two clients on one HiBy (Windows and phone)

- Both use the card lock; the device job lock and sync lock are the same file.
- Each client reads the other's card edit segments newer than its watermark (per card id). For tracks that map to **its own** files it offers "Apply 3 changes made from *Desktop* to the files on this phone" (ticked by default, owner decision 2). Otherwise it only patches its view of the HiBy library and its overlay (`c:` keys).
- Segments older than 180 days and read by every client known in `playlists.json` are pruned by any client.
- Playlists converge through the shared `ByOS/sync/playlists.json` merge base (SLD §6.2).

### 6.5 Failure modes

| Failure | What happens | Recovery |
|---|---|---|
| PC crash during PC writes | Log line exists; files old or new | Next start re-checks post values; continues `partial` |
| Android killed mid-SAF write | SAF journal | Replayed at start |
| Cable/Wi-Fi drop during a job | Job keeps running on the device | EDIT_STATUS re-attach; results fetched; journals written |
| Daemon idle-exit/restart | Job detached; `running.json` | State via EDIT_STATUS |
| Device power loss / OOM | Each file old or new; result fsynced every 50 | `edit-apply` resumes |
| Card removed / USB storage | `paused_sd` | Resumes when the same card is back |
| Staged DB waiting | `replay.list` | Replayed after the swap |
| Full stock rescan after an edit | FLAC/MP3/M4A cover: tags in the file, fix survives; DB-only formats revert | "Library only" badge; next PC-built DB re-applies the overlay |
| SQLITE_BUSY on the live DB | Retried 3× with backoff | Then NODB for the rest and `edit_db_deferred`: the client offers a staged DB |
| Different card inserted | E_EDIT_INVALID (card) | Edits wait for the right card |
| Low battery | E_BATTERY | Pending, retried on charge |
| Stale catalogue | Field base check | Conflict sheet; nothing overwritten silently |
| Unsupported format on the card | `UNSUP` | DB-only + badge |

---

## 7. Performance and limits

### 7.1 Windows page at 50,000 tracks

- **Column model:** the catalogue becomes typed arrays (`Int32Array` duration, year, track, disc, plays, dates; one rank array per text column) plus string arrays. Ranks come from one `Intl.Collator(lang, {numeric:true, sensitivity:'base'})` over unique folded sort values; a sort compares `Int32` ranks. `edit` patches update cells and re-rank only changed values.
- **Search:** one pre-folded haystack per track; a query that extends the previous one filters the previous result; debounce 80 ms.
- **Rendering:** the Songs table keeps a pool of row nodes (visible + 10) and rewrites their text on scroll (today's code rebuilds nodes every frame). Albums grid virtualised by tile rows; art via the existing `IntersectionObserver` and `artQueue`; column-browser panes are virtual lists.
- **Catalogue transfer:** compact JSON with short keys (about 18 MB at 50k), served gzip (`node:zlib`), parsed once per start (~250 ms); cached data paints first.

| Action (headless Edge on the 50k fixture) | Budget |
|---|---|
| First paint of cached Songs | ≤ 400 ms |
| Gzip catalogue route | ≤ 300 ms |
| Sort | ≤ 50 ms |
| Search keystroke | ≤ 30 ms |
| Scroll frame | ≤ 8 ms of script |
| Get Info open on 50k (Mixed scan) | ≤ 150 ms |
| Patch of 1,000 rows | ≤ 20 ms |
| Page memory | ≤ 350 MB |
| `serve` JVM heap | ≤ 100 MB |

### 7.2 Core

| Action | Budget |
|---|---|
| `edit-preview` of a genre rename on 8,000 files of a 50k library | < 2 s |
| PC writes, in place, SSD | 5–15 ms per file (10,000 files ≈ 1–3 min; 50k ≈ 4–12 min), with progress and Cancel |
| Backups of an 8,000-file edit with shared covers | ≤ 200 MB |
| Edit log for a 50k-track rename | ≤ 6 MB |

### 7.3 Android

| Action | Budget |
|---|---|
| `catalog.db` build from a 50k snapshot | < 20 s on a mid-range phone |
| Search keystroke | < 50 ms |
| Scrolling | 60 fps, checked frame by frame on screenshots |
| SAF first scan | 10k files < 60 s; 50k < 8 min |
| Catalogue patch after a 500-file job | < 1 s, no snapshot pull |

### 7.4 HiBy

Peak RSS < 4 MB for any job size; ≤ 200 rows per DB transaction (64 if DC-E2/E7 fail); live limits 2,000 files for tags and 200 for embedded covers; one job at a time at nice 10 and I/O idle; card backups ≤ 256 MB / 30 days.

### 7.5 Online

MusicBrainz 1 req/s (`mb.lock`), LRCLIB 2 req/s serial (`net.lock`), both cross-process and cached 30 days. Whole-library online runs show an ETA and resume.

---

## 8. Dependencies needing approval

**Required: none.** Everything uses Edge/Chromium built-ins (HTML media, Web Audio, Media Session, Document Picture-in-Picture; WebCodecs/AudioWorklet only in the post-v1 gapless engine), Node built-ins (`node:http`, `node:zlib`, `node:fs`), Windows built-ins (PowerShell `Microsoft.VisualBasic` Recycle Bin), the JDK (`java.util.zip`, `java.awt`, `HttpURLConnection`), the Android platform (`SQLiteDatabase` FTS4, `DocumentsContract`, `BitmapFactory`, `MediaPlayer`, Photo Picker via the existing `androidx.activity`), the existing app dependencies (Compose foundation, activity-compose, documentfile), the player's own `hibytag`, `hibysync`, `lrc_core` and libsqlite3, and our own code (TagWriter, new tag readers, EpubMeta, CoverImage, demuxers).

**Optional items for the owner to approve or reject** (none is in the plan unless approved):

| # | Item | Licence | What it adds | Recommendation |
|---|---|---|---|---|
| O1 | Port of Apple's ALAC decoder to JS | Apache-2.0 | ALAC playback on Windows if the Edge probe fails | Only if the probe fails and the library has ALAC |
| O2 | Port of the Monkey's Audio (APE) decoder | BSD-3-Clause in recent releases (check the exact release) | APE playback on Windows | No for v1 |
| O3 | Use a user-installed `ffmpeg.exe` on PATH, never bundled | LGPL-2.1+/GPL (user's own install) | WMA/APE/other playback via transcoding | Off by default, "Advanced" switch |
| O4 | Room, Paging 3, Coil/Glide, WorkManager (Android) | Apache-2.0 | Convenience only | Not proposed |
| – | Platform `JobScheduler` for background delivery | Platform API, no approval needed | Edits delivered while the app is closed | Owner's choice (§10 Q5) |
| – | MusicBrainz genre list | CC0 | Genre suggestions | Already approved |

Not used: iTunes Search API, Discogs, Last.fm, any npm package, any GPL code in the products.

---

## 9. Milestones, in order, with acceptance tests

Rules for every milestone:

- Each is sized for one or two engineer-days and ships something usable on its own.
- Windows UI milestones add `byos-cli-mock.mjs` scenarios, `demo=` states, `node --test` tests and a frame-by-frame review with `&frame=`; performance tests use `mock/library-fixture.mjs --tracks 50000`.
- Android milestones add Roborazzi screenshots in EN/ES/FR.
- Device milestones are verified in the fake device and the VM harness first, then by the owner's device check (§5.6) on a backed-up card.
- **Prerequisites from SLD:** M3 (desktop sync UI), M5 (PC-built DB), M6 (playlists), M7 (`MetaStore`, analysers), M8 (TagWriter for FLAC/ID3/M4A/Ogg + corpus), M9 (online client), A1 (`SafLocalSource`). They are not repeated here; each milestone names the ones it needs.

### Phase 1: Windows player (ships a usable music player)

| # | Scope | Needs | Acceptance |
|---|---|---|---|
| LM1 | `/api/play/:tid` with Range and MIME map; CSP `media-src`; element engine; play/pause/next/previous; LCD with scrubber | – | Plays fixture MP3/AAC/FLAC/Opus/WAV/M4B; Range tests (start, middle, end, invalid); `tid` cannot escape the link root (symlink, `..`, other link); no token in any media URL |
| LM2 | Up Next popover, Play Next / Add to Up Next, shuffle (songs/albums), repeat, `player.json` persistence, plays counting | LM1 | Queue, position and shuffle state survive a restart on a new port; turning shuffle off returns to the source order at the same song |
| LM3 | Keyboard shortcuts, Media Session, Now Playing view (art, queue, lyrics display from existing `.lrc`/embedded), mini player (Document PiP + popup fallback) | LM2 | Every shortcut has a test; media keys drive playback; synced lyrics highlight and seek on click |
| LM4 | Format probe with "Can't play on this computer" rows, ReplayGain (off/track/album), audiobook speed, skip, remembered positions, Mark as finished | LM1 | Probe cached per Edge version; RG gain matches reference values without clipping; audiobook position restored to ±1 s |

### Phase 2: Windows library views at 50k (ships an iTunes-style browser)

| # | Scope | Needs | Acceptance |
|---|---|---|---|
| LM5 | Column model with rank sort, row-node pool, column chooser/resize/reorder saved per view, field-token search with shared `fold()` vectors | – | §7.1 budgets for paint, sort, search, scroll in headless Edge on the 50k fixture |
| LM6 | Virtual albums grid, column browser (Ctrl+B), Genres view, Artists view with "Appears on" | LM5 | Grid scroll frame ≤ 8 ms; column browser narrowing ≤ 50 ms |
| LM7 | Tag readers for Ogg/Opus, APEv2, ASF, DFF, AIFF; filename-only rows for anything unreadable; M4B chapter reader (read-only) | M8 corpus | Library count equals the file count on disk; reader corpus tests per format; Opus/Ogg rows play (LM1) |
| LM8 | Sidebar: kinds with per-link sub-sources (All default), DEVICES › HiBy views reusing the same views, `sync-track-status` and the On HiBy / ✓ columns, Send to HiBy Now (`sync-plan --only`) | LM6, SLD M3 | Status glyphs match a fixture manifest; unticking never produces a delete; `--only` plan emits only adds/updates |
| LM9 | Audiobooks and Books views: book grid, EPUB/TXT covers, read-only Get Info for books, Open in default app | LM6 | EPUB cover found via OPF `cover` meta and `cover-image`; 600+ ebooks warning in the sync preview |

### Phase 3: edit pipeline on the computer (ships editing; first overlay-only, then into files)

| # | Scope | Needs | Acceptance |
|---|---|---|---|
| LM10 | `EditStore`: log segments, index, base ranges, overlay derivation, re-keying; `edit-preview`/`edit-commit`/`edit-ls`/`edit-show`; EditGuard; `meta-set` alias | SLD M7 | 8,000-file genre rename previews in < 2 s; log ≤ 6 MB at 50k; EditGuard levels at 50/1,000/5 %; re-keying after a local rename keeps pending edits on the right file |
| LM11 | `byos-cli serve`, `edits.lock`, `edit` events over SSE, catalogue patching in the page | LM10, LM5 | Inline edit round trip < 150 ms without JVM start; 1,000-row patch ≤ 20 ms; device command and `serve` never lose an index update (concurrency test) |
| LM12 | Get Info sheet (1 and N, album/artist/genre scopes), Mixed, per-field apply, Clear, combo suggestions, harmonise chip, inline cell edit; writes go to the overlay ("Library only" until LM14) | LM11 | Mixed scan ≤ 150 ms on 50k; only dotted fields are in the commit; the next PC-built DB (SLD M5) shows the edited values |
| LM13 | Undo/Redo as inverse edits, History panel, Ctrl+Z/Ctrl+Y, "changed again afterwards" prompt | LM12 | Undo restores every field; undo of an undo restores the edit; undo with a later conflicting edit lists the songs |
| LM14 | PcWriter for FLAC and MP3: tagbak + CAS, in place / temp + atomic move, read-back, audio hash, locked-file retry, now-playing deferral, Cancel at a file boundary | SLD M8, LM12 | Corpus: every non-edited key byte-identical, audio hash unchanged; crash after N files resumes with no double writes; locked file gives `edit_pc_in_use`; Cancel at 50 % leaves a consistent `partial` edit |
| LM15 | PcWriter for M4A and Ogg/Opus; `edit-revert-files`; backup budget and pruning; Settings › Library (write into files, undo history size) | LM14 | Revert restores sha256 of the whole file; budget prompt appears when an edit would exceed 2 GB |
| LM16 | Helpers: Number Tracks, Fill from File Name, Change Case, Swap, feat., Find & Replace, Compilation; Rename/Merge artist and genre with `rules.*` | LM12 | Each helper's preview equals the committed values; a rename also settles the matching Tidy Up proposal; variants can be unticked |

### Phase 4: artwork and lyrics on the computer

| # | Scope | Needs | Acceptance |
|---|---|---|---|
| LM17 | Artwork from file, paste, drop, another song, remove; `CoverImage` (baseline JPEG, sizes); embed in files; optional `cover.jpg`; upload route | LM14 | Only the front picture is replaced; output is baseline JPEG ≤ 1200 px/≤ 1 MB; `folder.jpg` untouched |
| LM18 | Lyrics tab: synced editor with nudges, offset, tap-to-sync; plain editor; `.lrc` UTF-8 BOM for synced, embedded for plain; lyrics column | LM14, LM3 | Timestamps validated (rising, inside duration); `.lrc` has BOM; plain lyrics read back from the file |
| LM19 | Online artwork: Find Online grid, batch "Get Album Artwork" with pre-accept rules and review grid | SLD M9, LM17 | Recorded-response tests; ≤ 1 MusicBrainz request/s across two processes; nothing written before Apply |
| LM20 | Online lyrics: LRCLIB single and batch review sheet | SLD M9, LM18 | ≤ 2 req/s serial across two processes; only exact matches pre-ticked; request log contains only artist/title/album/duration |

### Phase 5: device side (VM first)

| # | Scope | Needs | Acceptance |
|---|---|---|---|
| LM21 | hibytag `dbset`, `tagbak`, `tagrestore` | – | Round trip restores bytes exactly; `dbset` keeps COUNT and aggregate tables consistent (`dbcheck` clean) |
| LM22 | hibytag `edit-apply`: job parser (F/S/C/L/D/B/P/E), base checks, result file, resume, NOWPLAYING deferral, card backups, M4A cover | LM21 | kill -9 at random points → resumed job with identical results; `CONFLICT` leaves the file untouched; VmHWM < 4 MB on a 60,000-record job; audio hash unchanged |
| LM23 | hibysync D6 async helper, ops 0x48–0x4C, caps 15, card check, lock, control-pipe `EDITJOB/EDITDONE/NOWPLAYING`, `lib.tsv` hold in the shell, `replay.list` in `apply-staged` | LM22 | Fake-device + VM: drop and re-attach with EDIT_STATUS; E_MASS_EDIT without MASS_OK; E_BUSY while a sync holds the lock; a staged DB + edits shows the edits after the swap; then owner device checks DC-E1…E4, E6–E8 |

### Phase 6: delivery to the HiBy (ships "Update the HiBy")

| # | Scope | Needs | Acceptance |
|---|---|---|---|
| LM24 | `core/meta/Delivery` + `edit-push`: merge (latest value, earliest base), split per method, sidecar PUTs, job PUT, EDIT_APPLY, result → card checkpoint journals and card edit segments, HiBy view patch, `edit-status/cancel` | LM23, LM15 | After a push, `sync-plan` shows **zero** items and zero conflicts for the edited files; no client writes manifest-v2 outside a sync |
| LM25 | Windows delivery UI: Ask banner (default), Right away agent in `server.mjs` (3 s debounce), At the next sync; pending bar, sidebar badge, History filter, conflict sheet, mass-edit and re-send prompts | LM24 | Live mode never starts a > 2,000-file job or any re-send without a prompt; banner merges edits made while it is up |
| LM26 | Planner integration: `retag/sidecar/dbfix` actions, explained changes from the ledger, foreign tags-only retags via `afp`, manifest `card_tags_rev` + `sidecars`, prepared-copy key with overlay rev | LM24 | A foobar2000-style retag of 1,000 files plans `retag` with 0 bytes; `.lrc` moves and deletes with its track; DeleteGuard counts sidecars |
| LM27 | Large sets: "Update the library now, files in the background" (DB stage with overlay + NODB chunks), "Library only"; HiBy rows editable from the Windows device views (`c:` keys) | LM24, SLD M5 | 6,000-track genre merge: after restart the stock library shows the new genre count with zero audio bytes sent; chunks resume after a disconnect |

### Phase 7: Android library manager

| # | Scope | Needs | Acceptance |
|---|---|---|---|
| LA1 | `catalog.db` + FTS (folded columns for API 26–29), build from the snapshot, patch API | – | 50k snapshot builds in < 20 s; search < 50 ms; Robolectric tests on API 26 and 34 |
| LA2 | `Screen.LIB` with kinds, HiBy source, grids/lists, search, selection mode, context menus; old `SYNC_LIBRARY` actions moved | LA1 | Roborazzi screens EN/ES/FR; frame-by-frame scroll review; delete/move/reveal behave as before |
| LA3 | Edit sheet (1 and N, Mixed, helpers, preview, EditGuard) on the in-process `EditStore`; History with Undo; `c:` keys for HiBy-only items | LA2, LM10 | Overlay round trip; undo restores values; draft survives process death |
| LA4 | Delivery on Android: `SyncSession.editApply/editStatus/editCancel`, the three modes with the Ask sheet, Pending changes screen, conflict sheet, catalogue patch from results, `TransferService` kinds | LA3, LM24 | Pending edits survive app kill and phone restart; delivered on reconnect; after delivery no snapshot pull; Live mode never re-sends audio without a prompt |
| LA5 | Phone source and Both: SAF scan, PcWriter over SAF with the journal and temp-document replace, post-write ledger | SLD A1, LA4 | 10k-file scan < 60 s, 50k < 8 min; providers without `rw` take the fallback; kill mid-write recovers; the phone's next sync does not re-send edited files |
| LA6 | Covers (Photo Picker, Files, crop, from another song, remove) and lyrics (single, editor with sync mode, bulk "Find missing lyrics"); online sources | LA4, SLD M9 | Baseline JPEG output; ≤ 1 req/s MB, ≤ 2 req/s LRCLIB; bulk queue survives `onTimeout` |
| LA7 | Playlists manager on `core/playlists` (sections, smart playlists, radio, delivery via `playlist-save`) | LA4, SLD M6 | Playlist edited on the phone and on Windows converges through `playlists.json`; LIB_M3U registers live when available |

### Phase 8: two clients and edits made on the player

| # | Scope | Needs | Acceptance |
|---|---|---|---|
| LM28 | `edit-import`: read the other client's card segments; "Apply N changes made from *Desktop*/*phone* to your files" on both clients | LM24, LA4 | A desktop edit appears on the phone after connecting, and vice versa |
| LM29 | `db-build` and `sync-plan` refuse with `edits_not_imported` until segments and device records are imported | LM28 | A phone edit followed by a desktop sync and DB build keeps the phone's value in the DB; no `card_changed` conflict |
| LM30 | Caps 16 EDIT_LOG: shell tag editor, lyrics and cover search write records; "Bring N changes made on the HiBy to your files" on both clients | LM28 | An edit made on the player reaches the PC file after confirmation; Live mode imports automatically unless it conflicts |

### Phase 9: audiobooks, ebooks and library housekeeping

| # | Scope | Needs | Acceptance |
|---|---|---|---|
| LB1 | Audiobook edits: `book.json` as source of truth (new fields, `id` kept, unknown keys kept), tag mirror table, chapter fan-out, tags-only renumber | LM24 | `book.json` round trip keeps unknown keys and `id`; chapter files never renamed |
| LB2 | `POS_LIST` op (caps 17) and progress/"Finished" in both UIs | LM23 | Values match fixture `.pos` files; DC-B2 |
| LB3 | `EpubMeta` (OPF + cover rewrite, EPUB3 refines, `dcterms:modified`, both cover declarations, DRM refusal) and the device `P` record; EPUB edit prompt with re-send cost | LM22, LM24 | Rewritten EPUB reopens with the player's `bk_zip` code in the VM; position and cover cache follow the new key (DC-E5) |
| LB4 | Media Kind move music ↔ audiobooks (PC move + `book.json` + `cover.jpg`; card via sync `move` items) | LB1 | Undo returns the file; sync preview shows the card move; ByOS Audiobooks lists the book |
| LB5 | Add to Library (`library-import`, drag onto the window; SAF picker on the phone) with duplicate report; Remove from Library (Recycle Bin / hide) | LM15, LA5 | Imported files land in the Artist/Album layout with rules applied; duplicates reported; Recycle Bin move by id only |
| LB6 | Windows playlists on this computer (New, from selection, reorder, smart, radio, export to the HiBy) | LM24, SLD M6 | Same convergence test as LA7 from the Windows side |

### After v1 (only if the owner wants them)

| # | Scope |
|---|---|
| LM-later-1 | Gapless engine (own demuxers + WebCodecs + AudioWorklet), crossfade, output device picker. Acceptance: sample-exact join on an album split from one sine sweep (no step above −90 dB) |
| LM-later-2 | DSD → PCM in an AudioWorklet (DSD64/128 within tolerance of reference PCM) |
| LM-opt | hibytag M4A `ilst` text writer (removes the re-send prompt for M4A text edits on the card) |
| LM-later-3 | Mark as finished / reset progress written to the HiBy; Find duplicates; "Keep my music folder organised"; Android background delivery |

---

## 10. Risks and open questions

### 10.1 Risks

| Risk | Mitigation |
|---|---|
| Live DB writes while the stock UI runs (crash or stale lists) | ≤ 200-row transactions with pauses, busy_timeout; DC-E2/E7; fallback to 64 rows; NODB + staged DB for large jobs |
| A device rewrite under a playing file | NOWPLAYING deferral (device) and the same rule in the Windows player |
| The PC-built DB reverting edits made elsewhere | `edit-import` before `db-build`, `edits_not_imported` refusal (LM29) |
| Manifest races between clients | Card checkpoint journals only; nobody rewrites manifest-v2 outside a sync |
| Tag writers damaging files | Tag-region backups, read-back, audio hash, `UNSUP` instead of best effort, corpus tests (M8 + LM7) |
| Losing ebook positions | `P` records; EPUB edits blocked for books with a position until LB3 |
| Online lookups at library scale taking hours | Scoped defaults, ETA, resume, 30-day cache |
| OOM on the 57 MB player | Streaming job parser, < 4 MB RSS target, `oom_score_adj`, resumable jobs |
| Backups filling disks | 2 GB PC budget, 256 MB / 30 days on the card, prompts before exceeding |
| M8 TagWriter slipping | Phase 3 ships overlay-only editing first (LM10–LM13 reach the HiBy through the PC-built DB) |

### 10.2 Open questions for the owner

1. **Owner decision 2 wording:** confirm "live retag on the device is allowed in Live mode; re-sending audio is never automatic" (§0.3).
2. **Default delivery mode:** "Ask me" (proposed) or "Update the HiBy right away"?
3. **Live limits:** 2,000 files for tags, 200 for embedded covers. Lower them until DC-E2/E7 pass?
4. **Backups:** 2 GB on the PC and 256 MB on the card, "Tags only" by default on the phone: acceptable?
5. **Android background delivery** with the app closed (platform `JobScheduler`, twice an hour on unmetered Wi-Fi, no approval needed): wanted?
6. **M4A text edits on the card:** add the hibytag `ilst` writer (LM-opt) or keep the costed re-send / "Library only"?
7. **Sort fields on the HiBy:** does the stock DB have sort-name columns? If yes, Sorting fields reach the HiBy at the DB build; if not they stay desktop-only (device research).
8. **Shell work for books:** raise or page `BK_MAXB` (600), and/or re-key ebook positions by `dc:identifier` instead of `P` records?
9. **Optional decoders O1–O3:** decide after the Edge probe runs on the owner's Windows 11 (ALAC/WMA via Media Foundation).
10. **Find duplicates** and **"Keep my music folder organised"**: wanted for v1.1?
11. **Gapless/DSD playback on Windows** (LM-later-1/2): wanted at all?

### 10.3 Text (EN/FR/ES)

- Windows strings go into `desktop/ui/i18n.js`; Android strings into a new `res/values{,-es,-fr}/strings_library.xml` (about 180 strings). All three languages ship together. Plurals everywhere (`<plurals>` on Android, ICU-style helpers in `i18n.js`). Online-service sentences name the service and exactly what is sent. Canonical genre names follow `meta.settings.genreLanguage`, not the UI language.
- **One glossary** for both apps:

| Meaning | EN | FR | ES |
|---|---|---|---|
| Send edits to the HiBy (button, prompt title) | Update the HiBy | Mettre à jour le HiBy | Actualizar el HiBy |
| Value that differs across the selection | Mixed | Valeurs multiples | Varios valores |
| Fix held only by the library DB | Library only | Bibliothèque uniquement | Solo la biblioteca |
| Delivery setting | When I change songs, books or covers | Quand je modifie des morceaux, livres ou pochettes | Cuando cambie canciones, libros o carátulas |
| Live mode | Update the HiBy right away | Mettre à jour le HiBy tout de suite | Actualizar el HiBy al momento |
| Ask mode | Ask me | Me demander | Preguntarme |
| Batched mode | At the next sync | À la prochaine synchronisation | En la próxima sincronización |
| Later | Later | Plus tard | Más tarde |
| Don't ask again | Don't ask again | Ne plus demander | No volver a preguntar |
| Pending count | {n} changes waiting for the HiBy | {n} modifications en attente pour le HiBy | {n} cambios pendientes para el HiBy |
| Offline save | Saved. It will go to the HiBy when it's connected. | Enregistré. Ce sera envoyé au HiBy dès qu'il sera connecté. | Guardado. Se enviará al HiBy cuando esté conectado. |
| Mass edit | Change {n} songs? · Type {n} to confirm | Modifier {n} morceaux ? · Tapez {n} pour confirmer | ¿Cambiar {n} canciones? · Escribe {n} para confirmar |
| Conflict | {n} songs were changed on the HiBy since you last synced | {n} morceaux ont été modifiés sur le HiBy depuis la dernière synchronisation | {n} canciones se cambiaron en el HiBy desde la última sincronización |
| Conflict choices | Use this computer's / Use the HiBy's | Garder celui de l'ordinateur / Garder celui du HiBy | Usar el del ordenador / Usar el del HiBy |
| DB-only badge | Library only: a full rescan on the HiBy would undo it | Bibliothèque uniquement : une analyse complète sur le HiBy l'annulerait | Solo la biblioteca: un reanálisis completo en el HiBy lo desharía |
| File in use | {n} songs are open in another app | {n} morceaux sont ouverts dans une autre app | {n} canciones están abiertas en otra app |
| Edits from the player | Bring {n} changes made on the HiBy to your files | Appliquer à vos fichiers {n} modifications faites sur le HiBy | Llevar a tus archivos {n} cambios hechos en el HiBy |
| Up Next | Up Next | À suivre | A continuación |
| Tidy Up | Tidy Up | Ranger | Ordenar |
| Not playable | Can't play on this computer | Lecture impossible sur cet ordinateur | No se puede reproducir en este ordenador |
| Undo | Undo | Annuler | Deshacer |
| Get Info | keep the existing "Information" labels in each language | idem | idem |
