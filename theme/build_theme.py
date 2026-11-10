#!/usr/bin/env python3
"""hibymod tema "R3 Glass" (estilo iOS 26) a partir de theme3 de fabrica.

Genera en out/ solo los ficheros que cambian (layout/theme3 y litegui/theme3).
  python build_theme.py              construye out/
  python build_theme.py --deploy     ademas los sube al aparato (/usr/data/hibymod/theme)

Principios de rendimiento (medidos en el aparato):
  - fondos y tarjetas con color solido + radiu (sin imagenes que leer al entrar)
  - textos fijos (static) salvo el titulo del reproductor: "roll" redibuja sin parar
  - iconos al tamano exacto en pantalla (sin escalado)
"""
import os, sys, shutil, subprocess
import viewlib as V

HERE = os.path.dirname(os.path.abspath(__file__))
STOCK = r"//wsl.localhost/Ubuntu/home/alber/hiby/rootfs/usr/resource"
OUT = os.path.join(HERE, "out")
ADB = os.path.expandvars(r"%LOCALAPPDATA%\platform-tools\adb.exe")
DEV = os.environ.get("HIBY_DEV", "192.168.1.147:5555")
REMOTE = "/usr/data/hibymod/theme"

# ---- paleta iOS (modo oscuro) ------------------------------------------------
BG = "0x000000"          # fondo de sistema
CARD = "0x1c1c1e"        # agrupado secundario
CARD2 = "0x2c2c2e"       # elevado / dock
LABEL = "0xffffff"
LABEL2 = "0x98989f"      # secundario (#EBEBF5 al 60 % sobre negro)
ACCENT = "0xfa2d48"      # rojo de Musica

changed = []


def load(rel):
    return V.load(os.path.join(STOCK, "layout", "theme3", rel))


def save(rel, root):
    path = os.path.join(OUT, "layout", "theme3", rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    V.save(path, root)
    changed.append(("layout/theme3/" + rel.replace("\\", "/"), path))


# ---- inicio ------------------------------------------------------------------
def home():
    root = load("launcher/hiby_launcher_apps.view")
    top = V.get(root, "viewgroup")
    # cuadricula de 4 columnas arriba y dock con Musica y Ajustes (como el prototipo)
    # OJO: las coordenadas del inicio son relativas a su area (y=0 es justo bajo la
    # barra de estado, que ocupa 30 px): el area util es 320x450.
    grid_order = ["wireless", "ebook", "dac"]          # stream_media oculto (solo Tidal/Qobuz)
    dock_order = ["player", "sysset"]
    CELL_W, ICON, M = 71, 56, 18
    GRID_Y, DOCK_Y = 16, 350
    for k, el in V.children(top):
        name = V.get(el, "name") or ""
        if "line" in name:                              # lineas de la cuadricula fuera
            V.set_(el, "visible", "hide")
            continue
        if not name.startswith("launcher_apps_vg_"):
            continue
        app = name[len("launcher_apps_vg_"):]
        iv = V.find(el, "launcher_apps_iv_" + app)
        tv = V.find(el, "launcher_apps_tv_" + app)
        # sin color de fondo: los iconos quedan sobre el fondo de pantalla
        V.delete(el, "color"); V.delete(el, "focus_color")
        if app in grid_order:
            i = grid_order.index(app)
            V.set_(el, "x", M + i * CELL_W); V.set_(el, "y", GRID_Y)
            V.set_(el, "w", CELL_W); V.set_(el, "h", 84)
            V.set_(iv, "x", (CELL_W - ICON) // 2); V.set_(iv, "y", 0)
            V.set_(tv, "x", 0); V.set_(tv, "y", 61); V.set_(tv, "w", CELL_W); V.set_(tv, "h", 18)
            V.set_(tv, "size", 12); V.set_(tv, "type", "static|h_center")
        elif app in dock_order:
            i = dock_order.index(app)
            # fondo transparente: se ve el dock de cristal (imagen) por debajo
            V.delete(el, "color"); V.delete(el, "focus_color")
            V.set_(el, "x", 160 - 8 - CELL_W if i == 0 else 160 + 8); V.set_(el, "y", DOCK_Y + 8)
            V.set_(el, "w", CELL_W); V.set_(el, "h", 76)
            V.set_(iv, "x", (CELL_W - ICON) // 2); V.set_(iv, "y", 10)
            V.set_(tv, "visible", "hide")              # el dock de iOS no lleva nombres
        else:
            # stream_media: oculto. Probado en el aparato: con x negativa el elemento
            # se queda con los toques de la fila superior; se reduce a 1x1 en una esquina.
            V.set_(el, "visible", "hide")
            V.set_(el, "x", 319); V.set_(el, "y", 449); V.set_(el, "w", 1); V.set_(el, "h", 1)
            V.set_(iv, "visible", "hide"); V.set_(tv, "visible", "hide")
            V.set_(tv, "x", 0); V.set_(tv, "y", 0); V.set_(tv, "w", 1); V.set_(tv, "h", 1)
        # imagenes nuevas del mismo nombre (56x56) -> no hace falta tocar img_path
    # fondo de pantalla y dock de cristal prerenderizado (imagenes de wallpaper.py)
    wall = V.element("imageview", name="hibymod_wallpaper", img_path="launcher\\wallpaper.png",
                     img_focus_path="launcher\\wallpaper.png", zorder=-20, x=0, y=0)
    dock = V.element("imageview", name="hibymod_dock", img_path="launcher\\dock.png",
                     img_focus_path="launcher\\dock.png", zorder=-10, x=10, y=DOCK_Y)
    top.insert(0, ["imageview", dock])
    top.insert(0, ["imageview", wall])
    save("launcher/hiby_launcher_apps.view", root)
    d = os.path.join(OUT, "litegui", "theme3", "launcher")
    subprocess.run([sys.executable, os.path.join(HERE, "wallpaper.py"), d] +
                   ([os.environ["HIBY_WALLPAPER"]] if os.environ.get("HIBY_WALLPAPER") else []), check=True)
    for f in ("wallpaper.png", "dock.png"):
        changed.append(("litegui/theme3/launcher/" + f, os.path.join(d, f)))


def icons():
    env = dict(os.environ, PWCORE=os.environ.get("PWCORE", ""))
    d = os.path.join(OUT, "litegui", "theme3", "launcher")
    subprocess.run(["node", os.path.join(HERE, "icons.js"), d], check=True, env=env, capture_output=True)
    for f in sorted(os.listdir(d)):
        changed.append(("litegui/theme3/launcher/" + f, os.path.join(d, f)))


def deploy():
    def sh(c):
        return subprocess.run([ADB, "-s", DEV, "shell", c], capture_output=True, text=True).stdout
    # carpeta del tema = copia completa de theme3 la primera vez
    sh(f"[ -d {REMOTE}/layout/theme3 ] || {{ mkdir -p {REMOTE}/layout {REMOTE}/litegui; "
       f"cp -a /usr/data/hibymod/fast/res/layout/theme3 {REMOTE}/layout/; "
       f"cp -a /usr/data/hibymod/fast/res/litegui/theme3 {REMOTE}/litegui/; }}")
    for rel, local in changed:
        subprocess.run([ADB, "-s", DEV, "push", local, f"{REMOTE}/{rel}"], check=True, capture_output=True)
    sh("sync")
    print(f"subidos {len(changed)} ficheros a {REMOTE}")


if __name__ == "__main__":
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    icons()
    home()
    print("cambiados:", len(changed))
    if "--deploy" in sys.argv:
        deploy()
