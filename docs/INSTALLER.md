# HiByOS installer (SD payload)

The rootfs has barely any room (~0.5 MB free), so the shell, its binaries, the display
library and the theme live on the data partition (`/usr/data/hibymod`, UBIFS, survives
flashing). A public release is distributed in two pieces:

| Piece | Where it goes | What it is |
|---|---|---|
| `r3ii_2025.upt` | SD root | firmware (rootfs with `hibyos_install.sh` and the hooks) |
| `HiByOS/` | SD root | payload that the installer copies to `/usr/data/hibymod` |

## Building the payload

```
python tools\make_payload.py            # version = the one in overlay/usr/bin/hibymod ("HiByOS 0.6.0")
python tools\make_payload.py --version 0.6.1
python tools\make_payload.py --out DIR --expect DIR2   # another output folder; --expect: see "Testing on the PC"
```

The script needs **your own unpacked copy** of the official root file system, outside the
repository: set `HIBY_STOCK_ROOT` to it (or `HIBY_STOCK` to its `usr/resource` folder). It
is only read: to compute the lists in `derive/`, to check that copy against known hashes
and to make sure nothing of it ends up in the payload.

Beforehand: build in WSL (`~/hiby/tools/zbuild_shell.sh`, `zbuild_so.sh`, `zbuild_wall.sh`,
`zbuild_mix.sh`, `zbuild_tag.sh` → `bin/`), `python tools\shell_assets.py`,
`python theme\build_theme.py`, `python theme\topbar.py`, `python theme\hibyos_brand.py`.
The script warns if a binary is older than its source code.

Output `out/sd/HiByOS/payload/` (~4 MB):

```
VERSION, MANIFEST.md5        version and md5 of everything (verified before installing)
bin/      hibyshell hibywall hibymix hibytag hibymod_disp.so
shell/    layers *.ov, icons/*.ov, wall.raw, home.sig
theme/    ONLY our own artwork for layout/theme3 and litegui/theme3 (paths under
          /usr/resource): home icons, wallpaper.png, dock.png (from theme/out)
mirror/   "mirror" home: transparent icons
derive/   how to build ON THE PLAYER the six files that are HiBy's layouts with our changes
          (home view, mirror home view, status bar, the two About views, volume dialog):
          INDEX + one list of operations per file. Only our changes, never HiBy's text
Pictures/ the 9 wallpapers (theme/wallpapers.py)
sys/      newer copies of root file system scripts + MANIFEST.sha256 (docs/OTA.md)
licenses/ THIRD_PARTY_NOTICES.md + the licence texts of the third-party components inside
          bin/ (chosen by tools/notices.py; the build stops if one is missing)
```

Nothing from HiBy is redistributed: the script refuses to ship any text file that exists in
the official firmware and deletes any image identical to the stock one. The full theme is
assembled on the device by copying its own stock `theme3`, putting our artwork on top and
rebuilding the edited layouts from the player's own copies (next section).

### Files rebuilt on the player (`derive/`)

A layout that is HiBy's file with our changes is not in the payload. The payload carries a
recipe and the installer rebuilds the file from the copy in the player's own firmware, then
checks the result against its SHA-256. Format (version 1):

```
derive/INDEX      text, one line per way of building a file ("#" lines are comments):
                    <req|opt> <dest> <sha256 of the result> <base> <sha256 of the base> <strip> <id>
                  dest   path inside the payload the result is installed as (theme/...,
                         mirror/..., sys/...), exactly as if the payload contained that file
                  base   file of the player's root file system (usr/resource/... or usr/bin/...)
                  strip  1 = delete every CR (0x0D) byte of the base before applying
                  id     "-" = the base already is the result (it is copied); otherwise the
                         name of derive/<id>.ops and derive/<id>.add
derive/<id>.ops   one operation per line:   c <offset> <length>   bytes of the base
                                            a <offset> <length>   bytes of derive/<id>.add
derive/<id>.add   the bytes we add, concatenated (our text only)
```

The result is the concatenation of the pieces in the order of the `.ops` file. Several
lines may have the same `dest`: the first one whose base has the stated SHA-256 is used.
That is how one payload fits the official firmware layout, the layout as the current
patcher leaves it, and the layouts left by older HiByOS versions (`previous` in
`tools/vendor_edits.json`). `MANIFEST.md5` covers every file of `derive/`.

What the installer does with it, before changing anything on the player:

- builds every file in a temporary folder and compares its SHA-256 with the one in `INDEX`;
  a result with another hash is thrown away;
- if no line matches the firmware's file but the right file is already installed (same
  SHA-256), it keeps that copy;
- otherwise, for a **`req`** file (the two home views; the shell cannot work without them)
  the installation is refused: nothing is changed, the log says `ERROR: derive: <file>
  cannot be built: the file of this firmware is not a known version`, and the player keeps
  running what it had;
- for an **`opt`** file (status bar, About, volume dialog) the file is skipped: the log
  says `derive: <file> skipped: the file of this firmware is not a known version (the
  firmware's own file stays in use)` and the rest is installed.

The pieces are cut with `awk`; if the result does not have the expected hash the installer
does it again byte by byte with `dd`. A payload in the old format (full files, no
`derive/`) still installs.

## On the device (`/usr/bin/hibyos_install.sh`)

| When | Command | What it does |
|---|---|---|
| `hiby_player.sh`, before the theme and the player | `boot` | If the SD is already mounted and its version differs, installs. If HiByOS is **not** installed, waits up to 15 s for the SD (the boot animation keeps running). Installed and no SD: exits immediately (0 ms wait). It also enables the home-screen mirror and removes a half-finished `theme2`. |
| `S95hibymod`, with the SD mounted (~8 s) | `late` | If the SD version differs, installs and S95 **reboots** (the SD is mounted ~4 s after the player starts, so an update costs one reboot). |
| ADB | `install` / `status` | Force the installation / show versions and the tail of the log. |

Never in safe mode; `/data/.hibymod_noinstall` disables it. Log in `HiByOS/install.log`
on the SD (and `/tmp/hibymod/install.txt`).

Installation steps:
1. Verifies `MANIFEST.md5`, builds the files of `derive/` (previous section) and checks the
   free space. If that fails, it touches nothing and records the version and the revision of
   the installer (`rev N`, its `HIBYOS_SYS_REV`) in `install_failed`. The same installer
   does not retry that version in a loop (`install` forces it); an installer with another
   revision tries again, because it may know how to install what the older one could not.
2. Deletes `payload_version` (if interrupted, it is repeated on the next boot).
3. `theme/layout/theme3` and `theme/litegui/theme3`: stock copy if they are missing or
   incomplete (if the theme is already mounted on top, it reads `/usr/resource` through a read-only
   `mount -o bind /`). Whatever was ours and is no longer in the payload reverts to stock.
4. Copies each file to `.new`, compares it and renames it (binaries in use are not broken;
   identical files are not rewritten). Binaries → `shell/`, library → `preload/hibymod_disp.so`.
5. `mirror_bak/` and `launcher_view.bak` (normal home screen if the launcher does not start) and
   `payload_mirror/`. If `theme/.../launcher/mirror.png` exists, the mirror is applied.
6. Deletes `shell/cache/` (app screenshots, regenerated).
7. `preload_ok` only on the first installation, with the library already verified and without
   `/data/.hibymod_nopreload` (command `SIN_LAUNCHER`).
8. Wallpapers to `/Pictures` on the SD if missing; `payload_files`, `payload_subs` and finally
   `payload_version`.

**User state that is left untouched**: `shell/launcher.conf`, `shell/wall_custom.raw`,
`shell/mirror*.sig`, `launcher/mirror.png`, `power.conf`, `audio.conf`, `apps/`, `appdata/`,
`archivos/`, `fondos/`, `override/`, the wallpapers on the SD and the music database.

### Mirror home screen
On the first boot `mirror.png` does not exist yet: the stock home screen uses the theme's normal
view and icons (and `home.sig`). hibyshell generates `mirror.png` and `mirror.next.sig`; on the
next boot `boot` applies the mirror view and transparent icons (`mirror_on`).

## Flows

- **Fresh install**: copy `r3ii_2025.upt` and `HiByOS/` to the SD → update the
  firmware from the SD. On first boot it is installed before the player.
- **Update**: replace `HiByOS/` on the SD and reboot: S95 installs and reboots by itself.
- **No SD or no payload**: boots like the stock firmware + patches; it is installed as
  soon as the folder appears (S95, with one reboot).

## Testing on the PC

```
python tools\make_payload.py --expect out\sd_expect
wsl -d Ubuntu -- bash /mnt/c/Users/alber/Downloads/Hiby/hibymod/tools/simtest_install.sh
```
Covers: from scratch, same version (no writes), no SD, mirror, update with the theme
mounted while preserving state, corrupted payload, first boot without SD, SD without payload,
`SIN_LAUNCHER`, migration of the current development device, and for `derive/`: a player
with the untouched official layouts, an unknown `opt` base (skipped and logged), an unknown
`req` base (refused, nothing changed, not retried), an unknown base with the right file
already installed (kept), an `awk` that fails (the `dd` path gives the same files), a
payload in the old format, and a firmware with the revision 1 installer receiving this
payload.

`--expect DIR` writes the full files that the player must end up with for everything in
`derive/` (they contain HiBy's text: keep that folder out of git and out of releases); the
simulation compares the rebuilt files with them byte by byte. Environment of the simulation
(all optional): `STOCKRES` (the `usr/resource` folder of your unpacked official firmware),
`SRC_SD` (the `HiByOS` folder to install, default `out/sd/HiByOS`), `EXPECT` (default
`out/sd_expect`) and `SIM_BB`, a command that runs the player's own busybox, for example
`"qemu-mipsel-static -L ROOTFS ROOTFS/bin/busybox"`: the installer and every tool it calls
(`awk`, `dd`, `sha256sum`, `cp`...) then run with that busybox instead of the PC's tools.

## Installer revisions

`hibyos_install.sh` carries `# HIBYOS_SYS_REV=N`; a payload brings the newest installer in
`sys/` and the installer in the firmware hands over to it when it is newer (docs/OTA.md).

- **Revision 2** (current): understands `derive/` and writes the revision in
  `install_failed`.
- **Revision 1** (development firmware only, never published): does not know `derive/`.
  With a payload that has `derive/`, its first attempt stops at the home-view backup and
  is recorded as failed (the start-up goes on; the installation is not complete yet). That
  attempt has already stored the revision 2 installer in `/usr/data/hibymod/sys/`, so the
  next start hands over to it and installs everything. To avoid the extra start, flash a firmware made by
  the current patcher (its installer is revision 2).
