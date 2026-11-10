# Letras y carátulas automáticas (`hibynet`)

`bin/hibynet` es una herramienta estática pequeña (mipsel, musl, coma flotante por hardware) que
usa la UI para buscar **letras** y **carátulas** en servicios públicos gratuitos sin clave de API,
y para descargar ficheros sueltos en el modo manual (navegador).

- Código: `src/hibynet.c` · Compilación: `tools/wsl/zbuild_net.sh [x86]` → `bin/hibynet` + `bin/cacert.pem`
  (con `x86` también genera `~/hiby/net/hibynet-x86` para medir en el PC).
- Dependencias (las construye el script la primera vez en `~/hiby/net/{mips,x86}`): mbedTLS 2.28.9
  (configuración recortada) y libjpeg 9f. Además usa `stb_image.h` y `stb_image_write.h` de `~/hiby/third`.
  **Importante:** el sysroot antiguo `~/hiby/vm/xsysroot` es *soft-float* y no sirve; por eso se recompila
  en hard-float.
- Instalación en el aparato: `hibynet` y `cacert.pem` **en el mismo directorio**. Si no, usa `HIBYNET_CA=/ruta/cacert.pem`.

## Comandos

Salida en stdout: UTF-8, un resultado por línea y campos separados por TAB. Los TAB y los saltos de línea
que traigan los datos se cambian por espacios. El progreso y los errores van a stderr, con el prefijo `hibynet: `.
Durante las descargas se emite `progress BYTES TOTAL`, donde TOTAL vale -1 si el servidor no lo indica.

| Comando | stdout |
|---|---|
| `hibynet lyrics ARTISTA TÍTULO [ÁLBUM] [DURACIÓN_S]` | `id  artista  título  álbum  duración_s  synced(0/1)` (máx. 12, el mejor primero) |
| `hibynet lyrics-get ID SALIDA.lrc` | `SALIDA.lrc  synced(0/1)` |
| `hibynet covers ARTISTA ÁLBUM` | `fuente  url  artista  álbum  año` (máx. 8; fuente = `itunes` o `caa`) |
| `hibynet cover-get URL SALIDA.jpg [TAMAÑO]` | `SALIDA.jpg  ancho  alto  SALIDA.thumb.raw` |
| `hibynet get URL SALIDA` | `SALIDA  bytes` |

Códigos de salida:

| Código | Significado |
|---|---|
| `0` | Correcto. |
| `1` | Sin resultados o no encontrado (404). |
| `2` | Fallo de red, HTTP o TLS (incluido un certificado no válido). |
| `3` | Uso incorrecto. |
| `4` | Error de E/S o de imagen (formato no soportado o imagen demasiado grande). |

Variables de entorno:

| Variable | Efecto |
|---|---|
| `HIBYNET_CA` | Ruta del bundle de CA. |
| `HIBYNET_NOBOM=1` | Escribe el LRC sin BOM. |
| `HIBYNET_VERBOSE=1` | Traza peticiones, TLS y redirecciones. |
| `HIBYNET_STATS=1` | Escribe `VmHWM` (pico real de RSS) al terminar. |

### Detalles
- **lyrics**: si se pasan ÁLBUM y DURACIÓN, primero llama a `GET /api/get` (coincidencia exacta, va la primera).
  Después llama a `GET /api/search?track_name=&artist_name=` y, si no sale nada, a `/api/search?q=ARTISTA TÍTULO`.
  Orden: primero las sincronizadas y luego la duración más cercana a DURACIÓN. Se descartan las entradas instrumentales o sin texto.
- **lyrics-get**: `GET /api/get/ID`. Escribe `syncedLyrics` y, si no hay, `plainLyrics` (líneas sin marcas de tiempo).
  Cabecera: `[ar:]`, `[ti:]`, `[al:]`, `[length:mm:ss]` y `[re:LRCLIB]`. El fichero va en UTF-8 **con BOM** y saltos `\n`.
  La escritura es atómica: primero `.tmp` y después `rename`.
- **covers**:
  - iTunes: `https://itunes.apple.com/search?media=music&entity=album&limit=10&term=…`. Se reescribe `100x100` → `600x600`
    en `artworkUrl100` y se descartan los artistas que no coinciden (tributos, versiones).
  - MusicBrainz: `/ws/2/release-group/?query=releasegroup:"…" AND artist:"…"`, con score ≥ 80 y hasta 3 resultados.
    Cada uno se comprueba con un `HEAD` a `https://coverartarchive.org/release-group/MBID/front-500`
    (307 = existe; 404 = no hay carátula).
  - Orden: primero la coincidencia exacta de álbum y artista; a igualdad, iTunes delante de CAA.
- **cover-get**: descarga a `SALIDA.dl` (en el mismo directorio, nunca en `/tmp`), decodifica y genera:
  - `SALIDA.jpg`: JPEG **baseline**, calidad 88, ajustado a TAMAÑO×TAMAÑO conservando la proporción. Por defecto mide 500 y nunca se amplía.
  - `SALIDA.thumb.raw`: 96×96 RGB565 *little-endian* (18 432 bytes), centrado sobre negro (*letterbox*). Si SALIDA acaba en
    `.jpg`/`.jpeg` se quita la extensión: `cover.jpg` → `cover.thumb.raw`.
  - JPEG: libjpeg con reducción en el IDCT (1/8…8/8) y escalado por área fila a fila, así que la memoria es la de la salida y poco más.
    Se rechazan los JPEG *progresivos* de más de 1600×1600, porque libjpeg necesita los coeficientes de toda la imagen.
  - PNG, GIF y BMP: stb_image, con un tope de pico estimado de unos 6 MB. Una imagen de 4000×4000 a 16 bits se rechaza con el código 4.
- **get**: descarga a `SALIDA.part` y hace `rename` al terminar. Sigue hasta 8 redirecciones y no tiene tope de tamaño.

### Red, plazos y TLS
- HTTP/1.1 propio (`Connection: close`, `Accept-Encoding: identity`, *chunked* y `Content-Length`) y redirecciones 301/302/303/307/308.
  User-Agent en todas las peticiones: `HiByOS/0.6 (https://github.com/Grohle/hibyos)`.
- Plazo: **15 s por llamada de red** (conexión, TLS y respuesta completa). En `get` y `cover-get` el plazo funciona como
  *inactividad*: se renueva con cada lectura, para que las descargas grandes no se corten. Además hay una alarma global de 90 s
  en los comandos de búsqueda, por si el DNS se cuelga.
- Las respuestas JSON tienen un tope de 512 KB. El análisis es un recorrido directo sobre el buffer, sin árbol de tokens.
- TLS 1.2 con mbedTLS 2.28 (no hay TLS 1.3; todos los servicios usados aceptan 1.2).
  Se verifican la cadena, la firma, el nombre de host (SNI + CN/SAN) y las fechas.
  Comprobado contra `expired.`, `self-signed.` y `wrong.host.badssl.com`: los tres se rechazan con el código 2.
- **Bundle de CA:** se usa el **bundle completo de Mozilla** (`https://curl.se/ca/cacert.pem`: 200 KB, 118 raíces).
  No se recorta porque lrclib.net, mzstatic.com, musicbrainz.org y archive.org cambian de CA con el tiempo, y un bundle
  recortado se rompería sin avisar. El coste de RAM no existe: con `MBEDTLS_X509_TRUSTED_CERTIFICATE_CALLBACK`, en cada
  verificación se recorre el PEM línea a línea y solo se parsea la raíz cuyo *subject* coincide con el *issuer* del servidor.
  Así no se carga el bundle entero en memoria, que serían más de 300 KB de heap.
  Para actualizarlo, borra `bin/cacert.pem` y vuelve a ejecutar el script.
- **Reloj:** si el año del sistema es menor que 2025 (reloj sin ajustar), se ignoran **solo** los errores de fecha de
  los certificados y se avisa por stderr. Todo lo demás se sigue verificando.
- **MusicBrainz** permite como máximo 1 petición por segundo. Para respetarlo entre procesos se usa una marca de tiempo en `/tmp/.hibynet_mb`
  (un fichero vacío): si la petición anterior fue hace menos de 1,1 s, se espera. Ante un 503 se reintenta una vez pasados 1,2 s.

## Servicios y condiciones de uso

| Servicio | Uso | Límites y notas |
|---|---|---|
| **LRCLIB** (`lrclib.net`) | Letras sincronizadas y planas | Gratis y sin clave. Piden un User-Agent que identifique la app (se envía). No publican un límite de peticiones; hay que ser razonable: una búsqueda por acción del usuario y nada de barridos masivos de la biblioteca sin pausa. Las letras las aporta la comunidad y sus derechos son de los autores: uso personal. |
| **iTunes Search API** (`itunes.apple.com`, imágenes en `*.mzstatic.com`) | Carátulas de 600×600 | Sin clave. Apple indica unas **20 llamadas por minuto** como orientación. Las imágenes tienen derechos de Apple y de los sellos: solo uso personal, para mostrarlas en el aparato y no para redistribuirlas. |
| **MusicBrainz** (`musicbrainz.org`) | Búsqueda de *release groups* | Sin clave. **Máximo 1 petición/s** por IP y **User-Agent con contacto obligatorio**; sin él te bloquean. Los datos son CC0. |
| **Cover Art Archive** (`coverartarchive.org` → `archive.org`) | Carátulas de 500 px (`front-500`) | Sin clave y sin límite estricto. Cada descarga pasa por 2 o 3 redirecciones (coverartarchive → archive.org → `ia*.us.archive.org`), así que tarda unos 5 s en el R3 II. Las imágenes tienen derechos de sus titulares: uso personal. |

Recomendación para la UI: búsquedas solo bajo demanda (al abrir "Buscar letra" o "Buscar carátula"). Si algún día se
hace un barrido automático de la biblioteca, debe ir en serie, con pausas de 1 s o más, y guardando lo que no se encontró
para no repetir la consulta.

## Medidas

El binario `bin/hibynet` ocupa 570 KB (mipsel estático, sin símbolos) y `cacert.pem`, 200 KB.

| Comando (consulta real) | RSS pico en PC (x86 glibc, `/usr/bin/time -v`) | VmHWM en el R3 II | Tiempo en el R3 II |
|---|---|---|---|
| `lyrics "Extremoduro" "So payaso"` | 2,7 MB | **0,82 MB** | 0,6 s |
| `lyrics … "Agila" 282` (get + search) | 2,7 MB | 0,82 MB | 1,3–3,5 s |
| `lyrics-get 3055312` | 2,2 MB | 0,61 MB | 0,45 s |
| `covers "Extremoduro" "La ley innata"` | 2,4 MB | 0,64 MB | 1,4 s |
| `covers "Radiohead" "OK Computer"` | 2,4 MB | 0,64 MB | 1,5 s (5,6 s si MusicBrainz da un 503 y reintenta) |
| `cover-get` iTunes 600→500 | 3,0 MB | **1,5 MB** | 0,9 s |
| `cover-get` CAA front-500 | 3,0 MB | 1,5 MB | 5 s |
| `get` PNG de 13,9 MB | 2,4 MB | 0,62 MB | 19 s |

En el PC el RSS es mayor porque incluye glibc dinámica. El dato que cuenta es el del aparato.

## Dónde guardar para que el reproductor de serie lo use

Esto se comprobó con las cadenas de `~/hiby/rootfs/usr/bin/hiby_player` (`~/hiby/hp3.txt`) y con la base de datos
real `/data/usrlocal_media.db` del aparato. No se reprodujo nada ni se reinició ningún proceso.

### Letras
- **Fichero `.lrc` junto al audio, con el mismo nombre base:** `…/Álbum/1-07 Canción.flac` → `…/Álbum/1-07 Canción.lrc`.
  Indicios en el binario:
  - visor `lg_lyric_view` y extensión `lrc` en la misma tabla de extensiones que `cue` e `iso`, con `<protocol>lrc`;
  - opciones `lrc_color`, `lrc_size`, `lrc_focus_*` y `no_lrc`;
  - columna `lrc_path` en `MEDIA_TABLE`.
- En la base de datos actual `lrc_path` está **vacía en las 2488 pistas**: el reproductor busca el `.lrc` al reproducir y no
  al escanear, así que no hace falta volver a escanear la biblioteca tras guardar.
- También lee letras **incrustadas** (cadenas `Lyrics`, `UNSYNCEDLYRICS` y `WM/Lyrics`), pero escribirlas no es necesario.
- Codificación: el reproductor trae tablas GB18030, Big5, CP932, CP949, ISO-8859-x y Windows-1258, y detecta UTF-8 y UTF-16. Por eso el
  `.lrc` se escribe en **UTF-8 con BOM**: es lo más seguro para que no lo interprete como una página de códigos local.
  Con `HIBYNET_NOBOM=1` se escribe sin BOM.
- Uso: `hibynet lyrics-get ID "/data/mnt/sd_0/Artista/Álbum/1-07 Canción.lrc"`.

### Carátulas
- En el binario de serie **no aparecen** `cover.jpg`, `folder.jpg`, `front.jpg` ni `albumart`, y tampoco en ningún otro
  binario del rootfs. Sí aparecen las cadenas de carátula **incrustada**: `METADATA_BLOCK_PICTURE` (FLAC/Vorbis),
  `WM/Picture` (WMA), `Cover Art` + `(Front)` (APE), además de los analizadores `album_cover_parser` y `cover_parser`.
  También aparece una caché propia: `/data/mnt/sd_0/list_cover/` y `/data/mnt/sd_0/.temp/image_cache.db`.
- La columna `album_pic_path` está vacía en todas las pistas, y las carpetas de álbum del aparato solo contienen `.flac`:
  las carátulas que ya se ven son **incrustadas**.
- **Conclusión:** para que el reproductor de serie muestre la carátula hay que **incrustarla en el audio**:
  - FLAC: bloque PICTURE tipo 3;
  - MP3: ID3v2 APIC;
  - M4A: `covr`.

  `hibytag` todavía **no** escribe imágenes, así que queda como tarea pendiente.
- Mientras tanto, la UI de HiByOS debe guardar `cover.jpg` en la carpeta del álbum, que es lo que usan casi todos los
  reproductores y lo que puede leer nuestra propia UI. Junto a él debe guardar `cover.thumb.raw` (la miniatura 96×96 RGB565
  para las listas). Uso: `hibynet cover-get URL "/data/mnt/sd_0/Artista/Álbum/cover.jpg"`.
  Para incrustarla después, usar el `cover.jpg` de 500 px: es baseline y pesa unos 60–80 KB, un tamaño razonable para el reproductor.
- Pendiente de verificar en vivo: si al reproducir un álbum **sin** carátula incrustada pero con `cover.jpg` en la
  carpeta, el reproductor de serie la muestra. No se pudo comprobar sin tocar el reproductor. Ninguna cadena del binario lo sugiere.
