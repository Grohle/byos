# hibymod app format (`.hpk`)

Third-party app packages for the hibymod shell on the HiBy R3 II (MIPS32r2 little-endian,
Linux 4.4, ~7 MB of free RAM). This document is the contract between app authors, the PC
packager (`tools/hpk.py`) and the on-device installer (`src/hpk.inc.c` / `src/hpk.h`).

## 1. The package

A `.hpk` is an **uncompressed POSIX ustar tar archive**. At the root:

| Entry          | Required    | What it is |
|----------------|-------------|------------|
| `manifest.ini` | yes         | metadata (UTF-8, max. 16 KB). Must be the first entry. |
| `icon.png`     | recommended | full-bleed square icon (128x128 RGBA recommended, 16–512 px, max. 1 MB). Second entry. |
| `exec`         | yes         | the executable named in the manifest (e.g. `bin/hola`). |
| `data/`, `lib/`, … | no      | any other file or directory of the app. |

Tar rules (the installer rejects the whole package if any of them fails):

- Only **regular files** (`'0'`) and **directories** (`'5'`). No symbolic or hard links,
  devices, FIFOs, pax headers (`x`/`g`) or GNU long names (`L`/`K`). The ustar `prefix` field is
  supported (paths of up to 255 bytes).
- **Relative** paths: absolute paths, any `..` component, backslashes and control characters are
  rejected. Leading `./` is tolerated.
- Every header must have a correct checksum; sizes are in octal (base-256 is not supported), each
  file ≤ 256 MB, ≤ 4096 entries, and nothing may run past the end of the archive.
- No duplicate entries, and no file that is also a directory.
- Modes are normalized: `0755` for the executable and files with the x bit, `0644` for the rest;
  never suid/sgid. Owner is always root.

`tools/hpk.py build` produces reproducible tars: ustar, uid/gid 0 without names, fixed mtime
(2000-01-01), `manifest.ini` and `icon.png` first and the rest in alphabetical order.

## 2. `manifest.ini`

`key=value` text, one per line. Empty lines, `#`, `;` and `[sections]` are ignored; values may be
quoted. Unknown keys are ignored (forward compatibility).

| Key | Req. | Value |
|-----|------|-------|
| `id` | yes | reverse-DNS style identifier, `[a-z0-9._-]`, starts with a letter or digit, max. 48, no `..`. E.g.: `org.hibymod.hola` |
| `version` | yes | semver `MAJOR.MINOR.PATCH[-pre][+build]` |
| `min_hibymod` | yes | minimum shell semver (if it is higher than the device's: `err.newer`) |
| `name` | yes | name in Spanish (it is the fallback for the other languages); also `name.es` |
| `name.en`, `name.fr` | rec. | name in English and French (max. 47 bytes) |
| `description`, `.en`, `.fr` | rec. | short description (max. 159 bytes) |
| `exec` | yes | relative path of the executable inside the package |
| `arch` | yes | exactly `mipsel-hf` |
| `input` | yes | `touch`, `keys` or `both` |
| `stop_player` | no | `1` if the app needs the RAM and the music must be stopped; `0` by default |
| `ram_kb` | rec. | estimate of the memory (KB) the app uses |
| `refresh` | no | `full`: while the app is open the panel does not drop to the idle refresh rate (30 Hz after 1.5 s without touch). For a browser, video or animations without touching the screen |
| `category`, `author`, `homepage` | no | informational |

Example (`apps/hola/manifest.ini`; the Spanish `name`/`description` values are the real file
content, since Spanish is the fallback language):

```ini
id=org.hibymod.hola
version=1.0.0
min_hibymod=0.1.0
name=Hola
name.en=Hello
name.fr=Bonjour
description=Demo: saludo, degradado y un círculo que sigue al dedo
description.en=Demo: greeting, gradient and a circle that follows your finger
description.fr=Démo : salutation, dégradé et un cercle qui suit le doigt
exec=bin/hola
arch=mipsel-hf
input=touch
stop_player=0
ram_kb=900
category=demo
author=hibymod
```

## 3. The executable

- **ELF32 little-endian, MIPS, ISA ≤ mips32r2, static** (no `PT_INTERP`; static-pie is accepted).
- **Hard floating point** (`musleabihf`): if `PT_MIPS_ABIFLAGS` says soft-float it is rejected
  (soft-float is 14 times slower on this SoC).
- Any helper binary in `lib/` or `bin/` must also be static: there are no compatible shared
  libraries on the device.

Build (see `~/hiby/tools/zbuild_app.sh`):

```sh
python -m ziglang cc -target mipsel-linux-musleabihf -mcpu=mips32r2 -Os -static -s \
  -I hibymod/src/sdk -o bin/miapp miapp.c -lm
```

## 4. The icon

`icon.png` must be the **complete square artwork, full-bleed and opaque**: no rounded corners,
no gloss and no border. The installer:

1. crops to the central square if it is not square and downscales by area averaging (upscales
   bilinearly if it is small); transparency is composited over black, as on iOS;
2. applies the mask of the built-in icons (`theme/launcher_icons.js`): rounded rectangle with a
   22.37 % radius, 30 % → 0 white gloss over the top half and a 1 px inner border in white at
   28 %, antialiased;
3. writes `icon_48.ov`, `icon_56.ov` and `icon_64.ov` into the app directory (`.ov` format:
   `"HMOV"`, uint16 width, uint16 height, RGB565[width·height], alpha[width·height], little-endian,
   non-premultiplied alpha — the same as hibyshell's `load_sprite()`).

Without `icon.png` a fallback icon is generated with a gradient whose hue is derived from the `id`.

## 5. Installation on the device

Paths (all under a root that can be configured for testing with `hpk_set_root()` or `HPK_ROOT`;
`/usr/data/hibymod` by default):

| Path | Contents |
|------|----------|
| `/usr/data/mnt/sd_0/Apps/*.hpk` | available packages (listed by `hpk_scan`) |
| `<root>/apps/<id>/` | installed app; its `manifest.ini` is the registry |
| `<root>/appdata/<id>/` | writable app data; survives updates and normal uninstallation |
| `<root>/apps/.tmp-<id>/`, `.old-<id>/`, `.del-<id>/` | transient states (never visible to the launcher) |

`hpk_install()` process:

1. **Validate without writing anything**: all headers (checksums, types, paths, sizes), manifest,
   presence and ELF format of the executable, icon size and **free space** on `/usr/data`
   (contents + icons + 512 KB of margin).
2. **Extract** into `apps/.tmp-<id>/` with `pread` in chunks of ≤ 64 KB (`O_EXCL|O_NOFOLLOW`,
   `fsync` per file), with progress callbacks.
3. **Generate the `.ov` icons** and create `appdata/<id>/`.
4. **Atomic swap**: `apps/<id>` → `apps/.old-<id>`, `apps/.tmp-<id>` → `apps/<id>`, delete
   `.old-<id>`. On any error `.tmp-<id>` is deleted and the previous version stays intact.
5. **Recovery** (`hpk_recover()`, called when installing and when listing): if a power cut leaves
   `.old-<id>` without `<id>`, the previous version is restored; any leftover
   `.tmp-`/`.del-`/`.old-` is deleted.

The shell must close the app (SIGTERM) before updating or uninstalling it.

**Apps on the SD card** (chosen when installing from the store): the full app goes to
`SD/HiByOS/apps/<id>/` (same atomic installer, root `SD/HiByOS`) and a *stub* with
`manifest.ini`, `icon_*.ov` and `sd_location` remains in `apps/<id>/`. `HM_APP_DIR` points to the
SD (exfat without `noexec`: it runs in place). Data stays in the HiBy's `appdata/<id>`. Details,
moving between SD and HiBy, and recovery after power cuts: `docs/STORE.md` §4.

`hpk_scan()` only reads the manifest and locates the icon (it walks the headers; it does not read
data), so it is fast but does not detect a bad ELF: `hpk_install()` does that.

### API (`src/hpk.h`)

```c
int  hpk_scan(const char *dir, struct hpk_info *out, int max);          /* packages on the SD */
int  hpk_install(const char *path, void (*progress)(long done, long total, void *ctx), void *ctx,
                 char *err, int errlen);                                 /* err = key */
int  hpk_uninstall(const char *id);                                      /* keeps appdata */
int  hpk_uninstall_ex(const char *id, int flags);                        /* HPK_PURGE_DATA */
int  hpk_list_installed(struct hpk_info *out, int max);
int  hpk_icon(const struct hpk_info *pkg, int s, unsigned short *rgb, unsigned char *a);
int  hpk_vercmp(const char *a, const char *b);
const char *hpk_errkey(int code);              /* -HPK_E_TRAVERSAL -> "err.traversal" */
const char *hpk_msg(const char *key, int lang);/* HPK_LANG_ES / _EN / _FR */
extern char hpk_err_detail[160];               /* offending entry or field */
```

All of them return 0 or `-HPK_E_*`. `struct hpk_info` carries the id, version, names and
descriptions in the three languages, `stop_player`, `ram_kb`, size, path, installed version
(`installed`) and `status` (0, or the error if the package is damaged). Status keys for the UI:
`st.install`, `st.update`, `st.current`, `st.older`, `st.installing`, `st.icons`, `st.uninstall`,
`st.stopplayer`, `st.damaged`, `st.noapps`… (see the table in `hpk.inc.c`).

## 6. Execution contract

The shell launches `apps/<id>/<exec>` with the working directory set to `HM_APP_DIR` and these
variables:

| Variable | Value |
|----------|-------|
| `HM_APP_DIR` | installation directory (read-only by convention) |
| `HM_DATA_DIR` | `/usr/data/hibymod/appdata/<id>` (writable, persistent) |
| `HM_LANG` | UI language: `es`, `en`, `fr`, `de`, `it`, `ja`, `ko`, `pl`, `ru`, `zh-Hans`, `th`, `zh-Hant` or `uk` (the manifest only carries `name`/`description` in es/en/fr: in the other languages the English one is shown) |
| `HM_TOUCH` | evdev node of a virtual touchscreen, same protocol as goodix-ts: type A multitouch (`BTN_TOUCH`, `ABS_MT_POSITION_X/Y`, `SYN_MT_REPORT` per contact, `SYN_REPORT` per frame; a frame with no contacts = finger lifted). Screen coordinates 0–319 × 0–479 |
| `HM_KEYS` | evdev node of the physical keys (`EV_KEY`, value 1/0/2) |
| `HM_FB` | `/dev/fb0`: 320x480 RGB565, double buffer with panning (virtual 320x960) |

**Display (SLCD with its own memory)**: each `FBIOPAN_DISPLAY` transfers the image to the panel.
Write the **complete** hidden buffer and **then** pan; never write to the buffer that is shown
after the pan (it corrupts the transfer and flickers). Useful maximum: 44 fps.

**Exit**: when the user swipes up from the home bar the shell sends `SIGTERM`; the app must exit
in **less than 1 s** (after that `SIGKILL` arrives). Do not leave child processes behind. If the
app exits on its own with code **99**, the shell shows "The app was closed due to lack of memory"
(used by the Browser's watchdog).

**Memory**: there is no swap, and exhausting the RAM does not trigger the OOM killer in time: the
kernel evicts the code of every process and the device stops responding (adb included). A large
app must watch `MemAvailable` and exit early (see `apps/navegador/navegador.c`). If
`stop_player=0` the app coexists with the player and must stay within a few MB (declare `ram_kb`
honestly). Never write large temporary files to `/tmp` (it is RAM).

### SDK (`src/sdk/hm_app.h`, header-only)

- `hm_app_init()` – installs SIGTERM/SIGINT/SIGHUP → `hm_quit`.
- `hm_fb_open / hm_fb_present(fb, canvas) / hm_fb_close` – mmap and correct presentation (copies
  the RAM canvas to the hidden buffer, then pans).
- `hm_touch_open / hm_touch_read(t, cb, ctx)` – `HM_DOWN / HM_MOVE / HM_UP` events of the first
  finger.
- `hm_keys_open / hm_keys_read(k, cb, ctx)` – keys.
- `hm_wait(t, k, ms)` – `poll` on both; `hm_lang()`, `hm_tr(es, en, fr)`, `hm_app_dir()`,
  `hm_data_dir()`, `hm_now_ms()`.

Complete example: `apps/hola/` (dithered gradient, greeting in an upscaled 8x16 font, a circle
that follows the finger with a spring and fade-out). It is built and packaged with
`~/hiby/tools/zbuild_app.sh hola` → `out/apps/hola.hpk`.

## 7. PC tools

```sh
python tools/hpk.py build apps/miapp/staging -o miapp.hpk   # validates and packages
python tools/hpk.py inspect miapp.hpk                       # manifest and file list
python tools/hpk.py verify miapp.hpk                        # same rules as the installer
```

Installer tests in WSL (temporary root, malicious packages included):
`~/hiby/tools/hpk_test.sh` (generates the packages with `tests/mkpkgs.py` and runs
`tests/hpk_test.c` with AddressSanitizer).

---

## Summary

A `.hpk` is an **uncompressed POSIX ustar** archive with `manifest.ini` (UTF-8 `key=value`: `id`,
`version`, `min_hibymod`, `name[.en|.fr]`, `description[.en|.fr]`, `exec`, `arch=mipsel-hf`,
`input=touch|keys|both`, `stop_player=0|1`, `ram_kb`, optional `category/author/homepage`),
`icon.png` (full-bleed opaque square, 128x128 recommended; the installer applies the rounded
mask, gloss and border and writes `icon_48/56/64.ov`) and the app files. Only regular files and
directories with relative paths are accepted; the executable must be a static hard-float MIPS32r2
LE ELF. Installation validates everything first, extracts into `apps/.tmp-<id>` with ≤64 KB
buffers, then atomically swaps it in; failures never leave a half-installed app. Apps get
`HM_APP_DIR`, `HM_DATA_DIR`, `HM_LANG`, `HM_TOUCH`, `HM_KEYS`, `HM_FB`, must write the whole back
buffer before panning, and must exit within 1 s of `SIGTERM`. Use `src/sdk/hm_app.h` and
`tools/hpk.py build|inspect|verify`.

## Résumé (français)

Un `.hpk` est une archive **tar POSIX ustar non compressée** contenant `manifest.ini` (UTF-8
`clé=valeur` : `id`, `version`, `min_hibymod`, `name[.en|.fr]`, `description[.en|.fr]`, `exec`,
`arch=mipsel-hf`, `input=touch|keys|both`, `stop_player=0|1`, `ram_kb`, et en option
`category/author/homepage`), `icon.png` (carré opaque plein cadre, 128x128 conseillé ; l'installeur
applique le masque arrondi, le reflet et la bordure et produit `icon_48/56/64.ov`) et les fichiers
de l'app. Seuls les fichiers ordinaires et les dossiers à chemin relatif sont acceptés ;
l'exécutable doit être un ELF MIPS32r2 LE statique en virgule flottante matérielle. L'installation
valide tout d'abord, extrait dans `apps/.tmp-<id>` par blocs de 64 Ko maximum, puis remplace
l'ancienne version de façon atomique : un échec ne laisse jamais d'app à moitié installée. Les apps
reçoivent `HM_APP_DIR`, `HM_DATA_DIR`, `HM_LANG`, `HM_TOUCH`, `HM_KEYS`, `HM_FB`, doivent écrire tout
le tampon caché avant le panoramique et quitter moins d'1 s après `SIGTERM`. Voir
`src/sdk/hm_app.h` et `tools/hpk.py build|inspect|verify`.
