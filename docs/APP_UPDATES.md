# Updating the apps

ByOS Install (Android) and ByOS Desktop (Windows) update themselves from the project's GitHub
releases. This page explains what each app does, the signed file every app release carries, and
the steps to publish an app release.

## In short

- Each app release is a GitHub release with its own tag: `install-vX.Y.Z` (Android) or
  `desktop-vX.Y.Z` (Windows). It carries the program (`hibyos-install-X.Y.Z.apk` or `ByOS.exe`)
  **and** `byos-app-update.json`, a small description signed with the project's release key.
- The newest release of the repository is the ByOS firmware release (the players read
  `releases/latest/download/hibyos-update.json`). App releases are therefore **never** marked
  "latest": the apps list the releases and take the highest version among the tags with their
  own prefix.
- An app installs nothing only because GitHub says so. It first checks the signature of
  `byos-app-update.json` with the keys built into it, then downloads the program over HTTPS and
  keeps it only if its size and SHA-256 are the ones in the signed description.

## What the user sees

### ByOS Install (Android)

- **About › Check for App Updates.** When a newer version exists the screen shows its version and
  release notes (in the phone's language: English, Spanish or French) and **Download and
  Install**. The download shows its progress; the file is checked, then handed to Android's own
  installer, which asks the user to confirm. The first time, Android asks to allow "install
  unknown apps" for ByOS Install: the app opens that setting and continues when the user comes
  back. The new APK is signed with the same key as the installed app, so Android installs it as an
  update and keeps the app's data.
- **Check once a day** (on by default, switch in About): when the app starts and the last check is
  more than a day old, it checks quietly. Nothing is shown unless a newer version is found; then
  the home screen shows "ByOS Install X.Y.Z is available", which leads to About.
- Separately, the home screen and the build screen have **Check for a Newer ByOS** (the signed
  ByOS files of the latest firmware release, the same check as About › Check for newer ByOS
  files), with **Download and Use ByOS X.Y.Z** right there.

### ByOS Desktop (Windows)

- **About ByOS Desktop** (bottom of the sidebar): the version, **Check for Updates**, the release
  notes, **Download** with its progress, then **Restart to Update**. A badge "Update" appears next
  to it when a newer version was found.
- When the window opens, the app checks quietly at most once a day (switch "Check for updates once
  a day" in the same window).
- The checked program is kept as `%LOCALAPPDATA%\ByOS Desktop\updates\ByOS-X.Y.Z.exe`. **Restart to
  Update** closes the window and starts it; it unpacks itself (a few seconds) and removes the older
  version's folder. A copy of an older `ByOS.exe` started later (for example from the Downloads
  folder or a shortcut) hands over to the newest one, so an update is never undone by accident.
- The **Updates** tab of a HiBy has **Check for a Newer ByOS** (byos-cli `bundle-update`) and
  **Download and Use ByOS X.Y.Z**, plus **Start Over** (see below).

## Starting a build over

A build that stopped half way, or one made from older ByOS files or by another version of the app,
is detected and removed automatically before the next build: every build folder holds a stamp
(`build.stamp`: the ByOS version, the SHA-256 of the ByOS files and the app's version), and only a
finished build with the same stamp is reused. **Start Over** (Android: build screen; Windows:
Updates tab; byos-cli: `build-clean`) removes the build by hand. Neither ever deletes the checked
official firmware: it is kept so it never has to be downloaded again, and it is only replaced when
its checksum no longer matches.

## The signed description: `byos-app-update.json`

```json
{
 "format": 1,
 "product": "byos-app-update",
 "app": "byos-install-android",
 "version": "1.2.0",
 "version_code": 3,
 "tag": "install-v1.2.0",
 "asset": "hibyos-install-1.2.0.apk",
 "size": 12225526,
 "sha256": "<64 hex digits>",
 "min_os": "26",
 "published": "2026-11-12",
 "notes": {"en": "…", "es": "…", "fr": "…"},
 "signature": "ed25519:<128 hex digits>"
}
```

| Field | Meaning |
|---|---|
| `format`, `product` | Always `1` and `byos-app-update`. An app refuses a format it does not know. |
| `app` | `byos-install-android` or `byos-desktop-windows`. An app refuses the other app's file. |
| `version` | Plain version (`1.2.0`). Must be the version in `tag`, and newer than the running app. |
| `version_code` | Android only: the APK's versionCode. Must be higher than the installed one; the downloaded APK must have it and the app's package name. |
| `tag` | The release the file belongs to (`install-v1.2.0`, `desktop-v0.9.2`). It must be the tag the app found it in, so an old description cannot be replayed under a newer tag. |
| `asset` | The program's file name in that release (`hibyos-install-1.2.0.apk`, `ByOS.exe`). The download link is always built by the app: `https://github.com/Grohle/hibyos/releases/download/<tag>/<asset>`. |
| `size`, `sha256` | Of the program. The downloaded file is kept only if both match. |
| `min_os` | Android: lowest API level (`26`). Windows: lowest version (`10.0.17134`). |
| `notes` | Release notes by language (`en` required). |
| `signature` | Ed25519, exactly as in [OTA.md](OTA.md): one `"signature"` key; the signed message is the whole file with those 128 digits set to `0`. |

The public keys are the player's OTA release keys (`OTA_PUBKEY_HEX`, `OTA_PUBKEY2_HEX` in
`src/ota_net.inc.c`), built into ByOS Install (`core/.../btp/UpdatePackage.kt`, `RELEASE_KEYS`) and
into ByOS Desktop (`desktop/lib/app-update.mjs`, `RELEASE_KEYS`). The desktop tests check that the
three lists agree. Nothing is ever taken from the network to decide which key is trusted.

Where the code is: Android `core/.../AppUpdate.kt` (`AppUpdater`: release list, index, verified
download; tests in `core/src/test/.../AppUpdateTest.kt`) and `InstallViewModel.kt`; Windows
`desktop/lib/app-update.mjs` (tests in `desktop/test/app-update.test.mjs`), the routes
`/api/app-update*` in `desktop/server.mjs`, and the hand-over in `desktop/windows/launcher.c`.

### GitHub's limits

The apps use GitHub's REST API without an account (`GET /repos/Grohle/hibyos/releases`), which
allows 60 requests per hour per address. When GitHub answers with its rate limit the apps say so
("GitHub asks to wait before the next check", with the time left) and try again later; the daily
check stays silent. A renamed repository is followed (GitHub redirects the old name).

## Publishing an app release

Use the real release key only on the release machine; never commit it. For a dry run, make a test
key: `python tools/ota_sign.py genkey --key out/test.key --allow-in-repo` (the apps refuse files
signed with it).

### ByOS Install (Android)

1. Raise `versionCode` and `versionName` in `android/hibyos-install/app/build.gradle.kts`.
2. Build the signed APK (`./gradlew :app:assembleRelease`, README "Build from source") and copy it
   to `out/release/hibyos-install-X.Y.Z.apk`.
3. Write the notes (`notes.json` with `en`, `es`, `fr`) and make the signed description:

   ```
   python tools/app_release.py android out/release/hibyos-install-X.Y.Z.apk --notes notes.json --key %USERPROFILE%\.hibyos\ota_ed25519.key
   python tools/app_release.py verify out/release/install-vX.Y.Z/byos-app-update.json --file out/release/hibyos-install-X.Y.Z.apk
   ```

4. Publish, **not** as latest:

   ```
   gh release create install-vX.Y.Z out/release/hibyos-install-X.Y.Z.apk out/release/install-vX.Y.Z/byos-app-update.json --repo Grohle/hibyos --title "ByOS Install X.Y.Z" --notes-file out/release/install-X.Y.Z.md --latest=false
   ```

### ByOS Desktop (Windows)

1. Build the launcher with the new version (`bash tools/wsl/zbuild_desktop_exe.sh X.Y.Z`), byos-cli
   (`gradlew :cli:fatJar`) and the program (`python tools/package_desktop.py --version X.Y.Z`, which
   writes `out/release/ByOS.exe`).
2. Make and check the signed description (the version must be the one built into the exe):

   ```
   python tools/app_release.py desktop out/release/ByOS.exe --version X.Y.Z --notes notes.json --key %USERPROFILE%\.hibyos\ota_ed25519.key
   python tools/app_release.py verify out/release/desktop-vX.Y.Z/byos-app-update.json --file out/release/ByOS.exe
   ```

3. Publish, **not** as latest:

   ```
   gh release create desktop-vX.Y.Z out/release/ByOS.exe out/release/desktop-vX.Y.Z/byos-app-update.json --repo Grohle/hibyos --title "ByOS Desktop X.Y.Z" --notes-file out/release/desktop-X.Y.Z.md --latest=false
   ```

After publishing, the latest release must still be the ByOS firmware release
(`gh release view --repo Grohle/hibyos` shows it).
