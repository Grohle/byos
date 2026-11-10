#!/usr/bin/env python3
"""Local fake radio server for tests/radio_helper_test.sh (no Internet, no device).

Routes (raw sockets, so the replies can be exactly what real stations send):
  /icy          "ICY 200 OK" Shoutcast reply, audio/mpeg 128 kbit/s, icy-metaint 8000, the title
                changes every 3 s; the body starts with junk before the first frame (sync test)
  /redir        302 to /icy
  /list.pls     .pls playlist: a dead entry (/dead -> 404) then /icy
  /list.m3u     .m3u playlist pointing at /aac
  /aac          HTTP/1.1 chunked ADTS (AAC) stream, no ICY metadata
  /hls.m3u8     an HLS playlist (must be refused)
  /ogg          an Ogg stream (must be refused)
  /dead         404
  /drop         MP3 that closes the connection after ~3 s (reconnect test)
Usage: radio_helper_server.py PORT
"""
import socket
import socketserver
import sys
import threading
import time

RATE = 16000                     # bytes per second (128 kbit/s)


def mp3_frame(pad):
    # MPEG-1 Layer III, 128 kbit/s, 44100 Hz; payload without 0xFF so it never fakes a sync word
    n = 144000 * 128 // 44100 + pad
    return bytes([0xFF, 0xFB, 0x90 | (pad << 1), 0x64]) + bytes(((i * 7) % 255) or 1 for i in range(4, n))


def adts_frame(n=372):
    return bytes([0xFF, 0xF1, 0x50, 0x80 | (n >> 11), (n >> 3) & 0xFF, ((n & 7) << 5) | 0x1F, 0xFC]) + b"\x22" * (n - 7)


MP3 = b"".join(mp3_frame(i % 2) for i in range(64))
AAC = adts_frame() * 64


def paced(sock, data_iter, seconds=None):
    """send audio at RATE (a 3 s burst first, like real servers); stop after `seconds`"""
    t0 = time.time()
    sent = 0
    for chunk in data_iter:
        if seconds is not None and time.time() - t0 > seconds:
            return
        sock.sendall(chunk)
        sent += len(chunk)
        ahead = sent / RATE - (time.time() - t0) - 3.0
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


class H(socketserver.BaseRequestHandler):
    def handle(self):
        s = self.request
        s.settimeout(30)
        req = b""
        while b"\r\n\r\n" not in req:
            d = s.recv(4096)
            if not d:
                return
            req += d
        line = req.split(b"\r\n", 1)[0].decode()
        path = line.split(" ")[1]
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
            elif path == "/hls.m3u8":
                body = b"#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:6\n#EXTINF:6,\nseg1.aac\n"
                s.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/vnd.apple.mpegurl\r\n\r\n" + body)
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
