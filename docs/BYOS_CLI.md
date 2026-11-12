# byos-cli: command-line client for ByOS

`byos-cli` is the command-line front end of the shared Kotlin client core
(`android/hibyos-install/core`). The desktop app runs it as a child process and reads its
output. People can also use it by hand. It speaks BTP/1 to `hibysync` on the HiBy (see
[SYNC_PROTOCOL.md](SYNC_PROTOCOL.md)), and it reuses the core's patcher, signature checks and
downloader for the firmware and update commands.

Status: **specification**. It describes the interface the `cli` Gradle module must provide.

## 1. Build and run

- Gradle module `:cli` in `android/hibyos-install` (next to `:core` and `:app`), plain
  Kotlin/JVM, with no dependencies beyond those already declared in the project. Its `fatJar`
  task writes `cli/build/libs/byos-cli.jar` with the core and the Kotlin standard library inside.
- Run it with Java 17 or newer:

  ```
  java -jar byos-cli.jar <command> [options] [arguments]
  ```

  The desktop app bundles its own runtime. A `byos-cli` / `byos-cli.cmd` launcher script MAY
  wrap the line above.
- USB needs Android's `adb` program: found with `--adb PATH`, then the `ADB` environment
  variable, then `PATH`. `byos-cli` never bundles or downloads platform-tools itself (see the
  research's licence note). The desktop app takes care of that.

## 2. Output: JSON lines

Unless `--human` is given, **stdout carries only JSON lines**: one UTF-8 JSON object per line,
ending in `\n`, never pretty-printed. Diagnostics go to stderr (only with `--verbose`). Every
object has:

| Key | Meaning |
|---|---|
| `"v"` | output format version, `1` |
| `"event"` | kind of line, see below |
| `"t"` | milliseconds since the command started |

Events:

| Event | When | Main keys |
|---|---|---|
| `start` | first line, always | `command`, `version` (byos-cli version), `protocol` ("BTP/1") |
| `device` | a device found or connected | `id` (device_id hex), `name`, `transport` (`usb`/`wifi`), `address`, `serial` (USB), `paired`, `pairing_open`, `sd_state`, `byos_version`, `firmware` |
| `entry` | one item of a listing | `name`, `path`, `kind` (`file`/`dir`/`other`), `size`, `mtime` (Unix seconds) |
| `progress` | long operations, at most 5 per second | `op`, `path`, `done`, `total` (bytes, or permille when `unit` is `"permille"`), `rate` (bytes/s), `unit` |
| `prompt` | input needed (pairing code, confirmation) | `kind`, `message`, plus kind-specific keys |
| `warn` | something the user should know, the command goes on | `code`, `message` |
| `result` | **last line on success** | `ok: true` plus command-specific keys |
| `error` | **last line on failure** | `ok: false`, `code`, `message`, optional `btp` (`{"code": 33, "name": "E_NOT_FOUND"}`), optional `path` |

Exactly one `result` or `error` line ends every run. Numbers are JSON numbers (sizes up to
2^53 are exact). Hashes are lowercase hex. Paths on the device are BTP paths (relative to the
SD root, `/` separators, NFC). Local paths are as given.

**Heartbeat.** While a command waits for something that can take longer than 5 s (the user
tapping *Allow* during pairing, an install, a reconnect after the shell restarts, a slow
device reply), byos-cli repeats the current `progress` line (same `op`, unchanged `done`) at
least every 5 s. Callers use silence as a sign that the CLI or the link is stuck: the desktop
app cancels a job after 25 s without any line (DESKTOP_APP.md 4.3), and longer only for the
operations listed there. Waiting on a `prompt` is the only silent state.

**Option parsing.** Options and positional arguments can be mixed, as in the examples. A lone
`--` ends the options (POSIX): every argument after it is a path, even one that starts with
`-`, so `byos-cli rm -r --yes -- -old` deletes the item `-old`. Callers that build command
lines from data (the desktop app) always put their options first, then `--`, then the paths.

**The SD root as a target.** Where a command takes a remote folder (`put`, `ls`), the SD root
is written as the empty string `""` or as a lone `/`. Any other path that starts with `/` is
refused (`usage`). Uploads to the SD root are refused over Wi-Fi for names ending in `.upt`
(see the firmware note in `put`).

`--human` prints the same information as plain English text for people. Its format is not
stable and must not be parsed.

### 2.1 Exit codes

| Code | `error.code` | Meaning |
|---|---|---|
| 0 | | success |
| 1 | `usage` | bad command line |
| 2 | `no_device` | no device found, connection refused, adb missing or unauthorised |
| 3 | `auth` | pairing failed, device identity changed, unknown client (needs pairing) |
| 4 | `remote` | the device answered with a BTP error (see `btp`) |
| 5 | `local_io` | cannot read or write a local file |
| 6 | `verify` | hash mismatch, bad signature, wrong firmware file |
| 7 | `cancelled` | cancelled by the caller |
| 8 | `rejected` | the user said no on the HiBy (pairing Deny, update not confirmed) |
| 9 | `timeout` | the device stopped answering |
| 10 | `internal` | bug |

### 2.2 Input and cancellation

Commands that need an answer print a `prompt` line and then read **one line from stdin**:
either a JSON object (`{"answer": "815365"}`, `{"answer": true}`) or the plain text answer. An
empty line or EOF means "no". The answer can also be given in advance with an option (for
example `--code`, `--yes`).

To cancel, the caller writes `{"cancel": true}` on stdin or sends SIGINT (Ctrl+C) /
`Process.destroy()`. byos-cli sends BTP `CANCEL`, waits up to 2 s for the device, prints
`error` with code `cancelled` and exits 7. Uploads keep their device-side part file, so running
the same `put` again resumes.

## 3. Global options

| Option | Meaning |
|---|---|
| `--device SPEC` | Which HiBy. `usb` (the only USB device), `usb:SERIAL`, `wifi` (the only paired one in reach), `wifi:DEVICE_ID` (16 hex digits, or a unique prefix of at least 4), `HOST[:PORT]`. Default: `usb` if exactly one ADB device is attached, otherwise the most recently used paired device. |
| `--adb PATH` | adb program (USB). |
| `--local-port N` | Local port for `adb forward` (default 7340; if busy, adb picks one with `tcp:0`). |
| `--config DIR` | Configuration folder (default: `%APPDATA%\ByOS\cli` on Windows, `~/Library/Application Support/ByOS/cli` on macOS, `$XDG_CONFIG_HOME/byos-cli` or `~/.config/byos-cli` on Linux). |
| `--name NAME` | Name this client shows on the HiBy when pairing (default: the computer's host name, at most 64 bytes). |
| `--timeout MS` | Connection and reply timeout (default 15000). |
| `--human` | Text output for people instead of JSON lines. |
| `--verbose` | Diagnostics on stderr. |

### 3.1 Configuration folder

| File | Content |
|---|---|
| `client.key` | the client static X25519 secret `cs` (32 bytes). Created on first use, readable only by the user (POSIX 0600; on Windows, an ACL for the current user only). Never printed. |
| `devices.json` | paired devices: `[{"id", "name", "public_key" (Ds hex), "paired", "last_address", "last_seen"}]`, written to `.tmp` and then renamed |
| `cache/` | downloaded release files (`update-send --latest`) |
| `cache/bundle/` | the ByOS files: `builtin.zip` (unpacked from the jar), `updated.zip` (`bundle-update`) |
| `cache/firmware/` | HiBy's official firmware and its `firmware.ok` check record (`firmware-fetch`) |
| `cache/build/` | the last build: `r3ii_2025.upt`, `ByOS/`, `result.txt`, `build.json` (`build`, `build-send`) |

## 4. Connection

Every device command connects first:

1. **USB**: `adb [-s SERIAL] forward tcp:<local> tcp:7340`, then TCP to `127.0.0.1:<local>`. If
   the connection is refused or closed before HELLO (adb accepts and drops it when nothing
   listens on the device), the daemon is not running. byos-cli then prints
   `warn {"code": "daemon_not_running"}` and, only with `--start`, runs
   `adb shell /usr/data/hibymod/shell/hibysync serve --idle-exit 300 --detach` (where the installer puts it; `bin/hibysync` for a hand-copied daemon) and retries for
   5 s. Otherwise it fails with `no_device` and a message asking the user to open
   *Settings › ByOS Sync* on the HiBy.
2. **Wi-Fi**: the address from discovery or `devices.json`, then TCP to port 7340, HELLO, and
   AUTH with the stored keys. Never in cleartext.
3. HELLO checks: version 1, DEVICE_ID equal to the stored one for Wi-Fi (`auth` error
   "different device or the HiBy was reset; pair again" otherwise).

A `device` line is printed after HELLO. The connection is closed with `BYE` when the command
ends. Each run is one command and one connection. The desktop app keeps no long-lived socket
through the CLI.

## 5. Commands

In the tables, "Result" lists the keys of the final `result` line, besides `ok`.

### devices

```
byos-cli devices [--wait MS] [--usb-only | --wifi-only]
```

Lists HiBy players reachable now: ADB devices (`adb devices -l`; each one is probed with HELLO
through a forward) and Wi-Fi devices (UDP PROBE broadcast on every IPv4 interface, collecting
ANNOUNCEs for `--wait` ms, default 1500). Paired devices from `devices.json` that did not
answer are listed with `"reachable": false`. Prints one `device` line per device, with
`reachable`, `paired` and `pairing_open`.

Result: `count`.

### connect

```
byos-cli connect [--pair] [--code NNNNNN] [--forget]
```

Checks the connection and, with `--pair`, pairs a Wi-Fi device (SYNC_PROTOCOL 8.2):

1. The user opens *Settings › ByOS Sync › Pair new device* on the HiBy (`pairing_open`). If it
   is not open: error `remote` with `E_PAIR_CLOSED`, message "Open Pair new device on the HiBy".
2. After the key exchange byos-cli prints
   `prompt {"kind": "pair_code", "message": "Type the 6-digit code shown on the HiBy", "device": "...", "digits": 6}`
   and reads the code (or uses `--code`). It does **not** print its own code: the user types
   the one shown on the HiBy, and the CLI compares it. A wrong code: `error auth`
   ("code does not match"), and the device is told with BYE.
3. Then `progress {"op": "pair_wait"}`, repeated every 5 s (heartbeat, section 2), while the
   HiBy asks the user to *Allow* (up to 120 s). On Allow, the device is stored in
   `devices.json`. On Deny or timeout: `error rejected`.

`--forget` removes the device from `devices.json` (the HiBy keeps its own peer list until the
user removes it there).

Result: `id`, `name`, `paired` (bool), `transport`, `encrypted` (bool).

### ls

```
byos-cli ls [PATH] [--all] [--recursive] [--max-depth N]
```

Lists a folder on the SD (`""` or omitted = SD root). One `entry` line per item. `--all` shows
hidden names. `--recursive` walks subfolders (LIST per folder, depth-first, `--max-depth`
default 32).

Result: `count`, `path`.

### put

```
byos-cli put LOCAL... REMOTE_DIR_OR_PATH [--overwrite | --skip-identical] [--recursive] [--mkdir] [--preserve-mtime | --no-mtime]
```

Uploads files (PUT, SYNC_PROTOCOL 11). With one local file and a remote path that does not
end in `/`, it uploads to that exact path. Otherwise every file goes into `REMOTE_DIR`.
`--recursive` uploads folders, recreating the tree (MKDIR PARENTS). `--mkdir` creates a missing
target folder.

- The default mode is CREATE: an existing file is an error (`remote` / `E_EXISTS`), and other
  files go on, with a `warn` for each skipped one. The run ends with `error` if any file failed.
- `--overwrite` is REPLACE. `--skip-identical` is SKIP_IDENTICAL.
- Local names are converted to NFC. Names the device would refuse (section 9.2 rules, for
  example `?` or a trailing dot) are reported **before** anything is sent, with
  `error usage` and the list in `invalid`.
- byos-cli hashes each file first (`progress op "hash"`), then sends it (`progress op "put"`).
  When the device answers with a non-zero offset, a `warn {"code": "resumed", "offset": N}`
  line is printed.
- `mtime` is sent from the local file unless `--no-mtime`.
- If the connection drops, it reconnects and resumes up to 3 times (2, 4, 8 s apart) before
  failing.
- Firmware files: an upload of a name ending in `.upt` (any case) to the SD root is refused
  over Wi-Fi (`error usage`, before anything is sent). `.upt` files carry no ByOS signature
  and the recovery loader flashes them from the SD root, so only a local USB session may put
  them there (DESKTOP_APP.md 5.7). The device enforces the same rule (it is the boundary);
  this check only gives a clearer message.

Result: `files`, `written`, `skipped`, `failed`, `bytes`, plus `items:
[{"path", "result": "written"|"skipped"|"failed", "sha256", "error"?}]`.

### get

```
byos-cli get REMOTE... LOCAL_DIR_OR_PATH [--recursive] [--overwrite] [--no-verify]
```

Downloads files (GET with the SHA flag). The data is written to `LOCAL.byos-part`, verified
against `GET_END.sha256` and then renamed. A leftover `.byos-part` whose size is smaller than
the remote file, with the same remote mtime (kept in `LOCAL.byos-part.json`), resumes from its
size. Existing local files are not replaced without `--overwrite`. `--no-verify` skips the
whole-file hash, which saves the device a read when resuming.

Result: `files`, `bytes`, `items: [{"path", "local", "sha256"}]`.

### mkdir

```
byos-cli mkdir PATH... [-p]
```

MKDIR (with PARENTS for `-p`). Result: `created` (list).

### mv

```
byos-cli mv SRC DST [--replace]
```

MOVE inside the SD. Result: `src`, `dst`.

### rm

```
byos-cli rm PATH... [-r] [--yes]
```

DELETE. With `-r` and no `--yes`, it first prints
`prompt {"kind": "confirm", "message": "Delete folder X and everything in it?", "path": "X"}`
and reads the answer. Result: `deleted` (list).

### df

```
byos-cli df
```

DF. Result: `sd_state` (`mounted`, `no_card`, `usb_storage`, `read_only`), `sd_fs`, `sd_total`,
`sd_free`, `data_total`, `data_free`. Works when the SD is unavailable, which is how the
desktop app shows "The HiBy's card is connected as a USB drive".

### db-pull

```
byos-cli db-pull OUT.db
```

DB_SNAPSHOT to `OUT.db` (through `OUT.db.byos-part`, verified, renamed). Also writes
`OUT.db.json`: `{"device": id, "sha256", "size", "time"}`. The `sha256` is the base revision
for `db-push`.

Result: `path`, `size`, `sha256`.

### db-build

```
byos-cli db-build --base SNAPSHOT.db --music ROOT --out NEW.db
                  [--sd-root ROOT] [--covers none|folder] [--lyrics none|sidecar]
                  [--sort stock] [--report REPORT.json]
```

Builds a library database on the computer (no device needed), with the PC-side DB builder of
the core:

- `--base`: a snapshot from `db-pull`. Its schema is kept unchanged, and the tables that hold
  the user's own data (history, favourites, playlists, the queue together with
  `MANAGER_TABLE`, books, version) are copied as they are.
- `--music`: the folder whose contents are on the SD, either the card itself mounted as a
  drive, or a local mirror. `--sd-root` gives the folder that maps to `a:\` when `--music` is
  a subfolder of it.
- Rules for the R3 II 2025 schema (re-implemented in Kotlin, with the ideas credited to
  hiby-mods, MIT, in `THIRD_PARTY_NOTICES.md`): `a:\` paths, NFC, UTF-8 with a trailing NUL,
  stock format codes, quality including MQA (`18`), filled `ctime`/`mtime`, CUE handling,
  `MEDIA2_TABLE` only as the SD-root folder cache, `SEARCH_TABLE` left empty, unique M3U ids.
- Tags are read with the core's own reader. No GPL tag library is used. Audio files are never
  modified. `--covers folder` only records existing `cover.jpg`/`folder.jpg`.
- `progress op "scan"` (files) and then `op "write"`.

Result: `out`, `tracks`, `albums`, `artists`, `skipped` (unreadable files, with `warn` lines
for each), `sha256` (of NEW.db), `base_sha256` (from `SNAPSHOT.db.json` or the hash of the
base).

### db-push

```
byos-cli db-push NEW.db [--base-sha256 HEX] [--force]
```

PUT in NS_DB (`staged.db`), then DB_STAGE with the base revision: from `--base-sha256`, from
the `base_sha256` that `db-build` wrote next to NEW.db (`NEW.db.json`), or with `--force`. On
`E_DB_STALE`: `error remote` with the message "The library changed on the HiBy since db-pull;
pull and build again".

Result: `staged` (true), `sha256`, `live_sha256`, `restart_needed` (true), `message` ("Restart
the HiBy to use the new library").

### Build ByOS: the ByOS files

The firmware and build commands below use the **same code as the Android app**: the core's
`BundleStore`, `FirmwareCache` and `ByosBuild` (`core/.../Build.kt`) on top of `Patcher` and
`FirmwareFetcher`. Which ByOS files (the "bundle") they build with:

1. `--bundle BUNDLE.zip`, when given;
2. otherwise the bundle **built into `byos-cli.jar`** (Gradle copies the Android app's
   `app/src/main/assets/hibyos-bundle.zip` into the jar), unpacked once to
   `cache/bundle/builtin.zip`;
3. or, when it is newer, a bundle downloaded by `bundle-update` (`cache/bundle/updated.zip`).
   The Android app follows the same rule.

### build-info

```
byos-cli build-info [--bundle BUNDLE.zip] [--dir DIR]
```

No device and no network. Says what a build would use and what is already done.

Result: `hibyos_version`, `generated_from`, `bundle` (`builtin`, `updated` or `file`),
`bundle_path`, `firmware` (`label`, `version`, `file`, `size`, `md5`, `sha256`, `build`,
`page`, `can_download`), `firmware_ready` (the official firmware is in `cache/firmware/` and
was checked), `firmware_path`, and `build`: `null`, or the finished build in `DIR` (default
`cache/build/`) made from these ByOS files (`dir`, `upt`, `size`, `sha256`, `hibyos_version`,
`build_id`, `files`, `sd_size`).

### bundle-update

```
byos-cli bundle-update [--check]
```

Asks the project's releases for newer ByOS files (the signed index named in the built-in
bundle, checked with the release keys of the built-in bundle, never with keys from a download)
and, without `--check`, downloads them (`progress op "download"`) to `cache/bundle/updated.zip`.
This is the Android app's "Check for updates" of the ByOS files.

Result: `current`, `latest`, `available` (bool), `updated` (bool), `size`.

### firmware-fetch

```
byos-cli firmware-fetch [--bundle BUNDLE.zip] [--from FILE.upt] [--out FILE.upt]
```

Gets the **official** HiBy firmware that the ByOS bundle is made for and keeps it, checked,
in `cache/firmware/` (the file under the bundle's name plus `firmware.ok` with its size and
SHA-256, written only after the check):

- Without `--from`: if the cache already holds the right file, nothing is downloaded
  (`cached: true`). Otherwise it downloads from the bundle's `FirmwareSpec` (HiBy's own links,
  tried in order, with resume): `progress op "download"` (bytes), then `op "check"`
  (permille). Size, MD5 and SHA-256 must match. If no link works, the error carries the `page`
  URL so the user can download the file themselves.
- `--from FILE.upt`: a file the user already has. It is checked (`op "check"`) and copied
  into the cache. A wrong file is `error verify` with its `size` and `md5`, and nothing is kept.
- `--out FILE.upt` also writes a copy there.

HiBy's firmware is never redistributed by ByOS: it always comes from HiBy's servers to the
user's computer.

Result: `path`, `version`, `size`, `md5`, `sha256`, `cached` (bool), `source` (`download`,
`file` or `cache`).

### build

```
byos-cli build [--bundle BUNDLE.zip] [--firmware FILE.upt] [--out DIR] [--force]
```

Builds ByOS for the HiBy from the official firmware (by default the one `firmware-fetch` kept)
into `DIR` (default `cache/build/`), with no device and no network:

```
DIR/r3ii_2025.upt   the ByOS firmware (the patched official firmware)
DIR/ByOS/...        the ByOS folder for the card (the bundle's SD files, each checked)
DIR/result.txt      the core's build record (the same file the Android app keeps)
DIR/build.json      what build-send copies, in copy order, with sizes and SHA-256
```

Progress: `op "patch"` with `unit "permille"` for the whole patch and an extra `stage` key
(`check`, `read`, `apply`, `build`, `verify_rootfs`, `write`, `verify_upt`, in this order),
then `op "export"` (bytes) for the ByOS folder. A finished build of the same ByOS files is
reused (`cached: true`) unless `--force` or `--firmware` is given. The patch takes a few
seconds on a PC. Wrong firmware: `error verify`. No firmware yet: `error usage`.

Result: `out`, `upt`, `size`, `md5`, `sha256`, `hibyos_version`, `build_id`, `files` (files
in the ByOS folder), `sd_size`, `cached`.

### build-send

```
byos-cli build-send [--dir DIR] [--install [--yes] [--no-wait] [--wait-max S]]
```

Copies a build to the HiBy and, with `--install`, installs it **hands-free**. **USB only**:
over Wi-Fi it is `error usage` before anything is sent (firmware files never travel over
Wi-Fi, DESKTOP_APP.md 5.7).

1. Every file of `build.json` is hashed again (`op "hash"`). A file that changed since the
   build is `error verify` and nothing is sent.
2. Other `.upt` files at the card root are moved to `ByOS/firmware/parked/` (`warn` with code
   `parked` for each): the recovery flashes the first one it finds (`update.upt` first), so
   only the ByOS firmware may be there.
3. The ByOS folder is copied with SKIP_IDENTICAL (a second run copies only what changed), in
   `build.json` order: the payload's `MANIFEST.md5` and `VERSION` last, so a copy that stops
   half way leaves the old version on the card and the HiBy does not install a half-copied
   payload. Then the firmware goes to the card root with REPLACE. `op "put"` per file.
4. With `--install`: a `prompt {"kind": "confirm", "version": ...}` (or `--yes`), then a
   battery check (at least 30 % or charging, else `error remote` with `E_BATTERY`). Then
   byos-cli runs, over adb, the HiBy's own reinstall command
   `/usr/bin/hibymod reinstall --yes`, the same command the HiBy's *Software Update › On the
   SD card* page runs. It checks that HiBy's recovery is intact and that the `.upt` at the
   card root is a firmware image, switches to the recovery and restarts. If it refuses
   (`abortado: ...`), that is `error remote` with its message and nothing restarts.
5. byos-cli waits for the HiBy (`op "reboot"`, then `op "reconnect"`, both repeated as
   heartbeats): the recovery installs the firmware, ByOS starts and installs its folder from
   the card (one more restart when the payload changed). It starts `hibysync` over adb when
   needed and checks `byos_version` in HELLO. The same version as before only counts after the
   HiBy was seen restarting. Default limit 900 s (`--wait-max`), then `error timeout`.
   `--no-wait` returns right after the restart was started (`state "rebooting"`).

Result: `version`, `upt`, `files`, `written`, `skipped`, `bytes`, `parked` (list), `state`
(`sent` without `--install`, `rebooting` with `--no-wait`, `installed`), `installed` (the
version the HiBy reported).

### patch

```
byos-cli patch --firmware FILE.upt [--bundle BUNDLE.zip] --out BYOS.upt [--work DIR]
```

Builds only the ByOS firmware file from the user's official firmware with the core's
`Patcher` (the same code as the Android app). `progress` reports the patcher stages
(`op` = stage name, `unit` `"permille"`). Wrong firmware: `error verify`. `build` is the
complete version of this command (firmware plus ByOS folder plus record).

Result: `out`, `size`, `md5`, `sha256`, `hibyos_version`.

### update-send

```
byos-cli update-send (--latest | JSON TAR) [--allow-downgrade] [--install [--no-confirm] [--no-wait]]
```

Stages a ByOS update on the HiBy (SYNC_PROTOCOL 14):

1. `--latest` downloads `hibyos-update.json` and its payload tar from the official release
   (the same URL as OTA) into `cache/`. Otherwise it uses the given files.
2. **Local check first**: Ed25519 signature with the ByOS release keys built into the core
   (the same keys as `OTA_PUBKEY_HEX`), JSON fields, and the tar's size and sha256. A failure
   here sends nothing (`error verify`).
3. PUT `hibyos-update.json` and `hibyos-payload-<version>.tar` into NS_UPDATE (REPLACE,
   resumable), then UPDATE_STAGE.

Result: `version`, `restart` (`none`/`shell`/`reboot`), `needs_firmware`, `size`.

### update-install

```
byos-cli update-install [--no-confirm] [--allow-downgrade] [--wait | --no-wait]
```

UPDATE_INSTALL for the update staged on the HiBy by `update-send` (in this run or an earlier
one). The device checks the staged files again before installing. Nothing staged:
`error remote` with `E_NOT_STAGED`. `update-send ... --install` does both steps over one
connection. Without a shell attached (`E_UNSUPPORTED`), the update stays staged, and the user
can install it from *Software Update* on the HiBy. `--no-confirm` works over USB only.
Over Wi-Fi the HiBy always asks. With `--wait` (default) it polls UPDATE_STATUS every second,
printing `progress {"op": "install", "unit": "permille"}`, and handles the expected disconnect
when the shell restarts (state 4). It then reconnects for up to 60 s, printing
`progress {"op": "reconnect"}` at least every 5 s, and checks `byos_version` in HELLO.

Result: `version`, `state` (`installed`, `restart_pending`), `restart` (`shell`/`reboot`). A
reboot is left to the user (the HiBy shows *Restart Now*).

### status

```
byos-cli status
```

STATUS, DF, DB_STATUS and UPDATE_STATUS in one connection.

Result: `device` (as in the `device` event), `battery`, `charging`, `sd` (DF fields), `db`
(`staged`, `live_sha256`, `live_size`, `last_apply`), `update` (`state`, `version`,
`permille`), `transport`, `encrypted`.

## 6. Examples

```
$ java -jar byos-cli.jar df
{"v":1,"event":"start","t":0,"command":"df","version":"0.1.0","protocol":"BTP/1"}
{"v":1,"event":"device","t":212,"id":"e428dbee789931d8","name":"HiBy R3 II","transport":"usb","serial":"…","paired":false,"pairing_open":false,"sd_state":"mounted","byos_version":"0.8.0","firmware":"0.8.0"}
{"v":1,"event":"result","t":240,"ok":true,"sd_state":"mounted","sd_fs":"exfat","sd_total":127865454592,"sd_free":64000000000,"data_total":150994944,"data_free":25165824}

$ java -jar byos-cli.jar put "01 Waterloo.flac" Music/ABBA/ --mkdir
{"v":1,"event":"start","t":0,"command":"put","version":"0.1.0","protocol":"BTP/1"}
{"v":1,"event":"device","t":190,…}
{"v":1,"event":"progress","t":201,"op":"hash","path":"Music/ABBA/01 Waterloo.flac","done":31457280,"total":31457280,"rate":0,"unit":"bytes"}
{"v":1,"event":"progress","t":1402,"op":"put","path":"Music/ABBA/01 Waterloo.flac","done":5242880,"total":31457280,"rate":4800000,"unit":"bytes"}
…
{"v":1,"event":"result","t":6900,"ok":true,"files":1,"written":1,"skipped":0,"failed":0,"bytes":31457280,"items":[{"path":"Music/ABBA/01 Waterloo.flac","result":"written","sha256":"…"}]}

$ java -jar byos-cli.jar --device wifi connect --pair
…
{"v":1,"event":"prompt","t":830,"kind":"pair_code","message":"Type the 6-digit code shown on the HiBy","device":"HiBy R3 II","digits":6}
815365
{"v":1,"event":"progress","t":5120,"op":"pair_wait","done":0,"total":0,"unit":"none"}
{"v":1,"event":"progress","t":10120,"op":"pair_wait","done":0,"total":0,"unit":"none"}
{"v":1,"event":"result","t":7400,"ok":true,"id":"e428dbee789931d8","name":"HiBy R3 II","paired":true,"transport":"wifi","encrypted":true}
```

## 7. Tests

- `:core` unit tests load `tests/btp/vectors.json` (copied into the test resources, or read with
  `HIBYOS_REPO`) and check the frame codec, path rules, discovery packets, X25519, secretbox,
  HKDF, the pairing and AUTH transcripts, and the encrypted records byte for byte.
- `:cli` tests run every command against an in-process fake device (the BTP/1 server side
  written in Kotlin for tests only) over a loopback socket. They check the JSON-lines output
  (one `start`, one final `result`/`error`, exit codes), PUT resume after a dropped connection,
  CREATE vs REPLACE vs SKIP_IDENTICAL, `E_SD_UNAVAILABLE` handling, cancellation, and pairing
  with a right and a wrong code. For the build commands: the built-in bundle, a wrong firmware
  file, `build` without firmware, and `build-send` against the fake device (parking a stray
  `update.upt`, copy order with the payload version last, the confirmation prompt, a low
  battery, the HiBy refusing the reinstall, a changed build, Wi-Fi refused, and the wait for
  the new version after the restart, with a fake `adb`).
- `:core` tests `Build.kt` on made-up firmware (`BuildTest`): the build record, the copy
  order, the ByOS folder export, the firmware cache and the bundle store.
- No test talks to a real HiBy. Device tests follow the project's device rules and are run by
  hand.
