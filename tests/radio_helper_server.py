#!/usr/bin/env python3
"""Local fake radio server for tests/radio_helper_test.sh (no Internet, no device).

Continuous streams (raw sockets, so the replies can be exactly what real stations send):
  /icy          "ICY 200 OK" Shoutcast reply, audio/mpeg 128 kbit/s, icy-metaint 8000, the title
                changes every 3 s; the body starts with junk before the first frame (sync test)
  /redir        302 to /icy
  /list.pls     .pls playlist: a dead entry (/dead -> 404) then /icy
  /list.m3u     .m3u playlist pointing at /aac
  /aac          HTTP/1.1 chunked ADTS (AAC) stream, no ICY metadata
  /fast         MP3 at 4x the real rate (session cap test)
  /ogg          an Ogg stream (must be refused)
  /dead         404
  /drop         MP3 that closes the connection after ~3 s (reconnect test)
HLS (HTTP/1.1 with keep-alive; the live window advances with the clock: one 2 s segment every 2 s):
  /hls/master.m3u8          master: a video variant, a 256k and a 64k audio variant (-> lo/)
  /hls/lo/index.m3u8        live media playlist, 5 segments, relative URIs (every third absolute)
  /hls/lo/seg_N.ts          MPEG-TS: PAT, PMT (AAC 0x0F on PID 0x101), PES with ADTS frames
  /hls/packed/index.m3u8    the same audio as packed ADTS segments (seg_N.aac) with two ID3 tags
  /hls/gz/index.m3u8        the TS playlist, gzip-compressed (Content-Encoding: gzip)
  /hls/enc.m3u8             AES-128 encrypted (must be refused)
  /hls/fmp4.m3u8            fMP4 segments (must be refused)
  /hls/stats                "conns=N reqs=M" of the HLS requests (keep-alive check)
Every ADTS frame of the HLS audio carries its own frame number in its payload, so the test can
check that the helper wrote the frames in order, without holes or repeats.
Usage: radio_helper_server.py PORT
"""
import gzip
import socket
import socketserver
import sys
import threading
import time

RATE = 16000                     # bytes per second (128 kbit/s)
T0 = time.time()
SEG_S = 2.0                      # HLS segment length
FPS = 48000 / 1024               # AAC frames per second at 48 kHz
FRAMES_PER_SEG = 94              # 94 * 1024 / 48000 = 2.005 s
SEQ0 = 100
WINDOW = 5
STATS = {"conns": 0, "reqs": 0}


def mp3_frame(pad):
    # MPEG-1 Layer III, 128 kbit/s, 44100 Hz; payload without 0xFF so it never fakes a sync word
    n = 144000 * 128 // 44100 + pad
    return bytes([0xFF, 0xFB, 0x90 | (pad << 1), 0x64]) + bytes(((i * 7) % 255) or 1 for i in range(4, n))


def adts_frame(n=372, sr_index=4, payload=b""):
    body = payload + b"\x22" * (n - 7 - len(payload))
    return bytes([0xFF, 0xF1, 0x40 | (sr_index << 2), 0x80 | (n >> 11), (n >> 3) & 0xFF, ((n & 7) << 5) | 0x1F, 0xFC]) + body


def numbered_frame(k):
    """48 kHz AAC frame whose payload starts with k in base 200 (no 0xFF bytes)"""
    return adts_frame(200, 3, bytes([(k // 8000000) % 200, (k // 40000) % 200, (k // 200) % 200, k % 200]))


MP3 = b"".join(mp3_frame(i % 2) for i in range(64))
AAC = adts_frame() * 64


def ts_packet(pid, pusi, cc, payload):
    """one 188-byte TS packet; a short payload gets an adaptation field as padding"""
    head = bytes([0x47, (0x40 if pusi else 0) | (pid >> 8), pid & 0xFF])
    if len(payload) >= 184:
        return head + bytes([0x10 | (cc & 15)]) + payload[:184]
    alen = 183 - len(payload)
    af = bytes([alen]) + ((b"\x00" + b"\xff" * (alen - 1)) if alen else b"")
    return head + bytes([0x30 | (cc & 15)]) + af + payload


PAT = b"\x00" + bytes([0x00, 0xB0, 0x0D, 0x00, 0x01, 0xC1, 0x00, 0x00, 0x00, 0x01, 0xE1, 0x00, 0, 0, 0, 0])
PMT = b"\x00" + bytes([0x02, 0xB0, 0x12, 0x00, 0x01, 0xC1, 0x00, 0x00, 0xE1, 0x01, 0xF0, 0x00,
                       0x0F, 0xE1, 0x01, 0xF0, 0x00, 0, 0, 0, 0])


def seg_frames(seq):
    first = (seq - SEQ0) * FRAMES_PER_SEG
    return [numbered_frame(first + i) for i in range(FRAMES_PER_SEG)]


def ts_segment(seq):
    out = [ts_packet(0x11, 1, 0, b"\x00\x42\xf0\x25"), ts_packet(0, 1, 0, PAT), ts_packet(0x100, 1, 0, PMT)]
    cc = 0
    frames = seg_frames(seq)
    for i in range(0, len(frames), 8):                      # one PES every 8 frames
        es = b"".join(frames[i:i + 8])
        pes = bytes([0, 0, 1, 0xC0, 0, 0, 0x80, 0x80, 5, 0x21, 0, 1, 0, 1]) + es
        first = True
        while pes:
            out.append(ts_packet(0x101, first, cc, pes[:184]))
            pes = pes[184:]
            cc += 1
            first = False
    return b"".join(out)


def id3(owner, data):
    frame = b"PRIV" + len(owner + b"\0" + data).to_bytes(4, "big") + b"\0\0" + owner + b"\0" + data
    n = len(frame)
    return b"ID3\x04\x00\x00" + bytes([(n >> 21) & 127, (n >> 14) & 127, (n >> 7) & 127, n & 127]) + frame


def packed_segment(seq):
    return id3(b"com.apple.streaming.transportStreamTimestamp", b"\0\0\0\0\1\2\3\4") + id3(b"com.example.timestamp", b"x" * 40) + b"".join(seg_frames(seq))


def live_seq():
    return SEQ0 + WINDOW - 1 + int((time.time() - T0) / SEG_S)


def media_playlist(host, kind, ext):
    last = live_seq()
    first = last - WINDOW + 1
    lines = ["#EXTM3U", "#EXT-X-VERSION:3", "#EXT-X-TARGETDURATION:2", "#EXT-X-MEDIA-SEQUENCE:%d" % first]
    for s in range(first, last + 1):
        lines.append("#EXTINF:%.3f," % (FRAMES_PER_SEG * 1024 / 48000))
        lines.append("http://%s/hls/%s/seg_%d.%s" % (host, kind, s, ext) if s % 3 == 0 else "seg_%d.%s" % (s, ext))
    return ("\n".join(lines) + "\n").encode()


def paced(sock, data_iter, seconds=None, rate=RATE):
    """send audio at rate (a 3 s burst first, like real servers); stop after `seconds`"""
    t0 = time.time()
    sent = 0
    for chunk in data_iter:
        if seconds is not None and time.time() - t0 > seconds:
            return
        sock.sendall(chunk)
        sent += len(chunk)
        ahead = sent / rate - (time.time() - t0) - 3.0
        if ahead > 0:
            time.sleep(ahead)


def audio_stream(src):
    while True:
        for i in range(0, len(src), 2000):
            yield src[i:i + 2000]


def icy_stream(metaint):
    """audio with an ICY metadata block every metaint bytes"""
    buf = b""
    gen = audio_stream(MP3)
    k = 0
    t0 = time.time()
    while True:
        while len(buf) < metaint:
            buf += next(gen)
        out, buf = buf[:metaint], buf[metaint:]
        title = "Test Artist - Song %d" % int((time.time() - t0) // 3)
        meta = ("StreamTitle='%s';" % title).encode() if k % 2 == 0 else b""
        if meta:
            ln = (len(meta) + 15) // 16
            meta = bytes([ln]) + meta.ljust(ln * 16, b"\0")
        else:
            meta = b"\0"
        k += 1
        yield out + meta


def chunked(gen):
    for c in gen:
        yield b"%x\r\n" % len(c) + c + b"\r\n"


def reply(s, body, ctype="application/vnd.apple.mpegurl", status="200 OK", extra=b""):
    s.sendall(b"HTTP/1.1 " + status.encode() + b"\r\nContent-Type: " + ctype.encode() + b"\r\nContent-Length: %d\r\n" % len(body) + extra + b"\r\n" + body)


class H(socketserver.BaseRequestHandler):
    def handle(self):
        s = self.request
        s.settimeout(30)
        buf = b""
        counted = False
        while True:                                   # HTTP/1.1: several requests per connection
            while b"\r\n\r\n" not in buf:
                d = s.recv(4096)
                if not d:
                    return
                buf += d
            req, buf = buf.split(b"\r\n\r\n", 1)
            line = req.split(b"\r\n", 1)[0].decode()
            path = line.split(" ")[1]
            if path.startswith("/hls/"):
                if not counted:
                    STATS["conns"] += 1
                    counted = True
                STATS["reqs"] += 1
                try:
                    if not self.hls(s, path, req):
                        return
                except (BrokenPipeError, ConnectionResetError, socket.timeout):
                    return
                continue
            self.stream(s, path, req)
            return

    def hls(self, s, path, req):
        host = "127.0.0.1:%d" % self.server.server_address[1]
        if path == "/hls/master.m3u8":
            reply(s, b"#EXTM3U\n#EXT-X-VERSION:3\n"
                     b"#EXT-X-STREAM-INF:BANDWIDTH=32000,RESOLUTION=320x180,CODECS=\"avc1.42c00d,mp4a.40.2\"\nvideo/index.m3u8\n"
                     b"#EXT-X-STREAM-INF:BANDWIDTH=256000,CODECS=\"mp4a.40.2\"\nhi/index.m3u8\n"
                     b"#EXT-X-STREAM-INF:BANDWIDTH=64000,CODECS=\"mp4a.40.2\"\nlo/index.m3u8\n")
        elif path in ("/hls/lo/index.m3u8", "/hls/hi/index.m3u8"):
            reply(s, media_playlist(host, path.split("/")[2], "ts"))
        elif path == "/hls/packed/index.m3u8":
            reply(s, media_playlist(host, "packed", "aac"))
        elif path == "/hls/gz/index.m3u8":
            reply(s, gzip.compress(media_playlist(host, "lo", "ts")), extra=b"Content-Encoding: gzip\r\n")
        elif path.startswith("/hls/") and "/seg_" in path:
            seq = int(path.rsplit("seg_", 1)[1].split(".")[0])
            if seq > live_seq() or seq < live_seq() - 30:
                reply(s, b"", "text/plain", "404 Not Found")
            elif path.endswith(".aac"):
                reply(s, packed_segment(seq), "audio/aac")
            else:
                reply(s, ts_segment(seq), "video/mp2t")
        elif path == "/hls/enc.m3u8":
            reply(s, b"#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:1\n#EXT-X-KEY:METHOD=AES-128,URI=\"k.bin\"\n#EXTINF:2,\nlo/seg_100.ts\n")
        elif path == "/hls/fmp4.m3u8":
            reply(s, b"#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:1\n#EXT-X-MAP:URI=\"init.mp4\"\n#EXTINF:2,\ns1.m4s\n")
        elif path == "/hls/stats":
            reply(s, ("conns=%d reqs=%d\n" % (STATS["conns"], STATS["reqs"])).encode(), "text/plain")
        else:
            reply(s, b"", "text/plain", "404 Not Found")
        return True

    def stream(self, s, path, req):
        icy = b"icy-metadata: 1" in req.lower()
        host = "127.0.0.1:%d" % self.server.server_address[1]
        try:
            if path == "/icy":
                hdr = b"ICY 200 OK\r\nicy-name: Test FM\r\nicy-br: 128\r\nContent-Type: audio/mpeg\r\n"
                if icy:
                    hdr += b"icy-metaint: 8000\r\n"
                s.sendall(hdr + b"\r\n")
                junk = bytes((i * 31) % 250 + 1 for i in range(2000))
                s.sendall(junk + MP3[417:417 * 2][:100])   # junk, then a cut frame
                paced(s, icy_stream(8000) if icy else audio_stream(MP3))
            elif path == "/redir":
                s.sendall(("HTTP/1.1 302 Found\r\nLocation: http://%s/icy\r\nContent-Length: 0\r\n\r\n" % host).encode())
            elif path == "/list.pls":
                body = ("[playlist]\nNumberOfEntries=2\nFile1=http://%s/dead\nFile2=http://%s/icy\nVersion=2\n" % (host, host)).encode()
                s.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: audio/x-scpls\r\nContent-Length: %d\r\n\r\n" % len(body) + body)
            elif path == "/list.m3u":
                body = ("#EXTM3U\n#EXTINF:-1,AAC\nhttp://%s/aac\n" % host).encode()
                s.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: audio/x-mpegurl\r\n\r\n" + body)
            elif path == "/aac":
                s.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: audio/aacp\r\nTransfer-Encoding: chunked\r\n\r\n")
                paced(s, chunked(audio_stream(AAC)))
            elif path == "/fast":
                s.sendall(b"HTTP/1.0 200 OK\r\nContent-Type: audio/mpeg\r\n\r\n")
                paced(s, audio_stream(MP3), rate=RATE * 4)
            elif path == "/ogg":
                s.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/ogg\r\n\r\n" + b"OggS\0\2" + b"\0" * 4000)
            elif path == "/drop":
                s.sendall(b"HTTP/1.0 200 OK\r\nContent-Type: audio/mpeg\r\n\r\n")
                paced(s, audio_stream(MP3), seconds=3)
            else:
                s.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")
        except (BrokenPipeError, ConnectionResetError, socket.timeout):
            pass


class S(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    srv = S(("127.0.0.1", int(sys.argv[1])), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass
