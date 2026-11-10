# Formato de apps de hibymod (`.hpk`)

Paquetes de apps de terceros para el shell de hibymod en el HiBy R3 II (MIPS32r2 little-endian,
Linux 4.4, ~7 MB de RAM libre). Este documento es el contrato entre quien hace una app, el
empaquetador de PC (`tools/hpk.py`) y el instalador del aparato (`src/hpk.inc.c` / `src/hpk.h`).

## 1. El paquete

Un `.hpk` es un archivo **tar POSIX ustar sin comprimir**. En la raíz:

| Entrada        | Obligatoria | Qué es |
|----------------|-------------|--------|
| `manifest.ini` | sí          | metadatos (UTF-8, máx. 16 KB). Debe ser la primera entrada. |
| `icon.png`     | recomendada | icono cuadrado a sangre (128x128 RGBA recomendado, 16–512 px, máx. 1 MB). Segunda entrada. |
| `exec`         | sí          | el ejecutable indicado en el manifest (p. ej. `bin/hola`). |
| `data/`, `lib/`, … | no      | cualquier otro fichero o directorio de la app. |

Reglas del tar (el instalador rechaza el paquete entero si alguna falla):

- Solo **ficheros normales** (`'0'`) y **directorios** (`'5'`). Nada de enlaces simbólicos o duros,
  dispositivos, FIFOs, cabeceras pax (`x`/`g`) ni nombres largos GNU (`L`/`K`). Se admite el campo
  `prefix` de ustar (rutas de hasta 255 bytes).
- Rutas **relativas**: se rechazan las absolutas, cualquier componente `..`, barras invertidas y
  caracteres de control. Se toleran `./` iniciales.
- Cada cabecera debe tener la suma de comprobación correcta; los tamaños van en octal (no se admite
  base-256), cada fichero ≤ 256 MB, ≤ 4096 entradas y nada puede salirse del final del archivo.
- Sin entradas repetidas ni un fichero que sea a la vez directorio.
- Los modos se normalizan: `0755` para el ejecutable y ficheros con bit x, `0644` el resto; nunca
  suid/sgid. Propietario siempre root.

`tools/hpk.py build` genera tars reproducibles: ustar, uid/gid 0 sin nombres, mtime fijo
(2000-01-01), `manifest.ini` e `icon.png` primero y el resto en orden alfabético.

## 2. `manifest.ini`

Texto `clave=valor`, una por línea. Se ignoran líneas vacías, `#`, `;` y `[secciones]`; los valores
pueden ir entre comillas. Las claves desconocidas se ignoran (compatibilidad hacia delante).

| Clave | Oblig. | Valor |
|-------|--------|-------|
| `id` | sí | identificador estilo DNS inverso, `[a-z0-9._-]`, empieza por letra o cifra, máx. 48, sin `..`. Ej.: `org.hibymod.hola` |
| `version` | sí | semver `MAYOR.MENOR.PARCHE[-pre][+build]` |
| `min_hibymod` | sí | semver mínima del shell (si es mayor que la del aparato: `err.newer`) |
| `name` | sí | nombre en español (es el de reserva para los demás idiomas); también `name.es` |
| `name.en`, `name.fr` | recom. | nombre en inglés y francés (máx. 47 bytes) |
| `description`, `.en`, `.fr` | recom. | descripción corta (máx. 159 bytes) |
| `exec` | sí | ruta relativa del ejecutable dentro del paquete |
| `arch` | sí | exactamente `mipsel-hf` |
| `input` | sí | `touch`, `keys` o `both` |
| `stop_player` | no | `1` si la app necesita la RAM y la música debe pararse; `0` por defecto |
| `ram_kb` | recom. | estimación de memoria (KB) que usa la app |
| `category`, `author`, `homepage` | no | informativos |

Ejemplo (`apps/hola/manifest.ini`):

```ini
id=org.hibymod.hola
version=1.0.0
min_hibymod=0.1.0
name=Hola
name.en=Hello
name.fr=Bonjour
description=Demo: saludo, degradado y un círculo que sigue al dedo
description.en=Demo: greeting, gradient and a circle that follows your finger
description.fr=Démo : salutation, dégradé et un cercle qui suit le doigt
exec=bin/hola
arch=mipsel-hf
input=touch
stop_player=0
ram_kb=900
category=demo
author=hibymod
```

## 3. El ejecutable

- **ELF32 little-endian, MIPS, ISA ≤ mips32r2, estático** (sin `PT_INTERP`; se admite static-pie).
- **Coma flotante hard** (`musleabihf`): si `PT_MIPS_ABIFLAGS` dice soft-float se rechaza
  (soft-float es 14 veces más lento en este SoC).
- Cualquier binario auxiliar en `lib/` o `bin/` también debe ser estático: en el aparato no hay
  bibliotecas compartidas compatibles.

Compilación (ver `~/hiby/tools/zbuild_app.sh`):

```sh
python -m ziglang cc -target mipsel-linux-musleabihf -mcpu=mips32r2 -Os -static -s \
  -I hibymod/src/sdk -o bin/miapp miapp.c -lm
```

## 4. El icono

`icon.png` debe ser la **ilustración cuadrada completa, a sangre y opaca**: sin esquinas
redondeadas, sin brillo y sin borde. El instalador:

1. recorta al cuadrado central si no lo es y reduce por promedio de área (amplía bilineal si es
   pequeño); la transparencia se compone sobre negro, como en iOS;
2. aplica la máscara de los iconos de la casa (`theme/launcher_icons.js`): rectángulo redondeado
   con radio del 22,37 %, brillo blanco del 30 % → 0 en la mitad superior y borde interior de 1 px
   blanco al 28 %, con antialias;
3. escribe `icon_48.ov`, `icon_56.ov` e `icon_64.ov` en el directorio de la app (formato `.ov`:
   `"HMOV"`, uint16 ancho, uint16 alto, RGB565[ancho·alto], alfa[ancho·alto], little-endian, alfa
   sin premultiplicar — el mismo que `load_sprite()` de hibyshell).

Sin `icon.png` se genera un icono de reserva con un degradado cuyo tono sale del `id`.

## 5. Instalación en el aparato

Rutas (todas bajo una raíz configurable para pruebas con `hpk_set_root()` o `HPK_ROOT`;
por defecto `/usr/data/hibymod`):

| Ruta | Contenido |
|------|-----------|
| `/usr/data/mnt/sd_0/Apps/*.hpk` | paquetes disponibles (los lista `hpk_scan`) |
| `<raíz>/apps/<id>/` | app instalada; su `manifest.ini` es el registro |
| `<raíz>/appdata/<id>/` | datos escribibles de la app; sobreviven a actualizaciones y a la desinstalación normal |
| `<raíz>/apps/.tmp-<id>/`, `.old-<id>/`, `.del-<id>/` | estados transitorios (nunca visibles para el launcher) |

Proceso de `hpk_install()`:

1. **Validar sin escribir nada**: todas las cabeceras (sumas, tipos, rutas, tamaños), manifest,
   presencia y formato ELF del ejecutable, tamaño del icono y **espacio libre** en `/usr/data`
   (contenido + iconos + 512 KB de margen).
2. **Extraer** en `apps/.tmp-<id>/` con `pread` en trozos de ≤ 64 KB (`O_EXCL|O_NOFOLLOW`, `fsync`
   por fichero), con llamadas de progreso.
3. **Generar los iconos** `.ov` y crear `appdata/<id>/`.
4. **Cambio atómico**: `apps/<id>` → `apps/.old-<id>`, `apps/.tmp-<id>` → `apps/<id>`, borrar
   `.old-<id>`. Ante cualquier error se borra `.tmp-<id>` y la versión anterior queda intacta.
5. **Recuperación** (`hpk_recover()`, se llama al instalar y al listar): si un corte de luz deja
   `.old-<id>` sin `<id>`, se restaura la versión anterior; cualquier `.tmp-`/`.del-`/`.old-` sobrante
   se borra.

El shell debe cerrar la app (SIGTERM) antes de actualizarla o desinstalarla.

`hpk_scan()` solo lee el manifest y localiza el icono (recorre cabeceras; no lee datos), por lo que
es rápido pero no detecta un ELF incorrecto: eso lo hace `hpk_install()`.

### API (`src/hpk.h`)

```c
int  hpk_scan(const char *dir, struct hpk_info *out, int max);          /* paquetes de la SD */
int  hpk_install(const char *path, void (*progress)(long done, long total, void *ctx), void *ctx,
                 char *err, int errlen);                                 /* err = clave */
int  hpk_uninstall(const char *id);                                      /* conserva appdata */
int  hpk_uninstall_ex(const char *id, int flags);                        /* HPK_PURGE_DATA */
int  hpk_list_installed(struct hpk_info *out, int max);
int  hpk_icon(const struct hpk_info *pkg, int s, unsigned short *rgb, unsigned char *a);
int  hpk_vercmp(const char *a, const char *b);
const char *hpk_errkey(int code);              /* -HPK_E_TRAVERSAL -> "err.traversal" */
const char *hpk_msg(const char *key, int lang);/* HPK_LANG_ES / _EN / _FR */
extern char hpk_err_detail[160];               /* entrada o campo culpable */
```

Todas devuelven 0 o `-HPK_E_*`. `struct hpk_info` trae id, versión, nombres y descripciones en los
tres idiomas, `stop_player`, `ram_kb`, tamaño, ruta, versión instalada (`installed`) y `status`
(0 o el error si el paquete está dañado). Claves de estado para la interfaz: `st.install`,
`st.update`, `st.current`, `st.older`, `st.installing`, `st.icons`, `st.uninstall`,
`st.stopplayer`, `st.damaged`, `st.noapps`… (ver la tabla en `hpk.inc.c`).

## 6. Contrato de ejecución

El shell lanza `apps/<id>/<exec>` con el directorio de trabajo en `HM_APP_DIR` y estas variables:

| Variable | Valor |
|----------|-------|
| `HM_APP_DIR` | directorio de instalación (solo lectura por convención) |
| `HM_DATA_DIR` | `/usr/data/hibymod/appdata/<id>` (escribible, persistente) |
| `HM_LANG` | `es`, `en` o `fr` |
| `HM_TOUCH` | nodo evdev de un táctil virtual, mismo protocolo que goodix-ts: multitáctil tipo A (`BTN_TOUCH`, `ABS_MT_POSITION_X/Y`, `SYN_MT_REPORT` por contacto, `SYN_REPORT` por trama; una trama sin contactos = dedo levantado). Coordenadas de pantalla 0–319 × 0–479 |
| `HM_KEYS` | nodo evdev de las teclas físicas (`EV_KEY`, valor 1/0/2) |
| `HM_FB` | `/dev/fb0`: 320x480 RGB565, doble búfer con panning (virtual 320x960) |

**Pantalla (SLCD con memoria propia)**: cada `FBIOPAN_DISPLAY` transfiere la imagen al panel.
Escribe el búfer oculto **completo** y **después** haz el pan; nunca escribas en el búfer que se
muestra tras el pan (corrompe la transferencia y parpadea). Máximo útil: 44 fps.

**Salida**: al deslizar hacia arriba desde la barra de inicio el shell envía `SIGTERM`; la app debe
terminar en **menos de 1 s** (después llega `SIGKILL`). No dejes procesos hijos.

**Memoria**: si `stop_player=0` la app convive con el reproductor y debe quedarse en unos pocos MB
(declara `ram_kb` con honestidad). Nunca escribas grandes ficheros temporales en `/tmp` (es RAM).

### SDK (`src/sdk/hm_app.h`, solo cabecera)

- `hm_app_init()` – instala SIGTERM/SIGINT/SIGHUP → `hm_quit`.
- `hm_fb_open / hm_fb_present(fb, lienzo) / hm_fb_close` – mmap y presentación correcta (copia el
  lienzo de RAM al búfer oculto, luego pan).
- `hm_touch_open / hm_touch_read(t, cb, ctx)` – eventos `HM_DOWN / HM_MOVE / HM_UP` del primer dedo.
- `hm_keys_open / hm_keys_read(k, cb, ctx)` – teclas.
- `hm_wait(t, k, ms)` – `poll` de ambos; `hm_lang()`, `hm_tr(es, en, fr)`, `hm_app_dir()`,
  `hm_data_dir()`, `hm_now_ms()`.

Ejemplo completo: `apps/hola/` (degradado con tramado, saludo con fuente 8x16 ampliada, círculo que
sigue al dedo con muelle y desvanecido). Se compila y empaqueta con
`~/hiby/tools/zbuild_app.sh hola` → `out/apps/hola.hpk`.

## 7. Herramientas de PC

```sh
python tools/hpk.py build apps/miapp/staging -o miapp.hpk   # valida y empaqueta
python tools/hpk.py inspect miapp.hpk                       # manifest y lista de ficheros
python tools/hpk.py verify miapp.hpk                        # mismas reglas que el instalador
```

Pruebas del instalador en WSL (raíz temporal, paquetes maliciosos incluidos):
`~/hiby/tools/hpk_test.sh` (genera los paquetes con `tests/mkpkgs.py` y ejecuta `tests/hpk_test.c`
con AddressSanitizer).

---

## Summary (English)

A `.hpk` is an **uncompressed POSIX ustar** archive with `manifest.ini` (UTF-8 `key=value`: `id`,
`version`, `min_hibymod`, `name[.en|.fr]`, `description[.en|.fr]`, `exec`, `arch=mipsel-hf`,
`input=touch|keys|both`, `stop_player=0|1`, `ram_kb`, optional `category/author/homepage`),
`icon.png` (full-bleed opaque square, 128x128 recommended; the installer applies the rounded
mask, gloss and border and writes `icon_48/56/64.ov`) and the app files. Only regular files and
directories with relative paths are accepted; the executable must be a static hard-float MIPS32r2
LE ELF. Installation validates everything first, extracts into `apps/.tmp-<id>` with ≤64 KB
buffers, then atomically swaps it in; failures never leave a half-installed app. Apps get
`HM_APP_DIR`, `HM_DATA_DIR`, `HM_LANG`, `HM_TOUCH`, `HM_KEYS`, `HM_FB`, must write the whole back
buffer before panning, and must exit within 1 s of `SIGTERM`. Use `src/sdk/hm_app.h` and
`tools/hpk.py build|inspect|verify`.

## Résumé (français)

Un `.hpk` est une archive **tar POSIX ustar non compressée** contenant `manifest.ini` (UTF-8
`clé=valeur` : `id`, `version`, `min_hibymod`, `name[.en|.fr]`, `description[.en|.fr]`, `exec`,
`arch=mipsel-hf`, `input=touch|keys|both`, `stop_player=0|1`, `ram_kb`, et en option
`category/author/homepage`), `icon.png` (carré opaque plein cadre, 128x128 conseillé ; l'installeur
applique le masque arrondi, le reflet et la bordure et produit `icon_48/56/64.ov`) et les fichiers
de l'app. Seuls les fichiers ordinaires et les dossiers à chemin relatif sont acceptés ;
l'exécutable doit être un ELF MIPS32r2 LE statique en virgule flottante matérielle. L'installation
valide tout d'abord, extrait dans `apps/.tmp-<id>` par blocs de 64 Ko maximum, puis remplace
l'ancienne version de façon atomique : un échec ne laisse jamais d'app à moitié installée. Les apps
reçoivent `HM_APP_DIR`, `HM_DATA_DIR`, `HM_LANG`, `HM_TOUCH`, `HM_KEYS`, `HM_FB`, doivent écrire tout
le tampon caché avant le panoramique et quitter moins d'1 s après `SIGTERM`. Voir
`src/sdk/hm_app.h` et `tools/hpk.py build|inspect|verify`.
