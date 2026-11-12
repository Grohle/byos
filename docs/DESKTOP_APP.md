# ByOS desktop app: design

Status: **design**, plus a working dependency-free scaffold in [`desktop/`](../desktop/README.md).

The ByOS desktop app is an iTunes/iOS-style music manager for the HiBy R3 II (2025) running
ByOS. It manages the user's music library on the computer, and it manages the player: status,
files, music sync, the player's library database (built on the computer), and ByOS updates and
installs without the user having to touch the player. Windows comes first. macOS and Linux
follow with the same code.

Related documents:

- [SYNC_PROTOCOL.md](SYNC_PROTOCOL.md): BTP/1, the protocol of the `hibysync` daemon on the HiBy.
- [BYOS_CLI.md](BYOS_CLI.md): `byos-cli`, the command-line client the app runs for every device
  operation.
- [research_companion_apps.md](research_companion_apps.md): the research behind this design,
  in particular the desktop-stack section and the critic's Decisions 0 to 7.
- [OTA.md](OTA.md), [PATCHER.md](PATCHER.md), [INSTALLER.md](INSTALLER.md): the update,
  patch and install paths the app reuses.

## 1. Goals and non-goals

Goals:

1. **Feels like iTunes on macOS/iOS**: a sidebar with Library and Devices, an album grid, a
   track list, a device summary page with a capacity bar, sheets for questions, and short
   spring animations. It must look finished, not generic.
2. **One place for everything the player needs**: see it, browse its files, copy music,
   rebuild its library, update or install ByOS, all over USB, or over Wi-Fi once paired.
3. **No second implementation** of anything ByOS already has. Device access goes through
   `byos-cli` (the shared Kotlin core, also used by the Android app). Firmware patching is
   the core's `Patcher` (byte-identical to the Python patcher). Updates use the same Ed25519
   check as OTA.
4. **Safe by default**: nothing on the player is overwritten without asking. Every transfer is
   verified with SHA-256. Every destructive action has a confirmation sheet. The player's
   library database is never replaced while the player is running (critic Decision 6).
5. **English, French and Spanish** from the first release, with every string in a
   translation table.

Non-goals (for the first releases):

- Streaming services, stores, podcasts. Audio playback on the computer is kept from the base
  player, but it is not the focus.
- Editing audio files on the player itself. Tags are edited on the computer copy, then synced.
- Talking to stock firmware other than for the first ByOS install (section 5.8).
- Bundling Google's `adb`. Its licence does not allow it (research, critic correction 3).

## 2. Architecture

```
+--------------------------------------------------------------------+
| ByOS desktop (Tauri 2 window, WebView2 on Windows)                  |
|                                                                    |
|  UI: React + TypeScript (Museeks base, reskinned)                  |
|    Library views | Devices views | Activity | Settings | Sheets     |
|        |  invoke()/events                                          |
|  Rust core (Museeks base)                                          |
|    library DB (sqlx/SQLite), tags + covers (lofty), playback,      |
|    playlists, m3u, folder watching                                 |
|    + new: device_jobs (runs byos-cli, parses JSON lines)           |
|    + new: adb_manager (finds or downloads platform-tools, with     |
|           consent)                                                  |
|        |  child process, stdin/stdout (JSON lines)                  |
+--------|-----------------------------------------------------------+
         v
  byos-cli (sidecar: Kotlin core, jlink JRE or GraalVM native image)
         |  adb forward tcp:7340 tcp:7340  (USB)  /  TCP 7340 + AUTH (Wi-Fi)
         v
  hibysync on the HiBy (BTP/1)
```

Rules:

- **The UI never talks to the device directly.** Every device operation is one `byos-cli` run
  (one command, one connection, BYOS_CLI section 4). The Rust side owns these processes.
- **One device job at a time per device.** `hibysync` serves one session at a time, and the
  HiBy has 57 MB of RAM. Jobs for one device are queued. Jobs for different devices run in
  parallel. Purely local jobs (`db-build`, `patch`, `firmware-fetch`) do not hold the device
  slot.
- **The computer library and the player library are separate.** The computer library is the
  Museeks database (the user's music folders). The player library is the HiBy's
  `usrlocal_media.db`, which the app builds with `byos-cli db-build` (Kotlin core, schema
  rules credited to hiby-mods, MIT). The Museeks database is never copied to the player.
- **State the app keeps** (in `%APPDATA%\ByOS\desktop\`): `settings.json`, per-device sync
  settings (`devices/<device_id>.json`: which playlists, artists or albums to sync, the
  music folder on the SD, the last DB revision), and logs. Pairing keys belong to `byos-cli`
  (`%APPDATA%\ByOS\cli`, BYOS_CLI 3.1). The app passes `--config` so both always agree.

### 2.1 Why the device logic is not in Rust

The research (critic Decision 5) asked for one shared core language. It is decided: **Kotlin**,
in `android/hibyos-install/core`, already used by the Android app, with the patcher, Ed25519,
bundle and downloader, and now BTP/1. The desktop app calls it as a sidecar process. Two
implementations of BTP/1 (C on the device and Kotlin on clients) are enough. A third one in Rust
would have to be kept in step with `tests/btp/vectors.json` forever. The cost is one child
process per operation, which is small next to a transfer (the JVM starts in about 0.3 s, a
native image in about 20 ms).

## 3. Screens

The window has a translucent sidebar (vibrancy on Windows 11 with Mica, a solid fallback
elsewhere), a toolbar with an "LCD" status area in the middle (as in iTunes: the song playing,
or the current device job with its progress bar), and the content area. Light and dark follow
the system. Fonts: the system UI font (Segoe UI Variable on Windows, SF on macOS through the
system stack, never an embedded SF Pro, whose licence forbids it), or Inter (OFL) as a bundled
fallback.

### 3.1 Sidebar

```
LIBRARY            DEVICES                PLAYLISTS
  Recently Added     HiBy R3 II  ⏏ ●       Mixes
  Artists            (summary, music,     Smart playlists
  Albums              files, ...)         Playlists
  Songs              HiBy R3 II (Wi-Fi)
  Genres             + Pair a device...
  Audiobooks
```

- A device row shows its name, the transport icon (cable or Wi-Fi), a sync spinner while a job
  runs, and an eject button (it ends the session: `BYE`, then `adb forward --remove`).
- Paired devices that are not reachable are greyed out, with "Not connected".
- "Pair a device..." opens the pairing sheet (5.2).

### 3.2 Library (computer)

Kept from Museeks and reskinned:

- **Albums**: grid of covers (lazy loading, 180 px tiles), an album opens as an expanded panel
  below its row, like iTunes 11, with the track list and the dominant colour of the cover as the
  background.
- **Artists**, **Genres**: a list on the left, the albums of the selection on the right.
- **Songs**: the virtualized track list (Museeks' TanStack Virtual), sortable columns, with a
  column "On HiBy" (a dot when the track is on the selected device; from the last device scan).
- **Recently Added**, **Playlists**, **Smart playlists** (new: a rule table in SQLite, rules on
  artist, album, genre, year, format, quality, date added, play count), **Mixes** (read from
  the player's `Mezclas/*.m3u` on the last sync).
- **Audiobooks** (new): folders marked as books (a `book` flag per folder, or files in
  `Audiobooks/`), shown as a grid. Synced to the folder the HiBy uses for books.
- **Get Info** sheet: tags, cover (embed or replace, the stock player shows only embedded art),
  lyrics (`.lrc` next to the file, UTF-8 with BOM, as the player expects).
- Drag and drop of files and folders into the library adds them to watched folders.

### 3.3 Device: Summary

The page shown when a device is selected (like the iPod summary in iTunes):

- Big device image (the R3 II render), name (renamable later, through BTP when it supports a
  name), ByOS version, firmware, serial (USB) or device id, transport and whether it is
  encrypted.
- **Capacity bar** (from `df` / `status`): the SD split by colour into Music, Audiobooks, Other
  and Free. The split comes from the last file scan (`ls --recursive` on the music folder,
  cached). Below it, the internal `/usr/data` free space, because the library database lives
  there.
- Battery level and charging state.
- State banners (one at a time, the most important first):
  - "The HiBy's card is connected as a USB drive" (`sd_state: usb_storage`), with the help text
    "Choose ByOS Sync on the HiBy's USB menu to manage it from here."
  - "No card in the HiBy" (`no_card`).
  - "A ByOS update is ready to install" (from `status.update`).
  - "The new library will be used after the HiBy restarts" (`status.db.staged`).
- Buttons: **Sync** (5.4), **Check for Update** (5.6), **Eject**.

### 3.4 Device: Music

What to keep on the player. Modelled on the iTunes "Music" tab:

- [x] Sync music: (o) Entire library  ( ) Selected playlists, artists, albums and genres.
- [ ] Include audiobooks. [ ] Include lyrics (`.lrc`). [ ] Embed missing covers in copies.
- Music folder on the SD (default `Music/`), and the layout `Artist/Album/NN Title.ext`, or
  "Keep my folder structure".
- "Remove music from the HiBy that is not selected" (off by default; when on, the sync plan
  lists every deletion and asks first).
- The bottom bar shows the plan: "Add 312 songs (2.1 GB), remove 0, update 4", and the
  **Apply** button.

### 3.5 Device: Files

A Finder-style browser of the SD (`ls`, `mkdir`, `mv`, `rm`, `put`, `get`):

- Breadcrumbs, list view (name, size, kind, date), double-click to open a folder.
- Drag files or folders from Explorer to upload (`put --recursive --mkdir`, with a "Replace,
  Skip or Keep both" sheet on `E_EXISTS`). Drag out, or "Save to...", to download (`get`).
- New folder, Rename (`mv`), Move to Trash is not available on the HiBy: **Delete** asks first
  ("Delete 3 items from the HiBy? This cannot be undone."), with `rm -r --yes` only after the
  user agrees in the app's sheet.
- Hidden files only with "Show hidden files" (`ls --all`).
- Names the player would refuse are flagged before the upload starts (BYOS_CLI `put`:
  `error usage` with `invalid`), with the reason ("A name cannot end with a dot").

### 3.6 Device: Library database

For people who want control. Normal users only see the result of Sync.

- Live database: size, track count, `live_sha256` (short), last applied.
- **Rebuild on this computer** (5.5), **Ask the HiBy to rescan** (`LIB_RESCAN` full, through
  the shell), **Download a copy** (`db-pull` to a file, for backups).
- A space check: "The library database needs 3 times its size free on the HiBy's internal
  memory: 6.6 MB needed, 24 MB free." Above about 9,000 tracks (critic correction 5) this turns
  into a warning and the PC rebuild is disabled until the DB-on-SD mode exists.
- History of applied databases (from `status.db`).

### 3.7 Device: Updates, build and install

- "ByOS 0.8.0 is installed. ByOS 0.9.0 is available." **Update** runs 5.6. Release notes from
  the signed `hibyos-update.json`.
- **Install from a file...**: a signed update JSON + tar the user picked (same checks).
- **Build ByOS for this HiBy** (advanced, 5.7): fetches HiBy's official firmware, patches it
  locally, and installs it through recovery. Shown with a clear explanation of what happens
  and how long it takes.
- **Install ByOS on a new HiBy** (5.8): the first-install assistant for a stock player.

### 3.8 Device: Settings

- Name shown when pairing (`--name`), allow Wi-Fi sync for this device, forget this device
  (`connect --forget` plus a reminder to remove it on the HiBy too).
- Sync on connect (off by default), skip identical files (on), keep modification times (on).
- Advanced: the `adb` path, the local forward port, the CLI timeout, verbose logs.

### 3.9 App settings

Library folders, language (System, English, Français, Español), appearance (System, Light,
Dark), where `adb` comes from (download Google's platform-tools on first use with consent, or
a path), updates of the app itself, licences (3.11), and "Send logs" (opens the log folder;
nothing is sent automatically).

### 3.10 Activity

A popover from the LCD area lists running and recent jobs: each has a title ("Copying 312
songs to HiBy R3 II"), a progress bar, speed and time left (from `progress.rate`), a Cancel
button, and on failure the plain message and a "Details" disclosure with the raw `error` line.

### 3.11 About and licences

ByOS licence, the third-party notices (section 7), versions of the app, of `byos-cli` (from
its `start.version`), of the bundled runtime, and of each connected device.

## 4. Calling byos-cli

### 4.1 Process

- Location: the sidecar next to the app (`byos-cli.exe` for a native image, or
  `runtime\bin\java.exe -jar byos-cli.jar` for a jlink build). In development: the
  `BYOS_CLI` environment variable, then `android/hibyos-install/cli/build/libs/byos-cli.jar`.
- Always passed: `--config <app config>/cli`, `--adb <resolved adb>` when known, and
  `--device <spec>` for device commands (the spec comes from the `device` event's `id`:
  `usb:SERIAL` or `wifi:DEVICE_ID`).
- Arguments are passed as an array, never through a shell. Remote paths are BTP paths and are
  checked against the same rules as the CLI before the process starts, so the UI can show the
  problem next to the file.
- Order: global options, command, the command's options, then `--`, then the paths
  (BYOS_CLI.md section 2). A BTP name may start with `-`, so without the marker a folder
  called `-r` or `--yes` would change the command (a recursive delete, a skipped
  confirmation). Every flag is put before `--`.
- Never with `--human`. stderr is collected into the log (and `--verbose` is used only when
  the user enables verbose logs).

### 4.2 Reading the output

Each stdout line is parsed as JSON and checked: `v == 1`, `event` known. Then:

| Event | App action |
|---|---|
| `start` | Check `version` against the minimum the app needs. Older: "Update the app's command-line tool" (a packaging bug, logged). |
| `device` | Update the device row and the Summary. |
| `entry` | Append to the listing (streamed into the Files view). |
| `progress` | Update the job's bar. `unit: "permille"` means `done/total` is a fraction of 1000. Rates are smoothed over 3 s for "time left". |
| `prompt` | Open the matching sheet (`pair_code`: 6 digit fields; `confirm`: a yes/no sheet). Write the answer as one JSON line to stdin. |
| `warn` | Show inline in the job (`resumed`: "Resuming at 45 %"; `daemon_not_running`: offer **Start ByOS Sync** over USB, which re-runs with `--start`). |
| `result` / `error` | End of the job. Exactly one must arrive. |

Invalid JSON, an unknown `v`, or a process that exits without a `result`/`error` line is
treated as `internal` with the last stderr lines in Details.

### 4.3 Cancel, timeouts and exit

- **Cancel**: write `{"cancel": true}` to stdin. If the process is still alive after 5 s, kill
  it (`taskkill /T /F` on Windows, SIGTERM then SIGKILL elsewhere). An upload keeps its part
  file on the HiBy, so starting it again resumes.
- **Watchdog**: byos-cli repeats its `progress` line at least every 5 s while it waits
  (BYOS_CLI.md section 2, heartbeat). If no line arrives for 25 s, the job is cancelled and
  reported as `timeout`. After a `progress` line for a wait that is long by design, the limit
  is longer, so a missing heartbeat cannot cut a legitimate wait short:

  | Last `progress` `op` | Silence allowed | Why |
  |---|---|---|
  | `pair_wait` | 130 s | the user has 120 s to tap *Allow* (SYNC_PROTOCOL section 17); cutting it short would also count as a failed pairing |
  | `install`, `reconnect` | 70 s | `update-install` reconnects for up to 60 s across the shell restart |
  | anything else | 25 s | |

- **Prompts**: while a `prompt` waits for the user, the watchdog is replaced by a 130 s prompt
  limit. When it runs out, the app answers "no", cancels the job, closes the sheet and reports
  "No answer was given in time".
- **Closing the app** with jobs running asks "Stop copying to the HiBy?" and cancels them. It
  waits for each job to end (at most the 5 s kill grace) so temporary files are removed.
- **Queues**: one queue per device, so a long upload to one HiBy, or a sheet left open, never
  blocks another HiBy. Discovery (`devices`) never waits in a queue.

### 4.4 Errors shown to the user

The CLI's `error.code` (and `btp.name` for `remote`) maps to a short message and an action. A
few of the most common:

| From the CLI | Message | Action |
|---|---|---|
| `no_device` (adb missing) | "To connect over USB, ByOS needs Android's adb tool." | **Download** (Google's platform-tools, with their licence shown) or **Choose...** |
| `no_device` (unauthorised) | "Allow this computer on the HiBy." | Retry |
| `no_device` + `daemon_not_running` | "ByOS Sync is not open on the HiBy." | **Start it** (USB only) or "Open Settings › ByOS Sync on the HiBy" |
| `auth` | "This HiBy does not recognise this computer any more. Pair again." | Pair |
| `remote` / `E_SD_UNAVAILABLE` | "The HiBy's card is connected as a USB drive." | Help |
| `remote` / `E_NO_SPACE` | "Not enough space on the HiBy." | Show what to remove |
| `remote` / `E_EXISTS` | "A file with that name already exists." | Replace, Skip, Keep both |
| `remote` / `E_DB_STALE` | "The library changed on the HiBy since it was read. Sync again." | Retry the whole sync |
| `remote` / `E_PAIR_CLOSED` | "Open Settings › ByOS Sync › Pair new device on the HiBy." | Retry |
| `verify` | "The file did not arrive intact." or "This update is not signed by ByOS." | Retry, or nothing for a bad signature |
| `rejected` | "Not allowed on the HiBy." | None |
| `timeout` | "The HiBy stopped answering. Check the cable or Wi-Fi." | Retry (uploads resume) |
| `internal` | "Something went wrong in ByOS." | Details, open logs |

The full table lives in the UI's string catalogue (EN/FR/ES), keyed by `code` and `btp.name`.
Any code without an entry falls back to the CLI's English `message`.

## 5. Flows

Each flow lists the CLI commands, then what can go wrong.

### 5.1 Plug in over USB

1. The app polls `byos-cli devices --usb-only` every 3 s while the window is visible and
   every 15 s when hidden (a Windows device-arrival notification can replace polling later).
2. A new USB device with `reachable: true` appears in the sidebar and its Summary loads with
   `status`.
3. Errors:
   - adb missing: the banner in 4.4, nothing else breaks (Wi-Fi devices still work).
   - Daemon not running: "Start ByOS Sync" runs `status --start` once. If that fails: "Open
     Settings › ByOS Sync on the HiBy".
   - The SD is exported as a USB drive: the device is shown, the Summary shows the banner,
     Files and Music are disabled. The app can also notice the drive (a volume with a `ByOS`
     folder) and say "This is the HiBy's card. Eject it and choose ByOS Sync on the HiBy."
   - Two HiBys plugged in: both appear, by serial.

### 5.2 Pair over Wi-Fi

1. The user clicks "Pair a device...". The sheet says: "On the HiBy, open Settings › ByOS Sync ›
   Pair new device." and lists devices from `devices --wifi-only` with `pairing_open`.
2. The user picks one: `--device wifi:<id> connect --pair`. On the `pair_code` prompt the sheet
   shows six digit boxes: "Type the code shown on the HiBy". The answer goes to stdin.
3. `progress pair_wait`: "Tap Allow on the HiBy".
4. `result paired: true`: the device appears under Devices with a Wi-Fi icon.
5. Errors: wrong code (`auth`, "The code does not match. Try again." The CLI has told the
   device); Deny or timeout (`rejected`); pairing closed (`E_PAIR_CLOSED`); no device found
   (firewall hint: "Allow ByOS on private networks", and a manual "Enter address" field).

### 5.3 Browse and copy files

- Open a folder: `ls PATH`. The listing streams in (`entry`), so large folders show at once.
- Upload: `put LOCAL... DIR/ --mkdir [--recursive] [--skip-identical]`. Progress per file.
- Download: `get REMOTE... LOCALDIR [--recursive]`.
- New folder `mkdir`, rename and move `mv`, delete `rm [-r] --yes` (the app asked first).
- Errors: `E_EXISTS` per file shows one sheet for all conflicts ("Apply to all"); a dropped
  connection is retried by the CLI three times, then the job fails with Retry (which resumes);
  `E_NO_SPACE` stops the job and shows the capacity bar.

### 5.4 Sync music

1. **Plan** (local, fast): the app takes the selection (3.4), maps each track to its SD path
   (layout rule), and compares with the cached device scan (`ls --recursive Music`, refreshed
   if older than this connection). Result: lists to add, update (size or mtime differs) and,
   if enabled, remove. Shown in the bottom bar.
2. **Apply**:
   1. `put` the added and updated files with `--skip-identical --mkdir --preserve-mtime`
      (grouped into one CLI run per destination folder batch, at most 500 files per run).
   2. `rm` the removed files (only if enabled and confirmed).
   3. Lyrics and covers (if enabled): `.lrc` files are uploaded with the tracks. Covers are
      embedded only in the computer copies the user owns, never silently in place: the option
      says so, and changed files are re-sent.
   4. **Library**: either 5.5 (rebuild on the computer, the default when the HiBy has the
      `DB_STAGE` capability (HELLO caps bit 3) and the track count is under the space limit) or `LIB_RESCAN` on the
      HiBy.
3. **Done**: "Sync complete. The new library will be used after the HiBy restarts." with
   **Restart HiBy** only if a restart command is offered by the protocol; otherwise the text
   tells the user to restart it.
4. Errors: any failed file is listed at the end with Retry (which re-runs only the failures).
   A sync cancelled half way is safe: copied files stay, nothing is half-written (part files),
   and the database is not staged.

### 5.5 Rebuild the player's library on the computer

1. `db-pull <cache>/<id>/snapshot.db` (keeps history, favourites, playlists, queue, books).
2. `db-build --base snapshot.db --music <mirror or SD view> --sd-root ... --out new.db
   --report report.json`. The "music" input is the app's own view of what is now on the SD
   (the synced files are local copies the app knows), so the build needs no second scan of the
   HiBy. Unreadable files are listed as warnings.
3. `db-push new.db` (the base revision from `new.db.json`). On `E_DB_STALE`, the app pulls and
   builds again once automatically, then asks.
4. The HiBy applies the database before the player starts on the next boot (SYNC_PROTOCOL
   DB_STAGE, critic Decision 6).
5. Errors: not enough internal space (`E_NO_SPACE` on DB_STAGE): "The library is too large for
   the HiBy's internal memory"; the app falls back to `LIB_RESCAN`.

### 5.6 Update ByOS (no hands on the player)

1. `update-send --latest --install --no-confirm` over USB (`--no-confirm` is refused over
   Wi-Fi; the HiBy asks there).
2. Progress: download, then `put`, then `install` permille. The expected disconnect when the
   shell restarts is handled by the CLI (it reconnects for 60 s).
3. Result `installed` or `restart_pending` ("Restart the HiBy to finish": the HiBy shows
   *Restart Now*).
4. Errors: bad signature (`verify`, nothing was sent); downgrade without consent; a HiBy
   without a shell attached (`E_UNSUPPORTED`): "The update is ready on the HiBy. Open Settings ›
   Software Update to install it."; `needs_firmware: true`: the update needs a new firmware
   build, which leads to 5.7.

### 5.7 Build and install ByOS through recovery (advanced)

1. `firmware-fetch` (HiBy's official file, from HiBy's servers to the user's computer; checks
   MD5 and SHA-256).
2. `patch --firmware <upt> --out <cache>/r3ii_2025.upt` (the core patcher, permille
   progress).
3. Copy the patched `.upt` and the `ByOS/` folder from the bundle to the SD root (REPLACE,
   verified): `put --overwrite -- <cache>/r3ii_2025.upt /` and
   `put --overwrite --recursive -- <bundle>/ByOS /` (a lone `/` is the SD root, BYOS_CLI.md
   section 2). **USB only.** `.upt` files carry no signature, only MD5 and ISO structure, and
   the recovery loader flashes whatever `.upt` it finds at the SD root. So a `.upt` at the SD
   root must never arrive over Wi-Fi: byos-cli refuses it, and hibysync must refuse `.upt`
   names at the SD root on any remote session (a device-side deny-list, to be added to
   SYNC_PROTOCOL with Q3). The app records the patched file's SHA-256 (the patcher's output
   hash) for step 4.
4. Reboot into recovery. **This step does not exist in BTP/1 yet.** It needs a device-side
   command that runs the checks of `hibymod reinstall` plus the three missing ones the critic
   listed (park other `.upt` files, require the mtd5 flag clear before, read it back) and a
   reboot method proven on the device. Until then, the app stops after step 3 and shows "The
   ByOS firmware is on the HiBy. On the HiBy, open Settings › About › Reinstall ByOS" (or the
   recovery key combination, with pictures). See open question Q3. Requirements for that
   future command:
   - only on a local USB session, never over Wi-Fi, even when paired;
   - always confirmed on the device itself (a full-screen *Reinstall ByOS?* with *Cancel*);
   - the `.upt` must match the expected build: its SHA-256 equals the one in the signed ByOS
     bundle index (Ed25519, the same key as the OTA), or the one the patcher recorded for this
     build and sent in the same USB session;
   - a `.upt` that arrived through a remote session is refused (hibysync tags files it wrote
     for a Wi-Fi peer, and the deny-list above should make that case impossible anyway).
5. After the reboot, the app waits up to 15 minutes for the device to come back with the new
   `byos_version`, and shows the result.

### 5.8 First install on a stock HiBy

A stock HiBy has no `hibysync`. The assistant:

1. Asks the user to choose **USB storage** on the HiBy, then finds the drive (a removable
   volume with the HiBy's folders).
2. Fetches and patches the firmware (5.7 steps 1 and 2) and copies `r3ii_2025.upt` and `ByOS/`
   to the drive, with write, flush and a read-back SHA-256 check.
3. Shows the steps on the HiBy with screenshots: eject, *System › Firmware update › Via SD
   card*.
4. Waits for the HiBy to come back over USB with ByOS (adb), then continues as 5.1.

The Android app's stock Wi-Fi (port 4399) path stays the alternative for people without a
computer.

### 5.9 Errors common to all flows

- Disconnect in the middle: jobs fail with `timeout` or `no_device`; uploads resume on Retry.
- The HiBy is low on memory (hibyguard may stop helpers): `hibysync` holds a transfer lock that
  protects it; if the session is killed anyway, the job fails with Retry.
- The computer sleeps: the app blocks sleep while a job runs (Museeks already depends on
  `nosleep`).

## 6. Base: Museeks fork, and alternatives

### 6.1 Museeks (chosen)

Museeks 0.23.4 (MIT, Tauri 2.11, React 19, TypeScript, Rust with lofty, sqlx/SQLite, axum,
TanStack Router/Query/Virtual, Zustand, Lingui, StyleX) is the base. It is the only maintained
MIT desktop music library manager on a modern stack. It already depends on
`tauri-plugin-shell`, which runs sidecars.

Plan:

1. Fork at the tag `0.23.4` into `desktop/app` (or a separate repository `Grohle/byos-desktop`;
   decided at M1). Keep the upstream history and the MIT `LICENSE` file as
   `LICENSES/Museeks-MIT.txt`, and add the notice to `THIRD_PARTY_NOTICES.md`.
2. Rename the product (ByOS), app id `org.hibyos.desktop`, icons, and the Lingui catalogues
   (EN/FR/ES; Museeks already uses Lingui).
3. Keep: library scan, tags, covers, playlists, m3u, queue, playback, the virtual track list,
   settings storage, window state, single instance.
4. Reskin: replace the StyleX theme with the iTunes/iOS look (sidebar vibrancy, album grid with
   expansion panel, LCD toolbar, sheets). Animations: CSS springs (`linear()` easing), 250 to
   350 ms, `prefers-reduced-motion` honoured.
5. Add: Devices sidebar section and pages (3.3 to 3.8), `device_jobs` in Rust (4), smart
   playlists, audiobooks, the Activity popover, the `adb` manager (download with consent into
   `%LOCALAPPDATA%\ByOS\platform-tools`, never bundled).
6. The UI work from the scaffold in `desktop/ui` is ported view by view: its logic is plain JS
   and its CSS tokens become StyleX variables.
7. Upstream: merge Museeks releases when they fix things we use; our changes are kept in new
   files where possible, so merges stay small.

### 6.2 Alternatives considered

| Option | For | Against |
|---|---|---|
| **Compose Multiplatform Desktop** (Apache-2.0) | Same language as the core: no sidecar, one process. The Compose libraries are already in this PC's Gradle cache (Android app). | No MIT player base; JVM audio is weak (vlcj is GPL); MIT tag libraries are limited (jaudiotagger is LGPL). Harder to reach iOS-level polish. |
| **Electron** (MIT) | Mature, easy | 100 MB or more, and it is still "a web UI", like Tauri, only heavier. |
| **.NET / Avalonia** (MIT) | Good tag library (ATL, MIT) | No player base; one more language. |
| **Compás Tools fork** | Closest features | GPL-3.0-only; not usable while ByOS keeps its non-commercial licence (Decision 0). Only facts and ideas are reused. |
| **Dependency-free web UI + local runner** (the `desktop/` scaffold) | Runs today with nothing installed; good for developing the device side and the CLI | Runs in the browser, no native window, no library or playback. It is a test bench, not the product. |

The scaffold in `desktop/` exists so the device pages and the CLI contract can be developed and
tested now, before the Museeks fork and its packages are approved.

## 7. Licences and notices

ByOS keeps its own non-commercial licence. Only permissive components are combined with it.

| Component | Licence | How it is used | Notice |
|---|---|---|---|
| Museeks | MIT | Forked source | Keep its copyright and MIT text: `LICENSES/Museeks-MIT.txt`, `THIRD_PARTY_NOTICES.md` |
| Tauri 2 and its plugins | Apache-2.0 or MIT | Library | Listed in the generated notices |
| React, TanStack, Zustand, Lingui, StyleX, base-ui, dnd-kit, lodash, nanoid, eventemitter3, semver, normalize.css | MIT | npm libraries | Generated notices (from the lockfile) |
| lofty, sqlx, axum, tokio, serde, rayon, walkdir and the other Rust crates | MIT or Apache-2.0 | Crates | Generated notices (`cargo about` style, from `Cargo.lock`) |
| hiby-mods database logic | MIT (tooling only) | Ideas re-implemented in the Kotlin core | `THIRD_PARTY_NOTICES.md` (done by the core work); hiby-mods firmware binaries are never used |
| Eclipse Temurin runtime (jlink build) | GPL-2.0 with Classpath Exception | Bundled runtime for `byos-cli.jar` | Its `legal/` folder shipped as is, plus a line in the notices |
| GraalVM CE native-image (alternative) | GPL-2.0 with Classpath Exception (tool) | Builds `byos-cli.exe`; the output is not encumbered | Notice for the included substrate libraries |
| Kotlin standard library | Apache-2.0 | Inside `byos-cli` | Notices |
| WebView2 runtime | Microsoft licence, part of Windows 11 | Not shipped (the bootstrapper is used on Windows 10) | None |
| Google platform-tools (`adb`) | Android SDK licence | **Not bundled.** Downloaded on first use with consent, as Compás does | The licence is shown before the download |
| Inter font (fallback) | SIL OFL 1.1 | Bundled | `LICENSES/OFL-1.1-Inter.txt` (already in the repository) |
| GPL code (Compás Tools, mutagen, vlcj, ...) | GPL | **Never** | n/a |

Packaging step: the release script generates `THIRD_PARTY_NOTICES.desktop.txt` from
`pnpm licenses list --json` and `Cargo.lock`, and fails the build on any licence not in an
allow-list (MIT, Apache-2.0, BSD-2/3, ISC, Zlib, MPL-2.0 unmodified, Unicode-3.0, OFL-1.1,
CC0-1.0, 0BSD).

## 8. Packaging

- **Windows (first)**: the Tauri bundler, NSIS installer (per-user, no admin) plus an MSI for
  managed installs. The WebView2 bootstrapper for Windows 10. Code signing with an
  Authenticode certificate (owner's decision; unsigned builds show SmartScreen warnings).
- **Sidecar**: `byos-cli` built in CI from `android/hibyos-install` (`:cli:fatJar`), then:
  - preferred: a jlink runtime with only the modules `jdeps` reports for the jar (normally
    `java.base`; the CLI's own crypto needs no JDK provider; an estimated 35 MB on disk and
    15 MB compressed, to be measured), launched as
    `runtime\bin\java.exe -XX:+UseSerialGC -Xmx256m -jar byos-cli.jar`;
  - later: a GraalVM native image (`byos-cli.exe`, about 20 MB, starts in about 20 ms) once
    reflection config for `MessageDigest` and the zip classes is checked (research open
    question).
- **App updates**: the Tauri updater with its own Ed25519 key (not the OTA key), from GitHub
  releases. The app never updates the HiBy without the user clicking Update.
- **Size target**: under 60 MB installed with the jlink runtime.
- **macOS and Linux** (later): `.dmg` (notarised) and AppImage/`.deb`, same code. adb comes
  from platform-tools downloaded on first use, or the system one.
- **Logs**: `%APPDATA%\ByOS\desktop\logs`, rotated at 5 x 2 MB, no network identifiers in
  file names. Screenshots in docs must blur SSIDs, IPs, MACs and serials (project rule).

## 9. Testing

- **Unit (UI side)**: JSON-lines parser and job state machine (one terminal line, prompts,
  cancel), the error catalogue (every CLI code and BTP name has EN/FR/ES text), path checks.
- **Contract**: the app runs against a fake `byos-cli` (`desktop/mock/byos-cli-mock.mjs`
  today) that emits the BYOS_CLI events, including errors, prompts and slow progress, so every
  sheet and banner can be exercised without a HiBy.
- **CLI against a fake device**: done in the `:cli` module (BYOS_CLI section 7).
- **End to end (manual, under the device rules)**: a checklist per release on a real R3 II,
  run by hand by the owner, never by an agent without permission.
- **Visual**: Playwright screenshots of every screen in light and dark, EN/FR/ES (Museeks
  already has Playwright), reviewed frame by frame for animations.

## 10. Milestones

| Milestone | Content | Depends on |
|---|---|---|
| **M0 Scaffold** (done) | `desktop/`: dependency-free UI (EN/FR/ES, light and dark) and local runner; device list, Summary (`status`), Files (browse, new folder, rename, delete with confirmation, upload by button or drag and drop), pairing sheet with the 6-digit code, Activity list; a mock CLI with scenarios; tests with `node --test`. | Nothing |
| **M1 CLI real** | Point the scaffold at the real `byos-cli.jar` (`:cli` module) and the fake device; fix contract mismatches. | `:cli` module |
| **M2 Fork** | Museeks fork builds on Windows with the ByOS name, EN/FR/ES, notices generated. | Approval of the npm/crate packages (section 11) |
| **M3 Devices** | `device_jobs` in Rust, Devices sidebar, Summary, Files (upload/download with drag and drop), Activity, adb manager. | M1, M2 |
| **M4 Sync** | Music tab, plan and apply, lyrics, covers, audiobooks; DB rebuild (5.5); Library DB tab. | `hibysync` DB_STAGE on the device, the shim apply step |
| **M5 Updates** | Update ByOS (5.6), install from file; build ByOS (5.7 steps 1 to 3); first-install assistant (5.8). | `hibysync` UPDATE_* |
| **M6 Reskin and polish** | iTunes/iOS look everywhere, animations reviewed frame by frame, accessibility, dark mode, Playwright screenshots. | M3 |
| **M7 Release** | Signed Windows installer, app updater, notices check, user guide (English, clear steps). | All |
| Later | Wi-Fi recovery reinstall (5.7 step 4), macOS/Linux, USB bulk transport (FunctionFS) instead of adb. | Device work |

## 11. What must be approved before M2

Nothing in this list is installed on this PC by the ByOS work. The owner decides.

Tooling already on this PC (checked 2026-11-12, nothing installed):

| Tool | Version | Use |
|---|---|---|
| Node.js / npm | 24.22.1 / 11.19.1 | Runs the scaffold today (no packages). The fork needs **pnpm 11** (Museeks' `packageManager`), which npm can install, or Corepack. |
| Rust (MSVC) | rustc / cargo 1.98.1, `stable-x86_64-pc-windows-msvc` | Tauri back end |
| Visual Studio Build Tools 2026, VS 2022 | present | MSVC linker for Tauri |
| WebView2 runtime | 157 | Tauri on Windows |
| JDK | Temurin 21.0.12 | `byos-cli`, jlink |
| .NET SDK | 10.0.402 | Not used |

Packages to approve for the Museeks fork:

1. **The Museeks source** (MIT): a clone of `github.com/martpie/museeks` at tag `0.23.4`. A
   read-only research copy exists in the session scratchpad, but the fork needs a proper clone.
2. **pnpm 11.0.9** (MIT), to install the JavaScript dependencies exactly from Museeks'
   lockfile.
3. **npm dependencies from Museeks' `package.json`** (all MIT or Apache-2.0): `react`,
   `react-dom` 19.2.6, `@tauri-apps/api` 2.11.0 and the `@tauri-apps/plugin-*` packages
   (clipboard-manager, dialog, fs, log, notification, opener, os, process), `@tanstack/*`
   (react-query, react-router, react-virtual), `@lingui/*`, `@stylexjs/stylex`,
   `@base-ui/react`, `@dnd-kit/*`, `zustand`, `lodash-es`, `nanoid`, `eventemitter3`,
   `semver`, `normalize.css`, `react-keybinding-component`, `browserslist`; dev tools:
   `@tauri-apps/cli` 2.11.2, `typescript` 6.0.3, `vite` (vite-plus), `vitest`, `playwright`
   1.60.0, `lightningcss`, the Babel/Lingui/StyleX/TanStack plugins, `simple-git-hooks`.
   Plus, for ByOS, `@tauri-apps/plugin-shell` (the JS side of the shell plugin).
4. **Rust crates from Museeks' `Cargo.toml`** (MIT/Apache-2.0, `lofty` MIT/Apache-2.0,
   `sqlx` MIT/Apache-2.0): `tauri` 2.11.2, `tauri-build`, the `tauri-plugin-*` crates,
   `axum`, `tokio`, `serde`, `serde_json`, `sqlx`, `lofty`, `m3u`, `rayon`, `walkdir`,
   `nosleep`, `uuid`, `ts-rs` and the rest of that file, with their transitive dependencies
   from `Cargo.lock`. Some Tauri crates (2.11.5, 2.12.x) are already in this PC's Cargo cache
   from earlier work, but the fork must not rely on a cache it did not lock.
5. **For packaging later**: the NSIS and WiX tools that the Tauri bundler downloads, and a
   code-signing certificate.
6. **At run time, by the end user**: Google's platform-tools, downloaded by the app on first
   use only after the user accepts their licence.

## 12. Open questions

- Q1. Fork location: `desktop/app` in this repository, or a separate repository (simpler
  upstream merges, separate releases)?
- Q2. Does `hibysync` get a `NAME` set command, so the app can rename the device?
- Q3. Reinstall through recovery from the app: which BTP command, and which reboot method
  really comes back on its own (critic correction 11)? Until answered, 5.7 stops after the copy.
  Whatever the answer, the command must follow the rules in 5.7 step 4 (USB only, confirmed
  on the device, `.upt` hash checked against the signed bundle index or the patcher's
  recorded hash, refused for files from a remote session), and SYNC_PROTOCOL must deny `.upt`
  names at the SD root to remote sessions.
- Q4. Does the stock player rescan on boot and overwrite a PC-built database? This decides
  whether 5.5 is the default for sync (critic Decision 7 open question).
- Q5. jlink runtime vs GraalVM native image for `byos-cli` (size, start time, reflection).
- Q6. Code signing certificate for Windows releases.
