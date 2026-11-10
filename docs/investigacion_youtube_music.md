# Investigación: "YouTube Music nativo" en el HiBy R3 II

Fecha: 2026-11-09. Inspiración: SimpMusic (Kotlin, Compose Multiplatform).
Restricciones del equipo: Ingenic X1600 (MIPS32r2 LE, ~1 GHz, un núcleo), 57 MB de RAM y ~7–10 MB libres con `hiby_player` (~15 MB RSS) y `hibyshell` (~4.5 MB) en marcha, Linux 4.4, sin swap ni JVM, pantalla 320x480 táctil y WiFi.

---

## 0. Resumen ejecutivo

- **YouTube Music no tiene una API pública.** SimpMusic, InnerTune, Metrolist y yt-dlp hablan con la API privada **InnerTube** (`/youtubei/v1/...`). La parte de catálogo (search/browse/next) es JSON fácil. La parte difícil es **obtener una URL de audio reproducible**. Para eso hace falta:
  1. ejecutar el JS del reproductor de YouTube (`base.js`, unos 2.5–3 MB) para resolver la firma (`s`) y el **reto `n`**;
  2. en muchos clientes, un **PoToken** (attestation BotGuard);
  3. esquivar SABR, el experimento de streaming "solo SABR".

  Todo esto cambia cada pocas semanas.
- **Hoy SimpMusic ejecuta `base.js` en QuickJS con un límite de 192 MB por runtime.** Eso es inviable en un equipo con 7 MB libres.
- **El reproductor de serie SÍ reproduce audio por HTTP/HTTPS.** Tiene su propio cliente HTTP con `Range`, chunked y `Location`, soporte HLS (m3u8, AES-128), entradas URL en m3u y un renderer DLNA con `dlna_set_uri`. Demuxers: MP3, M4A, OGG (Vorbis/**Opus**), FLAC, WAV, APE, AIFF, DSF/DFF, ASF, TS y AAC. **No tiene demuxer WebM/Matroska.**
- **Recomendación: arquitectura con relay** en el PC, un RPi o un NAS. El relay usa `ytmusicapi`, `yt-dlp` con Deno y un proveedor de PoToken, y `ffmpeg -c copy`. El HiBy lleva solo una UI nativa fina dentro de `hibyshell`, que habla HTTP plano por la LAN. El audio se entrega como **Ogg Opus remuxado sin recodificar** (itag 251 → .opus/.ogg), que la cadena de serie ya decodifica.
- El modo **descarga/sincronización** a la SD, para escuchar sin conexión, encaja como fase 2 sobre el mismo relay.

---

## 1. Cómo obtiene los datos SimpMusic

Lo he verificado clonando `maxrave-dev/core`, rama `multiplatform`, módulo `service/kotlinYtmusicScraper`.

### 1.1 Catálogo: InnerTube

- Endpoints `https://music.youtube.com/youtubei/v1/{browse,search,next,player,music/get_search_suggestions,...}`. Se envía un POST JSON con `context.client` (`clientName`, `clientVersion`, `hl`, `gl`, `visitorData`) y la cabecera `X-Goog-Visitor-Id`.
- Clientes definidos en `models/YouTubeClient.kt`:

  | Cliente | Versión |
  |---|---|
  | `WEB_REMIX` | 1.20260304.03.00 (cliente de music.youtube.com) |
  | `ANDROID_MUSIC` | 7.27.52 |
  | `ANDROID` | 21.26.364 |
  | `WEB` | — |
  | `WEB_EMBEDDED_PLAYER` | — |
  | `TVHTML5` | — |
  | `TVHTML5_SIMPLY_EMBEDDED_PLAYER` | — |
  | `IOS` | 19.45.4 |
  | `MWEB` | — |

  Cada uno lleva su API key y su User-Agent.
- `browse` cubre home, álbum, artista, playlist, biblioteca, moods y charts. `next` devuelve la cola "radio"/autoplay y la letra (lyrics browseId). `search` usa params para filtrar por canciones, álbumes, etc.
- **Las respuestas `WEB_REMIX` son enormes.** El JSON con renderers anidados ocupa unos 300 KB–2 MB por página. En un dispositivo pequeño hay que parsearlo en streaming o pedir a otro proceso que lo compacte.
- El `visitorData` se obtiene raspando `ytInitialData` de la web (`getVisitorData`). El login es opcional y va por cookies.

### 1.2 Resolución de la URL de audio: la parte difícil

Este es el flujo de `YouTube.player()` en 2026:

1. Hace un `player` con `WEB_REMIX` más `signatureTimestamp` para leer los metadatos y `playabilityStatus`.
2. Llama a `newPipePlayer()`, que delega en el **extractor PipePipe/NewPipe** (Tier 1, "ciphers solved locally") con BravePipe de respaldo. Piped también aparece como respaldo, con una lista de instancias públicas.
3. La firma y el reto `n` los resuelve **`FaradayCipherEngine`**. Descarga una tabla remota (`simpmusic-files/registry/player_configs.json` y la de Metrolist, `MetrolistGroup/faraday`) que **solo nombra** las funciones dentro de `base.js`. Después **descarga y ejecuta el `base.js` completo en QuickJS**. En el código:
   - `NATIVE_MEMORY_LIMIT_BYTES = 192 MB` por runtime;
   - un hilo con pila nativa grande, porque si no QuickJS hace SIGSEGV;
   - un mutex para no tener dos runtimes a la vez.
4. Los directos van por HLS con el cliente `ANDROID`, que es el único al que no le dan 403 en segmentos sin PoToken (medido por el autor el 2026-10-01).
5. El PoToken existe en el código (`createPoTokenChallenge`/`generatePoToken` vía la API `jnn-pa` de BotGuard), pero en la ruta principal está comentado. Se apoya en lo que haga PipePipe.

### 1.3 Estado del ecosistema (yt-dlp, 2025–2026)

- **Runtime JS obligatorio.** Desde yt-dlp 2025.11.12, el reto `n` y la firma se resuelven con scripts **EJS** (`yt-dlp-ejs`) ejecutados en **Deno** (por defecto), Node u otro runtime. Sin runtime faltan formatos. En 2026 son habituales roturas del tipo "found 0 n function possibilities" que se arreglan actualizando el solver (`--remote-components ejs:github`).
- **PoToken** (guía oficial del wiki, editada en julio de 2026):

  | Cliente | Requisito |
  |---|---|
  | `web`, `mweb`, `web_music` (= WEB_REMIX), `web_safari`, `tv_simply` | Exigen **GVS PoToken** |
  | `web` | Además da **solo formatos SABR** |
  | `android`, `ios` | GVS o Player PoToken |
  | `tv` | No exige PoToken, pero da formatos con DRM salvo cookies |
  | `web_embedded` | Sin PoToken; solo vídeos embebibles |
  | `android_vr` | Sin PoToken; sin "made for kids", y desde 2026 con episodios "solo SABR" (formato 18) y 403 intermitentes |

  - Los usuarios **Premium no necesitan GVS PoToken**.
  - Sin token, la URL da **HTTP 403** y hay riesgo de bloqueo de IP o cuenta.
  - Los tokens suelen ir ligados a la sesión (visitorData) o al videoId.
  - El proveedor de referencia es `bgutil-ytdlp-pot-provider`, que corre BotGuard en Node/Deno.
- **Formatos de audio** (googlevideo, `adaptiveFormats`):

  | itag | Formato | Bitrate | Notas |
  |---|---|---|---|
  | 251 | Opus en WebM | ~130–160 kbps VBR | El mejor gratis |
  | 250 / 249 | Opus en WebM | 70 / 50 kbps | — |
  | 140 | AAC-LC en MP4 | 128 kbps | MP4 fragmentado DASH (sidx/moof) |
  | 139 | HE-AAC | 48 kbps | — |
  | 141 | AAC | 256 kbps | Solo YouTube Music Premium |
  | 774 | Opus | 256 kbps | Solo YouTube Music Premium |

  Las URLs caducan a las ~6 h, van ligadas a la IP y miden fácilmente 800–1500 caracteres.

**Las partes duras, en orden:**
1. El reto `n` y la firma necesitan ejecutar JS real de YouTube. Va cambiando y requiere mantenimiento continuo.
2. El PoToken necesita BotGuard, que es otra VM JS ofuscada.
3. SABR, un protocolo de streaming propio basado en protobuf (UMP), que sustituye las URLs directas en clientes web.
4. Hay que rotar clientes y versiones cuando YouTube rompe uno.

SimpMusic, Metrolist y yt-dlp sacan parches casi mensuales.

---

## 2. ¿Puede el reproductor de serie reproducir streams HTTP?

Lo he analizado con `~/hiby/hp_strings.txt` (12 723 cadenas de `hiby_player`) y `rootfs/usr/lib`.

**Pila de red.** Encontré estas cadenas y bibliotecas:
- `libcurl.so.4` (7.68.0) más `libssl.so.1.1`/`libcrypto` (OpenSSL 1.1). Usa `curl_easy_*` y `curl_multi_*`.
- `../../source/unit/source/it_http.c` y `http_download_task`, que construye la petición a mano:
  - `GET %s HTTP/1.1`, `Range: bytes=%lld-%lld`;
  - interpreta `Content-Length`, `Content-Range`, `Accept-Ranges`, `Transfer-Encoding: chunked`, `Location:` (redirecciones), `Set-Cookie`, `TransferMode.DLNA.ORG`;
  - `Error(%d) while seeking.`, es decir, **seek por Range**;
  - `file_http`, con caché `%dkbps, cache [%dK/%dK]`.
- `file_hls` / `hls_download_task`, con `#EXT-X-TARGETDURATION`, `#EXT-X-MAP`, `#EXT-X-BYTERANGE`, `AES-128`, `SAMPLE-AES`, `#EXT-X-ENDLIST`. Es **HLS completo**, VOD y en directo.
- `file_samba` (SMB) y `dropbox`.
- **Límite: `the http url is too long (>1024)`.** Las URLs googlevideo directas suelen superarlo. Es otro motivo para usar URLs cortas servidas por un relay.

**Cómo entran las URLs en el reproductor:**
- **Radio por Internet**:
  - `radio.db`, con tablas `all_table` y `collect` y columnas `url_1..url_3`;
  - `/usr/resource/radio.txt`, en formato `grupo, nombre, url`, con emisoras CNR `.m3u8`;
  - `a:\radio.txt` en la SD y `radio.ini`;
  - en la UI, "Custom radio" (`radio_label_vg_self`).
- **M3U con URLs**: `malloc for m3uTag->url fail.`, `url:%s`, `#EXTM3U`. Las playlists m3u pueden contener `http://...`.
- **Renderer DLNA/UPnP**: `dlna_set_uri`, `dlna_play/pause/stop/seek/next/previous`, `dlna_get_position_info`, `system_if_dlna_turn_on`. Cualquier punto de control UPnP puede enviarle `SetAVTransportURI`, y eso incluye un proceso nuestro en el propio HiBy hablando con localhost. También hay AirPlay.
- Tidal y Qobuz ya funcionan así: `streamurl` → `file_http`.

**Códecs y contenedores** que muestran el motor "HiBy Engine 5e4c22e Build Aug 15 2025" y las `.so`:

| Contenedor (Demux) | Decodificador | Estado |
|---|---|---|
| MP3 Demux | `libmp3.so` | OK |
| M4A Demux (con iTunSMPB) | FDK-AAC (`libfdk-aac`, `libaac`), ALAC | OK. **Falta comprobar** si acepta MP4 fragmentado (DASH, itag 140) |
| OGG Demux (`OpusHead`, `OpusTags`, Vorbis, Speex) | `libopus.so`, `libvorbis.so` | **Ogg Opus OK** en principio |
| FLAC, WAV, AIF, APE, DSF/DFF, SACD, ASF/WMA, TS, AAC (ADTS), DTS | — | OK |
| **WebM/Matroska** | — | **No existe** (ni `matroska` ni `webm` ni `EBML`) |

**Conclusión:** el itag 251 (WebM) **no se puede pasar tal cual**. Hay dos caminos:
- **remux sin recodificar** a Ogg Opus (`ffmpeg -i x.webm -c:a copy x.opus`, coste de CPU despreciable);
- itag 140 (M4A). Si el demuxer no traga MP4 fragmentado, se remuxa con `ffmpeg -c copy -movflags +faststart` a MP4 normal.

Ambas cosas son triviales en un relay y muy costosas de hacer en el HiBy.

> Pendiente de validar en el dispositivo (fase 0):
> - que un `.m3u` con `http://PC:8000/x.opus` reproduzca y permita seek;
> - en qué puerto escucha el renderer DLNA y si acepta `SetAVTransportURI` con `.opus`;
> - el consumo de RAM del reproductor durante la reproducción HTTP (caché de `file_http`).

---

## 3. Opciones para nuestro dispositivo

### 3a) Cliente nativo 100 % en el HiBy (C estático, musl)

| Pieza | Elección | RAM estimada | Notas |
|---|---|---|---|
| TLS | BearSSL o mbedTLS estático | 40–80 KB por conexión, 0.3–0.6 MB de código | Handshake ECDHE de ~0.2–0.5 s en un X1600 |
| JSON InnerTube | jsmn o un tokenizer en streaming, extrayendo solo los campos necesarios | 64–256 KB de buffer | No se puede cargar 2 MB de JSON en un DOM |
| Catálogo (search/browse/next) | `WEB_REMIX` o `ANDROID_MUSIC` | ~0.5–1 MB | Factible |
| Firma y `n` con `base.js` completo | QuickJS (~600 KB de código) | **30–100 MB** (estimación; SimpMusic reserva 192 MB) | **Inviable** |
| `n` con solo la función extraída | QuickJS o duktape más extracción por regex/tabla | 2–5 MB | Frágil: la extracción es lo que se rompe cada mes |
| PoToken (BotGuard) | — | Decenas de MB y un DOM simulado | **Inviable** |
| Cliente sin JS ni PoToken (`android_vr`, `tv` + cookies) | — | Bajo | Funciona a ratos. Hoy da 403 y "solo SABR" de forma intermitente |
| Audio | Entregar la URL al reproductor | 0 extra | WebM no se puede, y la URL supera 1024 caracteres. Habría que remuxar en local con un proxy WebM→Ogg en C (~200–400 KB), o usar el itag 140 si el M4A Demux acepta fMP4 |

**Veredicto:** el catálogo sí es viable en local, pero la resolución de URLs no lo es de forma robusta con 7 MB libres. Lo máximo realista es un experimento con `android_vr` sin JS, que se rompería sin aviso. Además habría que reescribir en C la lógica que yt-dlp mantiene con un equipo dedicado. Coste de mantenimiento: muy alto.

### 3b) Relay en PC, RPi o NAS más UI fina en el HiBy (recomendado)

- **Relay (Python):**
  - `ytmusicapi` para search, home, álbum, artista, playlists, biblioteca (con cookies opcionales) y radio/`next`;
  - `yt-dlp` más Deno más `bgutil-ytdlp-pot-provider` para resolver streams, que se actualiza con `pip -U`;
  - `ffmpeg -c copy` para el remux.
  - Expone una **API HTTP plana y compacta pensada para C**:
    - TSV/JSON pequeño, de unos pocos KB por página;
    - portadas ya escaladas a 96/320 px en JPEG, o directamente RGB565 para hacer blit;
    - `/stream/<videoId>.opus` servido desde un archivo de caché con `Content-Length` y `Range`, de modo que el seek funcione.
- **RAM y CPU:** en el HiBy, **<1 MB extra** (cliente HTTP sin TLS más el parser de líneas, dentro de `hibyshell`). En el relay, ~150–300 MB (Python más Deno). Una Raspberry Pi 3/4 o cualquier PC sirve, y el remux tarda <1 s por canción.
- **Latencia al darle a play:** ~1.5–4 s (`yt-dlp` ~1–3 s más la descarga del primer trozo). Se puede reducir con prefetch de la siguiente canción de la cola.
- **Limitaciones:**
  - solo funciona en la LAN, o fuera de casa con un túnel o VPN hacia el relay, porque el HiBy no tiene Tailscale;
  - depende de que el relay esté encendido.

### 3c) Descarga/sincronización a la SD (offline)

- El relay o un script de PC descarga las playlists y canciones marcadas, las remuxa a `.opus` u `.m4a`, mete etiquetas y portada, y las copia a la SD (`Musica/YouTube/<playlist>/`). También genera `.m3u`, como ya hace `hibymix`.
- **Vías de transferencia:**
  - adb por WiFi;
  - la transferencia WiFi de serie (`http://<hiby>:4399`, "wifi_song");
  - el lector de tarjetas.
- **Ventajas:**
  - todas las funciones de serie: seek, gapless, ReplayGain, portadas, biblioteca y "recently added";
  - no consume batería en WiFi;
  - no tiene latencia.
- **Inconvenientes:** no es "streaming". Hay que esperar a sincronizar y ocupa SD.
- RAM en el HiBy: 0.

### Consideraciones legales y de ToS (breve)

- Los Términos de YouTube prohíben acceder al contenido por medios distintos del servicio y descargarlo sin un botón o licencia explícitos.
- Saltarse los mecanismos técnicos (firma y PoToken) se puede interpretar como elusión de medidas de protección (DMCA §1201 / art. 6 de la Directiva 2001/29/CE). Es el precedente de la retirada de youtube-dl en 2020, que GitHub revirtió.
- El riesgo práctico para un uso personal es bajo: bloqueos de IP o cuenta. Aun así:
  - uso **exclusivamente personal**;
  - nada de redistribuir ni exponer el relay a Internet;
  - mejor sin login, o con una cuenta secundaria. Con YouTube Music **Premium**, el uso es más defendible, se evita el PoToken GVS y se obtienen 256 kbps.
- La copia offline (3c) es la que más choca con el ToS, porque Premium solo permite descargas dentro de su app.

---

## 4. Recomendación y plan por fases

**Arquitectura: relay en la LAN más app nativa "YT Music" en `hibyshell`, con el reproductor de serie como motor de audio.**

```
[HiBy: hibyshell app "Música YT"]  --HTTP LAN-->  [relay: FastAPI + ytmusicapi + yt-dlp(+Deno, PoToken) + ffmpeg]
   | teclado iOS ya hecho, listas, portadas RGB565            | /search /home /album /playlist /next(radio) /cover /stream/<id>.opus
   v
[hiby_player de serie] <-- URL corta http://relay:8765/stream/<id>.opus  (vía DLNA local, m3u con URLs o radio)
```

### Fase 0: validación (1–2 días, sin firmware nuevo)

1. Levantar en el PC un `python -m http.server` con `test.opus` (Ogg Opus), `test.m4a` (MP4 normal) y `test_frag.m4a` (itag 140 tal cual).
2. Copiar a la SD un `.m3u` con esas URLs y comprobar en el HiBy reproducción, seek y metadatos.
3. Activar DLNA en el HiBy y localizar el puerto (`netstat -ltnp`, descripción UPnP). Desde el PC, mandar `SetAVTransportURI` y `Play` con `curl` (SOAP). Comprobar `.opus` y la posición.
4. Probar a añadir una entrada en "Custom radio" o `radio.txt` con una URL del relay.
5. Medir `MemAvailable` y el RSS del reproductor durante la reproducción HTTP.

### Fase 1: MVP mínimo funcional

**Relay "hibytube"**, ~300 líneas de Python:
- `GET /search?q=` → `id\ttitulo\tartista\tduracion\tthumb` (solo canciones, 20 resultados).
- `GET /stream/<id>.opus`:
  1. `yt-dlp -f 251 -o -` → `ffmpeg -c copy -f ogg`;
  2. guarda en caché un archivo completo (~4 MB por canción) y lo sirve con `Range`;
  3. respaldo: `-f 140` → m4a con faststart.
- `GET /cover/<id>?w=96` → JPEG pequeño, que el HiBy decodifica con el IJG que ya usa `hibywall`.

**App en `hibyshell`**: icono "YouTube Music", campo de búsqueda con el teclado iOS existente, lista de resultados, y un toque que dispara la reproducción. Para la reproducción, por orden de preferencia:
- DLNA a localhost, si la fase 0 lo confirma; da play, pausa, seek y siguiente;
- `.m3u` "YouTube.m3u" en la SD más una fila en `M3U_TABLE` (como `hibymix`), abierto con navegación inyectada.

Con esto ya se busca y se escucha una canción desde el HiBy. El "Now Playing" es el de serie.

**Variante aún más mínima (MVP-0, sin código en el HiBy):** el relay hace de punto de control DLNA y empuja la URL al HiBy. Se controla desde una web en el móvil. Sirve para validar la cadena completa en una tarde.

### Fase 2: experiencia "SimpMusic-lite"

- Home, Explorar, álbumes, artistas, playlists de la biblioteca (`ytmusicapi` con cookies del navegador) y **radio/autoplay** (`next`/`get_watch_playlist`). La cola se genera como un m3u de URLs del relay.
- Prefetch de la siguiente canción en el relay y caché LRU en disco.
- **Sincronización offline (3c)**: botón "Descargar" sobre una playlist o álbum que deja en la SD archivos `.opus`/`.m4a` etiquetados más un `.m3u`, y reescanea la biblioteca.
- Letras sincronizadas (LRCLIB) servidas como `.lrc` junto a la canción. El reproductor de serie ya muestra `.lrc`.

### Fase 3: opcional

- Scrobbling a YouTube, para que el historial alimente las recomendaciones (`playbackTracking` en el relay).
- Acceso fuera de casa: relay detrás de un VPS o túnel con token (solo uso personal).
- Experimento puramente local con el cliente `android_vr` sin JS, como "modo sin relay" de emergencia, asumiendo que se romperá.

### Por qué no el cliente 100 % nativo

- La pieza crítica, la URL de audio, exige hoy un motor JS con decenas de MB, PoToken (BotGuard) y seguir los cambios semanales de YouTube.
- El relay concentra esa fragilidad donde hay RAM y `pip install -U yt-dlp` lo arregla.
- El HiBy se queda con lo que hace bien: UI rápida y decodificación Opus/AAC de calidad. Para eso no gasta más de ~1 MB de su escasa memoria.

---

## Fuentes

- SimpMusic: https://github.com/maxrave-dev/SimpMusic. El código analizado es https://github.com/maxrave-dev/core (rama `multiplatform`, `service/kotlinYtmusicScraper`: `YouTube.kt`, `Ytmusic.kt`, `models/YouTubeClient.kt`, `cipher/FaradayCipherEngine.kt`, `cipher/QuickJsEngine.kt`).
- Tablas de player: https://raw.githubusercontent.com/maxrave-dev/simpmusic-files/main/registry/player_configs.json y https://raw.githubusercontent.com/MetrolistGroup/faraday/master/registry/player_configs.json
- yt-dlp PO Token Guide (editada el 2026-07-12): https://github.com/yt-dlp/yt-dlp/wiki/PO-Token-Guide
- yt-dlp EJS / runtime JS: https://github.com/yt-dlp/yt-dlp/wiki/EJS · https://gigazine.net/gsc_news/en/20251113-yt-dlp-required-deno-javascript-runtime · https://github.com/yt-dlp/yt-dlp/issues/15897
- `android_vr` y SABR: https://github.com/yt-dlp/yt-dlp/issues/16150 · https://discuss.cachyos.org/t/audiotube-26-04-3-stopped-playing-youtube-music-http-403-yt-dlp-works/34459
- Análisis local: `~/hiby/hp_strings.txt` (cadenas de `hiby_player`, HiBy Engine build 2025-08-15), `~/hiby/rootfs/usr/lib` (libcurl 7.68.0, OpenSSL 1.1, libopus, libvorbis, libfdk-aac, libmp3, libwma) y `~/hiby/rootfs/usr/resource/radio.txt`.
