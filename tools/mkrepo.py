"""mkrepo.py - genera el repositorio de la tienda (repo/index.json + repo/icons/) a partir de .hpk.

  python tools/mkrepo.py out/apps/hola.hpk out/apps/navegador.hpk [--tag apps-1] [--featured ID,...]

Cada paquete se publica como asset de la release de GitHub TAG con el nombre <id>-<version>.hpk
(gh release upload TAG FICHERO --clobber); el indice y los iconos van en la rama main y se sirven
por raw.githubusercontent.com. Formato del indice: docs/TIENDA.md."""
import argparse, hashlib, io, json, os, shutil, sys, tarfile
sys.path.insert(0, os.path.dirname(__file__))
from hpk import parse_manifest  # noqa: E402

REPO = "Grohle/hibyos"
ap = argparse.ArgumentParser()
ap.add_argument("hpk", nargs="+")
ap.add_argument("--tag", default="apps-1")
ap.add_argument("--featured", default="org.hibymod.navegador")
ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "repo"))
ap.add_argument("--dist", default=os.path.join(os.path.dirname(__file__), "..", "out", "repo_assets"))
a = ap.parse_args()
os.makedirs(os.path.join(a.out, "icons"), exist_ok=True); os.makedirs(a.dist, exist_ok=True)
feat = set(filter(None, a.featured.split(",")))
apps = []
for p in a.hpk:
    data = open(p, "rb").read()
    t = tarfile.open(fileobj=io.BytesIO(data))
    mf = parse_manifest(t.extractfile("manifest.ini").read().decode("utf-8"))
    i = mf["id"]; v = mf["version"]
    fname = "%s-%s.hpk" % (i, v)
    shutil.copyfile(p, os.path.join(a.dist, fname))
    try:
        open(os.path.join(a.out, "icons", i + ".png"), "wb").write(t.extractfile("icon.png").read())
        icon = "icons/%s.png" % i
    except KeyError:
        icon = ""
    loc = lambda k: {l: mf.get(k + "." + l) or mf.get(k) for l in ("es", "en", "fr")}
    apps.append({
        "id": i, "version": v, "min_hibymod": mf["min_hibymod"],
        "name": loc("name"), "desc": loc("description"),
        "author": mf.get("author", ""), "category": mf.get("category", ""),
        "ram_kb": int(mf.get("ram_kb", "0") or 0), "stop_player": mf.get("stop_player", "0") == "1",
        "featured": i in feat,
        "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
        "icon": icon,
        "hpk": "https://github.com/%s/releases/download/%s/%s" % (REPO, a.tag, fname),
    })
apps.sort(key=lambda x: (not x["featured"], x["name"]["es"].lower()))
idx = {"format": 1, "name": {"es": "HiByOS", "en": "HiByOS", "fr": "HiByOS"},
       "homepage": "https://github.com/%s" % REPO, "apps": apps}
open(os.path.join(a.out, "index.json"), "w", encoding="utf-8", newline="\n").write(json.dumps(idx, ensure_ascii=False, indent=1) + "\n")
print("repo/index.json:", len(apps), "apps; assets en", os.path.abspath(a.dist))
for x in apps: print(" ", x["id"], x["version"], x["size"], x["sha256"][:16])
