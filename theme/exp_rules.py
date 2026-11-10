"""Prueba reglas del motor de vistas sobre la pantalla Musica (no residente).
  python exp_rules.py neg     elemento con x negativa
  python exp_rules.py after   propiedad 'visible' DESPUES del marcador de tipo
  python exp_rules.py before  propiedad 'visible' ANTES del marcador de tipo
  python exp_rules.py stock   restaura
"""
import os, sys, subprocess
import viewlib as V
ADB = os.path.expandvars(r"%LOCALAPPDATA%\platform-tools\adb.exe"); DEV = "192.168.1.147:5555"
STOCK = r"//wsl.localhost/Ubuntu/home/alber/hiby/rootfs/usr/resource/layout/theme3/hiby_main_category.view"
REMOTE = "/usr/data/hibymod/theme/layout/theme3/hiby_main_category.view"
LOCAL = os.path.join(os.path.dirname(__file__), "work", "hiby_main_category.view")
os.makedirs(os.path.dirname(LOCAL), exist_ok=True)
mode = sys.argv[1]
root = V.load(STOCK)
el = V.find(root, "main_category_vg_genre")
if mode == "neg":
    V.set_(el, "x", -400)
elif mode == "after":
    V.set_(el, "visible", "hide")          # set_ anade al final (tras "viewgroup":true)
elif mode == "before":
    el.insert(len(el) - 1, ["visible", "hide"])
V.save(LOCAL, root)
subprocess.run([ADB, "-s", DEV, "push", LOCAL, REMOTE], check=True, capture_output=True)
print("subido", mode)
