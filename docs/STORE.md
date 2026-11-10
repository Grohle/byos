# HiByOS App Store

The **Apps** app (`src/store.inc.c`) is a lightweight store in the style of the iOS App Store that
serves three purposes:

- installing apps from **repositories** on the Internet;
- installing the `.hpk` files copied to the SD card (`/Apps`);
- managing installed apps: opening them, moving them between the SD card and the HiBy, backing
  them up and uninstalling them.

The package format is described in `docs/APP_FORMAT.md`.

UI strings below are given in English; the code keys them by their Spanish literal (see the
appendix).

## 1. Screens

At the bottom there is a tab bar like the iOS one:

| Tab | Content |
|---|---|
| **Today** | Large cards with the featured apps (`featured`). The background is taken from the icon's average colour. If no app is featured, all of them are shown. |
| **Apps** | All the apps from the repositories, with icon, name, description and button. Below them, the packages on the SD card (`/Apps/*.hpk`). |
| **Search** | Search field using the house keyboard. Filters by name and description in the current language, ignoring case and accents. |
| **Installed** | Installed apps with their version and location (HiBy or SD card). If a new version exists, UPDATE appears, and the tab carries a red badge with the number of updates. Below are the repositories: status, number of apps, "Update now", "Remove" and "Add repository…". |

App page: it opens when a row or card is tapped and slides in from the right.
It shows:

- the 96 px icon, name and author;
- the GET, UPDATE or OPEN button;
- a strip with SIZE, VERSION and MEMORY (`ram_kb`);
- the description and a notice if the app pauses the music;
- the INFORMATION section, with the repository, category and identifier.

To go back, tap "‹ Today" or drag from the left edge; the page follows the finger.

Gestures:

- **Scrolling**: iOS inertia (deceleration 0.998/ms) and bounce at the ends.
- **Pull down**: refreshes the repositories (except in Search). As in iOS, the activity indicator's
  spokes appear one by one while pulling; past the threshold it starts spinning, and on release the
  list stays open by the indicator's height until the indexes arrive, then springs back.
- **Refresh button** (circular arrow, top right of the large title): the same refresh; the button
  turns into the spinning indicator while it runs.
- **Tab bar**: the tab changes on touch-down (iOS). From its bottom strip, swiping up is the home
  gesture (`own_home_gesture`: the window follows the finger; holding still opens multitasking).
- **Tap the active tab**: scrolls back to the top.
- **Home bar**: exits. The store shrinks back into its icon.

An app's button:

1. **GET**. If there is an SD card, it first asks where to install (see §4). Then the button
   turns into the iOS **progress ring**; tapping the ring cancels the download.
2. When the download finishes, **size and sha256** are checked. The ring switches to "installing"
   (atomic installer with progress) and the app appears at the end of the last home screen page.
3. **OPEN** closes the store and opens the app from the centre of the screen.

If you leave the store during a download, the download is cancelled.

## 2. Repositories

A repository is **the URL of an `index.json`** over HTTPS (`http://` also works).

- The default repository, which cannot be removed, is
  `https://raw.githubusercontent.com/Grohle/hibyos/main/repo/index.json`.
- The others are stored in `/usr/data/hibymod/store_repos.conf`, one per line, with a maximum of
  8 repositories.
- If two repositories publish the same `id`, the one earlier in the list wins (HiByOS's comes
  first).

### `index.json` format (version 1)

```json
{
 "format": 1,
 "name": {"es": "HiByOS", "en": "HiByOS", "fr": "HiByOS"},
 "homepage": "https://github.com/Grohle/hibyos",
 "apps": [
  {
   "id": "org.hibymod.navegador",
   "version": "1.0.0",
   "min_hibymod": "0.1.0",
   "name": {"es": "Navegador", "en": "Browser", "fr": "Navigateur"},
   "desc": {"es": "…", "en": "…", "fr": "…"},
   "author": "NetSurf / hibymod",
   "category": "internet",
   "ram_kb": 7000,
   "stop_player": true,
   "featured": true,
   "size": 4300800,
   "sha256": "2d5593b9…",
   "icon": "icons/org.hibymod.navegador.png",
   "hpk": "https://github.com/Grohle/hibyos/releases/download/apps-1/org.hibymod.navegador-1.0.0.hpk"
  }
 ]
}
```

| Field | Required | Notes |
|---|---|---|
| `id`, `version` | yes | Same rules as `manifest.ini`: reverse-DNS id and semver version. |
| `name` | yes | A string or an object per language (`es`, `en`, `fr` and any other code from `lang_code()`). The current language is looked up first, then `en`, then `es` and, finally, whichever comes first. |
| `desc` | recommended | Same as `name`. |
| `hpk` | yes | Package URL, absolute or relative to the `index.json`. Up to 8 redirects are followed, so GitHub release URLs work. |
| `sha256` | yes | 64 hex digits of the `.hpk`. If it does not match, the package is deleted and not installed. |
| `size` | recommended | Size of the `.hpk` in bytes. Used for progress, free-space checks and as an extra check. |
| `icon` | recommended | Small square PNG (128×128 is enough, 512 px max, 1 MB max), absolute or relative. The house icon mask is applied to it. |
| `min_hibymod` | recommended | If it is greater than the installer's version, the button is greyed out with the notice "Requires a newer version of HiByOS". |
| `ram_kb`, `stop_player`, `author`, `category`, `featured` | no | Data for the app page and the Today tab. |

Apps with any required field missing or invalid are ignored. Unknown fields are ignored too,
for forward compatibility. The index can be at most 256 KB and list at most 64 apps in total.

### Where the HiByOS repository is published

- **Index and icons** on the `main` branch, folder `repo/`. They are served by `raw.githubusercontent.com`:
  direct response with no redirect and a cache of about 5 minutes.
- **Packages** as *assets* of a GitHub release that is not a firmware release: `apps-1`, with names
  `<id>-<version>.hpk`. This way binaries do not bloat the git history. The URL redirects (302) to
  `release-assets.githubusercontent.com`, and `hibynet` follows it.

To generate and publish it:

```sh
bash ~/hiby/tools/zbuild_app.sh hola                       # out/apps/hola.hpk
bash apps/netsurf-hiby/build/build_hf.sh                   # NetSurf hard-float (WSL)
bash apps/navegador/build.sh                               # out/apps/navegador.hpk
python tools/mkrepo.py out/apps/hola.hpk out/apps/navegador.hpk --tag apps-1
gh release upload apps-1 out/repo_assets/*.hpk --clobber -R Grohle/hibyos
git add repo && git commit -m "repo: …" && git push
```

A new version of an app is a new `.hpk`, with a different name because it includes the version, plus the
regenerated `index.json`. Devices see UPDATE the next time they refresh.

## 3. Network, cache and security

- Everything that goes over the network is done by **`hibynet get URL OUTPUT`**, in separate processes, so
  the UI never blocks. It uses TLS 1.2 with certificate verification (mbedTLS) and writes
  to `OUTPUT.part` first, then does an atomic `rename`.
- **One background job at a time**: first the indexes and then the icons, serially.
  A package download can run in parallel with that job.
- Cache in `/usr/data/hibymod/cache/store/`:
  - `<fnv32(url)>.json`: the indexes;
  - `icons/<fnv32(url)>.png`: the icons;
  - `dl/<id>.hpk`: the package while it downloads, if it is being installed on the HiBy.

  If it is being installed on the SD card, the package is downloaded to `SD/HiByOS/dl/`. In both cases it is deleted
  after installing.
- When the store opens, everything comes from memory/cache: the indexes are re-read only if their cache files
  changed, decoded icons are kept between sessions, and the SD/installed state is computed once. The network is
  used **only** on pull-down or the refresh button, plus once automatically when there is no cache at all (first
  run: loading indicator). Switching tabs or scrolling never touches the disk or the network.
- **Verification**: the sha256 is computed in the shell itself (C, FIPS 180-4) over the downloaded
  file, and the size is checked too. The installer validates everything again (tar, paths,
  hard-float ELF, space) before writing anything (`APP_FORMAT.md` §5).
- A third-party repository can publish any binary. The sha256 protects against corruption and
  against the package being swapped on the download server, but not against the index author. That is why
  you should only add repositories you trust.

## 4. Installing on the SD card or on the HiBy

`/usr/data` has about 24 MB free; the SD card has hundreds of GB. When GET is pressed with a card inserted,
a sheet asks where to install:

- "On the SD card (406 GB free)", which comes first;
- "On the HiBy (23 MB free)".

Before downloading, it checks that the destination has at least twice the package size plus
512 KB. Updates stay wherever the app is.

**Measured mount**: `/dev/mmcblk0` on `/usr/data/mnt/sd_0`, exfat
`rw,fmask=0022,dmask=0022`, **without `noexec`**. All files show up as 0755 and a binary
copied there runs fine (tested with `hibynet`). That is why the app runs directly from
the SD card, without copying it or using a loader.

Layout:

| Path | Content |
|---|---|
| `SD/HiByOS/apps/<id>/` | The complete app, installed by the same atomic installer with its root at `SD/HiByOS` (`hpk_set_root`). |
| `/usr/data/hibymod/apps/<id>/` | A **stub**: `manifest.ini`, `icon_48/56/64.ov` and `sd_location` (the real path). It takes about 30 KB. |
| `/usr/data/hibymod/appdata/<id>/` | The app's data, always on the HiBy. |

The launcher and the installed list only see the stub. `run_ext_app()` calls `exec` from the real
path, and `HM_APP_DIR` points to the SD card.

**Card missing** (detected by comparing the `st_dev` of `sd_0` with that of `/usr/data/mnt`):

- in Installed the app shows "SD card missing", in orange;
- OPEN and the home screen icon show "This app is on the SD card: insert it to open the app".

Pending outside my files: drawing the home screen icon dimmed. It needs a hook in
`launcher.inc.c`: if `apps[a].ext` is an SD app and `app_sd_missing()`, draw it with
alpha 0.4.

**Move** from Installed, with "Move to SD card" or "Move to HiBy":

- copies the tree to the destination, into `.tmp-<id>` and with `fsync`;
- performs the atomic switch with `.old-<id>`;
- then replaces or removes the stub and deletes the source.

**Power-loss safe**:

- At every moment there is a complete copy and the stub points to a valid path, or the previous
  version is still in place.
- Leftovers (`.tmp-`, `.old-`, `.del-`) are cleaned up or restored by `hpk_recover()` in `/usr/data` and
  `sd_recover()`, which is `hpk_recover` with its root on the SD card and runs when the store opens.

Uninstalling deletes the copy on the SD card, if the card is present, and then the stub. If it is uninstalled
without the card, an orphan folder `HiByOS/apps/<id>` remains on the SD card, which can be deleted by hand.

## 5. Memory and performance

- Store data: about 90 KB for 64 apps, plus a 48 px sprite per icon (4.6 KB) and a
  96 px one for the app page, which is freed on exit.
- Uses the shared buffers `img_under` and `img_sheet` for transitions; on exit it sets
  `have_under = 0`.
- Each `hibynet` takes 1–2 MB of RSS while it runs.
- It only redraws when something changes or animates: progress, inertia or notice. While only an activity
  indicator spins (refresh), just its rows are repainted (`present_rows`), so `hibynet` keeps the CPU.
- Measured in the QEMU harness (`docs/VM_SHELL.md`): touch-to-feedback 11–32 ms (one panel frame), full page
  render ~2 ms (max ~7 ms), tab switch render 2–10 ms.

## 6. Browser (NetSurf): "IN TESTING" status (blocked)

**Current status: it cannot be installed or opened.**

- In `repo/index.json` it appears with `"available": false`, with no `hpk` or `sha256`. The `.hpk` was removed
  from the release.
- The store shows it as IN TESTING.
- `run_ext_app()` refuses to open the ids in `app_blocked_ids[]` (`org.hibymod.navegador`), even
  if they are installed.

**Reason.** The first test on the device, with the player running (about 7.7 MB of MemAvailable),
**froze the HiBy** (adb included) 2 s after opening it. There is no swap and the OOM killer never gets to
act: the kernel keeps evicting the code of every process (*thrashing*).

### What has been changed

- **NetSurf 3.11 rebuilt with hard floating point** (`apps/netsurf-hiby/build/build_hf.sh`). The
  dependencies (zlib, mbedTLS, curl, libpng, libjpeg and expat) are hard-float too. Thread stacks
  are limited to 256 KB.
- **Custom backend** (`nsfb_hiby.c`):
  - canvas in RAM, copied whole to the hidden buffer and then `FBIOPAN`;
  - touch input from `HM_TOUCH`;
  - 1:1 scrolling with iOS inertia, with the `gui.c` patch that allows pixel-level
    scrolling;
  - `SIGTERM` closes the app.
- **Launcher and watchdog** (`apps/navegador/navegador.c`):
  - `oom_score_adj=1000` for `nsfb`;
  - `RLIMIT_AS` cap (prlimit) = initial VmSize + available RAM − margin;
  - `MemAvailable` watchdog every 50 ms: below 2.8 MB it sends `SIGKILL` and exits with code
    99, and the shell shows "The app closed: not enough memory";
  - the watchdog runs with `mlockall` and `SCHED_FIFO`.
- **Choices**:
  - 384 KB in-memory cache, no disk cache;
  - no JavaScript, no background images, no animations and no ad-blocking stylesheet;
  - 2 connections;
  - CA bundle reduced to 41 roots (`ca.pem`): −0.8 MB of anonymous RSS.
- **Manifest**: `stop_player=1` (pauses the music) and `refresh=full` (the shell keeps `hz_hold`).

### Off-device test protocol (mandatory before trying it again on the HiBy)

The harness is `apps/harness_vm` (`~/hiby/vm`): qemu malta MIPS32r2 LE, MemTotal of 56 928 kB like the
HiBy and no swap.

- **Memory**: the `coexist` profile (memhog of about 36 MB locked, like the player) plus
  `memhog 2`, which leaves **about 7.2–7.5 MB of MemAvailable**, like the device with music playing.
- **Script**: `share/nsm.sh SEG URL OUT` runs the real launcher with the same environment as the shell.
  Every 0.5 s it samples `nsfb`'s VmRSS and VmHWM, MemAvailable and the **gap between samples**: a gap
  of more than 1.5 s means the system stalled.
- **Test suite** (`scripts/navt2.cmds`):
  - DuckDuckGo Lite (home and search), text.npr.org and lite.cnn.com;
  - medium Wikipedia page (Ingenic) with 30 drags;
  - Wikipedia with images (Cat);
  - huge Wikipedia pages (Telegram ×2, Linux and a list of countries).

Acceptance criteria:

1. memhog (the "player") never dies;
2. the watchdog kills `nsfb` on every page that does not fit;
3. **no gap exceeds 1.5 s**;
4. light pages load and can be scrolled.

### Results (2026-11-10, VM, about 7.3 MB available at start)

| Page | nsfb VmHWM | Minimum MemAvailable | Result | Max gap |
|---|---|---|---|---|
| DDG Lite home | 5.2 MB (anon. 1.9) | 4.7 MB | loads | 0.55 s |
| DDG Lite search | 4.4–6.3 MB | 2.8–4.3 MB | loads or killed by the watchdog, depending on free RAM at start | up to 4.8 s |
| text.npr.org | 4.8–5.1 MB | 3.7–5.5 MB | loads or killed by the watchdog | 0.55 s |
| Wikipedia medium / Cat | 6–7 MB | 1.9–3.2 MB | killed by the watchdog | 0.8–2.1 s |
| Wikipedia huge ×4 | 5.3–7.8 MB | 1.2–2.8 MB | killed by the watchdog (4/4) | **7–10 s** |
| lite.cnn.com | 4.4–6.4 MB | 3.1–3.2 MB | loads or killed by the watchdog | **up to 15 s** |

memhog survived every test.

With the previous version, with no cap, no `mlockall` and the threshold at 2.3 MB, gaps reached
10 s and MemAvailable dropped to 1.2 MB. Also, MemAvailable at start keeps dropping from one test to
the next (from 7.5 to 3.7 MB), which is why the same page sometimes loads and sometimes does not.

**Verdict**: with the player running it is **not reliably usable**:

- only minimal pages fit;
- on the others the watchdog closes the app, as intended;
- even so, **multi-second stalls** remain before it can act, because thrashing
  starts above 2.8 MB.

It stays blocked. There are three options to move forward:

- **(a)** open it only with the player *stopped*, freeing its memory. It needs a hook in the
  shell to stop `hiby_player` and relaunch it on exit; still to be designed and measured.
- **(b)** replace it with a reading viewer backed by a *proxy* on the PC, under 2 MB (see
  `docs/research_apps_vm.md`).
- **(c)** trim NetSurf: smaller libcss/hubbub and dropping GIF, BMP and interlaced PNG, which is not
  enough on its own.

**It will not be opened on the device again** until the test suite above passes all four criteria.

## Summary

**Apps** is a light iOS-style App Store with repositories. A repository is the URL of an
`index.json` (format 1: `name`, `apps[]` with `id`, `version`, `min_hibymod`, localized
`name`/`desc` objects, `size`, `sha256`, `icon`, `hpk`, optional `featured`, `ram_kb`,
`stop_player`, `author`, `category`). The default repository is
`raw.githubusercontent.com/Grohle/hibyos/main/repo/index.json`; its packages are assets of the
GitHub release `apps-1`. Everything is fetched by `hibynet get` in background processes and cached
in `/usr/data/hibymod/cache/store/`. Packages are verified (size + sha256) before installation.
Tabs: Today, Apps, Search and Installed (installed apps, updates and repositories). Apps can be
installed on the SD card (`SD/HiByOS/apps/<id>`, executed in place because exfat is mounted
without `noexec`, with a small stub in `/usr/data`) or on the device, and moved between the two
crash-safely.

## Résumé (français)

**Apps** est une boutique légère façon App Store, avec des dépôts. Un dépôt est l'URL d'un
`index.json` (format ci-dessus). Le dépôt par défaut est `repo/index.json` sur la branche `main` ;
ses paquets sont des fichiers de la release `apps-1`. Tout passe par `hibynet get` en arrière-plan,
avec un cache dans `/usr/data/hibymod/cache/store/`, et la taille ainsi que le sha256 sont vérifiés
avant l'installation. Les apps peuvent s'installer sur la carte SD (exécutées sur place, avec un
petit talon dans `/usr/data`) ou dans le HiBy, et être déplacées de l'un à l'autre sans risque en
cas de coupure.

## Appendix: new store strings (for `i18n.inc.c`)

All of them go through `tr("literal en español")`, so the `es` column below is the literal lookup key
used in the code and must stay exactly as written. This table gives the English and French translations.

| es (key) | en | fr |
|---|---|---|
| Hoy | Today | Aujourd'hui |
| Buscar | Search | Rechercher |
| Instaladas | Installed | Installées |
| APP DESTACADA | FEATURED APP | APP À LA UNE |
| TODAS LAS APPS | ALL APPS | TOUTES LES APPS |
| Buscar apps | Search apps | Rechercher des apps |
| Nombre o descripción | Name or description | Nom ou description |
| %d resultados | %d results | %d résultats |
| Sin resultados | No results | Aucun résultat |
| Cargando… | Loading… | Chargement… |
| Actualizando… | Updating… | Mise à jour… |
| No hay apps en los repositorios | No apps in the repositories | Aucune app dans les dépôts |
| No se pudo conectar. Comprueba la WiFi y desliza hacia abajo para reintentar. | Couldn't connect. Check Wi-Fi and pull down to retry. | Connexion impossible. Vérifiez le Wi-Fi et tirez vers le bas pour réessayer. |
| Falta el componente de red (hibynet) | The network component (hibynet) is missing | Le composant réseau (hibynet) est absent |
| REPOSITORIOS | REPOSITORIES | DÉPÔTS |
| %s · %d apps | %s · %d apps | %s · %d apps |
| Sin conexión | Offline | Hors ligne |
| Añadir repositorio… / Añadir repositorio | Add repository… / Add repository | Ajouter un dépôt… / Ajouter un dépôt |
| URL del index.json | index.json URL | URL de l'index.json |
| URL no válida | Invalid URL | URL non valide |
| Ese repositorio ya está | That repository is already added | Ce dépôt est déjà ajouté |
| Repositorio añadido | Repository added | Dépôt ajouté |
| Actualizar ahora | Update now | Mettre à jour |
| Eliminar repositorio / Eliminar | Remove repository / Remove | Supprimer le dépôt / Supprimer |
| ¿Eliminar el repositorio? Las apps instaladas se conservan. | Remove the repository? Installed apps are kept. | Supprimer le dépôt ? Les apps installées sont conservées. |
| TAMAÑO / VERSIÓN / MEMORIA | SIZE / VERSION / MEMORY | TAILLE / VERSION / MÉMOIRE |
| INFORMACIÓN | INFORMATION | INFORMATIONS |
| Repositorio / Categoría / Identificador | Repository / Category / Identifier | Dépôt / Catégorie / Identifiant |
| Al abrirla se pausa la música para dejarle memoria. | Music pauses when it opens, to free memory. | La musique se met en pause à l'ouverture pour libérer de la mémoire. |
| Necesita una versión más nueva de HiByOS | Requires a newer version of HiByOS | Nécessite une version plus récente de HiByOS |
| EN PRUEBAS | IN TESTING | EN TEST |
| En pruebas: todavía no se puede instalar | In testing: can't be installed yet | En test : installation pas encore possible |
| Esta app está en pruebas y todavía no se puede abrir | This app is in testing and can't be opened yet | Cette app est en test et ne peut pas encore s'ouvrir |
| ¿Dónde instalar %s (%s)? | Where to install %s (%s)? | Où installer %s (%s) ? |
| En la tarjeta SD (%s libres) | On the SD card (%s free) | Sur la carte SD (%s libres) |
| En el HiBy (%s libres) | On the HiBy (%s free) | Sur le HiBy (%s libres) |
| Espera a que termine la descarga | Wait for the download to finish | Attendez la fin du téléchargement |
| No se pudo descargar | Download failed | Échec du téléchargement |
| La descarga está dañada (sha256) | The download is corrupted (sha256) | Le téléchargement est corrompu (sha256) |
| Versión %s disponible | Version %s available | Version %s disponible |
| Tarjeta SD | SD card | Carte SD |
| Falta la tarjeta SD | SD card missing | Carte SD absente |
| No hay tarjeta SD | No SD card | Pas de carte SD |
| Esta app está en la tarjeta SD: insértala para abrirla | This app is on the SD card: insert it to open the app | Cette app est sur la carte SD : insérez-la pour l'ouvrir |
| Mover a la tarjeta SD / Mover al HiBy | Move to SD card / Move to HiBy | Déplacer vers la carte SD / Déplacer vers le HiBy |
| Moviendo… | Moving… | Déplacement… |
| Movida a la tarjeta SD / Movida al HiBy | Moved to the SD card / Moved to the HiBy | Déplacée sur la carte SD / Déplacée sur le HiBy |
| No se pudo mover la app | Couldn't move the app | Impossible de déplacer l'app |
| La app se cerró por falta de memoria | The app closed: not enough memory | L'app s'est fermée : mémoire insuffisante |
| DOMINGO … SÁBADO | SUNDAY … SATURDAY | DIMANCHE … SAMEDI |
