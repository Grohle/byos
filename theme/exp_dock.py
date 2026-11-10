"""Experimento: elemento decorativo nuevo (dock con radiu) y ocultar lineas en el inicio."""
import os, subprocess, sys
import viewlib as V

ADB = os.path.expandvars(r"%LOCALAPPDATA%\platform-tools\adb.exe")
DEV = "192.168.1.147:5555"
REMOTE = "/usr/data/hibymod/fast/res/layout/theme3/launcher/hiby_launcher_apps.view"
LOCAL = os.path.join(os.path.dirname(__file__), "work", "hiby_launcher_apps.view")
os.makedirs(os.path.dirname(LOCAL), exist_ok=True)

stock = r"//wsl.localhost/Ubuntu/home/alber/hiby/rootfs/usr/resource/layout/theme3/launcher/hiby_launcher_apps.view"
root = V.load(stock)
top = V.get(root, "viewgroup")

# 1) ocultar las lineas de la cuadricula
for k, el in V.walk(top):
    if (V.get(el, "name") or "").startswith("launcher_apps_iv_") and "line" in V.get(el, "name"):
        V.set_(el, "visible", 0)

# 2) dock decorativo al principio (debajo de todo)
dock = V.element("viewgroup", name="hibymod_dock", type="lg_view", color="0x2c2c2e",
                 focus_color="0x2c2c2e", radiu=26, x=10, y=372, w=300, h=96)
top.insert(0, ["viewgroup", dock])

V.save(LOCAL, root)
subprocess.run([ADB, "-s", DEV, "push", LOCAL, REMOTE], check=True, capture_output=True)
print("subido")
