# Radio

The Radio app plays internet radio stations on the HiBy R3 II through the stock player engine.
It opens from the **Radio** icon on the home screen. **It needs a microSD card**: the stream is
written to a file on the card while it plays (see *How playback works*).

## What it does

- **Explore**:
  - **Sources**: the built-in list **European Public Radio** (about a hundred official streams of
    public broadcasters: RNE, Catalunya Ràdio, EITB, Canal Sur, Radio France, ARD, RAI, BBC, RTÉ,
    NPO, VRT/RTBF, SRG SSR, ORF, the Nordic broadcasters and more) and any station list you
    added. Tap one to browse its stations. *Manage Sources…* opens the Sources page.
  - **Radio Browser**: the most listened stations, the stations of a country (by default the
    country of the system language; tap *Change Country…* to pick another one) and 22 genres
    (Pop, Rock, News, Classical, Jazz…).
- **Search**: tap the search bar and type a station name; results appear as you type. Matches
  from the European list come first (they need no network), then the Radio Browser results.
- **Favourites**: tap the star on any station. The Favourites tab also shows the last 12 stations you
  listened to (*Recents*) and your own stations.
- **My Stations**: *Add Station…* asks for a stream URL (`http://` is added if missing) and a name.
  Tap the `…` button of an own station to rename it, change its URL or delete it.
- **Playback**: tap a station. The card at the bottom shows its state: *Connecting…*,
  *Buffering… N %*, then the song title the station sends (or *Live*), or an error
  (*No connection*, *Station unavailable*, *Unsupported format*, *Encrypted stream: not supported*,
  *No SD card*, *The SD card is full*, *The player is not responding*), or a clean end in grey
  (*Session limit reached*, *The broadcast has ended*). The button on the right stops it or plays
  it again.
- Radio keeps playing when you leave the app (volume, EQ, Bluetooth and the hardware buttons work as
  with any song). Playing any local music stops the radio by itself.

Gestures are the same as in the other HiByOS apps: swipe in from either edge to go back, swipe up
from the bottom bar to go home.

## Sources

*Explore → Manage Sources…* lists every source with a switch to turn it on or off. A source that
is off disappears from Explore (turning off Radio Browser also hides its lists and stops the search
from asking it). Tap a source for its options:

- **Open** shows its stations.
- **Refresh** downloads the list again (lists are otherwise kept for a day).
- **Rename**, **Edit URL** and **Remove Source** (user sources only; the two built-in sources can
  only be turned off).

*Add Source…* asks for a web address. What it can be:

| Kind | How it is recognised | Example |
| --- | --- | --- |
| M3U / M3U8 station list | `#EXTM3U` with `#EXTINF:` lines (or just one URL per line) | `#EXTINF:-1 tvg-country="ES" group-title="News",Radio 5` then the stream URL |
| PLS | `[playlist]` with `FileN=` / `TitleN=` | Shoutcast/Icecast `.pls` lists |
| JSON | an object with a `stations` array, or a bare array (format below) | `https://example.org/my-stations.json` |
| Radio Browser server | the root of a server that answers `/json/stats` like Radio Browser | `https://radio.example.org` |

The kind is guessed from the extension and confirmed by the content once downloaded. An M3U8 that
turns out to be an **HLS stream** (it has `#EXT-X-` tags: it is one broadcast, not a list of
stations) is not kept as a source: it is added to *My Stations* instead. A Radio Browser server
shows its most listened stations (its station ids are not reported to the official directory's
click counter).

### The JSON station list format

An open, minimal format; HiByOS reads it and anyone can write it by hand:

```json
{
  "hibyos_radio": 1,
  "name": "My stations",
  "stations": [
    { "name": "RNE Radio Clásica", "url": "https://rtvelivestream.rtve.es/rtvesec/rne/rne_r2_main.m3u8",
      "country": "ES", "codec": "AAC", "bitrate": 192, "tags": "classical" },
    { "name": "Deutschlandfunk", "url": "https://st01.sslstream.dlf.de/dlf/01/128/mp3/stream.mp3",
      "country": "DE", "codec": "MP3", "bitrate": 128, "tags": "news, talk" }
  ]
}
```

| Field | Required | Meaning |
| --- | --- | --- |
| `hibyos_radio` | no | format version (1) |
| `name` (list) | no | name shown for the source (unless you renamed it) |
| `stations[].name` | yes | station name |
| `stations[].url` | yes | `http://` or `https://` stream: MP3, AAC (ADTS), a `.m3u`/`.pls` playlist or HLS (`.m3u8`) |
| `stations[].country` | no | ISO 3166-1 alpha-2 code (`countrycode` is accepted too) |
| `stations[].codec` | no | `MP3`, `AAC`; other codecs (`OGG`, `OPUS`, `FLAC`…) are skipped |
| `stations[].bitrate` | no | kbit/s, shown next to the codec |
| `stations[].tags` | no | comma-separated words (the first three are shown) |

The top level may also be the `stations` array itself. Radio Browser station objects
(`url_resolved`, `countrycode`, `hls`…) are accepted as they are. At most 240 stations are read
from a list.

## The directory

The Radio Browser stations come from [Radio Browser](https://www.radio-browser.info/), a free,
community-run directory with an open API that needs no account or key. HiByOS follows its client
guidelines:

- it identifies itself with the User-Agent `HiByOS-Radio/1.0 (https://github.com/Grohle/hibyos)`;
- it reports every station you start to the directory's click counter (`/json/url/<station id>`),
  which is how the directory ranks popular stations;
- if a server does not answer, it tries the next one (`de1`, `fi1`, `de2` `.api.radio-browser.info`).

Only stations that the directory currently sees working and that stream MP3 or AAC (directly or
over HLS) are listed (see *Limits*). Streams, names and logos belong to their broadcasters;
HiByOS only plays what the station publishes.

## Where data is stored

Everything stays on the player; nothing is sent anywhere except the directory requests above,
the station lists you added and the stream itself.

| File | Contents |
| --- | --- |
| `/usr/data/hibymod/appdata/radio/stations.tsv` | favourites (`F`), recents (`R`) and own stations (`C`), one per line, tab separated: kind, directory id, name, URL, country, codec, kbit/s, tags |
| `/usr/data/hibymod/appdata/radio/sources.tsv` | the sources, one per line, tab separated: type, on, renamed, stations, last download (Unix time), last error, name (if renamed), URL (a few hundred bytes) |
| `/usr/data/hibymod/appdata/radio/config` | chosen country, last tab, `lead_s` (seconds written before playback starts, 15) and `cap_mb` (session limit, 128) |
| `/usr/data/mnt/sd_0/HiByOS/cache/radio/lists/` | the downloaded station lists (SD card; the internal storage is nearly full) |
| `/usr/data/mnt/sd_0/HiByOS/cache/radio/` | while playing: the stream file `<station>.mp3` / `.aac` (deleted on stop) |
| `/tmp/hibyradio/` | while playing: the helper's small state file and pid (a few hundred bytes of RAM) |

## How playback works

The stock player engine can only open files, not URLs, and it has two properties that shape the
design (both checked on the device):

- it does **not** play files in `/tmp` (tmpfs): the list is accepted but nothing sounds. The same
  file on the SD card (exFAT) plays with its plain Linux path. And `/tmp` is RAM: with 3–6 MB free,
  buffering audio there starved the whole system. So the audio goes to the SD card, never to RAM.
- it follows a file that keeps growing, **but stops if playback reaches the end of the file**.
  With 5 s written before it started it stopped; with 15 s it kept playing.

So when you start a station:

1. The shell starts the helper `hibynet radio URL DIR NAME LEAD_S CAP_MB STATEDIR` (a separate
   static binary; its buffers take a few hundred KB of RAM). It follows redirects, `.m3u` and `.pls`
   playlists (trying each entry), Shoutcast's `ICY 200 OK` and HLS.
   - **Icecast/Shoutcast**: it asks for ICY metadata (song titles), removes it from the audio and
     waits for three consecutive MP3 or AAC (ADTS) frames, so the file starts on a whole frame.
   - **HLS** (`.m3u8`): a master playlist gives the lightest audio-only variant (video variants
     only if there is nothing else; an audio rendition is preferred to a video variant). The media
     playlist is read as it arrives (the last 24 segments are kept); at the start the helper takes
     the newest segments that add up to the lead, so the pre-buffer fills at once; then it reloads
     the playlist every `TARGETDURATION` (half of it when nothing changed) and downloads new
     segments in media-sequence order, never twice; relative and absolute URIs work. Each segment
     is demultiplexed while it downloads (MPEG-TS: PAT → PMT → the audio PID with stream type
     `0x0F` AAC ADTS or `0x03`/`0x04` MPEG audio → PES payload), or, for packed audio segments
     (ADTS/MP3 with ID3 tags), the ID3 tags are skipped. One HTTP/1.1 keep-alive connection is
     reused (one TLS handshake per server, not one per segment). A playlist sent gzip-compressed
     is inflated. Encrypted streams (`EXT-X-KEY` other than `NONE`) are refused with a clear
     error, and so are fMP4 (`EXT-X-MAP`) and byte-range playlists.
2. It writes the audio to `/usr/data/mnt/sd_0/HiByOS/cache/radio/<station>.mp3` (or `.aac`), an
   ordinary file that grows (exFAT has no sparse files or hole punching), and counts the frames, so
   it knows how many seconds of audio the file holds.
3. When 15 seconds of audio are written (`lead_s`), the shell hands the file to the player as a
   one-song list (the same mechanism Vinyl and Audiobooks use for files outside the library),
   with its plain path on the card. The writer stays ahead: an Icecast stream keeps the lead it
   started with, HLS adds a segment at a time.
4. The helper watches the player's read position (`/proc/<player>/fdinfo`) and:
   - stops downloading when 3 minutes are waiting unread (the player is paused) and resumes later
     (Icecast reconnects; HLS continues where it was, or jumps ahead if those segments are gone);
   - if the player stops at the end of the file anyway (a network stall: it closes the file, or its
     position stays at the end for 20 s, or it starts the file again), it **restarts cleanly**: a new
     file, a new pre-buffer 5 s longer than the last one (up to 40 s), and the shell sends the list
     again;
   - reconnects automatically when the connection drops (up to 6 attempts);
   - exits and deletes its files when the player closes the file or opens other music, when the
     shell stops it, or if the shell dies.
5. **Session limit**: the file cannot be trimmed while it plays, so a session is limited to 128 MB
   on the card (`cap_mb`; never more than the free space minus 16 MB): about 2 h 20 min at 128
   kbit/s, 1 h 30 min at 192 kbit/s. At the limit the helper stops downloading, the player plays
   what is written and the card shows *Session limit reached* (tap play to start again). The file
   is not rotated to a new one at the limit because handing the stock engine another file always
   interrupts the sound (a new list), and a clean end is better than an unannounced gap.

Leftover stream files (after a crash) are deleted the next time a station starts or stops.

## Limits

- **An SD card is required** (*No SD card* otherwise). The card needs at least ~24 MB free.
- **Formats**: MP3 and AAC (ADTS), directly or over HLS (MPEG-TS or packed audio). Ogg
  Vorbis/Opus, FLAC, AAC-LATM, fMP4 HLS and encrypted HLS are refused (*Unsupported format* /
  *Encrypted stream*); the directory lists hide what cannot play.
- HLS: the lightest audio variant is chosen, so a broadcaster that offers several bitrates plays
  at its lowest one.
- Pausing for a long time and resuming plays the buffered minutes first, then the live stream.
- The player shows a growing duration and a seek bar for the stream; seeking is not meaningful.
- A network stall longer than the lead causes a short silence while a new buffer fills.
- The directory server list is fixed in the app (it does not use the `all.api.radio-browser.info`
  DNS lookup); if all three servers change, the app needs an update.
- Country names in the country list are the directory's (English) names.
- At most 120 favourites, 40 own stations, 12 sources and 240 stations per list.
- The built-in European list is a snapshot: broadcasters change their stream addresses from time
  to time (BBC's worldwide HLS addresses in particular). A station that moved shows *Station
  unavailable* until the app is updated; you can add the new address as your own station.

## The European Public Radio list

Official, free and legal streams of public broadcasters, built into the app (`RA_EU` in
`src/radio_src.inc.c`). Every URL was collected from the broadcaster's own stream server or CDN
(the address its web player uses, as also listed in Radio Browser) and **checked with curl from
Spain on 10 November 2026**: a direct stream had to return MP3 or ADTS frames; an HLS playlist had
to give segments whose audio is AAC (the segments were demultiplexed and the frames checked). The
HLS stations, and the AAC Icecast ones, were also played end to end through the helper (x86 build)
for 25 s each: whole frames from the first byte, no junk, files deleted at the end. BBC Radio 3
and Radio 4 are BBC's worldwide (`ww`) streams, which do play outside the UK; BBC's domestic
streams are geo-blocked and are not included. Candidates that failed were left out (some Yle HLS
addresses, À Punt, Aragón Radio, Radio Euskadi's old Icecast address, LRT (video only), Latvian
Radio, Polskie Radio Dwójka, Deutsche Welle and others).

| Station | Country | Format (kbit/s) | Stream server (operator) | URL |
| --- | --- | --- | --- | --- |
| RNE Radio 1 | ES | HLS, AAC 192 | RTVE (own CDN) | `https://rtvelivestream.rtve.es/rtvesec/rne/rne_r1_main.m3u8` |
| RNE Radio Clásica | ES | HLS, AAC 192 | RTVE (own CDN) | `https://rtvelivestream.rtve.es/rtvesec/rne/rne_r2_main.m3u8` |
| RNE Radio 3 | ES | HLS, AAC 192 | RTVE (own CDN) | `https://rtvelivestream.rtve.es/rtvesec/rne/rne_r3_main.m3u8` |
| RNE Ràdio 4 | ES | HLS, AAC 192 | RTVE (own CDN) | `https://rtvelivestream.rtve.es/rtvesec/rne/rne_r4_main.m3u8` |
| RNE Radio 5 | ES | HLS, AAC 192 | RTVE (own CDN) | `https://rtvelivestream.rtve.es/rtvesec/rne/rne_r5_madrid_main.m3u8` |
| RNE Radio Exterior | ES | HLS, AAC 192 | RTVE (own CDN) | `https://rtvelivestream.rtve.es/rtvesec/rne/rne_re_main.m3u8` |
| Catalunya Ràdio | ES | AAC 128 | CCMA | `https://shoutcast.ccma.cat/ccma/catalunyaradioHD.mp3` |
| Catalunya Música | ES | AAC 128 | CCMA | `https://shoutcast.ccma.cat/ccma/catalunyamusicaHD.mp3` |
| Catalunya Informació | ES | AAC 128 | CCMA | `https://shoutcast.ccma.cat/ccma/catalunyainformacioHD.mp3` |
| iCat | ES | AAC 128 | CCMA | `https://shoutcast.ccma.cat/ccma/icatHD.mp3` |
| Radio Euskadi | ES | HLS, AAC 128 | EITB | `https://multimedia.eitb.eus/live-content/radioeuskadi-hls/master.m3u8` |
| Euskadi Irratia | ES | MP3 128 | EITB (Flumotion CDN) | `http://mp3-eitb.stream.flumotion.com/eitb/euskadiirratia.mp3` |
| Gaztea | ES | HLS, AAC 128 | EITB | `https://multimedia.eitb.eus/live-content/gaztea-hls/master.m3u8` |
| Canal Sur Radio | ES | MP3 320 | RTVA (Flumotion CDN) | `https://rtva-live-radio.flumotion.com/rtva/csr.mp3` |
| Canal Fiesta Radio | ES | MP3 320 | RTVA (Flumotion CDN) | `https://rtva-live-radio.flumotion.com/rtva/cfr.mp3` |
| Radio Galega | ES | HLS, AAC 192 | CRTVG (Flumotion CDN) | `https://crtvg-radiogalega-hls.flumotion.cloud/playlist.m3u8` |
| IB3 Ràdio | ES | MP3 128 | IB3 | `https://icecast.ib3radio.com/ib3radio.mp3` |
| RTP Antena 1 | PT | HLS, AAC 160 | RTP | `https://streaming-live.rtp.pt/liveradio/antena180a/playlist.m3u8` |
| RTP Antena 2 | PT | HLS, AAC 256 | RTP | `https://streaming-live.rtp.pt/liveradio/antena280a/playlist.m3u8` |
| RTP Antena 3 | PT | HLS, AAC 160 | RTP | `https://streaming-live.rtp.pt/liveradio/antena380a/playlist.m3u8` |
| France Inter | FR | MP3 128 | Radio France | `https://icecast.radiofrance.fr/franceinter-midfi.mp3` |
| France Culture | FR | MP3 128 | Radio France | `https://icecast.radiofrance.fr/franceculture-midfi.mp3` |
| France Musique | FR | MP3 128 | Radio France | `https://icecast.radiofrance.fr/francemusique-midfi.mp3` |
| FIP | FR | MP3 128 | Radio France | `https://icecast.radiofrance.fr/fip-midfi.mp3` |
| FIP Jazz | FR | MP3 128 | Radio France | `https://icecast.radiofrance.fr/fipjazz-midfi.mp3` |
| franceinfo | FR | MP3 128 | Radio France | `https://icecast.radiofrance.fr/franceinfo-midfi.mp3` |
| Mouv' | FR | MP3 128 | Radio France | `https://icecast.radiofrance.fr/mouv-midfi.mp3` |
| Deutschlandfunk | DE | MP3 128 | Deutschlandradio | `https://st01.sslstream.dlf.de/dlf/01/128/mp3/stream.mp3` |
| Deutschlandfunk Kultur | DE | MP3 128 | Deutschlandradio | `https://st02.sslstream.dlf.de/dlf/02/128/mp3/stream.mp3` |
| Deutschlandfunk Nova | DE | MP3 128 | Deutschlandradio | `https://st03.sslstream.dlf.de/dlf/03/128/mp3/stream.mp3` |
| Bayern 2 | DE | MP3 128 | ARD (BR / rbb / hr, ARD stream dispatcher) | `https://dispatcher.rndfnk.com/br/br2/live/mp3/mid` |
| BR-Klassik | DE | MP3 256 | ARD (BR / rbb / hr, ARD stream dispatcher) | `https://dispatcher.rndfnk.com/br/brklassik/live/mp3/high` |
| BR24 | DE | MP3 128 | ARD (BR / rbb / hr, ARD stream dispatcher) | `https://dispatcher.rndfnk.com/br/br24/live/mp3/mid` |
| WDR 3 | DE | MP3 256 | WDR | `https://wdr-wdr3-live.icecastssl.wdr.de/wdr/wdr3/live/mp3/256/stream.mp3` |
| WDR 5 | DE | MP3 128 | WDR | `https://wdr-wdr5-live.icecastssl.wdr.de/wdr/wdr5/live/mp3/128/stream.mp3` |
| 1LIVE | DE | MP3 128 | WDR | `https://wdr-1live-live.icecastssl.wdr.de/wdr/1live/live/mp3/128/stream.mp3` |
| NDR Kultur | DE | MP3 128 | NDR | `https://icecast.ndr.de/ndr/ndrkultur/live/mp3/128/stream.mp3` |
| NDR Info | DE | MP3 128 | NDR | `https://icecast.ndr.de/ndr/ndrinfo/hamburg/mp3/128/stream.mp3` |
| SWR Kultur | DE | MP3 256 | SWR | `https://liveradio.swr.de/sw282p3/swr2/play.mp3` |
| MDR Kultur | DE | MP3 128 | MDR | `https://mdr-284310-0.sslcast.mdr.de/mdr/284310/0/mp3/high/stream.mp3` |
| radioeins | DE | MP3 128 | ARD (BR / rbb / hr, ARD stream dispatcher) | `https://dispatcher.rndfnk.com/rbb/radioeins/live/mp3/mid` |
| hr2-kultur | DE | MP3 256 | ARD (BR / rbb / hr, ARD stream dispatcher) | `https://dispatcher.rndfnk.com/hr/hr2/live/mp3/high` |
| Rai Radio 1 | IT | MP3 256 | RAI | `https://icestreaming.rai.it/1.mp3` |
| Rai Radio 2 | IT | MP3 256 | RAI | `https://icestreaming.rai.it/2.mp3` |
| Rai Radio 3 | IT | MP3 320 | RAI | `https://icestreaming.rai.it/3.mp3` |
| Rai Radio Classica | IT | MP3 256 | RAI | `https://icestreaming.rai.it/5.mp3` |
| BBC Radio 3 | GB | HLS, AAC 128 | BBC (Akamai, worldwide pool) | `http://as-hls-ww-live.akamaized.net/pool_23461179/live/ww/bbc_radio_three/bbc_radio_three.isml/bbc_radio_three-audio%3d128000.norewind.m3u8` |
| BBC Radio 4 | GB | HLS, AAC 128 | BBC (Akamai, worldwide pool) | `http://as-hls-ww-live.akamaized.net/pool_55057080/live/ww/bbc_radio_fourfm/bbc_radio_fourfm.isml/bbc_radio_fourfm-audio%3d128000.norewind.m3u8` |
| BBC World Service | GB | MP3 56 | BBC | `http://stream.live.vc.bbcmedia.co.uk/bbc_world_service` |
| RTÉ Radio 1 | IE | MP3 160 | RTÉ | `https://icecast.rte.ie/radio1` |
| RTÉ lyric fm | IE | MP3 160 | RTÉ | `https://icecast.rte.ie/lyric` |
| RTÉ 2fm | IE | MP3 160 | RTÉ | `https://icecast.rte.ie/2fm` |
| RTÉ Raidió na Gaeltachta | IE | MP3 160 | RTÉ | `https://icecast.rte.ie/rnag` |
| NPO Radio 1 | NL | MP3 192 | NPO | `https://icecast.omroep.nl/radio1-bb-mp3` |
| NPO Radio 2 | NL | MP3 192 | NPO | `https://icecast.omroep.nl/radio2-bb-mp3` |
| NPO 3FM | NL | MP3 192 | NPO | `https://icecast.omroep.nl/3fm-bb-mp3` |
| NPO Klassiek | NL | MP3 192 | NPO | `https://icecast.omroep.nl/radio4-bb-mp3` |
| VRT Radio 1 | BE | MP3 128 | VRT | `http://icecast.vrtcdn.be/radio1-high.mp3` |
| Klara | BE | MP3 128 | VRT | `http://icecast.vrtcdn.be/klara-high.mp3` |
| Studio Brussel | BE | MP3 128 | VRT | `http://icecast.vrtcdn.be/stubru-high.mp3` |
| MNM | BE | MP3 128 | VRT | `http://icecast.vrtcdn.be/mnm-high.mp3` |
| La Première | BE | MP3 160 | RTBF | `https://radios.rtbf.be/laprem1ere-128.mp3` |
| Musiq3 | BE | MP3 320 | RTBF | `https://radios.rtbf.be/musiq3-128.mp3` |
| Classic 21 | BE | MP3 160 | RTBF | `https://radios.rtbf.be/classic21-128.mp3` |
| SRF 1 | CH | MP3 128 | SRG SSR | `http://stream.srg-ssr.ch/m/drs1/mp3_128` |
| SRF 2 Kultur | CH | MP3 128 | SRG SSR | `http://stream.srg-ssr.ch/m/drs2/mp3_128` |
| RTS La 1ère | CH | MP3 128 | SRG SSR | `http://stream.srg-ssr.ch/m/la-1ere/mp3_128` |
| RTS Espace 2 | CH | MP3 128 | SRG SSR | `http://stream.srg-ssr.ch/m/espace-2/mp3_128` |
| RSI Rete Due | CH | MP3 128 | SRG SSR | `http://stream.srg-ssr.ch/m/retedue/mp3_128` |
| Radio Swiss Classic | CH | MP3 128 | SRG SSR | `http://stream.srg-ssr.ch/m/rsc_de/mp3_128` |
| Radio Swiss Jazz | CH | MP3 128 | SRG SSR | `http://stream.srg-ssr.ch/m/rsj/mp3_128` |
| Ö1 | AT | MP3 192 | ORF (ORS) | `https://orf-live.ors-shoutcast.at/oe1-q2a` |
| FM4 | AT | MP3 192 | ORF (ORS) | `https://orf-live.ors-shoutcast.at/fm4-q2a` |
| Hitradio Ö3 | AT | MP3 192 | ORF (ORS) | `https://orf-live.ors-shoutcast.at/oe3-q2a` |
| DR P1 | DK | MP3 128 | DR | `https://live-icy.dr.dk/A/A03H.mp3` |
| DR P2 Klassisk | DK | MP3 128 | DR | `https://live-icy.dr.dk/A/A04H.mp3` |
| DR P3 | DK | MP3 128 | DR | `https://live-icy.dr.dk/A/A05H.mp3` |
| DR P8 Jazz | DK | MP3 128 | DR | `https://live-icy.dr.dk/A/A22H.mp3` |
| NRK P1 | NO | MP3 192 | NRK | `https://lyd.nrk.no/nrk_radio_p1_ostlandssendingen_mp3_h` |
| NRK P2 | NO | MP3 192 | NRK | `https://lyd.nrk.no/nrk_radio_p2_mp3_h` |
| NRK Klassisk | NO | MP3 192 | NRK | `https://lyd.nrk.no/nrk_radio_klassisk_mp3_h` |
| NRK Jazz | NO | MP3 192 | NRK | `https://lyd.nrk.no/nrk_radio_jazz_mp3_h` |
| Sveriges Radio P1 | SE | MP3 128 | Sveriges Radio | `https://sverigesradio.se/topsy/direkt/srapi/132.mp3` |
| Sveriges Radio P2 | SE | MP3 224 | Sveriges Radio | `https://sverigesradio.se/topsy/direkt/srapi/163.mp3` |
| Sveriges Radio P3 | SE | MP3 128 | Sveriges Radio | `https://sverigesradio.se/topsy/direkt/srapi/164.mp3` |
| Yle Radio 1 | FI | AAC 128 | Yle | `https://icecast.live.yle.fi/radio/YleRadio1/icecast.audio` |
| Yle Klassinen | FI | AAC 128 | Yle | `https://icecast.live.yle.fi/radio/YleKlassinen/icecast.audio` |
| YleX | FI | AAC 128 | Yle | `https://icecast.live.yle.fi/radio/YleX/icecast.audio` |
| RÚV Rás 1 | IS | HLS, AAC 320 | RÚV (Akamai) | `https://ruv-radio-live.akamaized.net/streymi/ras1/ras1/ras1.m3u8` |
| RÚV Rás 2 | IS | HLS, AAC 320 | RÚV (Akamai) | `https://ruv-radio-live.akamaized.net/streymi/ras2/ras2/ras2.m3u8` |
| Polskie Radio Jedynka | PL | HLS, AAC 192 | Polskie Radio | `https://stream11.polskieradio.pl/pr1/pr1.sdp/playlist.m3u8` |
| Polskie Radio Trójka | PL | HLS, AAC 192 | Polskie Radio | `https://stream13.polskieradio.pl/pr3/pr3.sdp/playlist.m3u8` |
| ČRo Radiožurnál | CZ | MP3 128 | Český rozhlas | `https://rozhlas.stream/radiozurnal_mp3_128.mp3` |
| ČRo Vltava | CZ | MP3 128 | Český rozhlas | `https://rozhlas.stream/vltava_mp3_128.mp3` |
| ČRo Radio Wave | CZ | MP3 128 | Český rozhlas | `https://rozhlas.stream/wave_mp3_128.mp3` |
| Rádio Devín | SK | MP3 128 | RTVS | `https://icecast.stv.livebox.sk/devin_128.mp3` |
| Bartók Rádió | HU | MP3 320 | MTVA (Connectmedia CDN) | `https://mr-stream.connectmedia.hu/4741/mr3.mp3` |
| Kossuth Rádió | HU | MP3 192 | MTVA (Connectmedia CDN) | `https://icast.connectmedia.hu/4736/mr1.mp3` |
| Radio Slovenija Ars | SI | AAC 192 | RTV Slovenija | `https://mp3.rtvslo.si/ars` |
| Radio Slovenija Prvi | SI | AAC 192 | RTV Slovenija | `https://mp3.rtvslo.si/ra1` |
| HRT HR3 | HR | MP3 128 | HRT (StreamTheWorld, HRT player) | `https://playerservices.streamtheworld.com/api/livestream-redirect/PROGRAM3.mp3` |
| Klassikaraadio | EE | MP3 128 | ERR | `https://icecast.err.ee/klassikaraadio.mp3` |
| Vikerraadio | EE | MP3 128 | ERR | `https://icecast.err.ee/vikerraadio.mp3` |
| ERT Proto | GR | MP3 256 | ERT | `https://radiostreaming.ert.gr/ert-proto` |
| ERT Trito | GR | MP3 256 | ERT | `https://radiostreaming.ert.gr/ert-trito` |

## Tests

`bash tests/radio_test.sh` (WSL, no device):

1. `tests/radio_test.c` (ASan/UBSan): Radio Browser JSON, station library, helper state and
   settings, `.m3u`/`.pls`, ICY metadata, chunked bodies, MP3/ADTS frame sync; HLS playlists
   (master and media, sequence tracking, the start at the lead, gaps and restarts, long windows,
   encrypted/fMP4); MPEG-TS demux of a real 1.4 s RNE Radio 3 segment
   (`tests/radio_samples/rne_r3_seg.ts`, checked against an independent reference) and of
   synthetic packets (PES headers across packets, padding, other PIDs, LATM, joining mid-PES);
   ID3 tags; frame-duration counting; M3U/PLS/JSON station lists; sources detection and
   `sources.tsv`; every entry of the European list.
2. Every `tr()` key of the app in the 13 languages (keys, `%d`, glyphs).
3. `tests/radio_helper_test.sh`: the stream helper end to end against a local fake server
   (`tests/radio_helper_server.py`: Icecast-style streams and a live HLS server whose playlist
   advances with the clock, with numbered frames) and a fake player (`tests/radio_fake_player.c`,
   which can stop at the end of the file like the stock engine): lead before the announcement and
   never caught up, clean restart after an underrun, HLS master/media/keep-alive/order of frames,
   packed audio with ID3, gzip playlists, refused streams, session limit, no SD card, cleanup on
   stop, on SIGTERM and when the shell dies.
4. `tests/radio_render.sh`: frames of the app (including the Sources page and the European list)
   in 7 languages, written to `out/radio_render/`, with touch-zone checks.
