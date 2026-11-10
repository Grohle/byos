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
```

Beforehand: build in WSL (`~/hiby/tools/zbuild_shell.sh`, `zbuild_so.sh`, `zbuild_wall.sh`,
`zbuild_mix.sh`, `zbuild_tag.sh` → `bin/`), `python tools\shell_assets.py`,
`python theme\build_theme.py`, `python theme\topbar.py`, `python theme\hibyos_brand.py`.
The script warns if a binary is older than its source code.

Output `out/sd/HiByOS/payload/` (~4 MB):

```
VERSION, MANIFEST.md5        version and md5 of everything (verified before installing)
bin/      hibyshell hibywall hibymix hibytag hibymod_disp.so
shell/    layers *.ov, icons/*.ov, wall.raw, home.sig
theme/    ONLY our files from layout/theme3 and litegui/theme3 (paths under /usr/resource):
          home (view + icons + wallpaper.png + dock.png, from theme/out), status bar and
          About (from overlay/usr/resource), dialog/volume.dlg (hidden volume dialog)
mirror/   "mirror" home: view with launcher/mirror.png background + transparent icons
Pictures/ the 9 wallpapers (theme/wallpapers.py)
```

Nothing from HiBy is redistributed: the script deletes any file identical to the stock one. The
full theme is assembled on the device by copying its own stock `theme3` and putting ours on
top (like `build_theme.py deploy()`).

## On the device (`/usr/bin/hibyos_install.sh`)

| When | Command | What it does |
|---|---|---|
| `hiby_player.sh`, before the theme and the player | `boot` | If the SD is already mounted and its version differs, installs. If HiByOS is **not** installed, waits up to 15 s for the SD (the boot animation keeps running). Installed and no SD: exits immediately (0 ms wait). It also enables the home-screen mirror and removes a half-finished `theme2`. |
| `S95hibymod`, with the SD mounted (~8 s) | `late` | If the SD version differs, installs and S95 **reboots** (the SD is mounted ~4 s after the player starts, so an update costs one reboot). |
| ADB | `install` / `status` | Force the installation / show versions and the tail of the log. |

Never in safe mode; `/data/.hibymod_noinstall` disables it. Log in `HiByOS/install.log`
on the SD (and `/tmp/hibymod/install.txt`).

Installation steps:
1. Verifies `MANIFEST.md5` and free space. If that fails, it touches nothing and records the version in
   `install_failed` (it is not retried in a loop; `install` forces it).
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
wsl -d Ubuntu -- bash /mnt/c/Users/alber/Downloads/Hiby/hibymod/tools/simtest_install.sh
```
Covers: from scratch, same version (no writes), no SD, mirror, update with the theme
mounted while preserving state, corrupted payload, first boot without SD, SD without payload,
`SIN_LAUNCHER` and migration of the current development device.
