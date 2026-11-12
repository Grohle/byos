# Folder sync, smart library and backups — requirements

Requirements from the project owner (12 Nov 2026), for the desktop app (Windows) and the
Android app (ByOS Install, connected mode). This file records *what* is wanted; the design
lives in `docs/SYNC_LIBRARY_DESIGN.md`.

## 1. Linked folders and sync

- Choose, on the computer or the phone, which folder the player's **music** (and its library
  database) is synced with. The same for **video**, **audiobooks** and **ebooks**.
- A **Sync** button per kind (music, video, audiobooks, ebooks) and one for **everything**.
- When the player and the folder differ, the user chooses what to do with what is only on the
  player: **keep it** on the device or **delete it**. (Shown as a clear preview before anything
  changes.)
- The sync **builds the player's library database on the computer/phone** — the same file the
  database script produces (hiby-mods database manager, MIT) — and sends it, so the player does
  not have to scan.

## 2. The library on the computer

- The top part of the app (Music, Video, Audiobooks, Books...) shows **what is on the computer**
  (the linked folders), can **rebuild** it, and offers tools:
  - make **playlists**;
  - set **cover art** (find online, from file), **lyrics** (find, edit, synced .lrc);
  - **fix albums** (names, album artist, track numbers, multi-disc), **genres**, artists.
- **Smart metadata**: genres are merged and harmonised — similar names are grouped by
  similarity (spelling, case, accents, synonyms such as "Hip-Hop / Hip Hop / Rap") and can be
  checked online, so the library does not end up with many near-duplicate genres. The same
  agreement-by-similarity approach for album names, artist names and lyrics.
- Better **playlist generation** algorithms (e.g. by genre/mood/era, similar artists, recently
  added, most played on the player, smart rules).

## 3. Playlists both ways

- Playlists made in the app are sent to the player.
- Playlists made on the player are saved to the computer/phone.

## 4. Backup and restore of the player

- Back up all of the player's data: settings (ByOS and HiBy), playlists, library database,
  lyrics, bookmarks, downloads... with **toggles per kind** to leave out audio, images, etc.
- Restore the backup onto a reset player to get back all settings, playlists and so on.

## Constraints

- Music libraries can be large (tens of thousands of files) and the player has 57 MB of RAM:
  heavy work (tag reading, database building, matching) runs on the computer/phone.
- Online lookups must use services whose terms allow it (e.g. MusicBrainz, Cover Art Archive,
  LRCLIB), with polite rate limits and no personal data sent.
- New external code only with the owner's approval; MIT/BSD/Apache sources can be adapted with
  attribution, GPL code cannot be copied.
- English/French/Spanish UI text; iOS/iTunes-like design consistent with the existing apps.
