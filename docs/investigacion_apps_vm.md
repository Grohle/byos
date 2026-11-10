# Investigación: arnés de emulación HiBy R3 II + navegador + Telegram

Fecha: 2026-11-09. No se ha tocado el dispositivo real (sin adb). Todo se ha medido en emulación.

## 0. Resumen ejecutivo

| Pregunta | Veredicto |
|---|---|
| ¿Hay un arnés reproducible? | **Sí.** `qemu-system-mipsel` (malta, MIPS32r2 LE, sin FPU) con un kernel propio. Da `/dev/fb0` real de 320x480 a 16 bpp (virtual 320x960, panning) y los 5 `/dev/input/eventN` con los mismos nombres y el mismo formato de eventos que el HiBy. `MemTotal` = 56928 kB (el HiBy real: 56936). Arranca en unos 4 s. |
| Navegador | **Viable con el reproductor parado y para páginas ligeras.** He portado NetSurf 3.11 en binario estático mipsel/musl con un backend propio para fb0, touch y teclas. Funciona en el arnés: toque, scroll arrastrando, teclas y teclado en pantalla. RSS: 4–7 MB en páginas ligeras y unos 20 MB en un artículo medio de Wikipedia. Las páginas grandes (más de 1 MB de HTML) acaban en OOM. Con el reproductor vivo (unos 10 MB libres) solo sirven páginas muy ligeras, y hay thrashing. |
| Alternativa más ligera | Modo texto o lectura: `links2 -dump`/`w3m` consumen 3–7 MB incluso con el artículo enorme. La opción más robusta es un **proxy de lectura en el PC más un visor nativo mínimo** (estimado en menos de 2 MB). |
| Telegram en el dispositivo (TDLib) | **No viable.** En x86_64, TDLib 1.8.66 ocupa unos 29 MB de RSS solo para llegar a "esperando número de teléfono", sin datos de cuenta. Además no hay paquete para mipsel y su compilación exige del orden de 1 GB de RAM por fichero. |
| Telegram recomendado | **Cliente fino en el HiBy más un relay en el PC/RPi con TDLib** (opción b). Para avisos basta un bot (opción c). |
| Bloqueo principal (no técnico de emulación) | El reproductor de serie es dueño del framebuffer y no hay forma limpia de lanzar apps externas desde su interfaz. Se necesita un demonio de combinación de teclas o un parche binario, y coordinar el audio (ver §6). |

---

## 1. El arnés

### 1.1 Ubicación

- Arnés: `~/hiby/vm` (en Windows: `\\wsl.localhost\Ubuntu\home\alber\hiby\vm`).
- Copia de fuentes y scripts: `C:\Users\alber\Downloads\Hiby\hibymod\apps\harness_vm\`.

Contenido de `~/hiby/vm`:

| Ruta | Qué es |
|---|---|
| `run_vm.sh` | Lanza la VM. |
| `hibyctl.py` | Cliente del host: tap, swipe, key, shot, sh. |
| `out/vmlinux-hiby` | Kernel 6.1.192 MIPS32r2 LE propio, configuración en `out/kernel.config`. |
| `rootfs.ext4` | Rootfs Debian 12 mipsel con netsurf-fb, links2, w3m y busybox, más las herramientas `/hiby/*`. |
| `share/` | Carpeta 9p montada en el guest como `/mnt/share`. Ahí se dejan binarios nuevos sin regenerar la imagen. |
| `tools/*.c` | `hibyinput` (demonio de entrada y captura), `evdump`, `memhog`, `fbtouch` (app de prueba y plantilla), `nsfb_hiby.c` (backend NetSurf). |
| `scripts/` | Scripts para reconstruir todo, más los `.cmds` de las pruebas. `vmexp.sh` ejecuta un experimento completo. |
| `qemu/`, `xtc/` | QEMU 8.2 y la cross-gcc 13 extraídos con `apt download` + `dpkg -x`, sin root. |
| `xsysroot/` | Dependencias estáticas mipsel: zlib, mbedtls 2.28, curl 8.11, libpng, libjpeg 9f, expat. |

### 1.2 Uso

```bash
# en WSL
cd ~/hiby/vm
./run_vm.sh --profile coexist        # o: solo | none ; --icount ; --interactive (consola serie en stdio)
python3 hibyctl.py fbinfo
python3 hibyctl.py tap 160 240
python3 hibyctl.py swipe 160 400 160 100 400
python3 hibyctl.py key VOLUMEUP       # POWER, VOLUMEDOWN, PLAYPAUSE, NEXTSONG, PREVIOUSSONG o código numérico
python3 hibyctl.py shot captura.png   # área VISIBLE de fb0 (respeta yoffset)
python3 hibyctl.py sh 'free; ps'
# experimento completo desatendido:
bash scripts/vmexp.sh logs/x.log solo scripts/nsfb.cmds
```

Para probar una app nueva:

1. Compilarla con `~/hiby/tools/zbuild.sh`.
2. Copiarla a `~/hiby/vm/share/`.
3. Ejecutarla en el guest como `/mnt/share/app`. Si existe `share/autorun.sh`, se lanza solo al arrancar.

Para medir: `sh /hiby/measure.sh SEGUNDOS cmd...`. Muestrea VmRSS cada 0,5 s y al final da VmHWM, MemAvailable mínimo y si intervino el OOM killer.

Reconstrucción desde cero, todo sin root, en este orden:

1. `scripts/getqemu.sh`
2. `aptsetup.sh` y `aptdl.sh` (Debian mipsel)
3. `getx.sh` (cross-gcc)
4. `build_kernel.sh`
5. `build_tools.sh`
6. `mkrootfs.sh`
7. `build_deps.sh`, `host_png.sh`, `getpkgconf.sh`, `getgperf.sh`
8. `build_netsurf.sh`
9. `pkg_netsurf.sh`

### 1.3 Qué es fiel y qué no

| Aspecto | Fidelidad | Detalle |
|---|---|---|
| ISA | Alta | MIPS32r2 little endian, CPU `24KEc` sin FPU, así que hay que usar soft-float como en el HiBy. Las instrucciones propias de XBurst (MXU/SIMD) no existen en QEMU. Los binarios de Debian (hard-float) funcionan por la emulación de FPU del kernel, lo que los hace más lentos de lo real. |
| Kernel | Media | 6.1.192, no 4.4.94. Las ABI de syscalls, fbdev y evdev son compatibles para binarios estáticos musl. Faltan los drivers Ingenic: audio, LCD real, gestión de energía. |
| Framebuffer | Alta | `vfb` parcheado. Resolución 320x480, 16 bpp RGB565 (R=11, G=5, B=0), virtual 320x960, `FBIOPAN_DISPLAY` y `mmap` funcionan. **No** emula el límite de unos 44 Hz ni `FBIO_WAITFORVSYNC`. Tampoco hay fbcon. El `id` es "hiby-vfb" y no el del driver real. |
| Entrada | Alta | Dispositivos `uinput` con los nombres y el orden exactos (`event0` md-gpio-keys, `event1` goodix-ts, `event2` jz adc keyboard, `event3` sa-ring-keys, `event4` earpods_adc). Touch con protocolo A idéntico, verificado con `evdump`: BTN_TOUCH, ABS_MT_POSITION_X/Y, TOUCH_MAJOR=22, WIDTH_MAJOR=22, TRACKING_ID=0, SYN_MT_REPORT, SYN_REPORT. Al soltar: BTN_TOUCH 0 + SYN_MT_REPORT + SYN_REPORT (configurable). Los códigos de tecla están en `/etc/hibyinput.conf`, porque los reales de event2/3/4 están **sin verificar**. |
| RAM total | Alta | `mem=66248K` deja MemTotal en 56928 kB (real: 56936). Sin swap. Un OOM killer real actúa igual que en el dispositivo. |
| RAM disponible | Media | Hay dos perfiles basados en los presupuestos que diste. `coexist` usa `memhog`, que fija memoria con mlock hasta dejar unos 10 MB en MemAvailable, simulando el reproductor vivo. `solo` deja unos 38 MB. Es una aproximación: el reproductor real tiene páginas de código desalojables, comparte libs y mantiene buffers de audio. |
| CPU / tiempos | Baja–media | En modo normal (TCG) la velocidad no representa el XBurst. Con `--icount` el reloj del guest avanza a 1 instrucción = 1 ns (1 GIPS, IPC=1). Sirve como cota optimista: el XBurst real, con cachés y memoria más lentas, tendrá IPC menor de 1, quizá 1,5–3 veces más lento. La red pasa por slirp, así que su latencia es la del PC. |
| Red | Media | virtio-net + slirp (NAT al PC), no WiFi. No reproduce la latencia ni la pérdida de una WiFi real. |
| Audio | Nula | No hay audio. No se puede medir la convivencia con la reproducción: cortes, CPU robada al decodificador. |

---

## 2. Navegador

### 2.1 Candidatos evaluados

1. **NetSurf 3.10 de Debian** (`netsurf-fb`, glibc, dinámico, 82 libs, JS con duktape). Solo trae los surfaces ram, sdl, x, vnc y wld. **La 3.11 ya no tiene surface `linux`** (fbdev directo), así que hay que escribirlo. Lo medí con el surface `ram` como referencia.
2. **NetSurf 3.11 portado (el mío).** Estático, musl, `-Os`, sin JS, fuente interna en bitmap (sin freetype), PNG/JPEG/GIF/BMP, curl + mbedtls. Compilado con zig cc.
   - Binario: **4,05 MB** en `apps/netsurf-hiby/nsfb`; con recursos, 4,7 MB.
   - Backend propio `nsfb_hiby.c`, registrado como surface `linux`, que hace lo siguiente:
     - pinta directamente en el `mmap` de fb0 (yoffset 0);
     - lee `event1`: un toque corto es un clic, y un arrastre de más de 10 px hace scroll con la rueda (cada 16 px);
     - lee las teclas: VOL+ y VOL- son RePág/AvPág, PREV y NEXT mueven a izquierda y derecha, PLAY es Escape y POWER cierra.
   - `Choices`: `memory_cache_size` 1 MB, sin caché en disco, teclado en pantalla activado, JS desactivado.
3. **links2 / w3m de Debian** en modo `-dump`, como medida del coste de un motor de solo texto.

### 2.2 Resultados medidos (MIPS, en el arnés)

RSS pico = VmHWM del proceso. "anon" es la memoria privada real; "file" son las páginas de código, que son desalojables.

| Página (HTML) | NetSurf 3.11 estático, perfil solo (38 MB) | NetSurf 3.11 estático, perfil coexist (10 MB) | NetSurf 3.10 Debian (solo) | links2 -dump (solo) |
|---|---|---|---|---|
| example.com (1 KB) | **4,1 MB** (anon 0,9) | — | 20,1 MB (anon 5,1 / file 15) | — |
| lite.cnn.com | **6,7 MB** (anon 3,1) | 9,1 MB; código desalojado a 0,9 MB, MemAvailable mínimo 1,2 MB, **thrashing**: abrir un enlace tardó 30 s | 23,5 MB (anon 7,3) | unos 4,6 MB* |
| text.npr.org | **5,3 MB** (anon 1,7) | 4,4 MB** | 23,1 MB (anon 6,2) | — |
| Wikipedia "Ingenic Semiconductor" (220 KB) | **20,0 MB** (anon 12,8) | 20,4 MB; **el OOM killer mató al "reproductor" (memhog)** | — | unos 4,6 MB*; w3m unos 3,1 MB* |
| Wikipedia "Telegram (software)" (1,39 MB) | **OOM** (anon en unos 39 MB en menos de 15 s) | OOM | **OOM** (anon 39 MB) | unos 7,0 MB*, completa |

\* links2 y w3m terminan antes de poder leer VmHWM. Lo que se da es la caída de MemAvailable durante la ejecución, que también incluye page cache. Es aproximado.

\*\* En esa prueba seguía viva la instancia anterior (cnn), así que el dato está contaminado.

Otras observaciones:

- **Arranque**, con `--icount` a 1 GIPS (estimación optimista del dispositivo):
  - `example.com` por HTTP estaba pintado unos 10 s después del lanzamiento, según cuándo se estabiliza el RSS;
  - `lite.cnn.com` por HTTPS tardó unos 45 s, por el handshake TLS con mbedtls soft-float, la carga y las subpeticiones;
  - en el dispositivo real cabe esperar entre 10 y 60 s por página según el tamaño y el TLS.
- **Usabilidad a 320x480 con solo touch** (capturas en `docs/capturas/vm/`):
  - La barra de NetSurf (atrás, adelante, historial, parar, recargar, URL) cabe en 28 px y el texto se lee bien.
  - El scroll por arrastre funciona (`c_cnn2_scrolled.png`), igual que VOL- para avanzar página.
  - Tocar un enlace navega (`c_cnn4_link.png`).
- **Teclado en pantalla:**
  - Hace falta para escribir URLs o buscar, y NetSurf ya trae uno (botón "a" abajo a la derecha, `r3_osk_key.png`).
  - Sus teclas miden unos 23x17 px en 320 px de ancho, menos que el contacto de un dedo (TOUCH_MAJOR 22). Es usable pero propenso a errores.
  - Recomiendo un teclado propio más grande, o usar marcadores y una página de inicio con enlaces y evitar teclear.
- Wikipedia redirige `en.m.` a la versión de escritorio y en una ocasión devolvió "429 Too many requests" al User-Agent de NetSurf.

### 2.3 Veredicto del navegador

- **Con el reproductor parado (unos 35–40 MB):** NetSurf estático es viable para webs ligeras (versiones lite y text de noticias, blogs, docs) y artículos medianos de Wikipedia, con 4–20 MB. No es viable para páginas grandes o modernas con mucho DOM.
  - Hay que añadir una **protección contra OOM**: `setrlimit(RLIMIT_AS/DATA)` de unos 24 MB en el lanzador y `oom_score_adj=1000` en el navegador, para que el kernel mate al navegador y no al reproductor ni a otros procesos.
- **Con el reproductor vivo (unos 8–12 MB):** solo páginas mínimas, y con thrashing de código porque no hay swap. Con un artículo de Wikipedia el OOM killer eligió matar al proceso que hacía de reproductor. **No es recomendable sin limitar la memoria.**
- **Alternativa más ligera recomendada:** un **"modo lectura" con proxy en el PC o la RPi**. El PC descarga la página, la simplifica (readability, quitar JS/CSS, imágenes a 320 px en JPEG o RGB565) y la sirve paginada. En el HiBy, un visor nativo mínimo (C estático con fuente bitmap, como `fbtouch`) pinta el texto, por debajo de 2 MB estimados. Es la única opción cómoda que convive con la música.
- Una opción intermedia, sin escribir visor: NetSurf apuntado a ese proxy, con unos 5 MB por página.

---

## 3. Telegram

### 3.1 Datos

Medido (TDLib 1.8.66, `libtdjson.so` precompilada de PyPI `python-telegram`, x86_64, script `apps/harness_vm/scripts/tdmem.py`):

- Cargar la librería suma **6,4 MB** de RSS; el `.so` pesa 41,5 MB en disco.
- Tras `setTdlibParameters` (DC de pruebas, sin cuenta, con bases de datos activadas) hasta `authorizationStateWaitPhoneNumber` suma **+22,4 MB**. En total son **unos 29 MB de RSS sin ningún chat cargado**, con 5 hilos.
- `MALLOC_ARENA_MAX=1` no cambia nada, así que no es efecto de las arenas de glibc.
- Inferencia: en MIPS32 los punteros son de 4 bytes, lo que daría quizá 60–70 % de esa cifra, unos 18–20 MB de base. A eso se suma la caché en RAM de usuarios y chats, que según los mantenedores no se libera sin cerrar la instancia.

Fuentes (comprobadas por web):

- **Memoria:** el mantenedor dice que "unos 10 MB por instancia nueva es aceptable" y que la memoria crece unos 180 KB/h en reposo ([td#2807](https://github.com/tdlib/td/issues/2807)). Desactivar las bases de datos **aumenta** la RAM ([td#2516](https://github.com/tdlib/td/issues/2516), [td#1704](https://github.com/tdlib/td/issues/1704)). La caché solo se vacía cerrando la instancia ([td#2573](https://github.com/tdlib/td/issues/2573)).
- **Opciones:** `message_unload_delay`, `ignore_background_updates`, `disable_*` ([core.telegram.org/tdlib/options](https://core.telegram.org/tdlib/options)).
- **Compilación:** C++17, OpenSSL, zlib, gperf y CMake; hasta unos 1 GB de RAM por fichero con GCC ([README](https://github.com/tdlib/td)). Hay que compilar en cruzado desde el PC.
- **Paquetes:** Debian no publica libtdjson para mipsel; el armhf instalado ocupa 19,7 MB ([packages.debian.org](https://packages.debian.org/sid/libtdjson1.8.38)).

Alternativas MTProto ligeras:

- **vysheng/tg, tgl, telegram-purple:** muertos. Usan una capa antigua; desde 2021 Telegram rechaza capas antiguas con `UPDATE_APP_TO_LOGIN` (IDs de 64 bits) y los usuarios reportan "outdated app" al hacer login ([kenorb-contrib/tg#64](https://github.com/kenorb-contrib/tg/issues/64)). No es viable mantener un cliente C propio que siga las capas (la capa actual es la 225).
- **gotd/td (Go):** declara unos 150 KB por cliente en reposo y Go soporta mipsle soft-float. Un binario estático rondaría los 15–30 MB, con un RSS estimado de unos 10–15 MB (sin medir). Es la única vía de cliente completo en el dispositivo con alguna posibilidad, y solo con el reproductor parado.
- **grammers (Rust):** mipsel es target tier 3; no hay datos de memoria.

### 3.2 Términos y seguridad

- **api_id/api_hash propios** son obligatorios (my.telegram.org; una api_id por número) ([obtaining_api_id](https://core.telegram.org/api/obtaining_api_id), [API ToS](https://core.telegram.org/api/terms)).
  - El nombre de la app no puede contener "Telegram" salvo precedido de "Unofficial", y no se puede usar el logo.
  - No se permiten funciones tipo "modo fantasma" ni romper los mensajes autodestructivos.
- **Vigilancia de clientes no oficiales:** las cuentas que usan clientes no oficiales "se ponen bajo observación", y hay reportes de baneos con api_ids nuevas. Recomiendo usar una cuenta antigua, uso personal normal y nada de automatizaciones agresivas.
- **Seguridad:**
  - Un cliente completo en el HiBy guarda la *auth key* de la sesión en una flash sin cifrar, en un dispositivo fácil de perder y con root abierto. Perderlo equivale a perder la sesión hasta revocarla desde otro dispositivo.
  - Con relay, la auth key se queda en el PC y el HiBy solo tiene un token revocable.
- **Bot API:** un bot no puede leer tus chats privados, solo aquellos en los que participa, y no puede iniciar conversaciones. Límite de unos 30 mensajes/s y 1 por chat por segundo. Existe un servidor `telegram-bot-api` autoalojable ([core.telegram.org/bots](https://core.telegram.org/bots), [faq](https://core.telegram.org/bots/faq)).

### 3.3 Arquitecturas

**(a) Cliente completo en el dispositivo: no viable.**
- Con TDLib: más de 20 MB de base, más la caché, más el TLS y la UI. No cabe junto al reproductor y, con el reproductor parado, iría al límite sin margen.
- Con gotd/td: posible solo con el reproductor parado y por verificar.

**(b) Cliente fino más relay: recomendado.** Esquema:

```
[HiBy] hgram (C estático, ~1-2 MB RSS, UI fb0+touch, fuente bitmap)
   │  TLS (mbedtls, cert del relay fijado) o TCP en LAN + HMAC-PSK
   ▼
[PC/RPi] hgram-relay (Python + TDLib o Telethon/Pyrogram) ── MTProto ──► Telegram
```

- **Transporte:** una conexión TCP persistente. Las tramas son JSON de una línea, de 64 KB como máximo.
- **Emparejamiento:** el relay muestra un código de 6 dígitos. El HiBy lo envía y recibe un `device_token` de 32 bytes, revocable desde el relay. Después, cada sesión hace `hello{device_token, nonce}` y el relay contesta con un `challenge` firmado con HMAC-SHA256.
- **Peticiones del HiBy al relay:**
  - `chats{offset,limit≤20}`
  - `history{chat_id, from_msg_id, limit≤20}`
  - `send{chat_id, text, reply_to?}`
  - `read{chat_id, max_msg_id}`
  - `media{msg_id, kind:"thumb320"}`: el relay transcodifica a JPEG de 320 px de ancho como máximo, o a RGB565 crudo.
  - `voice{msg_id}`: el relay convierte a MP3 o PCM para que el HiBy lo reproduzca.
- **Eventos del relay al HiBy:** `new_msg{chat_id,msg_id,from,text,date}`, `edit`, `delete`, `typing`, `unread{chat_id,count}`.
- **Robustez:** el relay mantiene la cola y el estado. El HiBy solo cachea en RAM los últimos N mensajes del chat abierto, y si se corta la WiFi vuelve a pedir `history` al reconectar.
- **Coste en el HiBy:** socket, TLS y UI de texto estimados en menos de 2–3 MB, así que **cabe con el reproductor vivo**. La entrada de texto necesita un teclado en pantalla grande o respuestas rápidas predefinidas.
- **Coste en el PC/RPi:** TDLib usa unos 30–100 MB según la cuenta, lo cual es irrelevante allí.
- **Atajo sin escribir el relay:** bitlbee + tdlib-purple expone tu cuenta como IRC, y en el HiBy bastaría un cliente IRC mínimo. Pero tdlib-purple se archivó en agosto de 2026, así que no lo recomiendo a largo plazo.

**(c) Bot API para notificaciones.**
- Lo más simple y seguro, pero solo cubre avisos.
- Un script en el PC hace de usuario-bot (con Telethon o TDLib) o de reenviador. Opcionalmente, el propio HiBy hace polling de `getUpdates` de un bot propio por HTTPS (curl + mbedtls ya compilados) y muestra un banner o un aviso sonoro.
- No permite leer los chats privados reales sin el relay de (b).

---

## 4. Recomendación

1. **Navegador:** usar el port NetSurf estático ya construido (`apps/netsurf-hiby/`) para el modo "reproductor parado", con un lanzador que haga lo siguiente:
   1. pare o pause el reproductor y libere fb0;
   2. aplique `ulimit -v` de unos 24–28 MB y `oom_score_adj=1000`;
   3. arranque `run.sh` con una página de inicio de marcadores ligeros (versiones lite y text);
   4. al salir con POWER, restaure el reproductor.

   A medio plazo, **proxy de lectura en el PC más un visor nativo** para poder navegar con la música sonando.
2. **Telegram:** opción **(b) relay**. Primero el relay en Python con TDLib en el PC o la RPi; después el cliente `hgram` en C estático reutilizando el patrón de `fbtouch` y mbedtls. Opción (c) como primer paso trivial para notificaciones.
3. **Siguiente paso de medición en hardware real** (cuando el HiBy esté libre):
   - confirmar los códigos de event2, event3 y event4 con `evdump`;
   - medir `free` con el reproductor sonando;
   - medir el tiempo real de carga de `lite.cnn.com`;
   - comprobar que `FBIOPAN_DISPLAY` y el formato RGB565 coinciden;
   - sobre todo, si el reproductor tolera que otra app escriba en fb0.

## 5. Entregables

- `C:\Users\alber\Downloads\Hiby\hibymod\apps\netsurf-hiby\`: `nsfb` (estático mipsel, 4,05 MB), `res/`, `Choices`, `run.sh`.
- `C:\Users\alber\Downloads\Hiby\hibymod\apps\fbtouch\`: app de prueba y plantilla (fb0 con doble buffer más touch y teclas).
- `C:\Users\alber\Downloads\Hiby\hibymod\apps\harness_vm\`: fuentes y scripts del arnés, más `config-6.1.192-hiby` y los logs de todas las mediciones (`logs_mediciones/`).
- `C:\Users\alber\Downloads\Hiby\hibymod\docs\capturas\vm\`: capturas del framebuffer emulado.

## 6. Bloqueos pendientes

1. **El framebuffer es del reproductor de serie.** Escribe en fb0 y hace panning. Una app externa necesita alguna de estas vías:
   - pararlo (SIGSTOP o kill) y restaurarlo al salir;
   - que el reproductor deje de pintar mientras tanto, lo que exige un parche;
   - pintar encima y aceptar parpadeos.

   No se ha podido probar sin el dispositivo.
2. **No hay forma limpia de lanzar apps desde la interfaz de serie**, que solo invoca unos pocos scripts del sistema. Hace falta una de estas dos cosas:
   - un **demonio de combinación de teclas**: un C estático diminuto que lee event0 y event3 sin grab, por ejemplo con POWER más VOL- largo, y lanza el lanzador;
   - un **parche binario** para que una entrada de menú o uno de esos scripts invoque el lanzador.
3. **Convivencia con la reproducción.** Con el reproductor vivo solo quedan unos 8–12 MB y no hay swap. En la prueba el OOM killer eligió al proceso que simulaba el reproductor. Hay que fijar `oom_score_adj` (reproductor en -1000, apps en 1000) y límites `RLIMIT_AS`. Además, el decodificador puede sufrir cortes por CPU, algo que el arnés no puede medir porque no tiene audio.
4. **Entrada de texto:** el teclado en pantalla de NetSurf es pequeño para el dedo. Hace falta uno propio.
5. **Códigos de tecla reales** de event2, event3 y event4: sin verificar. Son configurables en `/etc/hibyinput.conf` y el backend de NetSurf usa los códigos estándar.
6. **Velocidad real:** el arnés solo da una cota optimista con `--icount` (1 GIPS). Falta medir en el dispositivo, sobre todo el TLS con mbedtls soft-float.
7. **Telegram:** api_id propia y riesgo de vigilancia o baneo de la cuenta con clientes no oficiales. Hay que mantener la auth key fuera del HiBy (relay).
