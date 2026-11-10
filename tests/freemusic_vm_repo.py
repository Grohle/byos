#!/usr/bin/env python3
"""freemusic_vm_repo.py OUTDIR BASEURL : local free-music repository for the Vinyl VM test.

Writes into OUTDIR (served with `python3 -m http.server`):
  vinyl.json            a Vinyl catalog (docs/FREEMUSIC.md) with 3 albums: hi-res FLAC 24/96 +
                        WAV, CD FLAC 16/44.1, WAV only; relative URLs (resolved by the app)
  covers/*.png          generated covers (hibynet cover-get decodes PNG)
  audio/<album>/*.flac  real FLAC files (VERBATIM subframes: valid, a few seconds of tones)
  audio/<album>/*.wav   real WAV files
  feed.xml              an RSS 2.0 podcast whose episodes are the FLAC/WAV files
  feeds.opml            an OPML list pointing at feed.xml
Only the standard library is used (no PIL, no flac/ffmpeg)."""
import json, math, os, struct, sys, zlib

OUT = sys.argv[1] if len(sys.argv) > 1 else 'vinylrepo'
BASE = (sys.argv[2] if len(sys.argv) > 2 else 'http://10.0.2.2:8771').rstrip('/')


def png(path, w, h, f):
    rows = bytearray()
    for y in range(h):
        rows.append(0)
        for x in range(w):
            rows += bytes(f(x, y))
    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    data = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(bytes(rows), 9)) + chunk(b'IEND', b'')
    open(path, 'wb').write(data)


def cover(path, c0, c1, kind):
    def f(x, y):
        t = (x + y) / 798.0
        r, g, b = [int(c0[i] * (1 - t) + c1[i] * t) for i in range(3)]
        cx, cy = x - 200, y - 200
        d = math.hypot(cx, cy)
        if kind == 0 and abs(d - 120) < 6 or kind == 0 and d < 18:          # a record
            r, g, b = 250, 250, 250
        if kind == 1 and (int(d) // 22) % 2 == 0 and d < 170:               # rings
            r, g, b = min(255, r + 70), min(255, g + 70), min(255, b + 70)
        if kind == 2 and abs(cy - 60 * math.sin(cx / 40.0)) < 10:          # a wave
            r, g, b = 255, 230, 120
        return (r, g, b)
    png(path, 400, 400, f)


def crc8(d):
    c = 0
    for b in d:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xff if c & 0x80 else (c << 1) & 0xff
    return c


def crc16(d):
    c = 0
    for b in d:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x8005) & 0xffff if c & 0x8000 else (c << 1) & 0xffff
    return c


class Bits:
    def __init__(self):
        self.v = 0; self.n = 0; self.out = bytearray()
    def put(self, val, n):
        for i in range(n - 1, -1, -1):
            self.v = (self.v << 1) | ((val >> i) & 1); self.n += 1
            if self.n == 8:
                self.out.append(self.v); self.v = 0; self.n = 0
    def align(self):
        while self.n:
            self.put(0, 1)


def utf8num(n):
    if n < 0x80: return bytes([n])
    if n < 0x800: return bytes([0xC0 | n >> 6, 0x80 | n & 63])
    if n < 0x10000: return bytes([0xE0 | n >> 12, 0x80 | (n >> 6) & 63, 0x80 | n & 63])
    return bytes([0xF0 | n >> 18, 0x80 | (n >> 12) & 63, 0x80 | (n >> 6) & 63, 0x80 | n & 63])


def samples(rate, secs, freq, bits):
    amp = (1 << (bits - 1)) * 0.3
    n = int(rate * secs)
    out = []
    for i in range(n):
        env = min(1.0, i / (rate * 0.05), (n - i) / (rate * 0.05))
        l = int(amp * env * math.sin(2 * math.pi * freq * i / rate))
        r = int(amp * env * math.sin(2 * math.pi * freq * 1.5 * i / rate))
        out.append((l, r))
    return out


def flac(path, rate, bits, secs, freq):
    """FLAC with VERBATIM subframes, 4096-sample blocks, 2 independent channels."""
    s = samples(rate, secs, freq, bits)
    bs = 4096
    rate_code = {44100: 9, 48000: 10, 96000: 11}[rate]
    bits_code = {16: 4, 24: 6}[bits]
    frames = bytearray(); minf = 1 << 30; maxf = 0
    for fi, off in enumerate(range(0, len(s), bs)):
        blk = s[off:off + bs]
        b = Bits()
        b.put(0x3FFE, 14); b.put(0, 1); b.put(0, 1)                       # sync, reserved, fixed block size
        bcode = 12 if len(blk) == bs else 7                               # 4096 / 16-bit (n-1) at the end
        b.put(bcode, 4); b.put(rate_code, 4); b.put(1, 4)                 # 2 channels: left, right
        b.put(bits_code, 3); b.put(0, 1)
        for byte in utf8num(fi): b.put(byte, 8)
        if bcode == 7: b.put(len(blk) - 1, 16)
        head = bytes(b.out); b.put(crc8(head), 8)
        for ch in range(2):
            b.put(0, 1); b.put(1, 6); b.put(0, 1)                         # VERBATIM, no wasted bits
            for smp in blk:
                b.put(smp[ch] & ((1 << bits) - 1), bits)
        b.align()
        fr = bytes(b.out)
        fr += struct.pack('>H', crc16(fr))
        frames += fr; minf = min(minf, len(fr)); maxf = max(maxf, len(fr))
    si = Bits()
    si.put(bs, 16); si.put(bs, 16); si.put(minf, 24); si.put(maxf, 24)
    si.put(rate, 20); si.put(1, 3); si.put(bits - 1, 5); si.put(len(s), 36)
    streaminfo = bytes(si.out) + b'\0' * 16                               # MD5 unknown (allowed)
    data = b'fLaC' + bytes([0x80]) + struct.pack('>I', len(streaminfo))[1:] + streaminfo + bytes(frames)
    open(path, 'wb').write(data)
    return len(data), len(s) / rate


def wav(path, rate, bits, secs, freq):
    s = samples(rate, secs, freq, bits)
    pcm = bytearray()
    for l, r in s:
        pcm += struct.pack('<hh', l, r)
    hdr = b'RIFF' + struct.pack('<I', 36 + len(pcm)) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 1, 2, rate, rate * 4, 4, 16) + b'data' + struct.pack('<I', len(pcm))
    open(path, 'wb').write(hdr + pcm)
    return len(hdr) + len(pcm), len(s) / rate


os.makedirs(OUT + '/covers', exist_ok=True)
albums = []
spec = [
    ('vl001', 'Polar Lights', 'Aurora Fields', 2024, ['ambient', 'electronic'], 'en', 'https://creativecommons.org/licenses/by/4.0/',
     ((40, 60, 140), (10, 200, 200), 0), 96000, 24, [('Polar Night', 220), ('Solar Wind', 330), ('Aurora', 440)]),
    ('vl002', 'Café de Medianoche', 'Los Ejemplos', 2019, ['jazz'], 'es', 'https://creativecommons.org/licenses/by-sa/4.0/',
     ((120, 40, 20), (240, 170, 60), 1), 44100, 16, [('Primero', 262), ('Segundo', 294)]),
    ('vl003', 'Field Recordings', 'Aurora Fields', 2021, ['experimental'], 'en', 'https://creativecommons.org/publicdomain/zero/1.0/',
     ((20, 80, 30), (180, 220, 90), 2), 44100, 16, [('Rain', 330), ('Wind', 392)]),
]
items = []
for aid, title, artist, year, genres, lang, lic, cv, rate, bits, tracks in spec:
    os.makedirs('%s/audio/%s' % (OUT, aid), exist_ok=True)
    cover('%s/covers/%s.png' % (OUT, aid), cv[0], cv[1], cv[2])
    tr = []
    for n, (tt, fq) in enumerate(tracks, 1):
        files = []; fdur = None
        if aid != 'vl003':
            fn = 'audio/%s/%02d.flac' % (aid, n)
            size, dur = flac('%s/%s' % (OUT, fn), rate, bits, 3.0 if bits == 24 else 6.0, fq)
            files.append({'url': fn, 'codec': 'flac', 'bits': bits, 'rate': rate, 'channels': 2, 'size': size}); fdur = dur
        fn = 'audio/%s/%02d.wav' % (aid, n)
        size, dur = wav('%s/%s' % (OUT, fn), 44100, 16, fdur or 6.0, fq)
        dur = fdur or dur
        files.append({'url': fn, 'codec': 'wav', 'bits': 16, 'rate': 44100, 'channels': 2, 'size': size})
        tr.append({'title': tt, 'duration': round(dur, 2), 'track': n, 'files': files})
        items.append((title, artist, tt, files[0], dur, year))
    albums.append({'id': aid, 'title': title, 'artist': artist, 'year': year, 'genre': genres, 'language': lang,
                   'license': lic, 'cover': 'covers/%s.png' % aid, 'url': BASE + '/albums/' + aid,
                   'description': 'A test album of the Vinyl VM harness: generated tones, %d-bit / %g kHz.' % (bits, rate / 1000.0),
                   'tracks': tr})
cat = {'vinyl': 1, 'name': 'Vinyl Test Label', 'description': 'Local repository of the Vinyl VM test (tests/freemusic_vm_repo.py).',
       'homepage': BASE + '/', 'license': 'https://creativecommons.org/licenses/by/4.0/', 'albums': albums}
json.dump(cat, open(OUT + '/vinyl.json', 'w', encoding='utf-8'), ensure_ascii=False, indent=1)

# RSS podcast with the same files as episodes, and an OPML that lists it
ep = []
for i, (title, artist, tt, f, dur, year) in enumerate(items):
    ep.append('''  <item>
    <title>%s: %s</title>
    <pubDate>Mon, %02d Jun 2025 10:00:00 GMT</pubDate>
    <itunes:duration>%d</itunes:duration>
    <enclosure url="%s/%s" type="%s" length="%d"/>
  </item>''' % (title.replace('&', '&amp;'), tt, 20 - i, int(dur), BASE, f['url'], 'audio/flac' if f['codec'] == 'flac' else 'audio/wav', f['size']))
rss = '''<?xml version="1.0" encoding="UTF-8"?>
<rss version="2.0" xmlns:itunes="http://www.itunes.com/dtds/podcast-1.0.dtd" xmlns:creativeCommons="http://backend.userland.com/creativeCommonsRssModule">
<channel>
  <title>Vinyl Test Radio</title>
  <link>%s/</link>
  <description><![CDATA[<p>Weekly <b>test</b> episodes of the Vinyl harness.</p>]]></description>
  <itunes:author>Test Radio</itunes:author>
  <itunes:image href="%s/covers/vl002.png"/>
  <creativeCommons:license>https://creativecommons.org/licenses/by/4.0/</creativeCommons:license>
%s
</channel>
</rss>
''' % (BASE, BASE, '\n'.join(ep))
open(OUT + '/feed.xml', 'w', encoding='utf-8').write(rss)
open(OUT + '/feeds.opml', 'w', encoding='utf-8').write('''<?xml version="1.0" encoding="UTF-8"?>
<opml version="2.0"><head><title>Test feeds</title></head><body>
  <outline text="Vinyl Test Radio" type="rss" xmlUrl="feed.xml"/>
</body></opml>
''')
print('repository in %s (%s): %d albums, %d tracks' % (OUT, BASE, len(albums), len(items)))
