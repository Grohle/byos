#!/bin/bash
# Genera el firmware de HiByOS a partir del firmware OFICIAL de HiBy que te descargas tu.
# HiByOS no redistribuye el firmware de HiBy: este script lo modifica en tu ordenador.
#
# Uso (Linux, WSL en Windows o GitHub Codespaces):
#   bash tools/build_hibyos.sh /ruta/al/r3ii_2025.upt        # el .upt oficial de HiBy
# Resultado:
#   out/r3ii_2025.upt      firmware HiByOS (copialo a la raiz de la SD)
#   (la carpeta HiByOS/ para la SD va en el release de GitHub)
set -e
cd "$(dirname "$0")/.."
UPT="$1"
[ -f "$UPT" ] || { echo "uso: bash tools/build_hibyos.sh /ruta/al/r3ii_2025.upt (firmware oficial)"; exit 1; }

need() { command -v "$1" >/dev/null 2>&1; }
if ! need unsquashfs || ! need mksquashfs; then
    echo "Faltan squashfs-tools. En Debian/Ubuntu: sudo apt install squashfs-tools"; exit 1
fi
if ! need unshare; then echo "Falta unshare (util-linux)"; exit 1; fi
PY=python3
if ! $PY -c "import pycdlib" 2>/dev/null; then
    echo "Instalando pycdlib en un entorno virtual (.venv)..."
    $PY -m venv .venv && .venv/bin/pip -q install pycdlib pillow
    PY=.venv/bin/python
fi
echo "== Comprobando el firmware oficial"
md5sum "$UPT"
mkdir -p out
# build.py verifica que el firmware es la version soportada (los parches comprueban el
# texto y el md5 originales) y que el resultado cabe en la particion.
unshare -r $PY -I tools/build.py --stock "$UPT" --overlay overlay --out out/r3ii_2025.upt
echo
echo "Listo: out/r3ii_2025.upt"
echo "Copia a la raiz de la SD: out/r3ii_2025.upt y la carpeta HiByOS/ del release."
echo "En el reproductor: Ajustes > Actualizacion de firmware > Via tarjeta SD."
