#!/usr/bin/env python3
"""tags_mutagen_check.py - comprobacion cruzada (opcional) de los ficheros que deja tags_test con
una libreria de etiquetas independiente (mutagen). Si mutagen no esta, no hace nada.
Uso: python3 tests/tags_mutagen_check.py /tmp/hibytag_tags_test"""
import sys, os
try:
    import mutagen
    from mutagen.flac import FLAC
    from mutagen.id3 import ID3
except ImportError:
    print("mutagen no disponible: comprobacion cruzada omitida")
    sys.exit(0)

d = sys.argv[1]
fails = 0
def check(c, msg):
    global fails
    if not c:
        fails += 1
        print("FALLO:", msg)

f = FLAC(os.path.join(d, "a.flac"))
check(f["composer"] == ["CLI"], "flac composer %r" % f.get("composer"))
check(f["artist"] == ["Solo uno"], "flac artist %r" % f.get("artist"))
check(f["title"] == ["Canción ñandú 日本"], "flac title")
check(f["label"] == ["Sello ñ"], "flac label")
check(f["totaltracks"] == ["13"], "flac totaltracks")
check(len(f.pictures) == 1 and f.pictures[0].type == 3, "flac picture")
check(len(f["lyrics"][0]) == 20000, "flac lyrics")
b = FLAC(os.path.join(d, "b.flac"))
check(b["lyrics"] == ["1\n2\n3"] and b["composer"] == ["Ñ"], "b.flac")

for ver in (3, 4):
    t = ID3(os.path.join(d, "a%d.mp3" % ver), translate=False)
    check(t.version[1] == ver, "id3 version")
    check(str(t["TRCK"]) == "7/14", "TRCK %s" % t.get("TRCK"))
    check(str(t["TPOS"]) == "2/2", "TPOS")
    check(str(t["TCOM"]) == "Žofie", "TCOM")
    check(str(t["TIT2"]) == "Título nuevo", "TIT2")
    check(str(t["TSRC"]) == "ES-ABC-12-34567", "TSRC")
    comm = [c for c in t.getall("COMM") if c.desc == ""]
    check(len(comm) == 1 and comm[0].lang == "spa" and comm[0].text == ["otro comentario €"], "COMM %r" % comm)
    check(any(c.desc == "iTunNORM" for c in t.getall("COMM")), "iTunNORM")
    uslt = t.getall("USLT")
    check(len(uslt) == 1 and len(uslt[0].text) == 30000 and uslt[0].lang == "eng", "USLT")
    tx = {x.desc: x.text for x in t.getall("TXXX")}
    check(tx.get("MusicBrainz Album Id") == ["mb-2"], "TXXX mb %r" % tx)
    check("CATALOGNUMBER" not in tx, "TXXX borrado")
    if ver == 4:
        check(str(t["TMOO"]) == "Calm" and str(t["TDRC"]) == "2001", "v2.4 TMOO/TDRC")
    else:
        check(tx.get("MOOD") == ["Calm"] and str(t["TYER"]) == "2001", "v2.3 MOOD/TYER")
    check(len(t.getall("APIC")) == 1 and len(t.getall("PRIV")) == 1, "APIC/PRIV")
    n = ID3(os.path.join(d, "b%d.mp3" % ver), translate=False)
    check(str(n["TPE3"]) == "Karajan" and str(n["TIT2"]) == "Nuevo", "mp3 sin v2")
print("mutagen %s: %d fallos" % (mutagen.version_string, fails))
sys.exit(1 if fails else 0)
