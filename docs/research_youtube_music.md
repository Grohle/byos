# Research: "native YouTube Music" on the HiBy R3 II

Date: 2026-11-09. Inspiration: SimpMusic (Kotlin, Compose Multiplatform).
Device constraints: Ingenic X1600 (MIPS32r2 LE, ~1 GHz, single core), 57 MB of RAM with ~7–10 MB free while `hiby_player` (~15 MB RSS) and `hibyshell` (~4.5 MB) are running, Linux 4.4, no swap and no JVM, 320x480 touch screen and WiFi.

---

## 0. Executive summary

- **YouTube Music has no public API.** SimpMusic, InnerTune, Metrolist and yt-dlp talk to the private **InnerTube** API (`/youtubei/v1/...`). The catalog side (search/browse/next) is easy JSON. The hard part is **getting a playable audio URL**. That requires:
  1. running YouTube's player JS (`base.js`, about 2.5–3 MB) to solve the signature (`s`) and the **`n` challenge**;
  2. for many clients, a **PoToken** (BotGuard attestation);
  3. dodging SABR, the "SABR-only" streaming experiment.

  All of this changes every few weeks.
- **Today SimpMusic runs `base.js` in QuickJS with a 192 MB limit per runtime.** That is impossible on a device with 7 MB free.
- **The stock player DOES play audio over HTTP/HTTPS.** It has its own HTTP client with `Range`, chunked and `Location`, HLS support (m3u8, AES-128), URL entries in m3u, and a DLNA renderer with `dlna_set_uri`. Demuxers: MP3, M4A, OGG (Vorbis/**Opus**), FLAC, WAV, APE, AIFF, DSF/DFF, ASF, TS and AAC. **There is no WebM/Matroska demuxer.**
- **Recommendation: a relay architecture** on the PC, an RPi or a NAS. The relay uses `ytmusicapi`, `yt-dlp` with Deno and a PoToken provider, and `ffmpeg -c copy`. The HiBy only carries a thin native UI inside `hibyshell`, which speaks plain HTTP over the LAN. Audio is delivered as **Ogg Opus remuxed without re-encoding** (itag 251 → .opus/.ogg), which the stock pipeline already decodes.
- A **download/sync** mode to the SD card, for offline listening, fits as phase 2 on top of the same relay.

---

## 1. How SimpMusic gets its data

I verified this by cloning `maxrave-dev/core`, branch `multiplatform`, module `service/kotlinYtmusicScraper`.

### 1.1 Catalog: InnerTube

- Endpoints `https://music.youtube.com/youtubei/v1/{browse,search,next,player,music/get_search_suggestions,...}`. It sends a JSON POST with `context.client` (`clientName`, `clientVersion`, `hl`, `gl`, `visitorData`) and the `X-Goog-Visitor-Id` header.
- Clients defined in `models/YouTubeClient.kt`:

  | Client | Version |
  |---|---|
  | `WEB_REMIX` | 1.20260304.03.00 (music.youtube.com client) |
  | `ANDROID_MUSIC` | 7.27.52 |
  | `ANDROID` | 21.26.364 |
  | `WEB` | — |
  | `WEB_EMBEDDED_PLAYER` | — |
  | `TVHTML5` | — |
  | `TVHTML5_SIMPLY_EMBEDDED_PLAYER` | — |
  | `IOS` | 19.45.4 |
  | `MWEB` | — |

  Each one carries its own API key and User-Agent.
- `browse` covers home, album, artist, playlist, library, moods and charts. `next` returns the "radio"/autoplay queue and the lyrics (lyrics browseId). `search` uses params to filter by songs, albums, etc.
- **`WEB_REMIX` responses are huge.** The JSON with nested renderers takes about 300 KB–2 MB per page. On a small device it has to be parsed in streaming fashion, or another process has to be asked to compact it.
- `visitorData` is obtained by scraping `ytInitialData` from the website (`getVisitorData`). Login is optional and goes through cookies.

### 1.2 Resolving the audio URL: the hard part

This is the `YouTube.player()` flow in 2026:

1. It makes a `player` call with `WEB_REMIX` plus `signatureTimestamp` to read the metadata and `playabilityStatus`.
2. It calls `newPipePlayer()`, which delegates to the **PipePipe/NewPipe extractor** (Tier 1, "ciphers solved locally") with BravePipe as a fallback. Piped also appears as a fallback, with a list of public instances.
3. The signature and the `n` challenge are solved by **`FaradayCipherEngine`**. It downloads a remote table (`simpmusic-files/registry/player_configs.json` and Metrolist's, `MetrolistGroup/faraday`) that **only names** the functions inside `base.js`. It then **downloads and runs the full `base.js` in QuickJS**. In the code:
   - `NATIVE_MEMORY_LIMIT_BYTES = 192 MB` per runtime;
   - a thread with a large native stack, because otherwise QuickJS segfaults (SIGSEGV);
   - a mutex so that two runtimes never exist at once.
4. Live streams go over HLS with the `ANDROID` client, which is the only one that does not get 403 on segments without a PoToken (measured by the author on 2026-10-01).
5. The PoToken exists in the code (`createPoTokenChallenge`/`generatePoToken` via BotGuard's `jnn-pa` API), but it is commented out in the main path. It relies on whatever PipePipe does.

### 1.3 State of the ecosystem (yt-dlp, 2025–2026)

- **A JS runtime is mandatory.** Since yt-dlp 2025.11.12, the `n` challenge and the signature are solved with **EJS** scripts (`yt-dlp-ejs`) run in **Deno** (the default), Node or another runtime. Without a runtime, formats are missing. In 2026, breakages of the "found 0 n function possibilities" kind are common and are fixed by updating the solver (`--remote-components ejs:github`).
- **PoToken** (official wiki guide, edited in July 2026):

  | Client | Requirement |
  |---|---|
  | `web`, `mweb`, `web_music` (= WEB_REMIX), `web_safari`, `tv_simply` | Require a **GVS PoToken** |
  | `web` | Additionally gives **SABR-only formats** |
  | `android`, `ios` | GVS or Player PoToken |
  | `tv` | No PoToken required, but gives DRM formats unless cookies are used |
  | `web_embedded` | No PoToken; embeddable videos only |
  | `android_vr` | No PoToken; no "made for kids", and since 2026 with "SABR-only" episodes (format 18) and intermittent 403s |

  - **Premium** users **do not need a GVS PoToken**.
  - Without a token, the URL returns **HTTP 403** and there is a risk of an IP or account block.
  - Tokens are usually bound to the session (visitorData) or to the videoId.
  - The reference provider is `bgutil-ytdlp-pot-provider`, which runs BotGuard in Node/Deno.
- **Audio formats** (googlevideo, `adaptiveFormats`):

  | itag | Format | Bitrate | Notes |
  |---|---|---|---|
  | 251 | Opus in WebM | ~130–160 kbps VBR | The best free one |
  | 250 / 249 | Opus in WebM | 70 / 50 kbps | — |
  | 140 | AAC-LC in MP4 | 128 kbps | DASH fragmented MP4 (sidx/moof) |
  | 139 | HE-AAC | 48 kbps | — |
  | 141 | AAC | 256 kbps | YouTube Music Premium only |
  | 774 | Opus | 256 kbps | YouTube Music Premium only |

  URLs expire after ~6 h, are bound to the IP and easily run to 800–1500 characters.

**The hard parts, in order:**
1. The `n` challenge and the signature require running real YouTube JS. It keeps changing and needs continuous maintenance.
2. The PoToken requires BotGuard, which is another obfuscated JS VM.
3. SABR, a proprietary protobuf-based streaming protocol (UMP) that replaces direct URLs in web clients.
4. Clients and versions have to be rotated whenever YouTube breaks one.

SimpMusic, Metrolist and yt-dlp ship patches almost monthly.

---

## 2. Can the stock player play HTTP streams?

I analyzed it with `~/hiby/hp_strings.txt` (12,723 strings from `hiby_player`) and `rootfs/usr/lib`.

**Network stack.** I found these strings and libraries:
- `libcurl.so.4` (7.68.0) plus `libssl.so.1.1`/`libcrypto` (OpenSSL 1.1). It uses `curl_easy_*` and `curl_multi_*`.
- `../../source/unit/source/it_http.c` and `http_download_task`, which builds the request by hand:
  - `GET %s HTTP/1.1`, `Range: bytes=%lld-%lld`;
  - it parses `Content-Length`, `Content-Range`, `Accept-Ranges`, `Transfer-Encoding: chunked`, `Location:` (redirects), `Set-Cookie`, `TransferMode.DLNA.ORG`;
  - `Error(%d) while seeking.`, i.e. **seeking via Range**;
  - `file_http`, with a cache `%dkbps, cache [%dK/%dK]`.
- `file_hls` / `hls_download_task`, with `#EXT-X-TARGETDURATION`, `#EXT-X-MAP`, `#EXT-X-BYTERANGE`, `AES-128`, `SAMPLE-AES`, `#EXT-X-ENDLIST`. It is **full HLS**, both VOD and live.
- `file_samba` (SMB) and `dropbox`.
- **Limit: `the http url is too long (>1024)`.** Direct googlevideo URLs usually exceed it. That is another reason to use short URLs served by a relay.

**How URLs get into the player:**
- **Internet radio**:
  - `radio.db`, with tables `all_table` and `collect` and columns `url_1..url_3`;
  - `/usr/resource/radio.txt`, in `group, name, url` format, with CNR `.m3u8` stations;
  - `a:\radio.txt` on the SD card and `radio.ini`;
  - in the UI, "Custom radio" (`radio_label_vg_self`).
- **M3U with URLs**: `malloc for m3uTag->url fail.`, `url:%s`, `#EXTM3U`. m3u playlists can contain `http://...`.
- **DLNA/UPnP renderer**: `dlna_set_uri`, `dlna_play/pause/stop/seek/next/previous`, `dlna_get_position_info`, `system_if_dlna_turn_on`. Any UPnP control point can send it `SetAVTransportURI`, and that includes a process of ours on the HiBy itself talking to localhost. There is also AirPlay.
- Tidal and Qobuz already work this way: `streamurl` → `file_http`.

**Codecs and containers** shown by the engine "HiBy Engine 5e4c22e Build Aug 15 2025" and the `.so` files:

| Container (Demux) | Decoder | Status |
|---|---|---|
| MP3 Demux | `libmp3.so` | OK |
| M4A Demux (with iTunSMPB) | FDK-AAC (`libfdk-aac`, `libaac`), ALAC | OK. **Still to check** whether it accepts fragmented MP4 (DASH, itag 140) |
| OGG Demux (`OpusHead`, `OpusTags`, Vorbis, Speex) | `libopus.so`, `libvorbis.so` | **Ogg Opus OK** in principle |
| FLAC, WAV, AIF, APE, DSF/DFF, SACD, ASF/WMA, TS, AAC (ADTS), DTS | — | OK |
| **WebM/Matroska** | — | **Does not exist** (no `matroska`, `webm` or `EBML`) |

**Conclusion:** itag 251 (WebM) **cannot be passed through as is**. There are two paths:
- **remux without re-encoding** to Ogg Opus (`ffmpeg -i x.webm -c:a copy x.opus`, negligible CPU cost);
- itag 140 (M4A). If the demuxer cannot handle fragmented MP4, remux it with `ffmpeg -c copy -movflags +faststart` to a regular MP4.

Both are trivial on a relay and very expensive to do on the HiBy.

> To be validated on the device (phase 0):
> - that an `.m3u` with `http://PC:8000/x.opus` plays and allows seeking;
> - which port the DLNA renderer listens on and whether it accepts `SetAVTransportURI` with `.opus`;
> - the player's RAM usage during HTTP playback (`file_http` cache).

---

## 3. Options for our device

### 3a) 100 % native client on the HiBy (static C, musl)

| Component | Choice | Estimated RAM | Notes |
|---|---|---|---|
| TLS | Static BearSSL or mbedTLS | 40–80 KB per connection, 0.3–0.6 MB of code | ECDHE handshake of ~0.2–0.5 s on an X1600 |
| InnerTube JSON | jsmn or a streaming tokenizer, extracting only the needed fields | 64–256 KB buffer | 2 MB of JSON cannot be loaded into a DOM |
| Catalog (search/browse/next) | `WEB_REMIX` or `ANDROID_MUSIC` | ~0.5–1 MB | Feasible |
| Signature and `n` with the full `base.js` | QuickJS (~600 KB of code) | **30–100 MB** (estimate; SimpMusic reserves 192 MB) | **Not feasible** |
| `n` with only the extracted function | QuickJS or duktape plus regex/table extraction | 2–5 MB | Fragile: the extraction is what breaks every month |
| PoToken (BotGuard) | — | Tens of MB plus a simulated DOM | **Not feasible** |
| Client without JS or PoToken (`android_vr`, `tv` + cookies) | — | Low | Works on and off. Today it gives 403 and "SABR-only" intermittently |
| Audio | Hand the URL to the player | 0 extra | WebM is not possible, and the URL exceeds 1024 characters. It would need a local remux with a WebM→Ogg proxy in C (~200–400 KB), or itag 140 if the M4A Demux accepts fMP4 |

**Verdict:** the catalog is viable locally, but URL resolution cannot be done robustly with 7 MB free. The most that is realistic is an experiment with `android_vr` without JS, which would break without warning. On top of that, it would mean rewriting in C the logic that yt-dlp maintains with a dedicated team. Maintenance cost: very high.

### 3b) Relay on a PC, RPi or NAS plus a thin UI on the HiBy (recommended)

- **Relay (Python):**
  - `ytmusicapi` for search, home, album, artist, playlists, library (with optional cookies) and radio/`next`;
  - `yt-dlp` plus Deno plus `bgutil-ytdlp-pot-provider` to resolve streams, updated with `pip -U`;
  - `ffmpeg -c copy` for the remux.
  - It exposes a **plain, compact HTTP API designed for C**:
    - small TSV/JSON, a few KB per page;
    - covers pre-scaled to 96/320 px as JPEG, or directly as RGB565 ready to blit;
    - `/stream/<videoId>.opus` served from a cache file with `Content-Length` and `Range`, so that seeking works.
- **RAM and CPU:** on the HiBy, **<1 MB extra** (an HTTP client without TLS plus the line parser, inside `hibyshell`). On the relay, ~150–300 MB (Python plus Deno). A Raspberry Pi 3/4 or any PC will do, and the remux takes <1 s per song.
- **Latency when pressing play:** ~1.5–4 s (`yt-dlp` ~1–3 s plus downloading the first chunk). It can be reduced by prefetching the next song in the queue.
- **Limitations:**
  - it only works on the LAN, or away from home through a tunnel or VPN to the relay, because the HiBy has no Tailscale;
  - it depends on the relay being switched on.

### 3c) Download/sync to the SD card (offline)

- The relay or a PC script downloads the marked playlists and songs, remuxes them to `.opus` or `.m4a`, adds tags and cover art, and copies them to the SD card (`Musica/YouTube/<playlist>/`). It also generates `.m3u` files, as `hibymix` already does.
- **Transfer routes:**
  - adb over WiFi;
  - the stock WiFi transfer (`http://<hiby>:4399`, "wifi_song");
  - the card reader.
- **Advantages:**
  - all the stock features: seeking, gapless, ReplayGain, cover art, library and "recently added";
  - no battery spent on WiFi;
  - no latency.
- **Drawbacks:** it is not "streaming". You have to wait for the sync, and it takes up SD space.
- RAM on the HiBy: 0.

### Legal and ToS considerations (brief)

- YouTube's Terms prohibit accessing content by means other than the service and downloading it without an explicit button or license.
- Bypassing the technical mechanisms (signature and PoToken) can be interpreted as circumventing protection measures (DMCA §1201 / Art. 6 of Directive 2001/29/EC). This is the precedent of the youtube-dl takedown in 2020, which GitHub reversed.
- The practical risk for personal use is low: IP or account blocks. Even so:
  - **strictly personal** use;
  - no redistribution and no exposing the relay to the Internet;
  - preferably without login, or with a secondary account. With YouTube Music **Premium**, the use is more defensible, the GVS PoToken is avoided and 256 kbps is available.
- The offline copy (3c) is the one that clashes most with the ToS, because Premium only allows downloads inside its own app.

---

## 4. Recommendation and phased plan

**Architecture: a relay on the LAN plus a native "YT Music" app in `hibyshell`, with the stock player as the audio engine.**

```
[HiBy: hibyshell app "YT Music"]  --HTTP LAN-->  [relay: FastAPI + ytmusicapi + yt-dlp(+Deno, PoToken) + ffmpeg]
   | iOS keyboard already built, lists, RGB565 covers         | /search /home /album /playlist /next(radio) /cover /stream/<id>.opus
   v
[stock hiby_player] <-- short URL http://relay:8765/stream/<id>.opus  (via local DLNA, m3u with URLs, or radio)
```

### Phase 0: validation (1–2 days, no new firmware)

1. Start a `python -m http.server` on the PC with `test.opus` (Ogg Opus), `test.m4a` (regular MP4) and `test_frag.m4a` (itag 140 as is).
2. Copy an `.m3u` with those URLs to the SD card and check playback, seeking and metadata on the HiBy.
3. Enable DLNA on the HiBy and find the port (`netstat -ltnp`, UPnP description). From the PC, send `SetAVTransportURI` and `Play` with `curl` (SOAP). Check `.opus` and the position.
4. Try adding an entry to "Custom radio" or `radio.txt` with a relay URL.
5. Measure `MemAvailable` and the player's RSS during HTTP playback.

### Phase 1: minimal working MVP

**"hibytube" relay**, ~300 lines of Python:
- `GET /search?q=` → `id\ttitle\tartist\tduration\tthumb` (songs only, 20 results).
- `GET /stream/<id>.opus`:
  1. `yt-dlp -f 251 -o -` → `ffmpeg -c copy -f ogg`;
  2. caches a complete file (~4 MB per song) and serves it with `Range`;
  3. fallback: `-f 140` → m4a with faststart.
- `GET /cover/<id>?w=96` → small JPEG, which the HiBy decodes with the IJG library that `hibywall` already uses.

**App in `hibyshell`**: a "YouTube Music" icon, a search field using the existing iOS keyboard, a results list, and a tap that starts playback. For playback, in order of preference:
- DLNA to localhost, if phase 0 confirms it; this gives play, pause, seek and next;
- a "YouTube.m3u" `.m3u` on the SD card plus a row in `M3U_TABLE` (like `hibymix`), opened with injected navigation.

With this you can already search for and listen to a song from the HiBy. "Now Playing" is the stock one.

**An even more minimal variant (MVP-0, no code on the HiBy):** the relay acts as the DLNA control point and pushes the URL to the HiBy. It is controlled from a web page on the phone. It is enough to validate the whole chain in an afternoon.

### Phase 2: "SimpMusic-lite" experience

- Home, Explore, albums, artists, library playlists (`ytmusicapi` with browser cookies) and **radio/autoplay** (`next`/`get_watch_playlist`). The queue is generated as an m3u of relay URLs.
- Prefetching of the next song on the relay and an on-disk LRU cache.
- **Offline sync (3c)**: a "Download" button on a playlist or album that leaves tagged `.opus`/`.m4a` files plus an `.m3u` on the SD card, and rescans the library.
- Synced lyrics (LRCLIB) served as `.lrc` next to the song. The stock player already displays `.lrc`.

### Phase 3: optional

- Scrobbling to YouTube, so that the history feeds the recommendations (`playbackTracking` on the relay).
- Access away from home: relay behind a VPS or a tunnel with a token (personal use only).
- A purely local experiment with the `android_vr` client without JS, as an emergency "relay-less mode", assuming it will break.

### Why not a 100 % native client

- The critical piece, the audio URL, currently demands a JS engine using tens of MB, a PoToken (BotGuard) and keeping up with YouTube's weekly changes.
- The relay concentrates that fragility where there is RAM, and `pip install -U yt-dlp` fixes it.
- The HiBy keeps what it does well: fast UI and high-quality Opus/AAC decoding. For that it spends no more than ~1 MB of its scarce memory.

---

## Sources

- SimpMusic: https://github.com/maxrave-dev/SimpMusic. The analyzed code is https://github.com/maxrave-dev/core (branch `multiplatform`, `service/kotlinYtmusicScraper`: `YouTube.kt`, `Ytmusic.kt`, `models/YouTubeClient.kt`, `cipher/FaradayCipherEngine.kt`, `cipher/QuickJsEngine.kt`).
- Player tables: https://raw.githubusercontent.com/maxrave-dev/simpmusic-files/main/registry/player_configs.json and https://raw.githubusercontent.com/MetrolistGroup/faraday/master/registry/player_configs.json
- yt-dlp PO Token Guide (edited 2026-07-12): https://github.com/yt-dlp/yt-dlp/wiki/PO-Token-Guide
- yt-dlp EJS / JS runtime: https://github.com/yt-dlp/yt-dlp/wiki/EJS · https://gigazine.net/gsc_news/en/20251113-yt-dlp-required-deno-javascript-runtime · https://github.com/yt-dlp/yt-dlp/issues/15897
- `android_vr` and SABR: https://github.com/yt-dlp/yt-dlp/issues/16150 · https://discuss.cachyos.org/t/audiotube-26-04-3-stopped-playing-youtube-music-http-403-yt-dlp-works/34459
- Local analysis: `~/hiby/hp_strings.txt` (strings from `hiby_player`, HiBy Engine build 2025-08-15), `~/hiby/rootfs/usr/lib` (libcurl 7.68.0, OpenSSL 1.1, libopus, libvorbis, libfdk-aac, libmp3, libwma) and `~/hiby/rootfs/usr/resource/radio.txt`.
