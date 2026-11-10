# Instalador de HiByOS (payload de la SD)

El rootfs apenas tiene sitio (~0,5 MB libres), así que el shell, sus binarios, la librería de
pantalla y el tema viven en la partición de datos (`/usr/data/hibymod`, UBIFS, sobrevive al
flasheo). Una versión pública se distribuye en dos piezas:

| Pieza | Dónde va | Qué es |
|---|---|---|
| `r3ii_2025.upt` | raíz de la SD | firmware (rootfs con `hibyos_install.sh` y los ganchos) |
| `HiByOS/` | raíz de la SD | payload que el instalador copia a `/usr/data/hibymod` |

## Construir el payload

```
python tools\make_payload.py            # versión = la de overlay/usr/bin/hibymod ("HiByOS 0.6.0")
python tools\make_payload.py --version 0.6.1
```

Antes: compilar en WSL (`~/hiby/tools/zbuild_shell.sh`, `zbuild_so.sh`, `zbuild_wall.sh`,
`zbuild_mix.sh`, `zbuild_tag.sh` → `bin/`), `python tools\shell_assets.py`,
`python theme\build_theme.py`, `python theme\topbar.py`, `python theme\hibyos_brand.py`.
El script avisa si un binario es más viejo que su código.

Salida `out/sd/HiByOS/payload/` (~4 MB):

```
VERSION, MANIFEST.md5        versión y md5 de todo (se verifica antes de instalar)
bin/      hibyshell hibywall hibymix hibytag hibymod_disp.so
shell/    capas *.ov, icons/*.ov, wall.raw, home.sig
theme/    SOLO nuestros ficheros de layout/theme3 y litegui/theme3 (rutas de /usr/resource):
          inicio (vista + iconos + wallpaper.png + dock.png, de theme/out), barra de estado y
          Acerca de (de overlay/usr/resource), dialog/volume.dlg (diálogo de volumen oculto)
mirror/   inicio "espejo": vista con fondo launcher/mirror.png + iconos transparentes
Pictures/ los 9 fondos (theme/wallpapers.py)
```

No se redistribuye nada de HiBy: el script borra cualquier fichero idéntico al de fábrica. El
tema completo lo monta el aparato copiando su propio `theme3` de fábrica y poniendo encima el
nuestro (como `build_theme.py deploy()`).

## En el aparato (`/usr/bin/hibyos_install.sh`)

| Momento | Orden | Qué hace |
|---|---|---|
| `hiby_player.sh`, antes del tema y del reproductor | `boot` | Si la SD ya está montada y su versión es distinta, instala. Si HiByOS **no** está instalado, espera a la SD hasta 15 s (la animación de arranque sigue). Instalado y sin SD: sale al momento (0 ms de espera). También activa el espejo del inicio y quita un `theme2` a medias. |
| `S95hibymod`, con la SD montada (~8 s) | `late` | Si la versión de la SD es distinta, instala y S95 **reinicia** (la SD se monta ~4 s después de arrancar el reproductor, así que una actualización cuesta un reinicio). |
| ADB | `install` / `status` | Forzar la instalación / ver versiones y el final del registro. |

Nunca en modo seguro; `/data/.hibymod_noinstall` lo desactiva. Registro en `HiByOS/install.log`
de la SD (y `/tmp/hibymod/install.txt`).

Pasos de la instalación:
1. Verifica `MANIFEST.md5` y el espacio libre. Si falla, no toca nada y apunta la versión en
   `install_failed` (no se reintenta en bucle; `install` lo fuerza).
2. Borra `payload_version` (si se corta, se repite en el siguiente arranque).
3. `theme/layout/theme3` y `theme/litegui/theme3`: copia de fábrica si faltan o están
   incompletos (si el tema ya está montado encima, lee `/usr/resource` por un `mount -o bind /`
   de solo lectura). Lo que era nuestro y ya no está en el payload vuelve al de fábrica.
4. Copia cada fichero a `.new`, lo compara y lo renombra (no se rompen binarios en uso; lo
   idéntico no se reescribe). Binarios → `shell/`, librería → `preload/hibymod_disp.so`.
5. `mirror_bak/` y `launcher_view.bak` (inicio normal si el launcher no arranca) y
   `payload_mirror/`. Si existe `theme/.../launcher/mirror.png` se aplica el espejo.
6. Borra `shell/cache/` (capturas de apps, se regeneran).
7. `preload_ok` solo en la primera instalación, con la librería ya verificada y sin
   `/data/.hibymod_nopreload` (orden `SIN_LAUNCHER`).
8. Fondos a `/Pictures` de la SD si faltan; `payload_files`, `payload_subs` y por último
   `payload_version`.

**Estado del usuario que no se toca**: `shell/launcher.conf`, `shell/wall_custom.raw`,
`shell/mirror*.sig`, `launcher/mirror.png`, `power.conf`, `audio.conf`, `apps/`, `appdata/`,
`archivos/`, `fondos/`, `override/`, los fondos de la SD y la base de datos de música.

### Inicio espejo
En el primer arranque aún no existe `mirror.png`: el inicio de fábrica usa la vista e iconos
normales del tema (y `home.sig`). hibyshell genera `mirror.png` y `mirror.next.sig`; en el
siguiente arranque `boot` aplica la vista espejo e iconos transparentes (`mirror_on`).

## Flujos

- **Instalación desde cero**: copiar `r3ii_2025.upt` y `HiByOS/` a la SD → actualizar
  firmware desde la SD. En el primer arranque se instala antes del reproductor.
- **Actualización**: sustituir `HiByOS/` en la SD y reiniciar: S95 instala y reinicia solo.
- **Sin SD o sin payload**: arranca como el firmware de fábrica + parches; se instala en
  cuanto aparezca la carpeta (S95, con un reinicio).

## Prueba en el PC

```
wsl -d Ubuntu -- bash /mnt/c/Users/alber/Downloads/Hiby/hibymod/tools/simtest_install.sh
```
Cubre: desde cero, misma versión (sin escrituras), sin SD, espejo, actualización con tema
montado conservando el estado, payload dañado, primer arranque sin SD, SD sin payload,
`SIN_LAUNCHER` y la migración del aparato de desarrollo actual.
