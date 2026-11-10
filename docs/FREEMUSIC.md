# Vinyl app

> **Name.** The app was called "Free Music" until it gained user sources; its name is now
> **Vinyl** in every language (a brand, not translated). The internal id stays `"freemusic"`
> (home gesture, launcher entry, `src/freemusic*.inc.c`, `appdata/freemusic`, this file's name).

Search, listen to and download free / Creative Commons music on the HiBy, with the audio quality
shown in the search results themselves. Besides the built-in Internet Archive and Jamendo, the
user adds their own **sources** by URL, like the App Store's repositories: archive.org
collections, **Vinyl catalogs** (an open JSON format anyone can host), podcast / netlabel feeds
(RSS, Atom, OPML) and Subsonic-API servers (Navidrome, Funkwhale, Airsonic, Gonic...).

| | |
|---|---|
| Code | `src/freemusic.inc.c` (UI, threads, sources), `src/freemusic_core.inc.c` (parsers, labels, URLs), `src/freemusic_src.inc.c` (user sources: detection, catalog, RSS/Atom/OPML, Subsonic, MD5), `src/lang/freemusic_tr.inc.c` (13 languages) |
| Entry point | `freemusic_app_at(ix, iy)`; app id for the home gesture: `"freemusic"` |
| Tests | `bash tests/freemusic_test.sh` (parsers against recorded responses and the example sources in `tests/freemusic_samples/` + texts/glyphs in the 13 languages); VM walkthrough: see "VM test" |
| Data | settings, sources and the Jamendo key: `/usr/data/hibymod/appdata/freemusic/` (`settings`, `sources.tsv`, `pw_<sid>` 0600, `jamendo_client_id`, `queue.tsv`); HTTP cache: `/usr/data/hibymod/cache/freemusic/` |
| Downloads | `SD/Music/Free/<Artist>/<Album>/NN - Title.ext` + `cover.jpg` |
| Listening cache | `SD/Music/Free/.cache` (LRU, 500 MB by default, 250 MB–4 GB in the settings) |

## User sources

### Where

- **Settings (⋯ top right) › SOURCES › Sources**: the list of sources, in order, each with an
  iOS switch, its kind (icon: IA, J, V = Vinyl catalog, feed arcs = RSS/OPML, S = Subsonic), its
  name, and a status line: number of entries and last refresh ("3 albums · 5 min ago"), or the
  problem in orange ("No connection", "Not found", "Not a music source", "Wrong user name or
  password", "Sign In…").
- **Add Source…** opens our keyboard (no automatic capitals: it is a URL field). What was typed
  is classified at once (`fm_src_detect`) and checked in the background (`NJ_SRCTEST`, a
  hibynet process): a spinner on the row, then the name is taken from the source itself (catalog
  `name`, feed `<title>`, OPML `<head><title>`, collection `title`) unless the user renamed it.
- **Tap a row**: action sheet with *Test Connection*, *Sign In…* (Subsonic), *Key (client_id)*
  (Jamendo), *Rename*, *Edit URL*, *Move Up*, *Move Down* and *Remove Source* (red, confirmed;
  downloaded albums are kept). The built-in Internet Archive and Jamendo can be reordered and
  switched off, not removed.
- **Pull down** (held past 64 px, as in the App Store) refreshes every enabled user source; on the
  start page it also refetches every list (catalogs and feeds bypass their one-hour cache).
- The **source chip** (first chip under the search bar) lists *All Sources* and every enabled
  source. *All Sources* (the default) shows, in the order of the list, the Internet Archive
  sections, the Jamendo ones (with a key) and one shelf per user source (12 entries, "See All"
  shows only that source); a search runs on every enabled source at once and merges the answers
  round-robin (page, position in each answer, source order), each row with a blue **source
  badge** (its name, max. 14 characters) after the quality badges. With one user source selected
  the start page is its list (40 per page, more when the end comes into view), filtered by the
  Lossless / Genre / Language chips and sorted by the sort chip where the source allows it.

### Kinds, detected from what the user types

| Typed | Kind | How it is read |
|---|---|---|
| `archive.org/details/<id>`, `collection:<id>`, a bare id (`monotonik`; an auto-capital first letter is lowered) | archive.org collection | the same advancedsearch as the built-in source with `collection:(<id>)` instead of the built-in collections; test = `/metadata/<id>/metadata` (must be `mediatype: collection`) + `rows=0` count |
| `jamendo`, `jamendo.com/...` | the built-in Jamendo (switched on; asks for the key if there is none) | — |
| `…/*.json` | Vinyl catalog | the whole file (cached 1 h), searched and filtered on the HiBy |
| `…/*.xml`, `*.rss`, `*.atom`, `/feed`, `/rss`, `/podcast` | RSS / Atom feed | the whole feed (cached 1 h) = one album |
| `…/*.opml` | OPML list | every `<outline xmlUrl>` = one album (its feed read lazily for the cover, artist, tracks and quality of the rows on screen) |
| `…/rest/...` | Subsonic | asks for user and password at once |
| any other URL (`http(s)://` optional; `https` is assumed) | sniffed after the first fetch (`fm_sniff`): JSON with `vinyl`/`albums` → catalog, `<rss>`/`<feed>`/`<rdf:RDF>` → feed, `<opml>` → OPML; a web page or nothing → `<url>/rest/ping.view` without a login: a Subsonic answer (code 10/40) makes it a Subsonic source and the app asks for user and password |

Source ids (`sid`) are the FNV-1a hash of the URL (`builtin:ia` / `builtin:jm` for the built-in
ones); items, probes, the download queue and the album keys carry it, so two catalogs can use the
same album ids. `sources.tsv`: `type on named auth items refreshed err name url user salt`, one
line per source, in order (no passwords).

### Vinyl catalog (open format, version 1)

A UTF-8 JSON file served over HTTPS (HTTP works too) from anywhere: a netlabel's site, a GitHub
Pages repo, an S3 bucket, a home server. Anyone can publish their own free-music repository
with it (netlabel releases, Bandcamp "name your price" albums they host themselves, field
recordings...). Relative URLs are resolved against the catalog's own URL, so a folder with the
JSON, the covers and the audio files can be uploaded as is.

```json
{
  "vinyl": 1,
  "name": "Example Netlabel",
  "description": "What this repository is.",
  "homepage": "https://netlabel.example.org/",
  "license": "https://creativecommons.org/licenses/by-sa/4.0/",
  "albums": [
    {
      "id": "exn001",
      "title": "Northern Lights",
      "artist": "Aurora Fields",
      "year": 2024,
      "license": "https://creativecommons.org/licenses/by/4.0/",
      "cover": "covers/exn001.jpg",
      "genre": ["ambient", "electronic"],
      "language": "en",
      "description": "Liner notes (HTML tags are removed).",
      "url": "https://netlabel.example.org/releases/exn001",
      "tracks": [
        {
          "title": "Polar Night",
          "artist": "Aurora Fields",
          "duration": 412.5,
          "track": 1,
          "files": [
            { "url": "audio/exn001/01.flac", "codec": "flac", "bits": 24, "rate": 96000, "channels": 2, "size": 98765432 },
            { "url": "audio/exn001/01.mp3",  "codec": "mp3",  "bitrate": 320, "size": 16500000 },
            { "url": "audio/exn001/01.ogg",  "codec": "ogg",  "size": 9000000 }
          ]
        }
      ]
    }
  ]
}
```

| Field | Required | Notes |
|---|---|---|
| `vinyl` | recommended | format version (1). A file without it is still a catalog if it has an `albums` (or `items`) array |
| `name`, `description`, `homepage` | no | `name` becomes the source's name; `homepage` is the album page link when an album has no `url` |
| `license` (or `licence`) | no | default licence URL of every album (Creative Commons URLs are shown as "CC BY-SA 4.0") |
| `albums[]` (or `items[]`) | yes | |
| `.id` | recommended | unique in the catalog; without it a hash of title + artist is used |
| `.title`, `.artist`, `.year` (or `.date`) | title | year: number or a date string (the first 4 digits) |
| `.license`, `.cover` (or `.image`), `.url` (or `.page`), `.description` | no | cover: JPEG or PNG |
| `.genre` / `.genres` / `.tags` | no | string or array; matched by the Genre chip (Jamendo tag names: `rock`, `jazz`, `electronic`, `classical`, `hiphop`, `ambient`, `world`, `soundtrack`...) |
| `.language` | no | ISO 639-1 (`en`, `es`, ...), matched by the Language chip |
| `.tracks[]` | yes | in order; `track` (or `no`) numbers reorder them when all are present and different |
| `.title`, `.artist`, `.duration` | title | duration: seconds (number) or `"m:ss"` / `"h:mm:ss"` |
| `.files[]` | yes | the formats of the track; a track may instead carry one file's fields itself (`url`, `codec`...) |
| `..url` | yes | absolute or relative |
| `..codec` (or `format` / `type`) | recommended | `flac`, `wav`, `mp3`, `ogg` (Vorbis), or a MIME type (`audio/flac`); without it the URL's extension. Other codecs (AAC, Opus, ALAC) are ignored: the HiBy plays them, but the app only offers these four |
| `..bits`, `..rate` | lossless | `rate` in Hz (`96000`) or kHz (`96`, `44.1`); give them and the badge says `FLAC 24/96` with no probing |
| `..bitrate`, `..vbr` | MP3 | kbps (`320`) or bit/s (`320000`); `vbr: true` → `MP3 VBR` |
| `..size` | recommended | bytes; without it the size is estimated from the duration (shown with "≈") |

The example catalog of the tests is `tests/freemusic_samples/vinyl_catalog.json` (relative and
absolute URLs, `../`, a track-level file, a 44.1 kHz rate written as `44.1`, an AAC-only album
that is skipped); `tests/freemusic_vm_repo.py` writes a complete one with real FLAC/WAV files and
covers.

### RSS / Atom / OPML feeds

- A **feed is one album**: the channel title, `itunes:author` (or Atom `author/name`,
  `dc:creator`, `managingEditor`), `itunes:image` (or `image/url`, `media:thumbnail`, Atom
  `logo`), `itunes:summary` / `description`, `link`, licence from `creativeCommons:license`,
  `cc:license`, `media:license` or `copyright`, year from the newest item. Each `<item>` /
  `<entry>` with audio is a track, in feed order (newest first for podcasts): its
  `<media:content>` (first: it says the bitrate), `<enclosure>` and Atom `link rel="enclosure"`
  are its formats (FLAC, WAV, MP3, Ogg Vorbis; same URL only once), `itunes:duration` or
  `media:content@duration` its length, `length`/`fileSize` its size (else an estimate). Netlabels
  that publish FLAC and MP3 of each release through Media RSS get both formats.
- An **OPML** file lists feeds: each `<outline xmlUrl>` (nested folders too, duplicates once) is a
  row; the feed itself is read lazily for the rows on screen (cover, artist, tracks, quality).
- CDATA, entities (`&amp;`, `&#8220;`...), comments and HTML in descriptions are handled; text
  like `<live>` in a CDATA title is kept.

### Subsonic API (Navidrome, Funkwhale, Airsonic, Gonic...)

- The user enters the server URL (`https://music.example.org`, with or without `/rest`), then
  their user name and password (keyboard without automatic capitals). The password is kept only
  in `appdata/freemusic/pw_<sid>` with mode 0600 (never in `sources.tsv`).
- Authentication: token (`t = md5(password + salt)`, `s = salt`, a random salt per source); if the
  server refuses tokens (code 40/41: Funkwhale and LDAP users) the hex form `p=enc:...` is used
  and remembered (`auth` column). `v=1.16.1&c=Vinyl&f=json`.
- Test: `ping` + `getScanStatus` (number of songs). Lists: `getAlbumList2` (`newest`, or
  `alphabeticalByName` with the Popular/Relevance sort), search: `search3` (albums), album:
  `getAlbum` (cached 1 day). The quality of a row is unknown until its album is read: rows on
  screen are probed with `getAlbum` (low-priority worker, cached in `probes.txt` as `s<sid>:<id>`),
  which gives the formats, `bitDepth` / `samplingRate` (OpenSubsonic) → `FLAC 24/96` badges.
- Files: the original through `download` (exact size) and, when the original is not MP3, an MP3
  320 transcode through `stream?format=mp3&maxBitRate=320` (size estimated, "≈") — so a FLAC
  library can be listened to light and downloaded lossless.
- Note: download and stream URLs carry the token, and `queue.tsv` keeps the URLs of pending
  downloads (appdata, like the rest of the app's data).

### Not done (possible later)

- Funkwhale's own API (only its Subsonic API), Bandcamp pages (not an open API), Ampache API,
  DLNA/UPnP servers (would need SSDP), ALAC/AAC/Opus files from catalogs, feeds and Subsonic
  originals (offered only as the MP3 transcode on Subsonic).

## Built-in sources

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
   - In the app: **Vinyl > ⋯ (top right) > JAMENDO > Key (client_id)**, type it with the
     keyboard and press OK. The app tests it right away ("Valid key" / "Invalid key" /
     "No connection"); **Test key** repeats the test, **Remove key** deletes it.
   - Without typing: create the text file **`HiByOS/jamendo_client_id.txt`** on the SD card with the
     Client ID on the first line. The app reads it when it opens and copies it to
     `/usr/data/hibymod/appdata/freemusic/jamendo_client_id` (so the SD file can be removed).
3. Jamendo then appears in *All Sources* and in the source chip (first chip under the search bar).
   Without a key, choosing Jamendo in the chip shows a single "Add your free Jamendo key" row
   that opens the settings; in *All Sources* it is skipped.

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

## VM test (docs/VM_SHELL.md)

A private copy of the harness on its own port, a local repository and a Subsonic stand-in:

```bash
# WSL
bash out/agent_vinyl/build.sh                                   # or any build of src/hibyshell.c
VMDIR=~/hiby/vm_vinyl bash apps/harness_vm/shell_vm_prep.sh out/agent_vinyl/hibyshell
echo none > ~/hiby/vm_vinyl/share/hs/repo_url
cd ~/hiby/vm_vinyl/share/hs && python3 <repo>/tests/freemusic_vm_repo.py vinylrepo http://10.0.2.2:8771
(cd vinylrepo && python3 -m http.server 8771 --bind 127.0.0.1 &)          # catalog, covers, FLAC/WAV, feed, OPML
python3 <repo>/tests/freemusic_vm_subsonic.py vinylrepo 8773 &             # Subsonic API, user demo / vinyl
cd ~/hiby/vm_vinyl && ./run_vm.sh --cpu 24Kf --port 5572 &
cd <repo> && python3 tests/freemusic_vm_flow.py tests/freemusic_vm.cmds
cd apps/harness_vm && HIBY_PORT=5572 python3 vmdrive.py ../../tests/freemusic_vm.cmds /tmp/vinyl_flow
```

`tests/freemusic_vm_flow.py` writes the walkthrough (typing is expanded into key taps of our
keyboard): clean appdata, open Vinyl, Settings › Sources, add `monotonik` (archive.org
collection), `http://10.0.2.2:8771/vinyl.json` (catalog), `…/feeds.opml` and
`http://10.0.2.2:8773` (sniffed as Subsonic from its web page, then user and password), pull to
refresh, move a source up, switch one off and on, the merged start page, one source at a time,
an album of the catalog (FLAC 24/96 badges, download into Music/Free), the feed, the Subsonic
album, a search over every source, the back and home gestures. The FLAC files are real (VERBATIM
subframes written by the generator, no encoder needed); the guest reaches archive.org for real.
