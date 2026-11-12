# Research: companion apps (Android + desktop), transfer protocol, PC-side library database

Generated 2026-11-12 by a research workflow (read-only; repos cloned to a scratch folder). Licence notes are the researchers' reading; verify before reusing code.

## CompasOS (Starnished66/compas-player) desktop companion app and its device<->PC/phone exchange protocol, and what ByOS should adopt

I cloned four repos into C:\Users\alber\AppData\Local\Temp\claude\960b068394fc6611\d6a53856-6670-425f-991a-838192e7001d\scratchpad\research\compas\: compas-player (HEAD 8d7d435, 2026-11-08), compas-tools (458b12f, 2026-11-04), compas-app (2a13be1) and compas-firmware (e03bb8f). I did not touch any device.

**What CompasOS is.** It is a full replacement player for the HiBy R1, R3 Pro II and R3 II 2025, written in C/LVGL. It does not run alongside the stock hiby_player. That makes its library database its own format, and this matters for ByOS (see below).

**There is no special transfer protocol.** Compas uses three ordinary channels:
1. **USB mass storage.** The PC mounts the SD card.
2. **ADB.** It uses push, shell, pull, exec-out and forward.
3. **A plain-HTTP/1.1 "Remote Control" server built into the player.** It listens on TCP 8899 and, when it can, also on port 80. It is a hand-written server with 2 worker threads and `Connection: close` on every request (src/network/remote/control/remote_control.c). The same HTTP bytes can also run over Bluetooth Classic RFCOMM, using service UUID 9d2f8a7e-6b4c-4d51-8e43-38f7a2c0b719.

**Device discovery.**
- UDP on port 8899: the request is "COMPAS_REMOTE_DISCOVER_V1" zero-padded to 32 bytes, and the reply is "COMPAS_REMOTE_V1 8899\n". Requests from outside private LAN ranges are ignored (remote_control_discovery.c).
- mDNS service `_compas-remote._tcp.local` (remote_control_mdns.c).
- `GET /api/v1/capabilities` needs no authentication and lists the transports and features (`wifi_import_files`, `catalog_sync_v1`, `settings_v1`, `plugin_store`, ...).

**Security is weak.**
- There is no TLS and no encryption. The PIN travels in cleartext on Wi-Fi.
- Authentication is a 6-digit PIN generated from /dev/urandom with rejection sampling, sent in the `X-Compas-PIN` header. Browsers get it back as an HttpOnly, SameSite=Strict cookie.
- The PIN check is constant-time. After 5 distinct wrong PINs in 60 s, the server locks out for 60 s and answers 429.
- A Bluetooth peer that once sent the right PIN is trusted from then on (bt_rc_trust).
- Clients refuse redirects so the PIN cannot leak to another host (PinSafeHttp.kt in the app; the reqwest redirect policy in compas-tools).
- Request headers have a 10 s deadline and a 2 s idle timeout.

**File API (src/network/remote/import/remote_import_files.c).**
- It only works during an "import session" started with `POST /api/import/start`. The session is refused while the SD card is exported over USB.
- Routes: `GET /api/import/files?path&offset&limit` (paged, at most 100 entries, directories first), `POST .../upload?path&name` with the raw body and a required Content-Length (chunked encoding is not accepted, maximum 4 GiB), and `rename`, `delete`, `mkdir`.
- Uploads are refused over Bluetooth ("wifi_only").
- All access is confined to `/data/mnt/sd_0`: paths are opened step by step with openat and O_NOFOLLOW, and dot-files, `..` and control characters are rejected. Files are created with O_EXCL, so nothing is ever overwritten. Renames emulate RENAME_NOREPLACE on VFAT.
- Speed measures: a 64 KiB receive buffer and fdatasync every 4 MiB. Uploads stop after 12 s idle or 6 h total. A failed upload deletes its own partial file, checked by inode.
- There is **no checksum, no resume and no atomic temp-then-rename** in this API.

**Library sync (catalog_sync_v1).** Song and cover pages are tagged with a `library_id` and a `revision`. If the revision changed, the player answers 409 `revision_mismatch`, so a client never mixes snapshots. Cover keys use FNV-1a. The phone app also uses a deliberately non-standard SHA-256 for its cache file names (src/data/sha256.ts).

**PC app: Compás Tools.** Repo: github.com/Starnished66/compas-tools, GPL-3.0-only.
- Stack: Tauri 2 (Rust), React/TypeScript with Vite, KDE Breeze styling (LGPL-3 icons). Builds for Linux AppImage, macOS on Apple Silicon and Windows. The app updates itself from GitHub with minisign-signed updates.
- Music: it copies or mirrors music onto the **mounted SD card**. For each file it hashes the source with SHA-256, skips identical files, writes to a hidden temp sibling, fsyncs, re-reads and re-hashes, then renames into place (music/copy.rs, fs/atomic.rs).
- PC-side database: it then runs `compas-sdtool`, which is **the player's own library code compiled for the PC**. It runs `scan` and then `art`, so the database and album-art cache are built on the computer, not on the player.
- Firmware install over ADB (adb/install.rs):
  - Download the release and verify its SHA-256.
  - Check free space on the card.
  - `adb push` to `.name.pid-ms.part`, then check it with `sha256sum` on the device (or pull it back and hash it on the PC).
  - `mv` with an in-band "ok", then re-verify.
  - Check that /proc/mtd shows the expected recovery partition, move other `.upt` files aside as `.parked`, and require that the recovery flag in /dev/mtd5 was not already set.
  - Run `bootmode.sh Recovery`, read the flag back, reboot.
  - The install only counts as confirmed if boot_id changed and the same board comes back reporting the new version.
- Settings: changed over `adb forward tcp:0 tcp:8899` and HTTP with the PIN.
- Also: plugin and theme catalog installs verified by SHA-256, an allowlisted and redacted backup of /usr/data (uses exec-out), and redacted log bundles.

**Phone app: compas-app.** Expo / React Native 0.86 with Kotlin native modules for upload streaming over OkHttp, RFCOMM, discovery, media session and updates.
- It uploads any picked file or folder through the import API.
- Its LICENSE file is Expo's own MIT template ("Copyright 650 Industries"), and package.json says private. Its licence is therefore unclear, so treat it as all-rights-reserved.

**compas-firmware** is GPL-3.0 and holds the scripts that package `.upt` images.

**Licences and ByOS.**
- Everything usable here is GPL-3.0, and compas-tools is GPL-3.0-only. GPL code cannot go into ByOS under its current custom non-commercial licence, because GPL forbids extra restrictions such as "non-commercial".
- Reuse is only possible if ByOS (or that component) is relicensed to GPL-3.0-or-later, as docs/research_license.md already recommends.
- Until then, ByOS may re-implement the ideas: the protocol behaviour, the install checklist and the route shapes. It must not copy the code.

**What ByOS should adopt: one small static daemon, `byosd`, with one framed protocol on several links.**

Transports:
- (a) Wi-Fi TCP, encrypted.
- (b) USB through `adb forward tcp:N tcp:7340`. This works with our old adbd, because `forward` predates `exec-out`.
- (c) Later, a custom USB bulk interface through FunctionFS on the same configfs gadget as adb, with Microsoft OS 2.0 descriptors so Windows binds WinUSB without a driver install. Android can then use UsbManager.bulkTransfer over OTG with no adb.

Keep USB mass storage as the bulk path for whole-library syncs, and do the database work on the PC, as Compas does.

**Protocol sketch: "BTP/1".**

Frame layout: `magic 'BT' | ver u8 | type u8 | flags u8 | rsv u8 | stream u16 | len u32 | payload`. Payloads are at most 1 MiB. Control messages are CBOR or JSON.

Handshake:
- HELLO carries the protocol version, device id, board, ByOS and hiby versions, and the capabilities.
- First pairing: X25519 key exchange. Both screens show a 6-digit short authentication string (SAS) computed with BLAKE2b, and the user confirms. Each side then stores the other's static key, and later connections use an IK-style handshake.
- Wi-Fi traffic is encrypted with XChaCha20-Poly1305 (Monocypher, CC0/BSD-2, about 50 KB static).
- USB/adb traffic may skip encryption, since the cable and root adb are already trusted, but still carries per-chunk hashes.

File operations:
- `PUT_BEGIN{path, size, sha256, mtime, chunk=1MiB}` → `{upload_id, have_bytes}`. A non-zero `have_bytes` means resume.
- `DATA{upload_id, offset, bytes, blake2b-128}`. The daemon fdatasyncs every 8 MiB and sends ACK{durable_offset}.
- `PUT_COMMIT` makes the daemon re-hash the whole file, then fsync, rename from `.byos-part/<id>` into place and fsync the directory. Overwrite only happens when asked.
- Several files can be pipelined on separate stream ids over one connection.
- Also `GET`/`LIST` (paged, using sendfile on the device), `STAT`, `HASH`, `MKDIR`, `MOVE`, `DELETE`, a disk-space query, and `EVENT` push for progress, library and now-playing.

Update install (`INSTALL_UPDATE`): stage the file and verify its SHA-256. Validate the .upt structure on the device: Compas's firmware_image.c parses the ISO and checks the MD5 chain. Move other .upt files aside, set the recovery flag and read it back, reboot. Confirm through a boot_id change, the same board, and the new version.

Library operations: `LIBRARY_STATE{library_id, revision}` and `LIBRARY_RESCAN`, plus a PC-side database build. Because ByOS keeps the stock hiby_player, that build must produce HiBy's own database format; the hiby-mods database manager is the candidate, not Compas's sdtool.

Discovery: the UDP magic packet plus mDNS `_byos._tcp`, with the device id and key fingerprint in the TXT record.

Daemon limits for 57 MB of RAM: musl static MIPS, one epoll thread plus one disk thread, about 2 MiB RSS. It must stop on low memory or when the SD card is exported, and never run while the SD is exported over USB mass storage.

**Licence:** compas-player: GPL-3.0 (LICENSE; third-party notices in licenses/). compas-tools: GPL-3.0-only (Cargo.toml workspace license), with Breeze icons under LGPL-3.0-or-later and Noto Sans under OFL-1.1. compas-firmware: GPL-3.0. compas-app: unclear. Its LICENSE file is Expo's MIT template with Expo's copyright and package.json is private, so treat it as all-rights-reserved unless the author clarifies. ByOS reuse: GPL code cannot be combined with ByOS's current custom non-commercial licence, because GPL section 7/10 forbids added restrictions such as non-commercial. Ideas, protocol behaviour and route designs can be re-implemented clean-room. Copying code requires the receiving ByOS component to be GPL-3.0-compatible (GPL-3.0-or-later is fine with GPL-3.0-only code when distributed as GPLv3).

### Facts
- CompasOS is GPL-3.0 and is a full replacement player for HiBy R1, R3 Pro II and R3 II 2025, not a layer over hiby_player. (compas-player/LICENSE (GPLv3 text); compas-player/README.md 'CompásOS replaces the stock firmware')
- The player's Remote Control is a hand-written plain HTTP/1.1 server on TCP 8899, also on port 80 when it can bind it, with 2 client worker threads. Every response closes the connection. (compas-player/src/network/remote/control/remote_control.c lines with '#define REMOTE_CONTROL_PORT 8899', 'REMOTE_CONTROL_WEB_PORT 80', 'REMOTE_CLIENT_WORKERS 2')
- Authentication is a 6-digit CSPRNG PIN in the X-Compas-PIN header, or an HttpOnly SameSite=Strict cookie for browsers. The compare is constant-time and 5 distinct failures in 60 s cause a 60 s lockout (HTTP 429). There is no TLS or encryption. (remote_control.c: remote_control_random_pin, remote_control_pin_check, send_pin_rate_limited_response, send_session_cookie)
- The same HTTP protocol runs over Bluetooth Classic RFCOMM with UUID 9d2f8a7e-6b4c-4d51-8e43-38f7a2c0b719. A peer that once sent a valid PIN becomes trusted and skips the PIN. (compas-player/src/network/remote/control/remote_control.h (remote_control_handle_stream); compas-app/modules/compas-bluetooth/android/.../CompasBluetoothModule.kt line 37)
- Discovery: UDP port 8899 with request 'COMPAS_REMOTE_DISCOVER_V1' zero-padded to 32 bytes, reply 'COMPAS_REMOTE_V1 8899\n', private-LAN sources only; plus mDNS _compas-remote._tcp.local. (src/network/remote/discovery/remote_control_discovery.c and .h (PORT 8899, REQUEST_SIZE 32); remote_control_mdns.c line 20)
- GET /api/v1/capabilities needs no auth and returns version, authHeader, transports [wifi, bluetooth_classic_rfcomm], features (including wifi_import_files, catalog_sync_v1, settings_v1, plugin_store), libraryId and settingsSchemaVersion. (remote_control.c handle_capabilities_request)
- The file API (/api/import/files list|upload|rename|delete|mkdir) only works during an import session started with POST /api/import/start. The session is refused while the SD card is exported over USB storage, and uploads are refused over Bluetooth ('wifi_only'). (src/network/remote/import/remote_import_files.c handle_upload; src/ui/network/gui_network_services.c handle_remote_import_request ('player_storage_busy'))
- Upload framing: raw request body, Content-Length required, chunked encoding not accepted, at most UINT32_MAX bytes. 64 KiB receive buffer, fdatasync every 4 MiB, 12 s idle and 6 h total timeouts. File created with O_EXCL (never overwrites). On failure the server deletes its own partial file. No checksum, no resume, written directly at the final name. (remote_import_files.c: IMPORT_BODY_BUFFER 64KiB, IMPORT_SYNC_BYTES 4MiB, IMPORT_IDLE_SECONDS 12, handle_upload, remove_owned_partial)
- Paths are confined to /data/mnt/sd_0 by opening each component with openat and O_NOFOLLOW, and dot-files, '..', backslash and control characters are rejected. RENAME_NOREPLACE is emulated with a placeholder on VFAT. (remote_import_files.c path_string_valid, open_dir_relative, rename_noreplace)
- Library sync (catalog_sync_v1) pages songs and covers by library_id and revision. A changed revision returns 409 revision_mismatch or revision_or_cover_stale. Cover keys use FNV-1a. (remote_catalog.c lines 52, 184, 245, 362; remote_control_internal.h FNV comment)
- The desktop companion is a separate repo, Compás Tools: Tauri 2 (Rust) plus React/TypeScript, GPL-3.0-only, for Linux AppImage, macOS on Apple Silicon and Windows, with a minisign-signed self-updater. (github.com/Starnished66/compas-tools README.md, Cargo.toml (license GPL-3.0-only), src-tauri/tauri.conf.json plugins.updater.pubkey, docs/RELEASING.md)
- Compás Tools copies music onto the mounted SD card (mass storage). For each file it hashes the source with SHA-256, skips identical files, writes a hidden temp sibling, fsyncs, re-reads and re-hashes, then renames into place. (compas-tools/crates/compas-core/src/music/copy.rs copy_item and copy_verified; fs/atomic.rs)
- The library database and album art are built on the PC by compas-sdtool, which is the player's own library C code compiled for the PC (commands: info, scan [--full], art --board), run automatically after copying. (compas-tools/sdtool/src/cli/main.c; crates/compas-core/src/sdtool/mod.rs update_library; transfer.rs update_after_copy)
- Firmware install over ADB: SHA-256-verified download, free-space check, push to a .part file, sha256sum on the device (or pull back and hash), mv with an in-band 'ok' and re-verify, check the recovery partition in /proc/mtd, move other .upt files aside as .parked, set the recovery flag in /dev/mtd5 with bootmode.sh Recovery and read it back, reboot. Confirmed only by a boot_id change, the same board and the new version. (compas-tools/crates/compas-core/src/adb/install.rs Job::stage, matches_download, prepare_recovery, park_other_images, reboot_and_confirm)
- Compás Tools reaches player settings by running 'adb forward tcp:0 tcp:8899' and HTTP with the PIN. Its backup uses adb exec-out (which our old adbd does not support). (crates/compas-core/src/adb/forward.rs; settings/client.rs; adb/backup.rs line 184)
- The player's on-device OTA verifies a SHA256SUMS asset fetched over HTTPS from GitHub and the .upt structure (ISO parse plus MD5 chain) before moving the file to the SD root. There is no cryptographic signature on firmware. (compas-player/src/network/firmware/firmware_ota.h header comment; src/core/updates/firmware_image.c (mbedtls md5))
- The Android companion compas-app is Expo / React Native 0.86 with Kotlin modules. Uploads are streamed as raw OkHttp bodies to /api/import/files/upload and the client never follows redirects. Its LICENSE is Expo's MIT template (Copyright 650 Industries) and package.json is private, so its licence is unclear. (compas-app/package.json; modules/compas-files/android/.../PinSafeHttp.kt; src/data/RemoteApi.ts lines 234-290; compas-app/LICENSE)
- The author's other repos: compas-app (phone remote), compas-tools (desktop), compas-firmware (GPL-3.0, .upt packaging scripts), compas-plugins (Lua), compas-emulators, hiby-custom-kernel. (https://api.github.com/users/Starnished66/repos)
- ByOS's current licence is custom non-commercial. GPL code cannot be added to it without relicensing; docs/research_license.md recommends moving to GPL-3.0-or-later. (C:\Users\alber\Downloads\Hiby\hibymod\docs\research_license.md section 0)

### Recommendations
- Do not copy Compas code while ByOS keeps its non-commercial licence. Compas and Compás Tools are GPL-3.0(-only). Either relicense ByOS's transfer daemon and desktop app to GPL-3.0-or-later (see research_license.md) and then reuse code with attribution, or clean-room re-implement the protocol ideas, which is allowed.
- Build one small static MIPS daemon 'byosd' (musl, one epoll network thread plus one disk thread, about 2 MiB RSS) on TCP 7340. Make it reachable over Wi-Fi and over USB with 'adb forward tcp:N tcp:7340', which works on our old adbd because forward does not need exec-out. This is the same pattern Compás Tools uses with 8899.
- Use a binary framed protocol (BTP/1) instead of HTTP-with-Connection-close. Header: magic 'BT', version, type, flags, stream id u16, length u32. Payloads up to 1 MiB, several streams on one connection, JSON or CBOR control messages.
- Make it safer than Compas. Pair with X25519 plus a 6-digit short authentication string confirmed on both screens (no cleartext PIN), store the peers' static keys, and encrypt Wi-Fi traffic with XChaCha20-Poly1305 from Monocypher (CC0/BSD-2). The USB/adb link may run unencrypted but stays authenticated by the physical link.
- Add what Compas lacks: per-chunk BLAKE2b hashes plus a whole-file SHA-256 checked at PUT_COMMIT; resumable uploads (PUT_BEGIN returns the durable offset; write to .byos-part/<id>, then fsync, rename, fsync the directory); explicit overwrite and skip-if-identical by size and hash.
- Keep the good Compas device-side protections: openat/O_NOFOLLOW confinement to /usr/data/mnt/sd_0, rejection of dot-files and '..', O_EXCL creation, periodic fdatasync (4-8 MiB), idle and total timeouts, and refusing writes while the SD card is exported over USB mass storage.
- Copy Compás Tools' update-install checklist almost step by step for 'Install ByOS update' in both the desktop and Android apps: verify the hash after staging, validate the .upt structure on the device, move other .upt files aside, set the recovery flag and read it back, then confirm via a boot_id change, the board and the version. Also sign ByOS-built update manifests with Ed25519 so the device can tell an official build from a tampered one.
- For the PC-side library database, follow Compas's sdtool idea (the database is built on the PC and written to the card). Because ByOS keeps the stock hiby_player, the database must be in HiBy's format, so use the hiby-mods database manager. Expose LIBRARY_STATE{library_id, revision} and 409-on-stale semantics like catalog_sync_v1, so the phone and desktop caches stay consistent.
- Discovery: a UDP magic-packet responder limited to LAN sources, plus mDNS _byos._tcp with device id and key fingerprint in TXT, and an unauthenticated HELLO/capabilities message listing transports and features.
- Phase 2 for Android without adb: add a FunctionFS vendor bulk interface to the same configfs gadget as adb, with Microsoft OS 2.0 descriptors so Windows binds WinUSB automatically. Android then reaches it with UsbManager.bulkTransfer over OTG, and the desktop with libusb (LGPL-2.1, dynamic linking). Run BTP/1 unchanged over it.
- Desktop stack: Tauri 2 (Rust core plus web UI) is a proven fit for the iTunes-like app (small installer, cross-platform, signed updater). Keep the transfer and update logic in a reusable Rust crate, as compas-core does, so the Android app could share it through JNI later.

### Open questions
- Real throughput on the X1600: adb push over USB 2.0 vs Wi-Fi TCP vs a FunctionFS bulk interface, and XChaCha20-Poly1305 cost per MB on MIPS. These need measuring off the main device or in a controlled test, since no device was touched here.
- Does the R3 II stock kernel 4.4 configfs allow adding a FunctionFS function to the same gadget as adbd (Compas notes android0 for storage/DAC and adb_demo for ADB), and does vfat support fallocate for preallocation?
- Which hiby-mods database-manager output format matches the stock hiby_player database exactly (handled by the separate hiby-mods research task)?
- Is there an adb client library usable on Android over USB host (for example dadb or libadb-android) with a licence that fits, or should the Android app skip adb and use FunctionFS bulk plus Wi-Fi only?
- Will ByOS relicense to GPL-3.0-or-later, which would allow direct reuse of Compas code such as the import-file confinement logic and the install checklist, or stay non-commercial and re-implement everything?
- compas-app's licence status: ask the author whether MIT was intended.

## hiby-modding/hiby-mods: licence, PC database manager, other mods, and whether a PC-built library DB can replace the on-device scan on the R3 II 2025 (ByOS)

I cloned the repo (commit a5103f7c, 2026-08-30) to C:\Users\alber\AppData\Local\Temp\claude\960b068394fc6611\d6a53856-6670-425f-991a-838192e7001d\scratchpad\research\hibymods\hiby-mods. I also ran the PC updater offline against a scratch copy of our own device DB (C:\Users\alber\Downloads\Hiby\hibymod\backups\usrlocal_media_0644.db). No device was touched and nothing in hibymod was modified.

LICENCE. There is one licence file, LICENSE: a scoped MIT licence, "Copyright (c) 2026 noisetta and contributors". It covers only the "Project Tooling" (original tooling, scripts, build configuration, documentation and theme assets made by contributors). It explicitly does NOT cover the redistributed or modified firmware binaries. NOTICE.md says the patched hiby_player binaries and other system binaries are HiBy proprietary and are provided "for personal use, research, and educational purposes". Because the MIT text was edited, GitHub reports the licence as "Other"/NOASSERTION. LICENSE and NOTICE were added on 2026-06-30 (commit f69f05c5); earlier snapshots had no licence. In practice: Update_Database.py, HiBy_Database_Updater_GUI.py, the shell build scripts and the docs are MIT. ByOS can copy, modify and ship them, even under its non-commercial licence or a future GPL-3.0, as long as the MIT copyright and permission notice travel with them. The six binaries/*/hiby_player files are not licensed to us and must never be redistributed. The Unified Theme .view/.dlg files are edited copies of HiBy's stock layouts, so the MIT grant can only cover the contributors' own changes. Treat them like ByOS's own R2 problem: do not copy them. One indirect obligation matters: the updater imports mutagen, which is GPL-2.0-or-later (checked in the pip metadata). Shipping mutagen inside a frozen ByOS desktop exe creates a GPL combination, which ByOS's current non-commercial licence cannot satisfy. Pillow, customtkinter and tinytag are permissive.

DATABASE MANAGER (two parts). (1) PC side: tools/Update_Database.py (CLI) and HiBy_Database_Updater_GUI.py, both Python 3 with mutagen, optional Pillow and tinytag, and customtkinter for the GUI. They run on the PC against the SD card mounted as a drive. They do NOT create a schema: they need a usrlocal_media.db already copied from the device to the SD root. They open it, run DELETE plus executemany INSERT on MEDIA_TABLE, MEDIA2_TABLE, SEARCH_TABLE (CLI only), MEDIA3_TABLE (emptied), ARTIST/ALBUM/GENRE/ALBUM_ARTIST/FORMAT plus their *2_TABLE copies, COUNT_TABLE, CTIME_TABLE, MTIME_TABLE and M3U_TABLE. They leave HISTORY, COLLECT, PLAYLIST, MANAGER, list_tb_*, BOOK*, VERSION and the other tables alone. Stored paths look like "a:\\Artist\\Album\\file.flac" plus a trailing NUL, NFC-normalised, in UTF-8. The scripts reproduce the sorting rules of their patched build (strip leading punctuation and articles, sort tier symbols < digits < letters) and the stock format codes (FLAC 61868, MP3 85, and so on). Ids are assigned in path order and rows are inserted in title order. album_pic_path is filled from cover/folder/front/albumart .jpg/.jpeg/.png in the same folder, and lrc_path from a .lrc with the same name. The optional art pass rewrites the user's audio files in place, embedding a 360x360 JPEG at quality 90. That is destructive and needs opt-in and a backup. (2) Device side: their binary patch hijacks the Font Size page of Settings. It runs system("cp /usr/data/mnt/sd_0/usrlocal_media.db /usr/data/usrlocal_media.db && rm ...") and then reloads in place with lg_music_db_done(), lg_music_db_init("MEMORY",0x1000,"/data/usrlocal_media.db") and the UI events "1#1@[01]" and "2#0@[06]". Their v1.4-test release also tried a binary-free init script, S91_db_sync, that copies the SD DB before hiby_player starts.

COMPATIBILITY TEST ON OUR DB. On a copy of our R3 II 2025 DB (VERSION_TABLE 20181015), every column the script writes exists. The schema stayed byte-identical, integrity_check returned ok, text came out UTF-8 with a NUL like the device rows, and history, favourites and the queue were kept. So structurally, a PC build can replace the scan. Gaps I found for our build: (a) our MEDIA2_TABLE is NOT a copy of MEDIA_TABLE as the script assumes. It holds 608 rows that look like a folder-browse cache: directory rows "a:\\Dir\\*" with has_child_file=3 and cue_id=-2, ids starting at 0. Overwriting it could break the Files view [I, needs a device test]. (b) MQA: 242 of our tracks have quality '18' and FORMAT_TABLE has an 'MQA' row, but the script only writes 0 to 3 and never detects MQA. (c) It writes 0 for ctime/mtime in ALBUM/ARTIST tables, while the device fills them, and stock album lists can sort by ctime/mtime. (d) CUE sheets and .iso are not split: cue_id and end_time are always -1. (e) Every playlist gets M3U id=total. (f) Its sorting matches their patched binary, and the 'character' index letters may disagree with our stock sorting. (g) The path to the DB is hard-coded, so the tf_music_db_enable mode (DB stored on the SD under .temp) is ignored. (h) The GUI copy lacks the SEARCH_TABLE fix that the CLI has.

OTHER MODS. These are R3 Pro II binaries with fixed SHA-256 sums, built from the January 2026 stock firmware (5,598,244 bytes; App Sync is padded to 10 MiB): About Dev (Post Bar QR), App Sync (ADB on USB through the marker file /usr/data/adb_usb_mode), DB Manager, DB Manager Done dialog, Playlist three-dot off-by-one fix, Sorting (articles and symbols). As the ByOS notes already say, none of them apply to our build because the absolute offsets differ. Only the ideas carry over, and ByOS's shim or a shell UI can do the same without patching the binary. Tools: universal_mod_tool.sh, build_upt.sh and rebuild.sh unpack and repack .upt files (squashfs lzo, MD5 chunk chain, ISO). ByOS already does this. merge_arabic_font.py merges Noto Naskh Arabic into Thai.ttf using fontforge (OFL). bt_init_optimized is a modified copy of HiBy's BT init script that replaces fixed sleeps with polling for hci0 (reported 11 s down to 7 s). It is vendor-derived, but it is useful as a reference for ByOS's BT equivalence test. The Unified Theme is a set of reskinned stock views and icons. Its docs (FIRMWARE_FORMAT, FACTORY_MODE, ALBUM_ART, SCREENSHOTS) are MIT and are useful facts; ALBUM_ART reports that a 360x360 art area speeds up drawing. In the same org: open_hiby_player is GPL-3.0 and hiby_os_crack has no licence (all rights reserved).

**Licence:** hiby-mods: modified ("scoped") MIT, Copyright (c) 2026 noisetta and contributors. Covered: tools/*.py and *.sh, docs, build configuration, and contributor-original theme assets. Allowed for ByOS: copy, port, modify and redistribute, including under ByOS's non-commercial licence or a future GPL-3.0, provided the MIT copyright and permission notice is kept (in LICENSES/ and THIRD_PARTY_NOTICES, and in the desktop app's About screen if code is ported). Not covered: binaries/*/hiby_player (HiBy proprietary per NOTICE.md, personal and research use only; ByOS must never redistribute them). Unified Theme .view/.dlg files are modified stock HiBy layouts, and bt_init_optimized is a modified vendor script; MIT can only cover the contributors' changes, so do not copy them into ByOS (if ever needed, reapply as patch-time edits on the user's own firmware). Before 2026-06-30 the repo had no licence; use the current tree. Dependency obligations: mutagen is GPL-2.0-or-later, so bundling it into a distributed ByOS desktop binary creates a GPL combination that ByOS's current non-commercial licence cannot meet. Either relicense the desktop app GPL-3.0-or-later (compatible with GPL-2.0-or-later) or replace mutagen with a permissive tag reader (e.g. TagLib is LGPL-2.1/MPL-1.1; or our own hibytag C code). Pillow (MIT-CMU/HPND), tinytag (MIT) and customtkinter (MIT) only need notices. Related: open_hiby_player is GPL-3.0 (incompatible with the current ByOS licence); hiby_os_crack has no licence (reading it is fine, copying is not).

### Facts
- LICENSE is a modified MIT licence scoped to 'Project Tooling' (tooling, scripts, build config, docs, contributor-created theme assets); it excludes redistributed/modified firmware binaries. Copyright line: 'Copyright (c) 2026 noisetta and contributors'. (hiby-mods/LICENSE lines 1-9 (clone at scratchpad\research\hibymods\hiby-mods, commit a5103f7c))
- NOTICE.md says the patched hiby_player and other system binaries are HiBy proprietary, not MIT, and are provided for personal use, research and education only. (hiby-mods/NOTICE.md)
- GitHub reports the repo licence as 'Other' (NOASSERTION). LICENSE and NOTICE were added on 2026-06-30 in commit f69f05c5. (gh api repos/hiby-modding/hiby-mods; gh api commits?path=LICENSE)
- The PC updater requires mutagen, which is GPL-2.0-or-later. Optional dependencies: Pillow and tinytag. The GUI adds customtkinter. (tools/Update_Database.py header; tools/HiBy_Database_Updater_GUI.py line 7; pip show mutagen 1.47.0 -> License: GPL-2.0-or-later)
- The updater does not create a schema. It opens an existing usrlocal_media.db (copied from the device to the SD root) and runs DELETE + INSERT on MEDIA/MEDIA2/SEARCH/MEDIA3, ARTIST/ALBUM/GENRE/ALBUM_ARTIST/FORMAT (and their *2 copies), COUNT, CTIME, MTIME and M3U. It leaves HISTORY, COLLECT, PLAYLIST, MANAGER and list_tb_* untouched. (tools/Update_Database.py rebuild_db() lines 851-1110; tools/Update Database README.md 'Database Structure')
- Running the hiby-mods rebuild_db on a copy of our R3 II 2025 DB kept the schema byte-identical, integrity_check returned ok, and paths and text were written as UTF-8 with a trailing NUL, the same as the device rows (e.g. hex C391616E64C3BA00). (scratchpad\research\hibymods\scripts\compat_test.py run on dbtest\orig.db (copy of hibymod\backups\usrlocal_media_0644.db); output in this session)
- On our device DB, MEDIA2_TABLE (608 rows) is not a copy of MEDIA_TABLE (2488 rows). It contains directory rows such as 'a:\\A Tribe Called Quest\\*' with has_child_file=3 and cue_id=-2. Only 10 rows match MEDIA_TABLE by id and path. The hiby-mods script overwrites it with album-sorted track rows. (SQL on scratch copy: select has_child_file,count(*) from MEDIA2_TABLE -> (0,527),(3,81))
- Our DB marks 242 MQA tracks with quality '18', and FORMAT_TABLE contains an 'MQA' row. The script only writes quality 0-3 and has no MQA detection. (SQL: quality '18' rows = 44.1kHz/16-bit FLAC; FORMAT_TABLE [(26,'FLAC'),(1,'MP3'),(11,'MQA')]; Update_Database.py quality_tier() line 159)
- The script writes 0 for ctime/mtime in ARTIST/ALBUM/GENRE/ALBUM_ARTIST tables; the device fills real timestamps (0 of 367 albums have ctime=0 on device). CUE sheets are not split (cue_id and end_time are always -1), and every M3U row gets id=total. (Update_Database.py lines 1019-1106; device ALBUM_TABLE sample)
- In stock data, album_pic_path and lrc_path are empty for all tracks. The PC script fills them from folder cover files and same-name .lrc files. (hibymod/docs/LYRICS_AND_COVERS.md lines 132-146; compat test output rows)
- The DB Manager binary patch copies the DB with system('cp /usr/data/mnt/sd_0/usrlocal_media.db /usr/data/usrlocal_media.db && rm ...'), then reloads in place with lg_music_db_done() and lg_music_db_init('MEMORY',0x1000,'/data/usrlocal_media.db') plus the UI events '1#1@[01]' and '2#0@[06]'. Addresses are for the R3 Pro II January 2026 build. (binaries/DB Manager Patch/DB Manager Patch README.md lines 100-147)
- Release v1.4-test tried a binary-free init script, S91_db_sync, that copies an SD-root usrlocal_media.db into internal storage before hiby_player starts. (gh api releases, tag v1.4-test body)
- All six patched binaries are R3 Pro II builds with fixed SHA-256 sums (original size 5,598,244 bytes; App Sync padded to 10,485,760). ByOS notes record that these binary patches do not apply to the R3 II 2025 build. (sha256sum binaries/*/hiby_player; binaries/*/README.md; memory hiby-r3ii-mod-project.md line 27)
- The CLI updater writes SEARCH_TABLE (fix from 2026-06-05, commit 09da5580), but the GUI updater in the same tree does not. (grep SEARCH tools/*.py; gh api commits?path=tools/Update_Database.py)
- The art-embed pass rewrites the user's audio files in place (360x360 JPEG, quality 90, Lanczos), and folder covers can be overwritten or converted to .jpg. (Update_Database.py embed_art and resize_folder_covers, lines 497-560; Update Database README.md 'Album Art Embedding')
- Other repos in the org: open_hiby_player (GPL-3.0, open replacement for hiby_player), hiby_os_crack (no licence), Meowby-R1 and hiby-rockbox-theme (no licence detected). (gh api orgs/hiby-modding/repos)
- bt_init_optimized is an edited copy of the vendor BT init script: fixed sleeps are replaced by polling hciconfig hci0 for up to 50 x 0.1 s (claimed 11 s down to 7 s). It also creates /usr/data/alsa.conf with an LDAC bluealsa sink. (tools/bt_init_optimized lines 56-118; README v1.5 changelog)

### Recommendations
- Use a PC-side library build in the ByOS desktop app, but as a ByOS-owned module ported from Update_Database.py (MIT, with credit), not a copy of the script. Start from a copy of the live device DB (pulled over ADB or read from the SD), keep its schema, and keep HISTORY/COLLECT/PLAYLIST/MANAGER/list_tb_*/BOOK* untouched, as hiby-mods does.
- Fix the gaps for our build before shipping: do NOT overwrite MEDIA2_TABLE (on the R3 II 2025 it is a folder-browse cache); either leave it alone or empty it, after a device test confirms the player rebuilds it. Detect MQA (FLAC tags such as MQAENCODER/ENCODER, or the bitstream) and write quality '18' plus the FORMAT 'MQA' row. Fill ctime/mtime in the catalog tables. Handle .cue (cue_id/begin_time/end_time, has_child_file). Give M3U rows unique ids. Use stock (unpatched) sorting rules for 'character' and pinyin_charater, or make sorting a setting. Write SEARCH_TABLE. Honour tf_music_db_enable (DB on the SD under .temp).
- Install the DB without any binary patch. Option 1: the ByOS shim, which already runs inside hiby_player, finds and calls the player's own close/reopen pair (lg_music_db_done/lg_music_db_init; addresses must be found for our build) and posts the '1#1@[01]' and '2#0@[06]' refresh events. Option 2 (fallback): a ByOS boot-time step like S91_db_sync that swaps in a staged DB before hiby_player starts. Always keep the previous DB as usrlocal_media.db.bak and run 'pragma integrity_check' on the device before the swap.
- Avoid losing data between snapshot and install: pull a fresh DB right before building, or build only the library tables and merge them into the device DB on the device (ATTACH plus INSERT inside one transaction, run by ByOS's own hibytag/hibymix SQLite using the player's busy_timeout or lock), so history and favourites written in the meantime are kept.
- Make art embedding opt-in, non-destructive by default, and backed up: prefer writing a 360x360 cover.jpg per album folder and filling album_pic_path, rather than rewriting the user's audio files. The 360x360 size from docs/ALBUM_ART.md is worth adopting as the desktop app's default.
- Licence hygiene: add the hiby-mods MIT notice to THIRD_PARTY_NOTICES.md and LICENSES/ for every ported part. Do not bundle mutagen unless the desktop app is GPL-3.0-or-later; otherwise write the tag reader on top of permissive code or reuse ByOS's own hibytag C code (e.g. as a library or CLI the desktop app calls). Never ship their hiby_player binaries, Unified Theme views or bt_init_optimized.
- Reuse ideas, not bytes, from the other mods: App Sync (ADB starts automatically on USB connect through a marker file) maps to the desktop and Android 'plugged-in' mode; the sorting and playlist fixes can be done in ByOS's shell UI. bt_init_optimized's polling approach is a reference for ByOS's BT-equivalence test (measure the time to /tmp/bt_init_ok and compare bluealsa/LDAC behaviour between stock and ByOS).
- Validation plan before trusting a PC-built DB on device (to run later, by the main agent, under the device rules): build from the user's real SD on the PC; diff against a device-scanned DB (row counts, per-path fields, the MQA quality and format values, cover and lrc paths); then on device check the Albums, Artists, Genres, Files, Search, Favourites, History and Playlists views, the 'recently added' order, and playback of a CUE album.

### Open questions
- What does the R3 II 2025 player use MEDIA2_TABLE for (Files browser cache?), and does it rebuild the table if it is emptied? Needs a device test or a look at the binary.
- Are lg_music_db_done/lg_music_db_init exported symbols in our hiby_player or its libraries (callable with dlsym from the shim), or only internal functions whose addresses must be found for our build?
- Does the stock player run its own rescan on boot or on SD mount that would overwrite a PC-built DB (e.g. a media change detected from mtimes)? This decides whether a PC build stays in place.
- Which column or tag does the stock scanner use to set quality '18' (MQA), and what do the FORMAT_TABLE ids mean (FLAC=26, MP3=1, MQA=11 are first media ids, while cn is always 1)?
- Does the stock player apply the 50,000-track limit to a PC-built DB that holds more rows?
- Will the owner relicense the ByOS desktop app GPL-3.0-or-later (which would allow mutagen), or should it avoid GPL dependencies?

## Desktop app stack for an iTunes/iOS-style ByOS manager: MIT music player base, building ByOS on the PC, hands-free USB install, library sync

Recommendation: build the desktop app with Tauri 2 (Rust core plus a React/TypeScript UI) as a fork of Museeks (MIT). Museeks is the only maintained, MIT-licensed desktop music player/library manager of good quality that I found on a modern stack. Clone: research/desktop/museeks, v0.23.4, about 3.1k lines of Rust and 10.4k of TS. It uses Tauri 2.11, React 19, lofty 0.23 for tags and artwork (MIT OR Apache-2.0), sqlx/SQLite for the library, m3u import/export, playlists, a queue, a virtualized track list, cover art, an axum stream server and auto-refresh. It runs on Windows, macOS and Linux. Last commit 2026-05-26, 2.1k stars. It has no smart playlists and no device sync, so both are ours to add. Its UI is plain, so the iTunes/iOS look (sidebar, album grid, blur, sheets) is a reskin, and web tech makes that the cheapest route to iOS-faithful polish (backdrop-filter, CSS spring animations). MIT can go into ByOS under its current non-commercial licence if the MIT notice is kept.

Closest functional match: Compás Tools (github.com/Starnished66/compas-tools, cloned at research/desktop/compas-tools), the desktop app the user mentioned. Stack: Tauri 2 + React 18 + Rust crate `compas-core` + C sidecar `compas-sdtool`.
- It installs firmware over ADB. It downloads Google platform-tools if adb is missing, pushes the .upt to /data/mnt/sd_0, checks sha256 on the device, then runs `/usr/bin/bootmode.sh Recovery`, which writes `ota:kernel2` to /dev/mtd5 and is checked by reading it back. It then reboots, waits up to 15 min and confirms the result by boot_id and version.
- It copies music and rebuilds the library DB on the PC with the player's own indexer code compiled as a desktop CLI.
- It manages plugins.
- It is GPL-3.0, so its code cannot be copied into ByOS under the current custom non-commercial licence (GPL forbids extra restrictions). We can learn from its facts and design. If ByOS adopts GPL-3.0-or-later, as docs/research_license.md recommends, forking Compás Tools becomes the fastest path. That is a decision for the owner.

Rejected or secondary candidates:
- Harmonoid: PolyForm Strict, which forbids distributing changes.
- Nuclear: AGPL-3.0, a streaming aggregator.
- GPL-licensed, not usable under the current ByOS licence: Tauon (GPL-3.0, Python), Strawberry and Clementine (GPL-3.0, Qt/C++), Quod Libet (GPL-2.0), Feishin (GPL-3.0, a Navidrome client), Musicat (GPL-3.0, Tauri/Svelte, nice smart lists), Dopamine (GPL-3.0), Screenbox (GPL-3.0).
- Beets (MIT, a Python CLI with 15.9k stars): needs mediafile/mutagen (GPL-2.0) at runtime, so use it only as an optional external tool, not embedded.
- YesPlayMusic (MIT): a NetEase streaming client, not a local library manager.
- Auzi (MIT, Avalonia/.NET), Melodex/Lokal/Tplayer (MIT, Electron): small and unproven.

Framework comparison:
- Electron (MIT) is about 100 MB or more.
- Flutter (BSD-3) has no good MIT base now that Harmonoid is excluded.
- Avalonia/.NET MAUI (MIT) have the excellent ATL .NET tag library (MIT) but no player base.
- Qt is LGPL or commercial.
- Compose Multiplatform Desktop (Apache-2.0) is the strong runner-up. It could reuse the Android app's Kotlin `core` module directly and share one device/sync core with the phone app. But it has no MIT player base, JVM audio is weak (vlcj is GPL, JavaFX media has few formats), and the MIT tag libraries are limited (jaudiotagger is LGPL).

Building ByOS on the PC with no WSL. For end users, "build" should mean: download the signed ByOS bundle, then patch their own official r3ii_2025.upt locally. The pieces already exist:
- `android/hibyos-install/tools/export_bundle.py` produces a reproducible bundle zip (bundle.json, overlay/, sd/ByOS/, notices/) plus an index JSON.
- The Kotlin core (android/hibyos-install/core, 4,291 lines, plain `org.jetbrains.kotlin.jvm`, no Android imports, tests run in a 256 MB heap, output checked against the Python patcher) does ISO/UPT, SquashFS, LZO, Ed25519, bundle, patcher, the PlayerClient for HiBy's Wi-Fi transfer on port 4399, and the Sender with resume and read-back verification.

Do not write a third patcher. Ship the Kotlin core as a Tauri sidecar CLI, compiled with GraalVM native-image (no JRE, licence GPL-2.0 with Classpath Exception, output unencumbered) or with a jlink-trimmed JRE. The existing PyInstaller ByOS-Patcher.exe is the fallback.

For developers, a "build from source" mode is feasible on Windows with no WSL. zig is a native Windows cross compiler: `zig cc -target mipsel-linux-musleabihf -mcpu=mips32r2` with the pinned ziglang 0.17.1. Note that the PyPI wheel hash differs per platform, so Windows builds are not byte-identical to the published Linux ones. The bash fetch/zbuild scripts would be ported to Python. make_payload.py needs the unpacked stock rootfs, which the patcher's own SquashFS reader can extract instead of using unsquashfs. build.py's unshare/mksquashfs path is replaced by the pure patcher.

Hands-free install (proposed architecture):
1. Detect the player as a USB drive (volume with ByOS/ or the hibymod marker) and/or over ADB. ByOS shows SD and ADB together. On stock, Compás says ADB and USB storage are mutually exclusive.
2. Identify it: md5/BuildTime of the official 1.3 firmware, /etc/hibymod_build.
3. Fetch the official .upt from HiBy's links listed in bundle.json and check its sha256.
4. Fetch and Ed25519-verify the bundle and patch it in the sidecar.
5. Copy the .upt and ByOS/ to the SD, by mass storage when possible (fastest) or `adb push`, then verify sha256 on the device.
6. Trigger install: `hibymod reinstall --yes` (overlay/usr/bin/hibymod, which runs `bootmode.sh Recovery`, sync and reboot), wait and confirm.
7. For ByOS-to-ByOS updates that only touch the payload, deliver the signed OTA (hibyos-update.json + tar, docs/OTA.md) locally: shell restart, no recovery.
8. First install from stock without ADB: guided mass-storage copy plus System › Firmware update › Via SD-card.

Library sync: port the schema logic of hiby-mods `tools/Update_Database.py`:
- It is MIT (noisetta), about 1,160 lines. It writes MEDIA/ARTIST/ALBUM/GENRE/SEARCH tables with paths like `a:\rel\path`, format codes and sort keys, and embeds 360×360 JPEG art.
- Port it to Rust with lofty + rusqlite (MIT). Running the Python as-is pulls in mutagen (GPL-2.0-or-later).
- Validate the result against ByOS's own `src/hibytag.c` and a real R3 II 2025 database. The upstream script targets the R3 Pro II.
- On the R3 II the DB is internal: /usr/data/usrlocal_media.db (src/music.inc.c). The SD copy at /data/mnt/sd_0/.temp/ is used only if `tf_music_db_enable` is set.
- So a PC-built DB needs a safe import on the device: hiby_player stopped, or applied at boot (like the hiby-mods Database Manager), integrity_check, then swap.
- hiby-mods firmware binaries are NOT MIT (NOTICE.md). Never reuse them.

**Licence:** ByOS today: custom 'HiByOS Community License 1.0' (non-commercial; commercial use needs the author's licence). It is GPL-incompatible, so only permissive code can be merged in.
- Usable directly:
  - Museeks: MIT (keep the notice).
  - Tauri: Apache-2.0/MIT. Electron, Avalonia, MAUI: MIT. Compose Multiplatform: Apache-2.0. Flutter: BSD-3.
  - lofty: MIT/Apache-2.0. rusqlite, adb_client, zip: MIT. ed25519-dalek: BSD-3.
  - symphonia: MPL-2.0 (file-level copyleft, fine unmodified).
  - Google platform-tools adb: Apache-2.0, redistributable with its notice.
  - GraalVM CE native-image: GPL-2.0 with Classpath Exception, so the output binary is unencumbered.
  - hiby-mods tooling: MIT (Update_Database.py logic portable with attribution). The hiby-mods firmware binaries are HiBy proprietary: never reuse them.
- Not usable while ByOS stays non-commercial:
  - Compás Tools, Compás player, Strawberry, Clementine, Tauon, Feishin, Musicat, Dopamine, Navidrome: GPL-3.0.
  - Quod Libet, MPD, mutagen, Picard: GPL-2.0.
  - Nuclear, Swing Music, Symphony: AGPL-3.0.
  - Harmonoid: PolyForm Strict, never usable.
- Beets is MIT, but it pulls in GPL mutagen at runtime: use it only as an external tool.
- If ByOS relicenses to GPL-3.0-or-later as research_license.md recommends, the GPL-3.0 projects (notably Compás Tools) become forkable. GPL-2.0-only code stays incompatible with GPL-3.0.

### Facts
- Museeks is MIT, Tauri 2 + React 19 + Rust, uses lofty (tags/artwork) and sqlx SQLite, supports playlists, m3u import/export, queue, cover art, library auto-refresh; Windows/macOS/Linux (C:\Users\alber\AppData\Local\Temp\claude\960b068394fc6611\d6a53856-6670-425f-991a-838192e7001d\scratchpad\research\desktop\museeks\package.json (license MIT, v0.23.4), src-tauri\Cargo.toml (tauri 2.11.2, lofty 0.23.3, sqlx sqlite, axum, m3u), README.md; GitHub API: 2141 stars, pushed 2026-07-03)
- Harmonoid is PolyForm Strict 1.0.0 (no distribution of modifications), so it is unusable (https://raw.githubusercontent.com/harmonoid/harmonoid/master/LICENSE)
- Licences of the other candidates: Nuclear AGPL-3.0; Tauon, Strawberry, Clementine, Feishin, Musicat, Dopamine, Screenbox, Navidrome, gonic GPL-3.0; Quod Libet, MPD, mutagen, Picard GPL-2.0; Beets, tinytag, YesPlayMusic, psst MIT; musikcube BSD-3 (GitHub REST API repos/<name> license.spdx_id, queried 2026-11-12)
- Compás Tools is GPL-3.0, Tauri 2 + React 18 + Rust crate compas-core, bundles a C sidecar compas-sdtool (the player's own library indexer as a desktop CLI), and installs firmware over ADB (research\desktop\compas-tools\README.md, LICENSE, src-tauri\tauri.conf.json (externalBin binaries/compas-sdtool), crates\compas-core\src\adb\mod.rs)
- Compás install contract: adb push to /data/mnt/sd_0 temp file, mv, sha256sum check, verify /proc/mtd has mtd5 "ota", run /usr/bin/bootmode.sh Recovery, read 'ota:kernel2' back from /dev/mtd5, sync, reboot, wait up to 15 min, confirm by boot_id + version (research\desktop\compas-tools\crates\compas-core\src\adb\install.rs lines 140-330; adb\install\card.rs lines 64-80)
- Compás downloads Google platform-tools when adb is missing, and states that on stock firmware ADB and USB storage share the port and are mutually exclusive (compas-core\src\adb\locate.rs; src\i18n\catalogs\es.ts line 152)
- ByOS already has a hands-free reinstall command: 'hibymod reinstall --yes' checks the recovery and the SD .upt, then runs bootmode.sh Recovery; sync; reboot (C:\Users\alber\Downloads\Hiby\hibymod\overlay\usr\bin\hibymod lines 65-71)
- On ByOS a plain 'reboot' from ADB powers the player off; 'hibyguard reboot' restarts it (C:\Users\alber\Downloads\Hiby\hibymod\README.md lines 528-532)
- The Android app's core module is a plain Kotlin/JVM library (no Android APIs, 4291 lines) with Patcher, SquashFs, Lzo1x, Iso/Upt, Ed25519, Bundle, PlayerClient (stock Wi-Fi server on port 4399) and Sender (resume + read-back sha256); tests run with a 256 MB heap (C:\Users\alber\Downloads\Hiby\hibymod\android\hibyos-install\core\build.gradle.kts; core\src\main\kotlin\org\hibyos\install\core\*.kt)
- The ByOS bundle is generated reproducibly by export_bundle.py from the same Python definitions as the desktop patcher, together with an index JSON used to find and verify newer bundles (C:\Users\alber\Downloads\Hiby\hibymod\android\hibyos-install\tools\export_bundle.py docstring)
- The Python patcher is pure Python (pycdlib LGPL-2.1), needs no WSL or root, takes about 10 s, and ships as ByOS-Patcher.exe (C:\Users\alber\Downloads\Hiby\hibymod\docs\PATCHER.md)
- The device-binary build uses zig cc 0.17.1 (PyPI ziglang) targeting mipsel-linux-musleabihf, static; the scripts are bash with WSL paths, and the wheel hash is pinned for Linux x86_64 only (C:\Users\alber\Downloads\Hiby\hibymod\tools\wsl\fetch.sh, zbuild.sh, zbuild_shell.sh)
- make_payload.py needs the user's unpacked stock rootfs (HIBY_STOCK_ROOT) and WSL-built bin/ (C:\Users\alber\Downloads\Hiby\hibymod\docs\INSTALLER.md 'Building the payload')
- The hiby-mods tooling is MIT (noisetta), but the modified firmware binaries in that repo are HiBy's proprietary files and not MIT (research\hibymods\hiby-mods\LICENSE, NOTICE.md)
- hiby-mods Update_Database.py rebuilds usrlocal_media.db on the PC (MEDIA/ARTIST/ALBUM/GENRE/SEARCH tables, paths 'a:\\...', format codes, sort keys, 360x360 art embedding); it requires mutagen (GPL-2.0) and optional tinytag (MIT) and Pillow; it was tested on the R3 Pro II (research\hibymods\hiby-mods\tools\Update_Database.py lines 4-22, 57-111, 126-129, 851-1060; 'Update Database README.md'; README.md 'Tested On')
- On the R3 II the player DB is internal at /usr/data/usrlocal_media.db; tf_music_db_enable would move it to /data/mnt/sd_0/.temp/ (C:\Users\alber\Downloads\Hiby\hibymod\src\music.inc.c:34; docs\research_hidden_firmware.md:291)
- Rust libraries and their licences: lofty 0.25.5 MIT OR Apache-2.0; rusqlite 0.42 MIT; ed25519-dalek BSD-3-Clause; adb_client 3.2.3 MIT; zip MIT; symphonia MPL-2.0 (crates.io API, queried 2026-11-12)
- Frameworks and their licences: Tauri Apache-2.0 (also MIT); Electron MIT; Compose Multiplatform Apache-2.0; Flutter BSD-3; Avalonia and .NET MAUI MIT (GitHub REST API)
- ByOS today uses a custom non-commercial licence that is incompatible with GPL; the licence research recommends GPL-3.0-or-later for ByOS code (C:\Users\alber\Downloads\Hiby\hibymod\docs\research_license.md lines 29-98)

### Recommendations
- Base the desktop app on a fork of Museeks (MIT): Tauri 2 + React/TS UI reskinned in iTunes/iOS style, with the Rust core (lofty, sqlx) kept for library, tags, artwork and playlists. Add smart playlists (rule table in SQLite) and a Devices sidebar section. Keep the MIT notice in THIRD_PARTY_NOTICES.md.
- Do not write a third patcher. Compile android/hibyos-install/core as a CLI sidecar (GraalVM native-image, or a jlink'd JRE) and call it from Tauri the way Compás calls compas-sdtool. The PyInstaller ByOS-Patcher.exe is the fallback. Grow the shared Kotlin core into the one device/sync library for both the Android and desktop apps.
- End-user 'build': download the signed bundle index plus bundle.zip (verify Ed25519 and sha256), get the official r3ii_2025.upt from the HiBy links in bundle.json (verify md5/sha256), patch locally, never upload anything.
- Developer 'build from source' with no WSL: use native Windows zig 0.17.1 (pin the Windows wheel hash separately), port tools/wsl/*.sh fetch/zbuild to a Python build driver, extract the stock rootfs with the patcher's own SquashFS reader for make_payload.py, then run export_bundle.py and the patcher.
- Transport priority: (1) USB mass storage when the SD is mounted (fastest; write to a temp name, fsync, rename, read back sha256); (2) ADB over USB (bundle platform-tools, which is Apache-2.0, or try the MIT adb_client crate; ByOS adbd is old with no exec-out, so test); (3) the new secure ByOS Wi-Fi protocol from the protocol research task; (4) the stock 4399 HTTP server for first installs.
- Hands-free install: after push and verify, run 'hibymod reinstall --yes' (bootmode.sh Recovery + reboot), poll ADB/USB, confirm by /etc/hibymod_build and version. For payload-only updates, deliver the signed OTA locally (no recovery). For a stock player without ADB, guide the user through Via SD-card with screenshots.
- Library DB on the PC: port the MIT schema logic of hiby-mods Update_Database.py to Rust (lofty + rusqlite) rather than shipping Python with mutagen (GPL-2.0). Cross-check against ByOS src/hibytag.c and a real R3 II 2025 DB copy (MEDIA_TABLE, ARTIST2/ALBUM2, list_tb_*, MANAGER_TABLE, HISTORY_TABLE). Preserve play history, playlists and queue tables when merging instead of deleting all of them.
- Add an on-device import step to ByOS: a signed or hashed DB staged on the SD (ByOS/library/usrlocal_media.db + manifest), applied while hiby_player is stopped or at boot, with PRAGMA integrity_check and a backup kept for rollback. Never overwrite /usr/data/usrlocal_media.db live.
- Do not copy code from Compás Tools while ByOS keeps the non-commercial licence; reuse only facts (the mtd5 'ota:kernel2' contract, wait/confirm logic, the sdtool idea). If the owner adopts GPL-3.0-or-later (docs/research_license.md), re-evaluate forking Compás Tools directly.
- Runner-up stack if one language with the Android app matters more than UI polish: Compose Multiplatform Desktop with the existing Kotlin core, mp3agic/jaudiotagger (LGPL) for tags, and playback via a minimal native decoder. Avoid vlcj, which is GPL.

### Open questions
- Does 'bootmode.sh Recovery; sync; reboot' really enter recovery on ByOS, given that plain reboot powers the player off (README line 528)? Verify off-device by reading the recovery and boot code, then on-device only with the user's approval.
- Does the stock R3 II 2025 firmware v1.3 offer ADB at all (Compás asks users to 'turn on USB debugging' on stock), or is the first install mass-storage-only?
- Will the MIT adb_client crate work with ByOS's old adbd (no exec-out), or must the app bundle platform-tools adb.exe (about 15 MB)?
- Is the R3 II 2025 usrlocal_media.db schema identical to the R3 Pro II schema that hiby-mods targets? Which tables (play counts, playlists list_tb_*, MANAGER_TABLE, HISTORY_TABLE, queue list_tb_1) must be preserved on a PC rebuild?
- Can hiby_player be stopped safely to swap the DB, or must the swap happen at boot before hiby_player starts? How does the 50,000-track limit noted by hiby-mods apply?
- GraalVM native-image versus jlink JRE for the Kotlin patcher sidecar: binary size, and reflection config for java.util.zip/MessageDigest. Needs a quick prototype.
- Will the owner relicense ByOS to GPL-3.0-or-later? That changes whether Compás Tools can be forked instead of Museeks.
- How should the Windows zig wheel hash be pinned, and do Windows-built MIPS binaries need to be byte-identical to the published Linux builds (reproducibility claim)?

## ByOS internals relevant to a new transfer daemon, the Android app and a desktop manager (read-only map of C:\Users\alber\Downloads\Hiby\hibymod)

(a) ANDROID APP "ByOS Install" (C:\Users\alber\Downloads\Hiby\hibymod\android\hibyos-install). It has two Gradle modules. `core` is pure JVM, about 4.3k lines: Bundle, Patcher, SquashFs, Lzo1x, Iso, Upt, Ed25519, Download, Discovery, PlayerClient, Sender, Json, Io. `app` is Jetpack Compose, about 2.7k lines: MainActivity, InstallViewModel, UiState, Deliver, Notices, ui/Screens, Components, Theme, Glyphs. Screens (enum Screen): HOME, CHECK, FIRMWARE, PATCH, SEND, WIFI, MANUAL, INSTALL, ABOUT, LICENSES, LICENSE_TEXT.
- How it works: the app downloads HiBy's official v1.3 .upt over HTTPS (SHA-256 pinned), or takes a file the user picks. It patches the firmware on the phone; the output is byte-identical to tools/hibyos_patcher.py. It then sends r3ii_2025.upt plus the ByOS/ folder to the SD card.
- How it finds and talks to the player: there is NO ADB, NO USB OTG and NO ByOS-side server. It only speaks HiBy's STOCK "Import Music via Wi-Fi" HTTP server, port 4399, which exists only while that stock screen is open. Endpoints: GET /hostname, GET /list, POST /create, multipart POST /upload, POST /delete, POST /move, GET /download.
- The client is a hand-written HTTP/1.1 client over raw sockets (PlayerClient.kt). Discovery.kt does a TCP connect sweep of the phone's /24 (prefix clamped 24-30) on port 4399: 32 threads, 900 ms timeout, then /hostname plus a listing to identify the player.
- Sender.kt creates folders, skips files already present with the same content (so it can resume), uploads, checks the size in /list, then reads every file back through /download and compares SHA-256.
- Fallbacks: "Manual" writes through the Storage Access Framework (SAF) to a folder the user picks, with SHA read-back (Deliver.kt), or zips the files for Share.
- Permissions are only INTERNET and ACCESS_NETWORK_STATE. usesCleartextTraffic=false, but that setting does not apply to raw sockets.
- Updates of the bundle come from GitHub releases through an Ed25519-signed index (Download.kt, Bundle.kt).
- Tests: core tests are TransferTest (against MockPlayer: drops, stalls, full card, cancel, unreadable file), DownloadTest, Ed25519Test, IsoTest, JsonTest, Lzo1xTest, PatcherTest, SquashFsTest, and RealFirmwareTest (byte equality with the Python patcher). ScreensTest renders every screen with Roborazzi into docs/screens.
- Security of the stock channel: none. No auth and no encryption; anyone on the LAN can list, delete or download while that screen is open. Throughput was never measured.

(b) DEVICE-SIDE CHANNELS TODAY
1. ADB (overlay/usr/bin/hibymod_adb.sh off|wifi|usb|status; hibymod_adbwifi.sh).
   - Only one adbd at a time. "wifi" mode: TCP 5555, root, retried every 10 s until wlan0 has an IP; the address is written to SD/hibymod/adb_wifi.txt.
   - "usb" mode: adds ffs.adb (functionfs) to the configfs gadget android0 next to mass_storage.0. It has a 90 s trial: if no USB session starts, it falls back to Wi-Fi.
   - The mode is kept in /data/.hibymod_adbmode. It can also be enabled with the SD command file hibymod/ADB_WIFI, processed by etc/init.d/S95hibymod.
   - adbd is old: no exec-out. Measured speeds: Wi-Fi push about 0.3 MB/s, USB ADB about 4.9 MB/s (handoff.md).
2. USB mass storage (stock usbmode param 0x274: 0=DAC, 1=storage, 2=Dock). The PC gets the SD as a block device, so the player unmounts it. The empty mount point is then internal storage (src/hibyguard.c sd_mounted). Effects:
   - the library disappears from the player;
   - the installer and OTA abort and retry later (hibyos_install.sh fail(), docs/OTA.md);
   - booting with the cable plugged in makes the player start in storage mode.
3. No USB networking: no RNDIS, ECM or NCM function exists (memory notes, verified on device). functionfs works (tools/device/ffstest*.sh), so a custom USB bulk function is possible.
4. No ByOS network server exists. Every listen() in src/ is either loopback-only debug (hibyctl recnet/fbrec, used through adb forward), a test sink (hibyctl tcpsink, all interfaces), or the UNIX socket between hibymod_disp.so and the shell. hibynet is a CLIENT only: static, mbedTLS 2.28, about 570 KB plus a 200 KB cacert, VmHWM 0.6-1.5 MB on the device. It handles lyrics, covers, get with Range resume, and radio.
5. Store and OTA (docs/STORE.md, docs/OTA.md, src/ota_net.inc.c):
   - HTTPS pull from GitHub: hibyos-update.json is Ed25519-signed (OTA_PUBKEY_HEX plus a rotation slot; still all zeros in the repo, so OTA is disabled).
   - The payload is a plain ustar tar checked by sha256, extracted by a strict extractor to SD/ByOS/ota/stage, swapped into SD/ByOS/payload, then installed with `hibyos_install.sh install`. That step checks MANIFEST.md5 and writes `.new` files then renames them; it supports rollback.
   - Apps are .hpk files from repositories or from SD/Apps.
6. SD command files read at boot by S95: REINSTALL/ROLLBACK, SAFE, NORMAL, ADB_WIFI, NO_SHELL/SHELL_ON, NO_LAUNCHER/LAUNCHER_ON. Late install comes from SD/ByOS/payload.

(c) MUSIC LIBRARY
- The stock DB is SQLite at /usr/data/usrlocal_media.db; with tf_music_db_enable=1 it lives at SD/.temp/usrlocal_media.db instead.
- Tables: MEDIA_TABLE, MEDIA2_TABLE, HISTORY_TABLE, list_tb_N, ALBUM_TABLE, the artist/genre aggregate tables, M3U_TABLE. The column list is in tests/tags_test.c:543.
- Stored values: paths are `a:\Artist\Album\x.flac`; strings are stored WITH their trailing NUL; `character` holds the upper-case initial; pinyin_charater is the upper-cased name.
- hibymix (src/hibymix.c) reads MEDIA_TABLE and HISTORY_TABLE. For mixes it writes SD/Mezclas/*.m3u plus an M3U_TABLE row, without a rescan. It also has read-only lib, playlists and playlist dumps for the Music app.
- hibytag (src/hibytag.c):
  - writes tags safely (in place or temp file + rename);
  - setcover;
  - rename/renamedir plus DB path updates;
  - `dbupdate` UPDATEs only rows that already exist (BEGIN IMMEDIATE, busy_timeout 5 s) and refreshes the aggregate tables.
  - It never INSERTs, so new files appear only after the stock player rescans: Settings > update media library, or a long-press on Music (not traced).
- playctl.inc.c drives playback by injecting evdev keys, plus pc_list_send through hibymod_disp.so command 'L' (unscanned files allowed).
- Covers: stock only shows EMBEDDED art (FLAC PICTURE, ID3 APIC, M4A covr). Its caches are SD/list_cover/ and SD/.temp/image_cache.db. ByOS also writes cover.jpg plus cover.thumb.raw (96x96 RGB565). Lyrics are a .lrc next to the track (UTF-8 with BOM), read at play time, so no rescan is needed.

(d) CONSTRAINTS
- CPU: Ingenic X1600 at 1104 MHz, single core, no cpufreq.
- RAM: 57 MB, no swap or zram. hiby_player uses about 15 MB RSS; MemAvailable is about 7-12 MB with the player running.
- hibyguard (mlocked, real-time) acts when MemAvailable drops to 6/5/3/2 MB:
  - step 1 kills the helpers hibywall, hibytag and hibymix, and hibynet unless oom_adj < -500;
  - later steps restart or kill the shell.
- Storage: rootfs about 0.5 MB free, reflash only. /usr/data (UBIFS) about 24 MB free. The SD is large.
- OTA-updatable: payload bin/, shell, sys/ overrides. NOT updatable: S95hibymod, hibymod_adb*.sh, hiby_player.sh (hook not yet active).
- Restarting hiby_player without a reboot freezes the UI, so the DB cannot be hot-swapped under the running player.
- A resident daemon must be static mipsel-musl (zig), idle in poll() with no timers. The target is under 300 KB RSS idle and under 1.5 MB during a transfer (64 KB buffers, streaming SHA-256, no zlib), with nice/ionice idle so audio never starves.

WHERE THE DAEMON SHOULD LIVE
- What it is: a new static binary `hibysync`, shipped in the payload at /usr/data/hibymod/bin, so OTA and the installer update it.
- Who starts it: hibyshell starts it on demand (updatable), not S95 (frozen). Triggers: the user enables "Connect to computer/phone", a USB cable is attached in a new "ByOS Sync" usb mode, or the device is paired.
- Guard and OOM: add it to hibyguard's HELPERS list, but protect it while it holds a transfer lock (oom_adj -700 like the radio). In storage mode it must refuse SD operations.
- Wire format: one framed binary protocol over any byte stream. Requests: HELLO/PAIR, STAT/DF, LIST, PUT (chunked, resumable offset, sha256 then fsync + rename from .part with O_EXCL), GET (sendfile), MKDIR/MOVE/DELETE, DB_SNAPSHOT (consistent copy using the sqlite backup API), LIB ops (wrappers over hibytag dbupdate/rename/setcover and the hibymix m3u), STAGE_UPDATE plus INSTALL, and REBOOT_SAFE (hibyguard reboot).
- Install rule: an update is accepted only if the signed hibyos-update.json / MANIFEST verifies with the compiled Ed25519 key, exactly like OTA. A paired client never gets a raw root shell.
- Transports:
  1. Wi-Fi TCP. Discovery by UDP broadcast/mDNS instead of a /24 port sweep. Pairing by a QR code (src/qr.inc.c exists) carrying a one-time key. Encryption is X25519 + ChaCha20-Poly1305, or mbedTLS TLS-PSK, which is already linked in hibynet.
  2. USB, plugged in. Either the daemon listens only on loopback and the desktop app uses `adb forward` over USB functionfs (works today, 4.9 MB/s, no driver), or, better, its own second functionfs bulk function (e.g. ffs.byos with MS OS 2.0 descriptors, so WinUSB needs no driver and Android's UsbManager can do bulk I/O with no root). Either way the SD stays mounted on the player, unlike mass storage.
  3. Keep the stock 4399 HTTP client in the Android app for first install, before ByOS exists.
- Library from the PC: the desktop app pulls a DB snapshot, edits or rebuilds it on the PC, and pushes it to SD/ByOS/db/staged.db. The swap happens at the next boot in `hibyos_install.sh late` (updatable through sys/), keeping HISTORY_TABLE, list_tb_* and M3U_TABLE, and keeping the stock column conventions: NUL-terminated strings, a:\ paths, character, pinyin.

**Licence:** Repo LICENSE (the 'ByOS Community License 1.0', C:\Users\alber\Downloads\Hiby\hibymod\LICENSE): free use, modification and sharing for NON-COMMERCIAL purposes only. Shared and modified copies must keep the same licence and say who modified them. Selling the software, bundling it with something sold, or charging for installation needs a separate commercial licence from Grohle. Donations are allowed. Third-party parts keep their own licences: stb (public domain/MIT), SQLite (public domain), IJG libjpeg, mbedTLS (Apache-2.0, in hibynet), TweetNaCl-derived Ed25519 (public domain). Inconsistency: android/hibyos-install/README.md declares 'GPL-3.0-or-later, like the rest of ByOS', which contradicts the LICENSE. docs/research_license.md recommends relicensing to a layered standard scheme and flags the GPL-2.0 NetSurf binary under a non-commercial blanket. What this means for new apps:
- New ByOS daemon and apps: they can be under the ByOS licence. Any third-party code pulled in must allow that.
- MIT, BSD and Apache-2.0 code can be embedded (keep the notices).
- GPL code (for example umtprd as an MTP responder, GPL-3) cannot be combined under a non-commercial restriction without breaking the GPL. It would need to be a separate GPL program, or the relicensing decision must be settled first.

### Facts
- The Android app only talks to HiBy's stock 'Import Music via Wi-Fi' HTTP server on port 4399 (no ADB, no USB OTG, no ByOS server). The endpoints are /hostname, /list, /create, /upload (multipart), /delete, /move, /download. (C:\Users\alber\Downloads\Hiby\hibymod\android\hibyos-install\core\src\main\kotlin\org\hibyos\install\core\PlayerClient.kt lines 17-27, 90-91 (DEFAULT_PORT=4399, DEFAULT_HOME=/data/mnt/sd_0/))
- Discovery is a TCP connect sweep of the phone's /24 on port 4399 (32 threads, 900 ms timeout), then identify() via /hostname and an SD listing. (android\hibyos-install\core\src\main\kotlin\org\hibyos\install\core\Discovery.kt; app\...\Deliver.kt object Net (sockets bound to the Wi-Fi Network, hotspot fallback))
- Sender verifies every file: it skips identical files, checks the size in /list, reads the file back via /download and compares SHA-256. The manual path writes through SAF with read-back, or zips for Share. (core\...\Sender.kt header comment; app\...\Deliver.kt saveTree/zip)
- App screens: HOME, CHECK, FIRMWARE, PATCH, SEND, WIFI, MANUAL, INSTALL, ABOUT, LICENSES, LICENSE_TEXT. Tests: core TransferTest with MockPlayer, Download, Ed25519, Iso, Json, Lzo1x, Patcher, SquashFs, RealFirmware; app ScreensTest (Roborazzi). (app\src\main\kotlin\org\hibyos\install\UiState.kt:6; core\src\test\kotlin\...; README.md 'Build from source')
- Licence conflict: the Android README says 'GPL-3.0-or-later, like the rest of ByOS', but the repository LICENSE is the 'ByOS Community License 1.0' (non-commercial; commercial use needs the author's licence). docs/research_license.md also flags a non-commercial blanket over GPL-2.0 code (NetSurf). (android\hibyos-install\README.md 'Licence' section; C:\Users\alber\Downloads\Hiby\hibymod\LICENSE lines 1-18; docs\research_license.md lines 29, 157)
- ADB: only one adbd instance. Wi-Fi mode is TCP 5555 (root). USB mode adds ffs.adb functionfs to configfs gadget android0 next to mass_storage.0, with a 90 s trial and fallback to Wi-Fi. The mode file is /data/.hibymod_adbmode. (overlay\usr\bin\hibymod_adb.sh; overlay\usr\bin\hibymod_adbwifi.sh; docs\research_overclock_power.md:266)
- Measured transfer speeds: adb push over Wi-Fi about 0.3 MB/s; USB ADB about 4.9 MB/s. adbd is too old for exec-out. (memory note hiby-r3ii-mod-project.md (install flow); C:\Users\alber\Downloads\Hiby\handoff.md line 48)
- No USB network gadget function (RNDIS/ECM/NCM) exists; functionfs works, so a custom USB bulk function is feasible. (memory note hiby-r3ii-mod-project.md ('No USB-net gadget functions exist. functionfs works'); tools\device\ffstest.sh)
- USB mass storage mode exports the SD and unmounts it from the player. ByOS code treats this as 'SD gone' (guard, installer retry, OTA abort/resume). Booting with a PC cable plugged in starts the player in storage mode. (src\hibyguard.c:96-101; overlay\usr\bin\hibyos_install.sh:303-307; docs\OTA.md:200,327; handoff.md line 19; src\files.inc.c:180)
- No ByOS process serves the LAN. listen() exists only in hibyctl (recnet/fbrec bound to loopback; a tcpsink bandwidth test) and in hibymod_disp.so (a UNIX socket to the shell). hibynet is an HTTPS client only. (src\hibyctl.c:399,447,590; src\hibymod_disp.c:551; src\hibynet.c header)
- OTA trust: hibyos-update.json is Ed25519-signed (key compiled into hibyshell, rotation slot; currently all zeros, so OTA is disabled). The payload tar is checked by size + sha256, extracted by a strict extractor and installed by hibyos_install.sh with MANIFEST.md5, writing .new files and renaming them, with rollback. (docs\OTA.md sections 2-4; src\ota_net.inc.c)
- OTA-updatable: payload bin/ (hibyshell, hibynet, hibytag, hibymix, ...) and hibyos_install.sh via sys/. NOT updatable: S95hibymod, hibymod_adb*.sh, hibymod CLI; hiby_player.sh is not yet updatable (its hook is not applied). (docs\OTA.md table in section 1)
- The music DB is SQLite at /usr/data/usrlocal_media.db (at SD/.temp/usrlocal_media.db if tf_music_db_enable=1). Paths are stored as a:\... and strings include a trailing NUL. (src\hibymix.c:19-21,40; src\hibytag.c:2251-2270; docs\research_hidden_firmware.md:291-292; tests\tags_test.c:543-551 (schema))
- hibytag dbupdate only UPDATEs existing rows (BEGIN IMMEDIATE, busy_timeout 5000) and refreshes the aggregate tables. New files need a stock rescan. (src\hibytag.c ht_db_update ~2458-2530; docs\FREEMUSIC.md:252-255)
- The stock player shows only embedded covers. Its caches are SD/list_cover/ and SD/.temp/image_cache.db. Lyrics are a .lrc next to the track and are read at play time (lrc_path is empty in the DB). (docs\LYRICS_AND_COVERS.md 'Where to save so the stock player uses it')
- Restarting hiby_player without a reboot freezes the UI, so the DB cannot be swapped under the running player; a reboot is needed. (memory note hiby-r3ii-mod-project.md Findings)
- Resource limits: X1600 at 1104 MHz single core; 57 MB RAM with no swap; hiby_player about 15 MB RSS; /usr/data about 24 MB free; rootfs about 0.5 MB free. hibyguard steps in at MemAvailable 6/5/3/2 MB and first kills hibywall/hibytag/hibymix/hibynet. (docs\research_overclock_power.md:13,56; src\hibyguard.c:1-30,68-71,185-191; docs\STORE.md:164)
- hibynet's measured footprint gives a budget for a crypto-capable static helper: 570 KB binary, VmHWM 0.6-1.5 MB on the device. (docs\LYRICS_AND_COVERS.md 'Measurements')
- A QR encoder already exists in the shell (usable for pairing). (src\qr.inc.c; src\support.inc.c:4)

### Recommendations
- Build a new static mipsel-musl daemon 'hibysync' (zig, like hibyctl and hibynet) in the payload at /usr/data/hibymod/bin so OTA updates it. hibyshell starts it on demand (Connect to computer/phone, or USB cable in a new 'ByOS Sync' usb mode); never start it from S95, which is frozen. Idle in poll(): no timers, under 300 KB RSS. Under 1.5 MB peak, 64 KB buffers, nice 19 and idle I/O class during transfers.
- Use one framed binary protocol over any byte stream: HELLO/PAIR, DF/STAT, LIST, PUT (resumable offset, streaming SHA-256, write to .part with O_EXCL then fsync and rename), GET (sendfile), MKDIR/MOVE/DELETE, DB_SNAPSHOT (sqlite3 backup API), LIB ops wrapping hibytag dbupdate/rename/setcover and the hibymix M3U_TABLE insert, STAGE_UPDATE + INSTALL, and SAFE_REBOOT through 'hibyguard reboot'. Never expose a shell.
- Updates through the daemon must pass the same Ed25519 check as OTA (signed hibyos-update.json or signed MANIFEST), then go through the existing SD/ByOS/ota/stage -> payload swap -> 'hibyos_install.sh install' path with rollback. Do not invent a second installer.
- Wi-Fi transport: replace the /24 port sweep with UDP broadcast or mDNS announce/discovery. Pair through a QR code shown by the shell (src/qr.inc.c) carrying a one-time key. Encrypt with X25519 + ChaCha20-Poly1305, or TLS-PSK with the mbedTLS already in hibynet. Store paired clients in /usr/data/hibymod/sync/.
- USB transport (desktop and Android OTG): prefer a second functionfs function (e.g. ffs.byos with MS OS 2.0 descriptors) running the same protocol. Windows then binds WinUSB with no driver, and Android UsbManager can do bulk I/O with no root. The SD stays mounted on the player, unlike mass storage. Interim path: daemon on loopback plus 'adb forward' over USB ADB (works today, about 4.9 MB/s). First verify on a device that ffs.adb and a second ffs instance can coexist in configfs android0, and how removing mass_storage.0 from c.1 interacts with the stock usbmode switch.
- Keep the stock 4399 HTTP client in the Android app as the first-install path (before ByOS exists), and add a 'ByOS connected' mode using the new protocol for any-file transfer, library browsing and updates. Reuse core's Sender verify/skip logic as the model.
- PC-side library: pull a DB snapshot; on the PC, tag files, embed covers (the stock player shows only embedded art), write .lrc, precompute cover.jpg and cover.thumb.raw, and build or merge rows following the stock conventions (a:\ paths, NUL-terminated strings, character/pinyin_charater columns, aggregate tables). Push to SD/ByOS/db/staged.db. Swap it at the next boot in 'hibyos_install.sh late' (updatable via sys/), preserving HISTORY_TABLE, list_tb_* and M3U_TABLE, because the DB cannot be hot-swapped under hiby_player.
- Add hibysync to hibyguard's helper list, but give it OOM protection (oom_adj around -700, like the radio) only while a transfer lock is held. It must refuse SD operations when /usr/data/mnt/sd_0 is not really mounted (USB storage mode).
- Fix the licence statement in android/hibyos-install/README.md (GPL-3.0-or-later vs the ByOS Community License) before adding new apps, and avoid GPL components (for example umtprd) unless they ship as separate GPL programs.

### Open questions
- Does the old adbd require RSA authorisation, or is it open to anyone on the LAN on port 5555? Not verified in the repo.
- Can a second functionfs instance coexist with ffs.adb and mass_storage.0 in configfs android0 on this 4.4 kernel, and does the stock player's usbmode handling rewrite the gadget config when a cable is attached?
- How does the stock player trigger a library rescan (the settings item and the long-press Music path were not traced), and can it be triggered programmatically through hibymod_disp.so?
- Does the stock player accept a PC-generated usrlocal_media.db (are there hidden version columns or indexes, ctime semantics, MEDIA2_TABLE usage), and does it rebuild the list_cover/ and image_cache.db caches on its own?
- What is the real throughput of the 4399 stock server and of plain TCP over the device's Wi-Fi (hibyctl tcpsink could measure it)? This sets the target for the new protocol.
- Is fsync on the SD (vfat/exfat) honoured fast enough for per-file atomic writes with large libraries?

## Completeness critique of the four ByOS transfer, Android app and desktop app research reports: corrected facts, verified claims and design decisions

I checked the four reports against the cloned repos in C:\Users\alber\AppData\Local\Temp\claude\960b068394fc6611\d6a53856-6670-425f-991a-838192e7001d\scratchpad\research, the ByOS tree (read only) and a read-only copy of our device DB (C:\Users\alber\Downloads\Hiby\hibymod\backups\usrlocal_media_0644.db). No device was touched.

Licences: confirmed. compas-player, compas-tools and compas-firmware ship the GPL-3.0 text. compas-tools' Cargo.toml says license = "GPL-3.0-only". compas-player also contains a few GPL-2.0-only files (the DST decoder, boot_trace). compas-app's licence is Expo's MIT template with "private": true, so treat it as unlicensed. hiby-mods has a scoped MIT licence covering the tooling only; its binaries are not licensed to us (NOTICE.md). Museeks is MIT, v0.23.4, with tauri 2.11.2, lofty 0.23.3, sqlx 0.8.6 and axum 0.8.9. Monocypher is CC0-1.0 or BSD-2-Clause, adb_client is MIT, dadb is Apache-2.0, and libadb-android is GPL-3.0-or-later OR Apache-2.0. The ByOS licence itself is inconsistent today: the root LICENSE is the non-commercial "ByOS Community License 1.0", but android/hibyos-install/README.md line 197 says "GPL-3.0-or-later, like the rest of ByOS". This has to be settled first, because it decides between forking Compás Tools (GPL) and Museeks (MIT), and whether mutagen can be used.

Corrections to the reports:
1. OTA is not disabled. src/ota_net.inc.c:41 holds a real OTA_PUBKEY_HEX (6d87851e...). docs/OTA.md:160 ("all zeros as in the repository today") is out of date, and report 4 repeated it.
2. Compás Tools' recovery install is not proven on hardware. Its own code says "Not confirmed on hardware yet; if it fails the user falls back to the key combination" (crates/compas-core/src/adb/install.rs:237). The reports present the checklist as proven.
3. Google's platform-tools binaries are not Apache-2.0 for redistribution. They come under the Android SDK licence terms, which is why Compás downloads them at run time (adb/locate.rs) and does not bundle them [K]. Report 3's "bundle platform-tools (Apache-2.0)" is wrong. Download them, or use adb_client (MIT).
4. "Swap the DB at the next boot in hibyos_install.sh late" does not work as stated. S95hibymod runs `late` after hiby_player has started: docs/INSTALLER.md:102 says the SD is mounted about 4 s after the player starts. The SD-staged DB can therefore only be applied under a running player, and the frozen hiby_player.sh hook is not active.
   - A better option that no report mentions: the OTA-updatable LD_PRELOAD shim (preload/hibymod_disp.so). Its constructor runs before the player's main(), so it can swap a DB staged in /usr/data before the player opens it. The SD is not mounted yet at that point, so the staged copy must already sit on /usr/data.
5. Space limit nobody checked. Our DB is 2,174,976 bytes for 2,488 tracks, about 0.87 KB per track. /usr/data has about 24 MB free, and a live DB plus a staged copy plus a .bak needs about 3x the DB size. That fails above roughly 9,000 tracks [I]. Large libraries need tf_music_db_enable (DB on the SD), and ByOS's hibymix (src/hibymix.c:40), hibytag (:2254) and music.inc.c (:34) all hard-code /usr/data/usrlocal_media.db and ignore that mode.
6. MEDIA2_TABLE, more precisely. It is a cache of the SD root folder: 608 rows, of which 81 are directories ("a:\X\*", has_child_file=3, cue_id=-2) and 527 are files at depth 1, and all 527 files are also in MEDIA_TABLE. hiby-mods deletes it and refills it with the whole library (Update_Database.py:1009).
7. SEARCH_TABLE is empty (0 rows) on our device. Report 2's advice to write SEARCH_TABLE is not what the stock player does.
8. MANAGER_TABLE ties to the queue. Its single row ('album_artist\ABBA', count 18) matches list_tb_1's 18 rows: it describes the source of the queue. The two tables must be kept together.
9. Other DB checks. FORMAT_TABLE holds (26,'FLAC'), (1,'MP3'), (11,'MQA'). Quality counts are '2' ×1899, '18' ×242, '3' ×294, '1' ×53. VERSION_TABLE is 20181015. The only non-auto index is charater_index on MEDIA_TABLE(character). The 50,000-track limit is confirmed in hiby-mods' README:44.
10. USB descriptors. Linux 4.4 configfs gadgets support only MS OS 1.0 descriptors (os_desc, plus FunctionFS MS OS descriptors), not MS OS 2.0 BOS descriptors [K]. WinUSB binding without a driver still works with an extended compat ID "WINUSB" on Windows 8 and later. The reports' "MS OS 2.0" should read MS OS 1.0.
11. Rebooting into recovery. ByOS's `hibymod reinstall --yes` already checks the recovery magics (kernel2 27051956 and rootfs2 68737173), which is stronger than Compas's /proc/mtd check. It lacks three things Compas has:
    - parking other .upt files (sd_upt picks update.upt, then r3ii_2025.upt, then any *.upt);
    - a check that the mtd5 flag was not already set before the install;
    - reading the flag back after setting it.
    It also ends with a plain `reboot`, and README.md (around line 528) says plain reboot powers this player off. S95 also uses plain reboot after `late`. Whether a reboot with the recovery flag set comes back on its own decides whether the install is really hands-free. `hibyguard reboot` (watchdog reset) may be the right trigger.
12. adbd security is unverified and matters more than the new protocol's crypto. ByOS runs a root adbd on TCP 5555 over Wi-Fi, and nothing in the repo shows whether it requires RSA authorisation. If it does not, anyone on the LAN gets a root shell, which defeats any encrypted BTP/1. This can be checked off-device by extracting adbd with the patcher's SquashFS reader and looking for adb_keys or auth strings.
13. Daemon size targets disagree: report 1 says about 2 MiB RSS, report 4 says under 300 KB idle and under 1.5 MB peak. The names (byosd vs hibysync) and the port (7340) also need one decision.
14. Existing crypto. ByOS already ships TweetNaCl (public domain) in src/ota_net.inc.c, and hibynet links mbedTLS. Adding Monocypher is a choice, not a given.
15. Shared core: the desktop reports contradict each other. Report 1 proposes a Rust crate shared with Android through JNI. Report 3 proposes reusing the Kotlin core as a GraalVM sidecar. Report 3 also contradicts itself: a Rust-only Museeks fork, yet the Kotlin core as "the one device/sync library".

Missing from the reports, compared with what the user asked:
- Library features: books and audiobooks (BOOK_TABLE, BOOK_COLLECT_TABLE, BOOKMARK_TABLE exist), downloading and placing lyrics (.lrc in UTF-8 with BOM, filling lrc_path), and downloading songs from the app.
- Android platform needs:
  - a foreground service of type dataSync for long transfers (FOREGROUND_SERVICE_DATA_SYNC on Android 14+);
  - a WifiManager MulticastLock (CHANGE_WIFI_MULTICAST_STATE) for UDP and mDNS discovery, and NsdManager for mDNS;
  - the usb.host feature and permission flow for OTG;
  - SAF tree picks for "any file";
  - the coming Android local-network permission [needs checking].
- Whether the stock player rescans on SD mount or boot and overwrites a PC-built DB. Still unanswered, and it decides the whole PC-side DB design.
- Mass storage: while the SD is exported the player shows no library, and booting with the cable in starts storage mode. The desktop app's "plugged in" experience has to handle switching modes.
- Interoperability tests for one protocol written in C, Kotlin and Rust. Shared test vectors are needed, in the style of android/hibyos-install/tools/gen_test_vectors.py.

**Licence:** ByOS root: ByOS Community License 1.0 (non-commercial); the Android README says GPL-3.0-or-later, an inconsistency. CompasOS player/firmware: GPL-3.0 (some GPL-2.0-only files). Compás Tools: GPL-3.0-only. compas-app: Expo MIT template plus private:true, so effectively unlicensed. hiby-mods: MIT for the tooling only; hiby_player binaries are proprietary. mutagen: GPL-2.0-or-later. Museeks: MIT. Monocypher: BSD-2-Clause or CC0-1.0. adb_client: MIT. dadb: Apache-2.0. libadb-android: GPL-3.0-or-later or Apache-2.0. TweetNaCl: public domain (already in ByOS). Google platform-tools binaries: Android SDK licence, do not bundle [K].

### Facts
- The ByOS root LICENSE is the non-commercial 'ByOS Community License 1.0', but the Android app README says the project is GPL-3.0-or-later. The two contradict each other. (C:\Users\alber\Downloads\Hiby\hibymod\LICENSE lines 1-20; C:\Users\alber\Downloads\Hiby\hibymod\android\hibyos-install\README.md:197; README.md:13 badge 'free non-commercial')
- The OTA signing key is set (not all zeros), so docs/OTA.md and report 4 are out of date. (C:\Users\alber\Downloads\Hiby\hibymod\src\ota_net.inc.c:41 OTA_PUBKEY_HEX "6d87851ece77..." vs docs\OTA.md:160)
- compas-tools is GPL-3.0-only. compas-player and compas-firmware carry the GPL-3.0 text, and compas-player also has GPL-2.0-only files (DST decoder, boot_trace). (research\desktop\compas-tools\Cargo.toml:8; research\compas\compas-player\src\audio\decoders\dst\dst_decoder.c SPDX)
- compas-app's LICENSE is Expo's MIT template ('650 Industries') and package.json has "private": true. Treat it as unlicensed. (research\compas\compas-app\LICENSE; package.json:45)
- Compás Tools' recovery install is not confirmed on hardware by its own author. (research\desktop\compas-tools\crates\compas-core\src\adb\install.rs:237 comment 'Not confirmed on hardware yet; if it fails the user falls back to the key combination')
- Compás Tools downloads Google platform-tools (about 15 MB) at run time rather than bundling them. Their redistribution terms are the Android SDK licence, not Apache-2.0 [K]. (research\desktop\compas-tools\crates\compas-core\src\adb\locate.rs:1-17,74)
- Museeks is MIT, v0.23.4 (last commit 2026-05-26), using tauri 2.11.2, lofty 0.23.3, sqlx 0.8.6 and axum 0.8.9. (research\desktop\museeks\package.json:3,8; src-tauri\Cargo.toml:8,16,36,44,52)
- Library licences: Monocypher is BSD-2-Clause or CC0, adb_client is MIT, dadb is Apache-2.0, and libadb-android is GPL-3.0-or-later or Apache-2.0 (it has an LGPL dependency). (gh api repos/*; Monocypher LICENCE.md; libadb-android README License section)
- hiby-mods' LICENSE is a scoped MIT that covers the tooling only. Update_Database.py imports mutagen (GPL-2.0-or-later). It deletes MEDIA2_TABLE and SEARCH_TABLE and refills them from the full library. The README notes a 50,000-track limit. (research\hibymods\hiby-mods\LICENSE; tools\Update_Database.py:22,1006-1015; README.md:44)
- On our R3 II 2025 DB, MEDIA2_TABLE caches the SD root folder only: 608 rows, 81 directory rows ('a:\\X\\*', has_child_file=3) and 527 files at depth 1, all 527 also present in MEDIA_TABLE. (Read-only sqlite query of C:\Users\alber\Downloads\Hiby\hibymod\backups\usrlocal_media_0644.db)
- SEARCH_TABLE is empty on the device DB. MANAGER_TABLE has one row ('album_artist\\ABBA', 18, 18, 0, 0) that matches list_tb_1's 18 queue rows. (same DB: SEARCH_TABLE 0 rows, MANAGER_TABLE and list_tb_1 counts)
- Device DB contents: FORMAT_TABLE = (26,FLAC,F,1), (1,MP3,M,1), (11,MQA,M,1); quality values 1/2/3/18; VERSION 20181015; only one custom index, charater_index; book tables exist but are empty. (same DB)
- The DB is about 0.87 KB per track (2,174,976 bytes for 2,488 tracks). With about 24 MB free on /usr/data, keeping a live, a staged and a backup copy fails above roughly 9,000 tracks [I]. (backups\usrlocal_media_0644.db size; report 4 constraint '/usr/data about 24 MB free')
- hibyos_install.sh 'late' runs from S95 after hiby_player has already started (the SD mounts about 4 s later), so a DB swap at that step happens under a running player. (overlay\etc\init.d\S95hibymod:60-65; docs\INSTALLER.md:102)
- The display shim hibymod_disp.so is OTA-updatable. As an LD_PRELOAD library, its constructor runs before the player's main(), so it could swap a DB staged on /usr/data before the player opens it. (docs\OTA.md:28; docs\INSTALLER.md:34,119)
- ByOS 'hibymod reinstall --yes' checks the recovery magics (kernel2/rootfs2) but does not park other .upt files, does not check or read back the mtd5 flag, and ends with a plain reboot. README says plain reboot powers this player off. (overlay\usr\bin\hibymod:30-46,65-71; README.md around line 528)
- ByOS already ships TweetNaCl (in OTA verification), and hibynet links mbedTLS. (src\ota_net.inc.c (tweetnacl reference); LICENSES\TweetNaCl-public-domain.txt)
- Several hard-coded DB paths ignore tf_music_db_enable, which is read only in Settings. (src\hibymix.c:40, src\hibytag.c:2254, src\music.inc.c:34; src\settings_app.inc.c:690)
- The Android app has only the INTERNET and ACCESS_NETWORK_STATE permissions; its core is 4,291 lines and speaks the stock HTTP server on port 4399. (android\hibyos-install\app\src\main\AndroidManifest.xml; core\...\PlayerClient.kt:90)

### Recommendations
- Decision 0 (blocks everything else): fix the licence. The root LICENSE (non-commercial) and the Android README (GPL-3.0-or-later) contradict each other. If ByOS goes GPL-3.0-or-later, forking Compás Tools and using mutagen become possible. If not, use a Museeks (MIT) base, clean-room the Compas ideas, and port Update_Database.py logic with an MIT credit and no mutagen.
- Decision 1: check adbd authentication before designing any crypto. Extract adbd off-device with the patcher's SquashFS reader and look for adb_keys or auth strings. If it accepts unauthenticated root connections on Wi-Fi 5555, make Wi-Fi ADB off by default or USB-only, and let paired byosd/hibysync handle wireless transfers.
- Decision 2: pick one daemon name, port and budget. hibysync vs byosd; TCP 7340; idle under 300 KB and transfers under 1.5 MB (report 4) rather than about 2 MiB. It is started on demand by the shell, never by the frozen S95.
- Decision 3: crypto library. Use TweetNaCl (already shipped, slow), Monocypher (BSD-2/CC0, faster, new dependency) or mbedTLS TLS-PSK (already in hibynet). Measure XChaCha20-Poly1305 MB/s on the X1600 before choosing.
- Decision 4: USB transport. Use adb forward now. A second FunctionFS function can come later, with MS OS 1.0 descriptors (configfs os_desc plus an extended compat ID 'WINUSB'), not MS OS 2.0, which kernel 4.4 lacks [K]. Verify on a device that two ffs instances coexist in android0, and how the stock usbmode switch rewrites the gadget.
- Decision 5: shared core language. Either Rust (Museeks fork, Android through JNI) or Kotlin (the existing core as a GraalVM or jlink sidecar on desktop). Whichever is chosen, define BTP/1 with byte-level test vectors (like tools/gen_test_vectors.py) so the C, Kotlin and Rust implementations cannot drift.
- Decision 6: how the PC-built DB gets applied. The 'late' step runs under a live player, so it is wrong. Prefer staging on /usr/data and swapping in the shim constructor before main(), with integrity_check, a .bak copy and an atomic rename. Fall back to a rootfs hook after the next reinstall. Check that /usr/data has room for about three copies; otherwise require tf_music_db_enable and fix the hard-coded DB paths in hibymix, hibytag and music.inc.c.
- Decision 7: which tables a PC build rewrites and which it keeps. Keep HISTORY, COLLECT, COLLECT_OPERATE, PLAYLIST, M3U_TABLE (merge mixes), list_tb_* together with MANAGER_TABLE (they describe the queue as a pair), the BOOK* tables and VERSION. Rebuild MEDIA2_TABLE as a root-folder cache only (81 directories plus 527 root files, as on our DB), never the whole library. Leave SEARCH_TABLE empty as stock does. Write CTIME/MTIME/COUNT and FORMAT with MQA=quality '18'.
- Before trusting a PC-built DB, find out whether the stock player rescans on boot or SD mount and overwrites it. Do this off-device by reading the binary, then on-device only under the device rules.
- Hands-free install: add Compas's three missing checks to 'hibymod reinstall' (park other .upt files, require that the mtd5 flag was clear before, read the flag back). Then decide the reboot method: plain reboot (README says it powers the player off) or 'hibyguard reboot' (watchdog reset). Do not treat the Compas checklist as proven; it is unconfirmed on hardware by its own author.
- Do not bundle Google platform-tools (SDK licence). Download them on first use as Compás does, or use the MIT adb_client crate after testing it against our old adbd (no exec-out).
- Android app scope additions: a dataSync foreground service for long transfers, MulticastLock plus NsdManager for discovery, the usb.host feature with the permission flow for OTG bulk, SAF tree picks for any file, and checking Android's upcoming local-network permission. Keep the 4399 stock client for first installs.
- Cover the user's other requests in the library design: books and audiobooks (BOOK_* tables, folder conventions), lyrics (.lrc in UTF-8 with BOM next to the track, filling lrc_path, already read at play time), and song downloads going through the same verified PUT path.
- Fix docs/OTA.md:160, which says the key is all zeros, and make sure release builds use the intended key.

### Open questions
- Does ByOS's adbd require RSA authorisation on Wi-Fi 5555, or does it give root to anyone on the LAN?
- After bootmode.sh Recovery, does plain reboot (README says it powers the player off) still enter recovery on its own, or must 'hibyguard reboot' or the user's power button be used?
- Does the stock player rescan on boot or SD mount (based on mtimes) and overwrite a PC-built DB? Does it rebuild MEDIA2_TABLE if it is emptied?
- Can the shim constructor safely replace /usr/data/usrlocal_media.db before hiby_player opens it (timing, UBIFS free space)?
- Does kernel 4.4 configfs here support os_desc and a second FunctionFS function next to ffs.adb and mass_storage.0?
- Measured throughput: Wi-Fi TCP raw vs encrypted on the X1600, adb forward over USB, and the stock 4399 server.
- Does adb_client (Rust) work against our old adbd with no exec-out?
- Does Android's upcoming local-network permission affect LAN discovery and transfer for the target SDK?
- Does the 50,000-track limit apply to a PC-built DB?

