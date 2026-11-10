"""Convierte el inicio de fabrica en un "espejo" del launcher de hibyshell:
  - fondo (hibymod_wallpaper) = launcher/mirror.png (320x450, imagen del launcher);
  - etiquetas y dock de fabrica ocultos; los iconos de fabrica se quedan (tocables, para
    que el shell abra las apps) pero con imagenes transparentes (las pone mirror_icons).
Asi, al volver deslizando, lo que se ve debajo es identico al launcher.
  python mirror_view.py ENTRADA.view SALIDA.view"""
import sys
import viewlib as V

root = V.load(sys.argv[1])
top = V.get(root, "viewgroup")
for i, (k, e) in enumerate(list(top)):          # quitar restos de pruebas
    if k == "imageview" and isinstance(e, list) and V.get(e, "name") == "hibymod_mirror":
        top.pop(i)
        break
for k, el in V.children(top):
    name = V.get(el, "name") or ""
    if name == "hibymod_wallpaper":
        V.set_(el, "img_path", "launcher\\mirror.png"); V.set_(el, "img_focus_path", "launcher\\mirror.png")
    elif name == "hibymod_dock":
        V.set_(el, "visible", "hide")
    elif name.startswith("launcher_apps_vg_"):
        tv = V.find(el, "launcher_apps_tv_" + name[len("launcher_apps_vg_"):])
        if tv is not None:
            V.set_(tv, "visible", "hide")
V.save(sys.argv[2], root)
print("ok")
