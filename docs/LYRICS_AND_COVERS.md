# Automatic lyrics and covers (`hibynet`)

`bin/hibynet` is a small static tool (mipsel, musl, hardware floating point) that the UI uses to
search for **lyrics** and **covers** on free public services that need no API key, and to download
individual files in manual mode (browser).

- Code: `src/hibynet.c` · Build: `tools/wsl/zbuild_net.sh [x86]` → `bin/hibynet` + `bin/cacert.pem`
  (with `x86` it also produces `~/hiby/net/hibynet-x86` for measuring on the PC).
- Dependencies (the script builds them the first time in `~/hiby/net/{mips,x86}`): mbedTLS 2.28.9
  (trimmed configuration) and libjpeg 9f. It also uses `stb_image.h` and `stb_image_write.h` from `~/hiby/third`.
  **Important:** the old sysroot `~/hiby/vm/xsysroot` is *soft-float* and cannot be used; that is why everything is
  rebuilt as hard-float.
- Installation on the device: `hibynet` and `cacert.pem` **in the same directory**. Otherwise, use `HIBYNET_CA=/path/cacert.pem`.

## Commands

Output on stdout: UTF-8, one result per line and fields separated by TAB. Any TABs and line breaks
in the data are replaced with spaces. Progress and errors go to stderr, prefixed with `hibynet: `.
During downloads `progress BYTES TOTAL` is emitted, where TOTAL is -1 if the server does not report it.

| Command | stdout |
|---|---|
| `hibynet lyrics ARTIST TITLE [ALBUM] [DURATION_S]` | `id  artist  title  album  duration_s  synced(0/1)` (max. 12, best first) |
| `hibynet lyrics-get ID OUTPUT.lrc` | `OUTPUT.lrc  synced(0/1)` |
| `hibynet covers ARTIST ALBUM` | `source  url  artist  album  year` (max. 8; source = `itunes` or `caa`) |
| `hibynet cover-get URL OUTPUT.jpg [SIZE]` | `OUTPUT.jpg  width  height  OUTPUT.thumb.raw` |
| `hibynet get URL OUTPUT` | `OUTPUT  bytes` |

Exit codes:

| Code | Meaning |
|---|---|
| `0` | Success. |
| `1` | No results or not found (404). |
| `2` | Network, HTTP or TLS failure (including an invalid certificate). |
| `3` | Incorrect usage. |
| `4` | I/O or image error (unsupported format or image too large). |

Environment variables:

| Variable | Effect |
|---|---|
| `HIBYNET_CA` | Path of the CA bundle. |
| `HIBYNET_NOBOM=1` | Writes the LRC without a BOM. |
| `HIBYNET_VERBOSE=1` | Traces requests, TLS and redirects. |
| `HIBYNET_STATS=1` | Writes `VmHWM` (real peak RSS) on exit. |

### Details
- **lyrics**: if ALBUM and DURATION are given, it first calls `GET /api/get` (exact match, placed first).
  Then it calls `GET /api/search?track_name=&artist_name=` and, if nothing comes back, `/api/search?q=ARTIST TITLE`.
  Ordering: synced lyrics first, then the duration closest to DURATION. Instrumental entries and entries without text are discarded.
- **lyrics-get**: `GET /api/get/ID`. Writes `syncedLyrics` and, if there are none, `plainLyrics` (lines without timestamps).
  Header: `[ar:]`, `[ti:]`, `[al:]`, `[length:mm:ss]` and `[re:LRCLIB]`. The file is UTF-8 **with BOM** and `\n` line breaks.
  Writing is atomic: first `.tmp` and then `rename`.
- **covers**:
  - iTunes: `https://itunes.apple.com/search?media=music&entity=album&limit=10&term=…`. `100x100` is rewritten to `600x600`
    in `artworkUrl100`, and artists that do not match (tributes, covers) are discarded.
  - MusicBrainz: `/ws/2/release-group/?query=releasegroup:"…" AND artist:"…"`, with score ≥ 80 and up to 3 results.
    Each one is checked with a `HEAD` to `https://coverartarchive.org/release-group/MBID/front-500`
    (307 = exists; 404 = no cover).
  - Ordering: exact album and artist match first; on a tie, iTunes ahead of CAA.
- **cover-get**: downloads to `OUTPUT.dl` (in the same directory, never in `/tmp`), decodes and produces:
  - `OUTPUT.jpg`: **baseline** JPEG, quality 88, fitted to SIZE×SIZE preserving the aspect ratio. The default is 500 and it is never upscaled.
  - `OUTPUT.thumb.raw`: 96×96 RGB565 *little-endian* (18 432 bytes), centered on black (*letterbox*). If OUTPUT ends in
    `.jpg`/`.jpeg` the extension is removed: `cover.jpg` → `cover.thumb.raw`.
  - JPEG: libjpeg with IDCT downscaling (1/8…8/8) and row-by-row area scaling, so memory use is that of the output and little more.
    *Progressive* JPEGs larger than 1600×1600 are rejected, because libjpeg needs the coefficients of the whole image.
  - PNG, GIF and BMP: stb_image, with an estimated peak cap of about 6 MB. A 4000×4000 16-bit image is rejected with code 4.
- **get**: downloads to `OUTPUT.part` and does a `rename` when finished. Follows up to 8 redirects and has no size cap.

### Network, timeouts and TLS
- Custom HTTP/1.1 (`Connection: close`, `Accept-Encoding: identity`, *chunked* and `Content-Length`) and 301/302/303/307/308 redirects.
  User-Agent on every request: `ByOS/0.6 (https://github.com/Grohle/hibyos)`.
- Timeout: **15 s per network call** (connection, TLS and the complete response). In `get` and `cover-get` the timeout works as an
  *inactivity* timeout: it is renewed on every read, so large downloads are not cut off. There is also a global 90 s alarm
  on the search commands, in case DNS hangs.
- JSON responses are capped at 512 KB. Parsing is a direct walk over the buffer, with no token tree.
- TLS 1.2 with mbedTLS 2.28 (there is no TLS 1.3; all the services used accept 1.2).
  The chain, signature, host name (SNI + CN/SAN) and dates are verified.
  Tested against `expired.`, `self-signed.` and `wrong.host.badssl.com`: all three are rejected with code 2.
- **CA bundle:** the **complete Mozilla bundle** is used (`https://curl.se/ca/cacert.pem`: 200 KB, 118 roots).
  It is not trimmed because lrclib.net, mzstatic.com, musicbrainz.org and archive.org change CAs over time, and a trimmed
  bundle would break silently. There is no RAM cost: with `MBEDTLS_X509_TRUSTED_CERTIFICATE_CALLBACK`, on each
  verification the PEM is walked line by line and only the root whose *subject* matches the server's *issuer* is parsed.
  That way the whole bundle is never loaded into memory, which would take more than 300 KB of heap.
  To update it, delete `bin/cacert.pem` and run the script again.
- **Clock:** if the system year is earlier than 2025 (clock not set), **only** certificate date errors are ignored,
  with a warning on stderr. Everything else is still verified.
- **MusicBrainz** allows at most 1 request per second. To respect that across processes a timestamp in `/tmp/.hibynet_mb`
  (an empty file) is used: if the previous request was less than 1.1 s ago, it waits. On a 503 it retries once after 1.2 s.

## Services and terms of use

| Service | Use | Limits and notes |
|---|---|---|
| **LRCLIB** (`lrclib.net`) | Synced and plain lyrics | Free and keyless. They ask for a User-Agent that identifies the app (it is sent). They do not publish a request limit; be reasonable: one search per user action and no unthrottled mass sweeps of the library. Lyrics are contributed by the community and their rights belong to the authors: personal use. |
| **iTunes Search API** (`itunes.apple.com`, images on `*.mzstatic.com`) | 600×600 covers | Keyless. Apple gives about **20 calls per minute** as guidance. The images are copyrighted by Apple and the labels: personal use only, to display them on the device and not to redistribute them. |
| **MusicBrainz** (`musicbrainz.org`) | *Release group* search | Keyless. **Max. 1 request/s** per IP and a **User-Agent with contact info is mandatory**; without it you get blocked. The data is CC0. |
| **Cover Art Archive** (`coverartarchive.org` → `archive.org`) | 500 px covers (`front-500`) | Keyless, no strict limit. Each download goes through 2 or 3 redirects (coverartarchive → archive.org → `ia*.us.archive.org`), so it takes about 5 s on the R3 II. The images belong to their rights holders: personal use. |

Recommendation for the UI: on-demand searches only (when opening "Search lyrics" or "Search cover"). If an automatic
library sweep is ever added, it must run serially, with pauses of 1 s or more, and remember what was not found
so the query is not repeated.

## Measurements

The `bin/hibynet` binary is 570 KB (static mipsel, stripped) and `cacert.pem` is 200 KB.

| Command (real query) | Peak RSS on PC (x86 glibc, `/usr/bin/time -v`) | VmHWM on the R3 II | Time on the R3 II |
|---|---|---|---|
| `lyrics "Extremoduro" "So payaso"` | 2.7 MB | **0.82 MB** | 0.6 s |
| `lyrics … "Agila" 282` (get + search) | 2.7 MB | 0.82 MB | 1.3–3.5 s |
| `lyrics-get 3055312` | 2.2 MB | 0.61 MB | 0.45 s |
| `covers "Extremoduro" "La ley innata"` | 2.4 MB | 0.64 MB | 1.4 s |
| `covers "Radiohead" "OK Computer"` | 2.4 MB | 0.64 MB | 1.5 s (5.6 s if MusicBrainz returns a 503 and it retries) |
| `cover-get` iTunes 600→500 | 3.0 MB | **1.5 MB** | 0.9 s |
| `cover-get` CAA front-500 | 3.0 MB | 1.5 MB | 5 s |
| `get` 13.9 MB PNG | 2.4 MB | 0.62 MB | 19 s |

On the PC the RSS is higher because it includes dynamic glibc. The figure that matters is the device one.

## Where to save so the stock player uses it

This was checked against the strings of `~/hiby/rootfs/usr/bin/hiby_player` (`~/hiby/hp3.txt`) and against the device's
real database `/data/usrlocal_media.db`. Nothing was played and no process was restarted.

### Lyrics
- **`.lrc` file next to the audio, with the same base name:** `…/Álbum/1-07 Canción.flac` → `…/Álbum/1-07 Canción.lrc`.
  Evidence in the binary:
  - the `lg_lyric_view` viewer and the `lrc` extension in the same extension table as `cue` and `iso`, with `<protocol>lrc`;
  - the options `lrc_color`, `lrc_size`, `lrc_focus_*` and `no_lrc`;
  - the `lrc_path` column in `MEDIA_TABLE`.
- In the current database `lrc_path` is **empty for all 2488 tracks**: the player looks for the `.lrc` at playback time and not
  at scan time, so there is no need to rescan the library after saving.
- It also reads **embedded** lyrics (strings `Lyrics`, `UNSYNCEDLYRICS` and `WM/Lyrics`), but writing them is not necessary.
- Encoding: the player ships GB18030, Big5, CP932, CP949, ISO-8859-x and Windows-1258 tables, and detects UTF-8 and UTF-16. That is why the
  `.lrc` is written as **UTF-8 with BOM**: it is the safest way to keep it from being interpreted as a local code page.
  With `HIBYNET_NOBOM=1` it is written without a BOM.
- Usage: `hibynet lyrics-get ID "/data/mnt/sd_0/Artista/Álbum/1-07 Canción.lrc"`.

### Covers
- The stock binary contains **no** `cover.jpg`, `folder.jpg`, `front.jpg` or `albumart`, and neither does any other
  binary in the rootfs. It does contain the strings for **embedded** covers: `METADATA_BLOCK_PICTURE` (FLAC/Vorbis),
  `WM/Picture` (WMA), `Cover Art` + `(Front)` (APE), plus the parsers `album_cover_parser` and `cover_parser`.
  There is also a cache of its own: `/data/mnt/sd_0/list_cover/` and `/data/mnt/sd_0/.temp/image_cache.db`.
- The `album_pic_path` column is empty for every track, and the device's album folders only contain `.flac`:
  the covers already shown are **embedded**.
- **Conclusion:** for the stock player to show the cover it has to be **embedded in the audio**:
  - FLAC: PICTURE block type 3;
  - MP3: ID3v2 APIC;
  - M4A: `covr`.

  That is what `ht_set_cover(audio, jpeg)` in `src/hibytag.c` is for and, from the command line, `hibytag setcover IMG.jpg FILE...`.
  It works with FLAC, MP3 (ID3v2.3/2.4) and M4A; Ogg/Opus is not supported.
  Writing is atomic and streamed, and leaves the audio byte-for-byte intact. The image can be at most 1 MB.
- Meanwhile, the ByOS UI should save `cover.jpg` in the album folder, which is what almost every
  player uses and what our own UI can read. Next to it, it should save `cover.thumb.raw` (the 96×96 RGB565 thumbnail
  for lists). Usage: `hibynet cover-get URL "/data/mnt/sd_0/Artista/Álbum/cover.jpg"`.
  To embed it later, use the 500 px `cover.jpg`: it is baseline and weighs about 60–80 KB, a reasonable size for the player.
- Still to be verified live: whether, when playing an album **without** an embedded cover but with a `cover.jpg` in the
  folder, the stock player shows it. It could not be checked without touching the player. No string in the binary suggests it does.
