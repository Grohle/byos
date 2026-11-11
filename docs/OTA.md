# ByOS online updates (OTA)

Once ByOS is installed, it can update itself from GitHub releases:
**Settings › General › Software Update**. We never distribute HiBy's firmware; an online update
only carries our own files (the *payload*, see [INSTALLER.md](INSTALLER.md)). Releases that
also change the read-only system image ask the user to run the ByOS Patcher
([PATCHER.md](PATCHER.md)).

| Piece | File |
|---|---|
| Core: SHA-256/512, Ed25519 verify, JSON reader, version compare, safe tar extractor, background worker | `src/ota_net.inc.c` |
| Settings page (iOS style), 13 languages | `src/ota.inc.c`, `src/lang/ota_tr.inc.c` |
| Installer hooks (`sys/` overrides, `sysok`) | `overlay/usr/bin/hibyos_install.sh` |
| `sys/` in the payload | `tools/make_payload.py` |
| Signing key and signatures | `tools/ota_sign.py` (+ `tools/ota_ed25519.py`, pure-Python fallback) |
| Release builder | `tools/ota_release.py` |
| PC tests | `tests/ota_test.sh` (`ota_test.c`, `ota_fixtures.py`, `ota_httpd.py`, `ota_mkpayload.py`) |
| PC renders of the page (PNG, 8 languages, truncation and glyph checks) | `tests/ota_render.sh` (`ota_render.c`) |

## 1. What an online update can change

The rootfs (SquashFS, read-only, ~0.5 MB free) can only change by reflashing. Everything else
lives in `/usr/data/hibymod` and is installed from the payload by `hibyos_install.sh`, so an
online update can replace all of it:

- the shell and its helpers: `hibyshell`, `hibywall`, `hibymix`, `hibytag`, `hibynet`,
  `cacert.pem`;
- the display/input shim `hibymod_disp.so` (`preload/`);
- shell assets (`shell/*.ov`, icons, `wall.raw`, `home.sig`);
- our theme files (`theme/`, laid over the device's own stock `theme3`), the home mirror
  (`mirror/`) and the wallpapers (`Pictures/`, copied to the SD only if missing);
- **rootfs scripts that delegate to a newer copy** (`sys/`, below).

Apps (`apps/`) are updated by the Apps store, not by OTA.

### `sys/`: newer copies of rootfs scripts, never bricking

A rootfs script that knows how to delegate checks `/usr/data/hibymod/sys/<name>` and runs it
**only if all of these hold**, otherwise it runs its own built-in version:

1. not in safe mode (`/data/.hibymod_safe`), no `/data/.hibymod_nosys` kill switch, and
   `HIBYOS_NO_OVERRIDE` is not set;
2. fewer than 2 failed boots in a row (`/data/.hibymod_bootfail`, counted by the rootfs
   `hiby_player.sh` *before* anything else). A bad override costs at most one failed boot,
   then the built-in script is used; 3 failed boots still give the existing safe mode;
3. its sha256 equals the line for it in `sys/MANIFEST.sha256`. Both files come from the same
   payload, whose tar is verified (signature + sha256) before installing, and are written with
   the installer's copy-to-`.new`-then-rename. A half-written, damaged or mismatched pair
   simply fails the check;
4. its revision (a `# HIBYOS_SYS_REV=N` line near the top) is **greater** than the one of the
   rootfs copy (`/usr/bin/<name>`), so an old payload never replaces the script of a newer
   firmware. **Bump `HIBYOS_SYS_REV` whenever you change a script that has it**: the firmware
   built from that source carries the same number, so the copy in the payload only takes over
   on devices whose firmware is older.

The check is `hibyos_install.sh sysok NAME` (exit 0 = use the override), so every rootfs
script uses the same tested logic. `make_payload.py` puts a script in `sys/` only when its
rootfs version already delegates (for `hiby_player.sh`: when it contains the marker
`HIBYOS_SYS_COUNTED`), so an override can never be shipped to a script that would mishandle it.

| Rootfs piece | Online-updatable? |
|---|---|
| `hibyos_install.sh` (payload installer) | **Yes**, from the first firmware that contains this version: `boot`, `late` and `install` re-exec `sys/hibyos_install.sh` when `sysok` passes (`status`/`sysok` never delegate). If the installed override fails during an online update, the shell retries once with `HIBYOS_NO_OVERRIDE=1` (built-in). A new installer takes effect from the *next* update (the running one is installed by the previous installer). |
| `hiby_player.sh` (supervisor, theme, shim, shell) | **Prepared, not active yet**: needs the hook below in one firmware build (then everything after the boot counter is online-updatable). |
| `S95hibymod` (SD command files `SAFE`, `NORMAL`, `REINSTALL`... and `late` install) | No, on purpose: it is the recovery path. Its install step already goes through `hibyos_install.sh` (updatable). |
| `hibymod` CLI (`reinstall`, `safe off`, `status`) | No, on purpose: recovery tool. It is also where the firmware version is read from. |
| `S11jpeg_display_shell`, `hibyboot`, `/usr/share/hibyos/hibyos.hba` (boot animation), `etc/logo*.jpeg`, `litegui/boot_animation` | No: they run before `/usr/data` matters (or are read by the bootloader). |
| Kernel, `codec_es9219.ko` patch, `mount_ubifs.sh`, `set_functions.json`/`config.json` patches (`TEXT_PATCHES`/`BIN_PATCHES` in `build.py`) | No (firmware). Config files can still be overridden by the user with `/usr/data/hibymod/override/`. |
| `hibymod_fast.sh`, `hibymod_tune.sh`, `hibymod_adb*.sh`, `hibymod_crashlog.sh`, `hibymod_memmon.sh`, `hibyctl`, `/usr/lib/hibymod/bin/wpa_cli`, `theme2` resources of the overlay | No (firmware); candidates for the `sys/` pattern if they start changing often. |

A release that changes any of the "No" rows is published with `--needs-firmware`: the page then
shows **Needs the ByOS Patcher** with instructions (download the Patcher from the release,
run it on the official firmware, copy `r3ii_2025.upt` to the root of the SD, install it from the
**ON THE SD CARD** list on the same page).

### Hook to add to the rootfs `hiby_player.sh` (next firmware build)

Not applied yet (it is another agent's file). Wrap everything up to and including the safe-mode
counter in `if [ -z "$HIBYOS_SYS_COUNTED" ]; then ... fi` (so the override copy does not count
the boot twice, restart `batd` twice or start a second 120 s timer), and right after it add:

```sh
# ByOS OTA (docs/OTA.md): newer supervisor installed by the payload. The boot counter above
# always runs from the rootfs; the copy is used only while fewer than 2 boots in a row failed.
if [ -z "$HIBYOS_SYS_COUNTED" ] && [ -x /usr/bin/hibyos_install.sh ] && \
   /usr/bin/hibyos_install.sh sysok hiby_player.sh; then
    export HIBYOS_SYS_COUNTED=1
    exec /bin/sh /usr/data/hibymod/sys/hiby_player.sh
fi
```

Also add `# HIBYOS_SYS_REV=1` as its second line. The marker string makes `make_payload.py`
start shipping `sys/hiby_player.sh`; it takes over on a device once a payload carries a
higher revision than that device's firmware.

`hiby_player.sh` is HiBy's script with our changes, so it is not a file of this repository:
edit it through `tools/vendor_edits.py export` / `update --keep-previous` (docs/PATCHER.md).
For the same reason the payload does not carry `sys/hiby_player.sh` as a file: it carries a
recipe in `derive/` with the destination `sys/hiby_player.sh`, and the installer rebuilds it
from `/usr/bin/hiby_player.sh` of the firmware (docs/INSTALLER.md, "Files rebuilt on the
player"). The line of `sys/MANIFEST.sha256` is the hash of the rebuilt file. This path is
implemented in `make_payload.py` and in the installer but has not shipped a script yet.

### Installer revisions and `derive/`

The installer of revision 2 (`HIBYOS_SYS_REV=2`) is the first one that understands payloads
with `derive/` (the edited layouts are rebuilt on the player instead of being shipped). A
firmware whose built-in installer is revision 1 was only ever a development build; if one is
updated with a payload that has `derive/`, see "Installer revisions" in docs/INSTALLER.md.
`install_failed` now holds the failed version and, on a second line, `rev N` (the revision of
the installer that failed); a newer installer retries a version that an older one failed.

## 2. Release format

One GitHub release per version, tag `vX.Y.Z`, marked **latest**, with two assets:

- `hibyos-payload-X.Y.Z.tar`: the payload folder (`out/sd/ByOS/payload`) as a plain ustar
  archive, every entry under `payload/`, sorted, root:root, fixed mtime (reproducible). Plain
  tar because the device has no streaming inflate (`stbi_zlib_decode_malloc` is whole-buffer
  and would need ~10 MB of the ~7 MB free RAM) and most of the payload is already compressed
  (PNG, wallpapers); the JSON has `"compression": "none"` so gzip can be added later.
- `hibyos-update.json`:

```json
{
 "format": 1,
 "product": "hibyos",
 "version": "0.6.2",
 "date": "2026-11-20",
 "min_version": "0.6.0",
 "min_firmware": "0.6.0",
 "needs_firmware": false,
 "firmware_url": "https://github.com/Grohle/hibyos/releases/tag/v0.6.2",
 "restart": "shell",
 "payload": {"name": "hibyos-payload-0.6.2.tar",
             "url": "https://github.com/Grohle/hibyos/releases/download/v0.6.2/hibyos-payload-0.6.2.tar",
             "size": 4718592, "sha256": "…64 hex…", "compression": "none"},
 "changelog": {"en": ["…", "…"], "es": "…", "fr": "…"},
 "signature": "ed25519:…128 hex…"
}
```

| Field | Meaning |
|---|---|
| `version` | Payload version (compared with `/usr/data/hibymod/payload_version`). |
| `min_version` | Oldest installed payload that may take this update; older ones are told to copy the full `ByOS` folder to the SD. |
| `min_firmware`, `needs_firmware` | Rootfs version needed (read from `/usr/bin/hibymod`, "ByOS X.Y.Z"); if the device is older, or `needs_firmware` is true, the Patcher instructions are shown instead of the button. |
| `restart` | Announced before downloading: `shell` (the interface restarts, music keeps playing), `reboot` or `none`. The device recomputes it from the old and new `MANIFEST.md5` after extracting and takes the larger of the two. |
| `payload` | `url` https (or http: integrity comes from the signature), `size` ≤ 64 MB, `sha256`. |
| `changelog` | Per language (`es`, `en`, `fr` required; any of the shell's codes optional), string or list of bullets. Fallback: current language, `en`, `es`, first. |

Devices fetch `https://github.com/Grohle/hibyos/releases/latest/download/hibyos-update.json`
(GitHub redirects it to the latest release's asset; `hibynet` follows up to 8 redirects).
Pre-releases are never "latest", so they never reach devices.

## 3. Security model

- **Root of trust**: an Ed25519 public key compiled into `hibyshell`
  (`OTA_PUBKEY_HEX` in `src/ota_net.inc.c`; `OTA_PUBKEY2_HEX` is a second slot for key
  rotation). While it is all zeros (as in the repository today) online updates are disabled and
  the page says so.
- **What is signed**: the whole `hibyos-update.json`, byte for byte, with the 128 hex digits of
  its `signature` value replaced by `0`. Exactly one `"signature"` key is allowed. The JSON
  carries the tar's size and sha256, so one signature covers the payload too.
- **Verification on the device** (worker thread, nothing in the UI thread): the JSON is
  verified *before* anything in it is shown (changelog included) or used; the verified copy is
  kept on the SD and re-verified when the page is opened. The tar is checked for size and
  sha256 after downloading; only then is it extracted, by a strict extractor (regular files
  and folders under `payload/` only; no links, devices, pax/GNU headers, `..`, `.`, empty
  components, absolute paths or unexpected characters; files opened with `O_EXCL` in a fresh
  folder; size and count limits). The installer then checks every file against
  `MANIFEST.md5` before touching anything (as for SD installs).
- **Ed25519 implementation**: verification derived from TweetNaCl (public domain) with the
  RFC 8032 `S < L` check (no malleable signatures); tested against the RFC 8032 vectors, the
  Python `cryptography` package and the pure-Python implementation (hundreds of valid and forged
  signatures).
- **Downgrades** are refused unless the user taps *Install Anyway* after a warning (an attacker
  replaying an old signed release cannot push a downgrade). Reinstalling the same version is
  not offered.
- **Not covered**: freeze attacks (a network attacker can hide new releases; the page then just
  says it is up to date or cannot connect), and anyone with root or physical access to the
  device or its SD card (they can already change `/usr/data`). TLS (mbedTLS + Mozilla roots)
  protects privacy and availability but is not what the trust relies on.
- **Private key**: never in the repository. `ota_sign.py genkey` refuses paths inside the repo
  and never overwrites a key; `ota_release.py` refuses a key inside the repo and a key that is
  not the one compiled into the source (devices would reject the release). Keep an offline
  backup: losing the key means shipping a new public key through a reflash (Patcher release)
  or through an update signed with the old key that installs a shell with `OTA_PUBKEY2`.

## 4. On the device

Settings › General › Software Update (`src/ota.inc.c`):

- **Installed version** (payload) and **Firmware** (rootfs).
- **Check for Updates**: downloads the JSON in the background (`hibynet`, 25 s timeout per
  network call), verifies it and shows a "ByOS X.Y.Z" group (size, button, what will
  restart) and a **WHAT'S NEW** text section with the changelog in the current language. Errors: no Internet, server unreachable, not signed by ByOS, invalid.
- **Download and Install**: confirmation alert (with "the HiBy will restart" or "the interface
  will restart; music keeps playing"), then a progress bar with MB done. Checks first and again
  before installing: SD present (and not exported over USB storage: then it is unmounted),
  battery ≥ 30 % or charging, free space on the SD ≥ 2 × size + 2 MB (the installer checks
  `/usr/data`).
  1. download to `SD/ByOS/ota/hibyos-payload-X.tar` (`.part` while downloading;
     **Pause Download** or a dropped connection keeps it, **Resume Download** continues with
     an HTTP Range request, also after a restart of the shell);
  2. size + sha256;
  3. extraction to `SD/ByOS/ota/stage/payload`, check that `VERSION` matches;
  4. `SD/ByOS/payload` (the installed version) moves to `SD/ByOS/ota/previous/payload` and
     the new one takes its place, so the SD always holds what is installed and the boot-time
     installer never reinstalls an old payload;
  5. `hibyos_install.sh install` (the usual path: MANIFEST check, `.new` + rename, user state
     untouched). If it fails, the SD folders are swapped back, the previous version is
     reinstalled and the error says so (details in `SD/ByOS/install.log`).
  6. **Restart Interface** (hibyshell exits, the supervisor starts the new one in ~3 s) or
     **Restart Now** (reboot), as computed from the manifests.
- **Restore Previous Version**: swaps `payload` and `ota/previous/payload` and reinstalls
  (always one step; tapping it again goes forward again). The previous copy is kept until an
  update succeeds, so a failed update keeps the old rollback target.
- **Check Automatically** (weekly, only on Wi-Fi while charging, never installs): implemented
  (`ota_auto_tick()`), shown only when the shell's main loop calls it: add
  `ota_auto_tick();` to the main loop and build with `-DOTA_AUTO_TICK_WIRED`. The result is
  `ota_st.state == OS_AVAILABLE` (shown on this page); a badge on the Settings icon would be
  part of that integration.
- Strings: `otr()` looks up `src/lang/ota_tr.inc.c` (13 languages per row) and falls back to
  `tr()`, so `i18n.inc.c` did not need changes. The navigation title uses a short form
  (`OTA_TITLE`) so that neither it nor the back label is cut in any language.
- Children (`hibynet`, the installer) get only stdin/stdout/stderr: a long download never
  keeps the shim's input socket open if the shell dies meanwhile.
- The SD firmware list (`.upt` via the official recovery) stays below, unchanged.

Never touched by an update (from [INSTALLER.md](INSTALLER.md)): `shell/launcher.conf`,
`shell/wall_custom.raw`, `shell/mirror*.sig`, `launcher/mirror.png`, `power.conf`,
`audio.conf`, `ota.conf`, `apps/`, `appdata/`, `archivos/`, `fondos/`, `override/`, the SD
wallpapers and the music database.

Files: `SD/ByOS/ota/hibyos-update.json` (last verified), `ota/*.tar(.part)`,
`ota/stage/`, `ota/previous/`; `/usr/data/hibymod/ota.conf` (`auto`, `last_check`).

## 5. Release procedure (owner)

One-time, on your computer (Windows example):

```
python tools\ota_sign.py genkey --key %USERPROFILE%\.hibyos\ota_ed25519.key --install
```

This writes the key **outside** the repo (back it up offline, e.g. on a USB stick) and puts its
public key in `src/ota_net.inc.c` (commit that line: it is public). Rebuild `hibyshell` and
ship it once (SD payload or Patcher release): from then on devices accept your releases.
Uses `cryptography` if installed (`pip install cryptography`), else `pynacl`, else pure Python.

Each release:

1. Bump the version in `overlay/usr/bin/hibymod` if the firmware changes, bump
   `HIBYOS_SYS_REV` in any `sys/` script you changed (`hibyos_install.sh`), build the binaries
   and assets (see INSTALLER.md), then `python tools\make_payload.py --version X.Y.Z`.
2. Write the changelog, e.g. `notes-X.Y.Z.json`:
   `{"en": ["…", "…"], "es": ["…", "…"], "fr": ["…", "…"]}` (other languages optional).
3. Build and sign:
   ```
   python tools\ota_release.py --key %USERPROFILE%\.hibyos\ota_ed25519.key --changelog notes-X.Y.Z.json ^
       --version X.Y.Z --prev-payload <previous release's payload folder>
   ```
   Add `--needs-firmware` if the rootfs changed (and attach the Patcher as usual).
   `--min-version` defaults to 0.6.0.
4. Check `out\release\vX.Y.Z\` (tar, `hibyos-update.json`, `notes.md`), optionally
   `python tools\ota_sign.py verify out\release\vX.Y.Z\hibyos-update.json`, then run the
   printed command:
   ```
   gh release create vX.Y.Z out\release\vX.Y.Z\hibyos-payload-X.Y.Z.tar out\release\vX.Y.Z\hibyos-update.json --repo Grohle/hibyos --title "ByOS X.Y.Z" --notes-file out\release\vX.Y.Z\notes.md --latest
   ```
   Also attach the usual `ByOS/` SD folder zip for fresh installs if you wish.

To pull a broken release: publish a fixed higher version (devices never downgrade on their own),
or mark an older release as latest only if users should be offered *Install Anyway*.

## 6. Tests (PC)

```
wsl -d Ubuntu -- bash /mnt/c/Users/alber/Downloads/Hiby/hibymod/tests/ota_test.sh
```

- unit (ASan/UBSan): SHA-256/512 at every padding edge, Ed25519 (RFC 8032 + 48 random keys,
  bit flips, wrong keys, `S + L`, invalid points), signatures (good, tampered text or size,
  unsigned, missing, two keys, other key, rotation), JSON (35 documents: limits, types, URLs,
  names, nesting, trailing data, Unicode, changelog fallback), versions, 23 tars (good and
  hostile), restart rules, the 13-language table (no empty cell, same `%` formats as English);
- tools: genkey refuses the repo and never overwrites; sign/verify with both backends;
  `install` on a copy of the source;
- flow, with the real worker thread, `hibynet` built for x86 from the current source, a local
  server with GitHub-like redirects and Range, and the real `hibyos_install.sh` under `dash`:
  check; dropped connection and user pause, both resumed with Range; children get no inherited
  descriptors; install with interface restart; next boot does nothing; `sys/` override
  delegation (valid, `NO_OVERRIDE`, 2 failed boots, safe mode, tampered, revision not newer
  than the rootfs); rollback both ways; tampered tar (same size); low battery; no
  SD; tampered JSON; other key; 404; downgrade refused then allowed; a release whose install
  fails half-way restores the previous version and keeps the rollback target; needs-firmware;
  min-version; user state untouched throughout.

`tools/simtest_install.sh` (the SD installer suite) also passes with the new installer and a
payload containing `sys/` and `derive/` (built with `make_payload.py --out <tmp> --expect <tmp2>`;
see "Testing on the PC" in docs/INSTALLER.md).

```
wsl -d Ubuntu -- bash /mnt/c/Users/alber/Downloads/Hiby/hibymod/tests/ota_render.sh
```

Renders every state of the page (idle, checking, up to date, errors, available, downloading,
paused, verifying, installing, failed, done with interface restart or reboot, needs Patcher,
min-version, newer installed, restoring, no key) in en/es/fr/de/ja/ru/zh-Hans/th with the
shell's own `sp_render` and the firmware font, into `out/ota_render/`, and fails if a title,
value, back label or bar text would be cut, or if a character of the 13-language table has no
glyph in the firmware fonts.

## 7. Still to verify on the device

- `hibynet` against GitHub's real redirect chain and CDN Range support (`objects`/
  `release-assets.githubusercontent.com`), with the shipped `cacert.pem`.
- Ed25519 verification time on the X1600 (estimated well under 2 s; it runs in the worker).
- The page while downloading (progress refresh about every 400 ms, scrolling and back gesture
  stay fluid), and leaving/reopening it during a download.
- **Restart Interface**: hibyshell exits and the supervisor brings the new one back (~3 s,
  music keeps playing); **Restart Now** reboots cleanly (counts as a clean boot).
- Install from the shell while the theme is bind-mounted over `/usr/resource` (the installer
  reads stock files through `mount -o bind /`, as in `late`).
- Free-space messages with a nearly full SD / `/usr/data`; behaviour when the SD is exported
  over USB storage mid-download (the download fails and resumes later).
- `sysok` with busybox `awk`/`sha256sum` on the device.
