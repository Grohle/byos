# Free Music app

Search, listen to and download free / Creative Commons music on the HiBy, with the audio quality
shown in the search results themselves.

| | |
|---|---|
| Code | `src/freemusic.inc.c` (UI, threads), `src/freemusic_core.inc.c` (parsers, labels, URLs), `src/lang/freemusic_tr.inc.c` (13 languages) |
| Entry point | `freemusic_app_at(ix, iy)`; app id for the home gesture: `"freemusic"` |
| Tests | `bash tests/freemusic_test.sh` (parsers against recorded responses in `tests/freemusic_samples/` + texts/glyphs in the 13 languages) |
| Data | settings and the Jamendo key: `/usr/data/hibymod/appdata/freemusic/`; HTTP cache: `/usr/data/hibymod/cache/freemusic/` |
| Downloads | `SD/Music/Free/<Artist>/<Album>/NN - Title.ext` + `cover.jpg` |
| Listening cache | `SD/Music/Free/.cache` (LRU, 500 MB by default, 250 MB–4 GB in the settings) |

## Sources

### Internet Archive (no key)

- Search: `https://archive.org/advancedsearch.php` with
  `collection:(etree OR netlabels OR audio_music) AND mediatype:(audio OR etree) AND format:(...)`
  plus the filters (`format:(Flac OR "24bit Flac")` for *Lossless*, `subject:(...)` for the genre,
  `language:(...)` for the language) and the sort (`downloads desc`, `publicdate desc` or relevance).
  Note: Live Music Archive items have `mediatype:etree`, not `audio`.
- Album page: `https://archive.org/metadata/<id>/metadata` and `.../files` (the sub-paths: the full
  item can be 2 MB because of the reviews).
- Files: `https://archive.org/download/<id>/<file>` (302 to a `dnNNNN.*.archive.org` data server;
  hibynet follows it). `private: true` files are skipped; `stream_only` items still allow the MP3
  derivatives.
- Tracks are grouped by the original file (derivatives point at it with `original`) and by the
  name without extension (alternates uploaded side by side). Track order: the `track` numbers when
  they are all present and different, otherwise natural order of the names (`d1t2` < `d1t10`).

### Jamendo (needs a free key)

- Album search: `tracks/?groupby=album_id&include=musicinfo&audioformat=mp32&audiodlformat=flac`
  (+ `search`, `tags`, `lang`, `order`, `featured=1` for the featured list).
- Album page: `albums/tracks/?id=<album>&audioformat=mp32&audiodlformat=flac`.
- FLAC (`audiodownload`) only when `audiodownload_allowed` is true; the MP3 VBR stream (`audio`,
  `mp32`) is always offered for listening. If the artist does not allow downloads, the album page
  says "Listen only" and the Download button is disabled. Sizes are estimates (≈).
- Errors: `headers.status != "success"`; code 5 = invalid client id (recorded in
  `tests/freemusic_samples/jm_err_*.json`). The success samples `jm_*_doc.json` are synthetic, built
  from the documented schema; replace them with a real capture once a key is available:
  `curl "https://api.jamendo.com/v3.0/tracks/?client_id=KEY&format=json&limit=5&groupby=album_id&include=musicinfo&audioformat=mp32&audiodlformat=flac" > tests/freemusic_samples/jm_search_real.json`.

#### Jamendo: getting and entering the key (steps for the user)

1. On a computer or phone, open **https://devportal.jamendo.com**, sign up (free) and create an
   application ("My apps" > "Add a new app"; any name, e.g. "HiBy"). Jamendo shows its
   **Client ID** (8 hexadecimal characters, e.g. `a1b2c3d4`). The client secret is not needed.
2. Enter it in one of these ways:
   - In the app: **Free Music > ⋯ (top right) > JAMENDO > Key (client_id)**, type it with the
     keyboard and press OK. The app tests it right away ("Valid key" / "Invalid key" /
     "No connection"); **Test key** repeats the test, **Remove key** deletes it.
   - Without typing: create the text file **`HiByOS/jamendo_client_id.txt`** on the SD card with the
     Client ID on the first line. The app reads it when it opens and copies it to
     `/usr/data/hibymod/appdata/freemusic/jamendo_client_id` (so the SD file can be removed).
3. Choose **Jamendo** in the source chip (first chip under the search bar). Without a key the
   Jamendo source only shows a single "Add your free Jamendo key" row that opens the settings.

## Quality in the results

Each row shows badges computed without opening the album:

- From the search (format names of the item): `FLAC 24-bit` (gold = high resolution), `FLAC`
  (green), `MP3 320` / `MP3 VBR`, `OGG`.
- Lazily, only for the rows on screen (low-priority worker, cached in `cache/freemusic/probes.txt`):
  the files list gives the number of tracks, the total duration (shown after the year) and the size
  of the best format; and the first ~4 KB of the first lossless track give the FLAC STREAMINFO
  (sample rate, bits, channels), so the badge becomes e.g. `FLAC 16/44.1` or `FLAC 24/96`.
  hibynet has no "first N bytes" request: the worker runs `hibynet get` and kills it as soon as the
  `.part` file holds the STREAMINFO (ID3v2 in front of `fLaC` is skipped). A HEAD/Range command in
  hibynet (`get-range URL OUT 0-8191`) would save the extra kilobytes; not needed for correctness.

The album page has a segmented control with the formats it offers (e.g. `FLAC 24/96 | MP3 VBR | OGG`),
the number of tracks and total size of the chosen one, and per track: duration, quality and size.

## Downloads

- One file at a time, in a worker thread that lives for the whole shell session: downloads go on
  after leaving the app. nice 19 + idle I/O class (audio first). The queue is stored in
  `appdata/freemusic/queue.tsv` and resumed when the app is opened again (hibynet is started with
  `HIBYNET_RESUME=1`, so a hibynet with Range resume continues the `.part`; an older one restarts it).
- Free-space check before queueing an album (needs size + 16 MB) and before each file (+ 8 MB).
  More than 1 GB asks for confirmation.
- After each file: tags (title, artist, album, track, year) with `hibytag set` (`HIBYTAG_NODB=1`;
  FLAC and MP3). The album gets `cover.jpg` (the item's cover image, else its tile; re-encoded by
  `hibynet cover-get`).
- **Library**: our Music app reads the player's database (`usrlocal_media.db`), and `hibytag
  dbupdate` only updates rows that already exist, so new files appear once the HiBy rescans the
  SD (stock settings > update media library). The app says so in the Downloads page and settings.

## Listening

There is no way to stream a URL into the stock player (`research_playback.md`: DLNA `set_uri`
discarded). "Listen" (tap a track) downloads the track into `Music/Free/.cache` and, when the file
is complete, hands the whole album to the stock player with `pc_list_send(paths, n, index, title,
PC_UNSCANNED | 0xff)` (paths as `a:\Music\Free\...`; tracks already in the library use the library
file). The next track is fetched right away, and each following one about a minute before the
player needs it (from the track durations). The cache is LRU-trimmed to the limit before each new
track (the tracks of the current session are never evicted).

**Needs device verification**: whether the player accepts unscanned files (`unscanned=1`, format
from the extension) and what it does when the queue reaches a track that is not cached yet
(skip, stop or error). The UI shows a ring while the track is cached and animated bars on the
track that is playing (from `np_get`).

## Licences

The album page shows the licence of the item (`licenseurl` on archive.org, `license_ccurl` on
Jamendo) as a short label (`CC BY-NC-SA 3.0`, `CC0 1.0`, `Public Domain Mark`) plus the URL and
the item page (`archive.org/details/<id>`, `jamendo.com/album/<id>`). Live Music Archive items have
no licence URL: the page shows the LMA terms (free to share, non-commercial). Items without a
licence say so and point to the item page.
