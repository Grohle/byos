# ByOS desktop: scaffold

This folder is the first, working step of the ByOS desktop app described in
[docs/DESKTOP_APP.md](../docs/DESKTOP_APP.md) (milestone M0). It is a small web UI in the
iTunes/iOS style plus a local runner that starts `byos-cli` for it. It lists HiBy players,
shows a device summary, browses, creates, renames, deletes and uploads files on the SD card,
and, in the **Updates** tab, builds ByOS from HiBy's official firmware and installs it on the
HiBy over the USB cable (see "Build and install ByOS" below).

It needs only **Node.js 20 or newer**. There are no npm packages to install.

It is a test bench for the device pages and for the `byos-cli` contract
([docs/BYOS_CLI.md](../docs/BYOS_CLI.md)). The real app will be the Museeks-based Tauri app
(DESKTOP_APP.md section 6), which reuses this UI logic.

## Windows program (`ByOS.exe`)

The release ships the app as one Windows program. Double-click it: the first start unpacks it
into `%LOCALAPPDATA%\ByOS Desktop\<version>` (a few seconds), then it starts the local runner
without a console and opens the app in its own Microsoft Edge window. Closing the window stops
the runner and everything it started. It carries its own Node.js, a reduced Java runtime and
adb, so nothing has to be installed (THIRD_PARTY_NOTICES.md, 7.5).

Build it: `gradlew :cli:fatJar` (android/hibyos-install), `bash tools/wsl/zbuild_desktop_exe.sh
<version>` (the launcher, `windows/launcher.c`), then `python tools/package_desktop.py --version
<version>` (its docstring lists the Node.js zip, the `jlink` command and the platform-tools it
packs). The program is not code-signed yet: Windows SmartScreen may ask for **More info › Run
anyway** the first time.

## Try it without a HiBy (demo mode)

```
cd desktop
node server.mjs --mock --open
```

`--mock` uses `mock/byos-cli-mock.mjs`, a fake `byos-cli` with a fake card. It never touches a
device or the network. `--open` opens the page in its own Microsoft Edge window (or your
default browser). Without `--open`, open the address that the runner prints. It looks like
`http://127.0.0.1:PORT/?t=TOKEN`: the port is random unless you give `--port N`, and the
token is new at every start.

Demo scenarios, set before starting:

| `BYOS_MOCK_SCENARIO` | What you see |
|---|---|
| `ok` (default) | One HiBy over USB, one paired over Wi-Fi but out of reach |
| `usb_storage` | The card is connected to the computer as a USB drive |
| `no_device` | No HiBy found |
| `daemon_not_running` | ByOS Sync is not open on the HiBy (the banner offers **Start ByOS Sync**) |
| `slow` | Slow uploads, to try progress and cancel |
| `silent_pair` | Pairing waits without heartbeat lines (tests the watchdog) |
| `offline` | The firmware download fails (no internet) |
| `build_fail` | The ByOS build fails its own check |
| `low_battery` | The install stops: the HiBy's battery is too low |
| `reinstall_refused` | The HiBy's own reinstall checks say no |

`BYOS_MOCK_STEP_MS` sets the pace of the fake build and install (default 60 ms per step).

`BYOS_MOCK_PAIR_WAIT_MS` sets how long the fake user takes to tap *Allow* (default 400 ms).

The fake card is kept in `BYOS_MOCK_STATE` (by default `byos-cli-mock-state.json` in your
temporary folder). Delete that file to start again. The pairing code of the fake HiBy is
`815365`.

## Use it with a real HiBy

1. Build `byos-cli`: in `android/hibyos-install`, run `gradlew :cli:fatJar` (needs the `:cli`
   module, see BYOS_CLI.md). The runner looks for
   `android/hibyos-install/cli/build/libs/byos-cli.jar`.
2. Make sure `adb` is on `PATH` (or pass `--adb PATH`), and open *Settings › ByOS Sync* on the
   HiBy.
3. Start the runner:

   ```
   node server.mjs --open
   ```

   Other options: `--cli PATH` (a `byos-cli.jar`, a native `byos-cli.exe` or a `.mjs` script),
   `--config DIR` (byos-cli's configuration folder), `--port N`, `--adb PATH`. The `BYOS_CLI`
   environment variable can also name the program.

## Build and install ByOS

Open a HiBy, then the **Updates** tab:

1. Press **Build and Install**. The app downloads HiBy's official firmware from HiBy's own
   servers (or press **Use a firmware file…** first if you already have `r3ii_2025.upt`),
   checks it, and builds ByOS on this computer. Both are kept, so the next time is instant.
2. Confirm the sheet. Keep the HiBy on the USB cable and do not press anything on it.
3. The app copies the ByOS folder and the ByOS firmware to the card, starts the HiBy's own
   reinstall through HiBy's recovery, and waits until the HiBy is back with the new ByOS
   (a few minutes). The page ends with "ByOS 0.9.0 is installed."

A HiBy on Wi-Fi can only get the build ready: firmware never travels over Wi-Fi. The details
are in [docs/DESKTOP_APP.md](../docs/DESKTOP_APP.md) section 5.7 and the commands in
[docs/BYOS_CLI.md](../docs/BYOS_CLI.md) ("Build ByOS").

In demo mode the whole flow runs against the fake CLI. For a fixed picture of one state, add
`demo=NAME` to the address the runner prints (`...?t=TOKEN&demo=building#updates`), with
`downloading`, `ready`, `building`, `sending`, `installing`, `done`, `failed` or `wifi`.

The Library tab and the Sync tab work against made-up music in demo mode (about 40 albums, 400
songs and 6 audiobooks, covers drawn by `mock/library-fixture.mjs`). Fixed pictures of the
Library tab: `demo=lib-unavailable`, `lib-scanning`, `lib-empty`, `lib-stale`, `lib-open&open=3`
(an open album; add `&menu=1`, `&info=1` or `&delete=1`), `lib-view` with `#library/songs`,
`/artists`, `/books` or `/playlists` (and `&q=TEXT`). `BYOS_MOCK_SCENARIO=snapshot_io` makes the
real flow fall back to a card scan.

### The Sync tab and the computer's library

The device's **Sync** tab (it replaced the Music tab) links one folder on this computer per kind
(music, audiobooks, books, video), with its options (entire library or chosen artists, albums and
genres; Artist/Album or the folder's own layout; what to do with files that are only on the
HiBy: keep, delete or ask; remove unselected; on/off). **Sync Music** (or Audiobooks, Books,
Video) and **Sync All** run `sync-plan` and open the preview: files to copy, update and move,
files only on the HiBy grouped by reason with Keep/Delete per group and per file (and Copy to
Computer), conflicts (Computer, HiBy, Both, Skip), files that cannot be copied, the space left,
and an Apply button that says what it deletes ("Sync and Delete 12 Files from the HiBy"). When
DeleteGuard trips (the folder is missing, on another drive, unreadable, or many files would go),
nothing is ticked and deleting needs the count typed. Then the progress (current album, rate,
time left, Pause, Cancel), the HiBy's library update, and the result. A sync that stopped
(cable, Wi-Fi, USB storage mode, battery, Pause) is resumed with **Resume**; nothing is copied
twice. **Cancel** really stops the sync, and an earlier sync that did not finish can be
**Discarded** (what it copied stays on the HiBy and is known to the next sync). A HiBy that
another computer is syncing shows **Take Over**; when the other sync runs on this same computer
(another window, or byos-cli in a terminal), the page only offers to wait and try again.
Deleting files that are not on this computer (added or downloaded on the HiBy, or synced from
another computer) is said to be permanent, and those groups offer **Copy All to Computer**. With
"Selected artists, albums and genres" and nothing ticked, that folder cannot sync until
something is chosen. The quiet "changes on this computer" check writes its own plan file, and
`sync-apply` runs only on the plan the preview showed (the page sends its `run_id`; another one
is refused with `plan_changed`). The page never names a file
on this computer: a folder is chosen in Windows' own dialog and the page only gets a token for
it (`/api/links/pick`); the plan and the choices stay in byos-cli's cache. The Music tab's
folders and selection become music links the first time the Sync tab opens.

The sidebar's **Library** (Recently Added, Artists, Albums, Songs, Audiobooks, Playlists) shows
the music **on this computer**: the linked folders, read by `music-scan`, with Rebuild and Sync
Music. The device's Library tab still shows the HiBy's own library.

Every state can be reached in demo mode. Scenarios (`BYOS_MOCK_SCENARIO`, set before starting):

| Scenario | What the Sync tab shows |
|---|---|
| `ok` | Plans with copies, updates, moves, files only on the HiBy (deleted here, from another computer, added on the HiBy, downloaded on the HiBy), a conflict, and a video that cannot be copied |
| `sync_unavailable` | The music folder is missing: "Your music folder isn't available. Nothing will be deleted." |
| `sync_mass_delete` | 240 files deleted here: nothing ticked; deleting them needs the count typed |
| `sync_locked` | Another computer (ANA-LAPTOP) is syncing the HiBy: Take Over |
| `sync_locked_here` | Another sync of the HiBy runs on this computer: wait, then Try Again (no Take Over) |
| `sync_pause` | The HiBy stops answering after 40 % of the copies: paused, then Resume |
| `sync_usb_pause` | The card becomes a USB drive during the copies |
| `sync_battery` | The HiBy's battery is too low to start |
| `sync_partial` | Three files fail: Retry Failed Songs (Books, Videos… for the other kinds) |
| `sync_db_pending` | The files are copied but the HiBy's library is not updated |
| `card_full` | Not enough space on the card |
| `ask_policy` | New links ask about each file that is only on the HiBy |

`BYOS_MOCK_SYNC_STEP_MS` sets the pace of the fake scan and copies (default 6 ms). Fixed
pictures: `demo=sync-empty`, `sync-migrate`, `sync-idle` (add `&options=music`, `&off=video`, `&nochoice=1`,
`&chosen=artist:NAME|genre:NAME`), `sync-planning`, `sync-preview&kind=music` (or `all`; add
`&only=add,move` for the open sections, `&choose=deleted_on_computer:delete` for choices,
`&typed=24&submit=1` for the typed confirmation, `&wifi=1` for the "faster over USB" hint),
`sync-syncing&freeze=1`, `sync-building&freeze=1`, `sync-paused&freeze=1&reason=user`, and the
real flow to its end with `sync-run` and `sync-result` (add `&resume=1`). `&kinds=music,video`
chooses which kinds get a demo folder. The computer's library: `demo=pc-albums#computer/albums`,
`pc-open&open=3`, `#computer/songs`. For a frame-by-frame review of an animation, `&frame=MS`
stops it MS after it starts (`&framesel=%23sync-preview` or `.sy-options` limits it to one element).

## How it works

```
browser page (ui/)  --HTTP + server-sent events-->  server.mjs  --spawn-->  byos-cli  --> HiBy
```

- `server.mjs` listens on `127.0.0.1` only, on a random port. Each start makes a random
  token. The window opens `/?t=TOKEN`, which stores the token in an HttpOnly, SameSite=Strict
  cookie and redirects to `/`. The page and every API call need that token (cookie or
  `X-ByOS-Token` header). The `Host` header must be the runner's own address, and a foreign
  `Origin` is refused. The page has a strict Content-Security-Policy.
- **What this protects, and what it does not.** It stops web pages in your browser and DNS
  rebinding from reaching the runner. It is not a wall between accounts or programs on the
  same computer: anyone who can read the printed address (your terminal, your process list,
  your browser profile) can use it, and with it the connected HiBy. This runner is a
  development bench for a single-user computer. Do not run it on a shared machine. The real
  desktop app (Tauri) replaces HTTP with in-process IPC.
- The UI never sends command lines. It sends a command name and parameters, and
  `lib/cli.mjs` builds the arguments (an allow-list of commands; device paths checked with the
  BTP/1 rules and normalised to NFC; device specs checked). `byos-cli` is started with an
  argument array, never through a shell.
- Jobs for the same device run one at a time, because `hibysync` serves one session at a
  time. Jobs for different devices run side by side, and device discovery (`devices`) never
  waits. Devices are told apart by the spec the page sends (`usb:SERIAL`, `wifi:ID`).
- Each job's JSON lines are checked (`lib/jsonl.mjs`) and streamed to the page. A job always
  ends with exactly one `result` or `error`: a process that exits without one becomes
  `internal`, and one that stops talking is cancelled and becomes `timeout`. "Stops
  talking" means 25 s without a line, or longer after a `progress` line for a long wait
  (130 s for `pair_wait`, 70 s for `install` and `reconnect`). An unanswered prompt is
  answered "no" after 130 s and the job ends as `timeout`; the sheet closes by itself.
- Arguments: options first, then `--`, then the device paths, so a file named `-r` or
  `--yes` is never read as an option.
- Prompts (`pair_code`, `confirm`) open a sheet in the page, and the answer goes back to
  `byos-cli` on its stdin. Cancel writes `{"cancel": true}` and then stops the process.
- Uploads: the browser sends the file to the runner, which keeps it in a private temporary
  folder, sets the copy's modification time to the original file's, runs
  `byos-cli put --mkdir --skip-identical -- ...`, and deletes the copy at the end (also when
  the job is refused, cancelled, or the runner is stopped with Ctrl+C). Folders left by a
  runner that crashed are removed at the next start. Uploads go into a folder, never the card
  root.

Files:

| Path | What |
|---|---|
| `server.mjs` | Local runner: static files, job API, uploads |
| `lib/cli.mjs` | Finds and runs `byos-cli`, builds the arguments, job state machine |
| `lib/jsonl.mjs` | JSON-lines splitting and checks |
| `lib/btp-path.mjs` | BTP/1 path rules (also served to the page) |
| `lib/build-flow.mjs` | The Updates page's build-and-install steps from the CLI lines (also served to the page) |
| `lib/sync-flow.mjs` | The Sync tab's rules: migration, preview groups, choices and DEC.json, deletions, outcomes, progress (also served to the page) |
| `lib/media-format.mjs`, `lib/media-store.mjs` | The libraries' formatting and the runner's own files (settings, cache names) |
| `mock/byos-cli-mock.mjs` | Fake `byos-cli` for demos and tests (`mock/sync-mock.mjs`: the folder sync commands) |
| `ui/` | The page: `index.html`, `app.css`, `app.js`, `media.js` (libraries), `sync.js` (Sync tab), `i18n.js` (English, French, Spanish) |
| `test/` | Tests (`node --test`) |

## Tests

```
cd desktop
npm test
```

(`npm test` runs `node --test "test/*.test.mjs"`; nothing is installed.) The tests check the
path rules against `tests/btp/vectors.json`, the JSON-lines reader, the argument builder, every
mock command with exit codes, prompts, pairing with a right and a wrong code, cancel, a broken
CLI, the watchdog, and the runner's API (token, Host and Origin checks, streaming, uploads).
`test/build-flow.test.mjs` checks the build contract: the arguments of the build commands
(and what the page may never send), the JSON lines of `build-info`, `firmware-fetch`, `build` and
`build-send` for the whole flow and every failure, how `lib/build-flow.mjs` maps them to the four
steps, and the runner's firmware upload (temporary copy removed, local jobs not queued behind a
device).
`test/sync-flow.test.mjs` and `test/sync.test.mjs` check the Sync tab: the migration of the Music
tab, the preview's groups and choices and the DEC.json they make, the typed mass-delete count,
every way a run ends (done, problems, library not updated, paused for each reason, locked and
taken over, resumed without copying twice), a 50,000-file preview, that the page never names a
file on this computer, and the English, French and Spanish texts.
