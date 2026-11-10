# Radio

The Radio app plays internet radio stations on the HiBy R3 II through the stock player engine.
It opens from the **Radio** icon on the home screen.

## What it does

- **Explore**: the most listened stations, the stations of a country (by default the country of the
  system language; tap *Change Country…* to pick another one) and 22 genres (Pop, Rock, News,
  Classical, Jazz…).
- **Search**: tap the search bar and type a station name; results appear as you type.
- **Favourites**: tap the star on any station. The Favourites tab also shows the last 12 stations you
  listened to (*Recents*) and your own stations.
- **My Stations**: *Add Station…* asks for a stream URL (`http://` is added if missing) and a name.
  Tap the `…` button of an own station to rename it, change its URL or delete it.
- **Playback**: tap a station. The card at the bottom shows its state: *Connecting…*,
  *Buffering… N %*, then the song title the station sends (or *Live*), or an error
  (*No connection*, *Station unavailable*, *Unsupported format*, *The player is not responding*).
  The button on the right stops it or plays it again.
- Radio keeps playing when you leave the app (volume, EQ, Bluetooth and the hardware buttons work as
  with any song). Playing any local music stops the radio by itself.

Gestures are the same as in the other HiByOS apps: swipe in from either edge to go back, swipe up
from the bottom bar to go home.

## The directory

Stations come from [Radio Browser](https://www.radio-browser.info/), a free, community-run
directory with an open API that needs no account or key. HiByOS follows its client guidelines:

- it identifies itself with the User-Agent `HiByOS-Radio/1.0 (https://github.com/Grohle/hibyos)`;
- it reports every station you start to the directory's click counter (`/json/url/<station id>`),
  which is how the directory ranks popular stations;
- if a server does not answer, it tries the next one (`de1`, `fi1`, `de2` `.api.radio-browser.info`).

Only stations that the directory currently sees working and that stream MP3 or AAC are listed (see *Limits*). Streams,
names and logos belong to their broadcasters; HiByOS only plays what the station publishes.

## Where data is stored

Everything stays on the player; nothing is sent anywhere except the directory requests above and
the stream itself.

| File | Contents |
| --- | --- |
| `/usr/data/hibymod/appdata/radio/stations.tsv` | favourites (`F`), recents (`R`) and own stations (`C`), one per line, tab separated: kind, directory id, name, URL, country, codec, kbit/s, tags |
| `/usr/data/hibymod/appdata/radio/config` | chosen country, last tab, how the player accepts the stream path, and advanced settings (`dir`, `presize_mb`, `prebuf_kb`) |
| `/tmp/hibyradio/` | while playing: the stream file and a small state file (RAM, deleted on stop) |

## How playback works

The stock player engine can only open files, not URLs. So when you start a station:

1. The shell starts the helper `hibynet radio URL DIR NAME`. It follows redirects, `.m3u` and `.pls`
   playlists (trying each entry) and Shoutcast's `ICY 200 OK`, asks for ICY metadata (song titles),
   removes it from the audio, and waits for three consecutive MP3 or AAC (ADTS) frames so the file
   starts on a whole frame.
2. It writes the audio to `/tmp/hibyradio/<station>.mp3` (or `.aac`). The file is created sparse with
   a large logical size; the engine reads it with ordinary reads, so it sees the audio as it arrives.
3. After about 8 seconds of audio the shell hands the file to the player as a one-song list (the same
   mechanism Vinyl and Audiobooks use for files outside the library).
4. The helper watches the player's read position (`/proc/<player>/fdinfo`) and:
   - frees the part already played (hole punching), so the file uses only a few hundred KB of RAM;
   - stops downloading when 1 MB is waiting unread (the player is paused) and reconnects later;
   - if the player catches up with the download (network too slow), starts a new file with a
     longer buffer and the shell gives it to the player;
   - reconnects automatically when the connection drops (up to 6 attempts);
   - exits and deletes its files when the player closes the file or opens other music, when the
     shell stops it, or if the shell dies.

The tmpfs that holds `/tmp` shares the device's RAM, so the helper keeps its file small: at most
about 1.2 MB is held at any time (1 MB ahead of the player plus 192 KB behind it).

## Limits

- **Formats**: MP3 and AAC (ADTS) streams only. HLS (`.m3u8`), Ogg Vorbis/Opus and FLAC streams are
  hidden from the directory lists and refused if added by hand (*Unsupported format*).
- Pausing for a long time and resuming plays the buffered minute first, then the live stream.
- The player shows a long duration and a seek bar for the stream; seeking is not meaningful.
- A slow connection can cause a short gap while a new buffer fills.
- The directory server list is fixed in the app (it does not use the `all.api.radio-browser.info`
  DNS lookup); if all three servers change, the app needs an update.
- Country names in the country list are the directory's (English) names.
- At most 120 favourites, 40 own stations and 240 stations per directory list.

## Tests

`bash tests/radio_test.sh` (WSL, no device): parsers and library (`tests/radio_test.c`), the texts in
the 13 languages, the stream helper end to end against a local fake station and a fake player
(`tests/radio_helper_test.sh`) and rendered frames of the app (`tests/radio_render.sh`, written to
`out/radio_render/`).
