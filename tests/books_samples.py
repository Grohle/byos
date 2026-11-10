#!/usr/bin/env python3
"""books_samples.py DIR - genera libros de prueba (dominio publico) para tests/books_test.c.

  alice_gutenberg.txt  UTF-8, partido a ~70 columnas con lineas en blanco (reflujo), cabeceras CHAPTER
  quijote_utf16le.txt  UTF-16LE con BOM
  quijote_utf16be.txt  UTF-16BE con BOM
  quijote_cp1252.txt   latin-1/CP1252 sin BOM
  lunyu_gbk.txt        GBK (Analectas de Confucio), cabeceras 第N章
  chekhov_cp1251.txt   CP1251 (ruso)
  big_utf8.txt         ~600 KB (varios bloques de 64 KB)
  alice3.epub          EPUB3: nav, portada PNG (cover-image), 4 capitulos (deflate y stored),
                       cursiva/negrita/entidades/ids, un capitulo grande (~300 KB) para el inflate
  alice2.epub          EPUB2: NCX, meta name=cover, rutas con ../ y %20
  broken.epub          zip truncado (no debe colgar ni romper nada)
"""
import os, sys, zipfile, zlib, random, struct

D = sys.argv[1]
os.makedirs(D, exist_ok=True)

ALICE = [
    ("CHAPTER I. Down the Rabbit-Hole", [
        "Alice was beginning to get very tired of sitting by her sister on the bank, and of having nothing to do: once or twice she had peeped into the book her sister was reading, but it had no pictures or conversations in it, “and what is the use of a book,” thought Alice “without pictures or conversations?”",
        "So she was considering in her own mind (as well as she could, for the hot day made her feel very sleepy and stupid), whether the pleasure of making a daisy-chain would be worth the trouble of getting up and picking the daisies, when suddenly a White Rabbit with pink eyes ran close by her.",
        "There was nothing so very remarkable in that; nor did Alice think it so very much out of the way to hear the Rabbit say to itself, “Oh dear! Oh dear! I shall be late!”",
    ]),
    ("CHAPTER II. The Pool of Tears", [
        "“Curiouser and curiouser!” cried Alice (she was so much surprised, that for the moment she quite forgot how to speak good English); “now I’m opening out like the largest telescope that ever was! Good-bye, feet!”",
        "Just then her head struck against the roof of the hall: in fact she was now more than nine feet high, and she at once took up the little golden key and hurried off to the garden door.",
    ]),
]
QUIJOTE = ("Capítulo primero\n\nEn un lugar de la Mancha, de cuyo nombre no quiero acordarme, no ha mucho tiempo que vivía "
           "un hidalgo de los de lanza en astillero, adarga antigua, rocín flaco y galgo corredor.\n"
           "Una olla de algo más vaca que carnero, salpicón las más noches, duelos y quebrantos los sábados, "
           "lantejas los viernes, algún palomino de añadidura los domingos, consumían las tres partes de su hacienda.\n")
LUNYU = ("第一章 学而\n"
         "　　子曰：“学而时习之，不亦说乎？有朋自远方来，不亦乐乎？人不知而不愠，不亦君子乎？”\n"
         "　　曾子曰：“吾日三省吾身：为人谋而不忠乎？与朋友交而不信乎？传不习乎？”\n"
         "第二章 为政\n"
         "　　子曰：“为政以德，譬如北辰，居其所而众星共之。”\n"
         "　　子曰：“温故而知新，可以为师矣。”\n")
CHEKHOV = ("Глава первая\n"
           "Все счастливые семьи похожи друг на друга, каждая несчастливая семья несчастлива по-своему.\n"
           "Всё смешалось в доме Облонских. Жена узнала, что муж был в связи с бывшею в их доме француженкою-гувернанткой.\n")


def wrap(t, n=70):
    out, line = [], ""
    for w in t.split(" "):
        if line and len(line) + 1 + len(w) > n:
            out.append(line); line = w
        else:
            line = (line + " " + w) if line else w
    if line: out.append(line)
    return out


def w(name, data):
    with open(os.path.join(D, name), "wb") as f: f.write(data)


# --- TXT
lines = ["The Project Gutenberg eBook of Alice's Adventures in Wonderland", "", ""]
for title, paras in ALICE:
    lines += [title, ""]
    for p in paras: lines += wrap(p) + [""]
    lines += [""]
w("alice_gutenberg.txt", ("\r\n".join(lines) + "\r\n").encode("utf-8"))
w("quijote_utf16le.txt", b"\xff\xfe" + QUIJOTE.encode("utf-16-le"))
w("quijote_utf16be.txt", b"\xfe\xff" + QUIJOTE.encode("utf-16-be"))
w("quijote_cp1252.txt", QUIJOTE.replace("“", "\"").encode("cp1252"))
w("lunyu_gbk.txt", LUNYU.encode("gbk"))
w("chekhov_cp1251.txt", CHEKHOV.encode("cp1251"))
random.seed(7)
WORDS = "the of and to a in that it was he for on are with as his they at be this from have or by one had not but what all were when we there can an your which their said".split()
big = []
for ch in range(12):
    big.append("Chapter %d" % (ch + 1))
    for p in range(60):
        big.append(" ".join(random.choice(WORDS) for _ in range(random.randint(30, 120))).capitalize() + ".")
w("big_utf8.txt", ("\n".join(big) + "\n").encode("utf-8"))

# --- EPUB
PNG = None
def png(wd, ht, rgb):
    raw = b"".join(b"\x00" + bytes(rgb) * wd for _ in range(ht))
    def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", wd, ht, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")

def xhtml(title, body):
    return ('<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE html>\n<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">\n'
            '<head><title>%s</title><style>p { text-indent: 1em; }</style><script>var x = "<p>no</p>";</script></head>\n<body>\n%s\n</body></html>\n' % (title, body)).encode("utf-8")

def chap(i):
    t, ps = ALICE[i]
    body = '<h2 id="c%d">%s</h2>\n' % (i + 1, t)
    for k, p in enumerate(ps):
        p = p.replace("&", "&amp;")
        if k == 1: p = p.replace("White Rabbit", "<i>White</i> <b>Rabbit</b>")
        body += '<p id="p%d_%d">%s</p>\n' % (i + 1, k, p)
    return body

def bigchap():
    random.seed(11)
    body = '<h1>A Very Long Chapter</h1>\n'
    for k in range(1500):
        body += "<p>%s.</p>\n" % " ".join(random.choice(WORDS) for _ in range(random.randint(10, 60)))
        if k == 777: body += '<p id="mid">MIDDLE MARKER &mdash; caf&#233; &#x4E2D; &nbsp;x &unknown; 5 &lt; 6</p>\n'
    body += "<p>THE END OF THE BIG CHAPTER</p>"
    return body

def make_epub3(path):
    z = zipfile.ZipFile(path, "w")
    z.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip", compress_type=zipfile.ZIP_STORED)
    z.writestr("META-INF/container.xml", '<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>', compress_type=zipfile.ZIP_DEFLATED)
    opf = '''<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">
 <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
  <dc:title>Alice&apos;s Adventures in Wonderland</dc:title>
  <dc:creator id="a">Lewis Carroll</dc:creator>
 </metadata>
 <manifest>
  <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
  <item id="img" href="images/cover.png" media-type="image/png" properties="cover-image"/>
  <item id="cov" href="text/cover.xhtml" media-type="application/xhtml+xml"/>
  <item id="c1" href="text/ch1.xhtml" media-type="application/xhtml+xml"/>
  <item id="c2" href="text/ch2.xhtml" media-type="application/xhtml+xml"/>
  <item id="c3" href="text/big.xhtml" media-type="application/xhtml+xml"/>
 </manifest>
 <spine><itemref idref="cov"/><itemref idref="c1"/><itemref idref="c2"/><itemref idref="c3"/></spine>
</package>'''
    z.writestr("OEBPS/content.opf", opf, compress_type=zipfile.ZIP_DEFLATED)
    nav = xhtml("Contents", '<nav epub:type="landmarks"><ol><li><a href="text/cover.xhtml">Cover</a></li></ol></nav>'
                '<nav epub:type="toc"><h1>Contents</h1><ol><li><a href="text/ch1.xhtml">Down the Rabbit-Hole</a>'
                '<ol><li><a href="text/ch1.xhtml#p1_2">The <i>Rabbit</i> speaks</a></li></ol></li>'
                '<li><a href="text/ch2.xhtml#c2">The Pool of Tears</a></li><li><a href="text/big.xhtml#mid">Middle</a></li></ol></nav>')
    z.writestr("OEBPS/nav.xhtml", nav, compress_type=zipfile.ZIP_DEFLATED)
    z.writestr("OEBPS/images/cover.png", png(60, 90, (200, 40, 40)), compress_type=zipfile.ZIP_STORED)
    z.writestr("OEBPS/text/cover.xhtml", xhtml("Cover", '<div><img src="../images/cover.png" alt="Cover"/></div>'), compress_type=zipfile.ZIP_DEFLATED)
    z.writestr("OEBPS/text/ch1.xhtml", xhtml("I", chap(0)), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    z.writestr("OEBPS/text/ch2.xhtml", xhtml("II", chap(1)), compress_type=zipfile.ZIP_STORED)
    z.writestr("OEBPS/text/big.xhtml", xhtml("Big", bigchap()), compress_type=zipfile.ZIP_DEFLATED, compresslevel=1)
    z.close()

def make_epub2(path):
    z = zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED)
    z.writestr("mimetype", "application/epub+zip")
    z.writestr("META-INF/container.xml", '<container><rootfiles><rootfile full-path="book/OPS/package.opf"/></rootfiles></container>')
    opf = '''<package version="2.0"><metadata><dc:title>Don Quijote</dc:title><dc:creator opf:role="aut">Miguel de Cervantes</dc:creator>
<meta name="cover" content="cv"/></metadata>
<manifest><item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
<item id="cv" href="../img/portada%20q.jpg" media-type="image/jpeg"/>
<item id="t1" href="Text/cap%201.html" media-type="application/xhtml+xml"/></manifest>
<spine toc="ncx"><itemref idref="t1"/></spine></package>'''
    z.writestr("book/OPS/package.opf", opf)
    ncx = '''<ncx><docTitle><text>Don Quijote</text></docTitle><navMap>
<navPoint id="n1"><navLabel><text>Capítulo primero</text></navLabel><content src="Text/cap%201.html"/>
<navPoint id="n2"><navLabel><text>La olla</text></navLabel><content src="Text/cap%201.html#olla"/></navPoint></navPoint>
</navMap></ncx>'''
    z.writestr("book/OPS/toc.ncx", ncx)
    z.writestr("book/img/portada q.jpg", b"\xff\xd8\xff\xe0 not really a jpeg")
    body = "<html><head><title>x</title></head><body><h1>Capítulo primero</h1>"
    for i, para in enumerate(QUIJOTE.split("\n")[2:]):
        if para: body += '<p%s>%s</p>' % (' id="olla"' if para.startswith("Una olla") else "", para)
    body += "<p>Verso uno<br/>verso dos<br/><br/>Estrofa dos</p><pre>linea 1\nlinea 2</pre><ul><li>uno</li><li>dos</li></ul><hr/><p>fin</p></body></html>"
    z.writestr("book/OPS/Text/cap 1.html", body.encode("utf-8"))
    z.close()

make_epub3(os.path.join(D, "alice3.epub"))
make_epub2(os.path.join(D, "alice2.epub"))
data = open(os.path.join(D, "alice3.epub"), "rb").read()
w("broken.epub", data[: len(data) // 2] + data[-200:])          # directorio central intacto, datos rotos
w("expected_big.txt", bigchap().encode("utf-8"))
print("ok", D)
