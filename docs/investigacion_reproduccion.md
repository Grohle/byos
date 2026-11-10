# Lo que suena y control de la reproducción (HiBy R3 II, hiby_player)

Fecha: 2026-11-10. **[V]** = verificado en el aparato o en el binario; **[I]** = inferido, sin probar.

## Resumen

| Necesidad | Solución recomendada | Estado |
|---|---|---|
| Pista actual (ruta, título, artista, álbum, duración) | `hibymod_disp.so` v5 anota los ficheros de audio que abre el reproductor; `np_get()` lo interpreta y lee las etiquetas con hibytag | Código hecho y probado en el PC; en el aparato se activa al reiniciar |
| ¿Suena o está en pausa? Posición | v5 cuenta las tramas escritas al PCM (tinyalsa o libasound) | Igual |
| Lo mismo **sin** la v5 (la que hay ahora) | `/proc/<pid>/fd` + `fdinfo` (posición de lectura del decodificador) | **Funciona ya** [V] |
| Carátula 128×128 RGB565 | `np_cover()`: imagen incrustada (FLAC/ID3/M4A) o la de la carpeta → `hibywall` → caché | **Funciona ya** [V] (0,28 s sin caché, <5 ms con caché) |
| Play/pausa, siguiente, anterior | Inyectar la tecla en el evdev del botón físico (`playctl.inc.c`) | Play/pausa **[V]**; siguiente y anterior [I], con el mismo mecanismo |
| Reproducir un fichero, álbum o lista desde un índice | No hay interfaz externa. Ver §5 | Pendiente (RE) |
| Saltar a una posición (seek) | No hay tecla. Pulsación larga de NEXT/PREV [I], o RE (§5) | Pendiente |

## 1. Qué expone el reproductor (y qué no)

- **D-Bus [V]**: en el bus del sistema solo están `org.bluez`, `org.bluealsa`, `sys_server` (:1.0) y `bt-agent`. hiby_player **no** está en D-Bus. La política del bus rechaza incluso `Introspect` hacia sys_server y bt-agent.
- **MPRIS [V]**: sys_server registra en bluez un `org.mpris.MediaPlayer2.Player` en `/org/bluez/hiby/player`, pero **solo con propiedades** (`PlaybackStatus`, `Position`, `Metadata`) y únicamente para AVRCP con auriculares BT. Los datos se los manda el reproductor (`BT:METADATA`, `BT:PLAYER_STATUS`, `BT:POSITION`). No sirve para controlar y solo existe con BT conectado.
- **Sockets unix [V]**:
  - `/var/run/sys_server` y `/var/run/sys_client` (datagramas): órdenes de sistema (`WIFI:*`, `BT:*`, `MOUNT:*`, `DLNA:*`, `INPUT:ADD`…). No hay órdenes de reproducción. `BT:CONTROL:%s %s %d` va del reproductor a sys_server, para mandar a un *sink* BT.
  - `/var/run/airplay_socket_server` y `airplay_meta_server`: audio y metadatos de shairport hacia el reproductor.
  - **`/data/dmr_streamer`** (servidor del reproductor, para `dmrd`): acepta `play`, `pause`, `stop`, `seek`, `next`, `previous`, `set_uri`, `set_meta`, `get_position_info`, `set_volume`… (tabla en `0x920998`, manejadores en `0x453a60`–`0x4551e0`).
    - Cada orden se ignora salvo que el ajuste nº 0x35 (`0x42a500(0x35)`, DLNA activado) valga 1.
    - `set_uri` entra en el modo DLNA (vista `lg_dlna`): no hay cola ni pantalla normal, y next/prev se reenvían a dmrd.
    - **No recomendado** (obliga a cambiar un ajuste y deja el reproductor en otro modo), pero es la única vía "reproduce esta URI" que existe. No se ha probado.
- **Ficheros [V]**:
  - `/usr/data/user.ini` (binario, se reescribe al cambiar de pista o de estado) guarda en `0x28` la ruta UTF‑16 de la última pista (`?:\Artista\Álbum\pista.flac`, con la letra de unidad a 0).
  - `momery_list.lst` guarda la posición de la lista.
  - El reproductor no relee ninguno de los dos en caliente: solo sirven para leer.
- **Teclas [V]**:

  | Dispositivo | Teclas |
  |---|---|
  | `md-gpio-keys` (event0) | 116 (encendido) y 165 (`PREVIOUSSONG`) |
  | `jz adc keyboard` (event2) | 163 (`NEXTSONG`) y 164 (`PLAYPAUSE`) |
  | `sa-ring-keys` (event3) | rueda de volumen, 105/106 |
  | `earpods_adc` (event4) | 163/164/165/168/208/114/115 |

  El reproductor tiene abiertos event0/2/3/4 (y escucha `INPUT:ADD` para dispositivos nuevos).
- **strace [V]** (está en `/usr/bin/strace`). Al pulsar play:
  1. `lg_music_player` (tid 1021) abre la pista 2 veces: un sondeo de unos 0,4 s y luego **el fd del decodificador, que queda abierto mientras la pista está cargada, también en pausa**.
  2. `album_cover_par` (1072) la abre otra vez para la carátula.
  3. `ot device` (993) abre `/dev/snd/pcmC0D0p`.
  4. **`HiBy_Main_Thread` (1022) escribe el audio con `ioctl(SNDRV_PCM_IOCTL_WRITEI_FRAMES)`** (tinyalsa propio; es el ioctl de libc, y la librería ya lo intercepta).

  **En pausa [V]**: el PCM y el fichero siguen abiertos, pero no hay más escrituras. Antes del primer play tras arrancar no hay nada abierto.

## 2. Control: inyección de teclas (`src/playctl.inc.c`)

Un `write()` de `struct input_event` en `/dev/input/eventN` inyecta el evento en ese dispositivo (`evdev_write` → `input_inject_event`), y el kernel se lo entrega a **todos** los que lo tienen abierto: el reproductor lo recibe como si se pulsara el botón.

- No hace falta uinput ni la librería.
- Funciona con la pantalla apagada y en cualquier vista.
- Es lo mismo que hace ya `player_pause()` en `store.inc.c`.

**[V] 2026-11-10**: `KEY_PLAYPAUSE` en "jz adc keyboard" arrancó la reproducción y, al repetirlo, la pausó. Se comprobó con strace: 0 escrituras al PCM en 2 s. Se dejó en pausa, como estaba.

API:

- `pc_toggle()`, `pc_next()` y `pc_prev()`. Buscan el dispositivo físico que tiene la tecla (`EVIOCGBIT`), saltando los virtuales.
- `pc_play()` y `pc_pause()`: solo pulsan si hace falta, mirando `np_get()`.
- `pc_key(code, hold_ms)`: pulsación larga para probar el avance rápido [I].
- `pc_play_file()` devuelve `-ENOSYS` (ver §5).

Limitación: es un control "de botón". No se puede elegir pista ni posición.

## 3. Lo que suena: librería v5 + `src/nowplaying.inc.c`

### Lado de la librería (`src/hibymod_disp.c` v5; `bin/hibymod_disp.so` compilada, **sin desplegar**)

- **Campos nuevos al final de `struct hm_state`** (`hm_shared.h`). `HM_MAGIC` no cambia, y además hay un `np_magic = HM_NP_MAGIC` propio: un shell viejo no ve los campos nuevos y una librería vieja deja `np_magic = 0`. Hay un `_Static_assert` que comprueba que cabe en 64 KB.
- **`np_open[8]`**: cada apertura de un fichero de audio que hace el reproductor, venga de `open`/`open64`/`openat`/`openat64`/`fopen`/`fopen64`:
  - qué se guarda: ruta, fd, tid y ms de apertura y de cierre (de `close`/`fclose`), tramas PCM en el momento de abrir, y la última `lseek`/`lseek64` (desplazamiento, ms y tramas);
  - al reutilizar una ranura se elige la cerrada más antigua, así las lecturas rápidas de carátulas al navegar no expulsan al decodificador;
  - todo va con cerrojo de secuencia `np_seq` y con un mutex propio, **nunca el de la pantalla**.
- **PCM**:
  - sobre el fd de `/dev/snd/pcmC*p`: `WRITEI`/`WRITEN_FRAMES` (tramas, `pcm_last_ms`), `HW_PARAMS` (frecuencia, bits, canales), `DROP`, `PREPARE` y `PAUSE`;
  - además se envuelven `snd_pcm_writei`, `snd_pcm_hw_params` y `snd_pcm_drop` de libasound (Bluetooth por bluealsa y DAC USB). Una marca por hilo evita contar dos veces los ioctl internos de libasound.
- **Coste**: unas comparaciones de fd por llamada, sin bloqueos en el camino normal.

### Lado del shell (`np_get`)

- **Pista** = el fichero de audio abierto más antiguo que lleve más de 600 ms abierto y cuyo fd siga vivo en `/proc/<pid>/fd` (esto protege de cierres que no se vean).
  - Con gapless, la siguiente se abre antes de que acabe la actual; se pasa a ella cuando el decodificador cierra la anterior, unos segundos antes de oírse [I].
  - Si no hay ninguno abierto: la última que estuvo abierta más de 1,5 s (`stopped = 1`).
- **Reproduciendo** = PCM abierto, sin pausa y con una escritura hace menos de 400 ms.
- **Posición** = tramas escritas desde que se abrió la pista ÷ frecuencia. Va por delante, como mucho, lo que ocupa el búfer del PCM.
  - Un salto se detecta como una `lseek` del decodificador a ±1,5 s de un `DROP`. Desde ese punto, la posición = bytes → tiempo, con la zona de audio que se lee de la cabecera, y luego otra vez tramas.
  - Es aproximado en VBR [I, sin probar en el aparato].
- **Sin la v5** (`NP_SRC_PROC`, lo que hay instalado ahora):
  - la pista sale de `/proc/<pid>/fd` (el fichero de audio con el fd más bajo);
  - "suena" si `fdinfo pos` avanzó en los últimos 2,5 s (el decodificador lee a ráfagas);
  - la posición sale de bytes → tiempo, menos 2 s.
- **Recién encendido** (`NP_SRC_LAST`): la ruta de `user.ini`, con `playing = 0`.
- **Etiquetas**:
  - `ht_read` de hibytag, si se incluyó antes (en el shell lo incluye `tageditor.inc.c`);
  - si no, el nombre del fichero y las carpetas.
- **Duración**: FLAC STREAMINFO, MP3 Xing/Info/VBRI o CBR, M4A `mvhd`, WAV y DSF. Se cachea por pista.
- **Coste**: con la v5 es `memcpy` de 3,5 KB más un `readlink`. Medido en el aparato con la fuente `/proc`: 20 ms en total, contando el arranque del proceso.
- **Recomendación**: llamar cada 500 ms con el Centro de control abierto y cada 1–2 s para los widgets; pedir la carátula cuando cambie `track_id`.

### Carátula (`np_cover(path, out[128*128])`)

1. Busca la portada incrustada (FLAC PICTURE tipo 3, ID3v2.2/2.3/2.4 APIC/PIC, M4A `covr`). Si no hay, la de la carpeta (`cover`, `folder`, `front`… en `.jpg`/`.png`).
2. Copia los bytes a `cache/np/tmp.img` en bloques de 16 KB.
3. Ejecuta `hibywall IMG RAW 128 128`. libjpeg reduce al decodificar y corre en otro proceso, así que no sube la memoria del shell.
4. Guarda el resultado en `/usr/data/hibymod/cache/np/<hash(carpeta, tamaño)>.raw` (32 KB): la siguiente pista del mismo álbum sale de la caché.

hibytag no leía imágenes (solo `ht_set_cover` las escribe); el localizador va en `nowplaying.inc.c`.

**[V]**: la carátula de "La ley innata" sale correcta. 0,28 s la primera vez, 0,00 s desde la caché. Como puede tardar hasta 0,5 s sin caché, hay que llamarla desde un hilo o al cambiar de pista, nunca dentro de una animación.

### Herramienta de prueba: `bin/hibyplay` (`src/hibyplay.c`, `tools/build_hibyplay.sh`)

```
hibyplay status [-w MS]    # fuente, PLAYING/paused, posición/duración, ruta, etiquetas
hibyplay raw               # volcado de hm_state.np_* (v5), incluido rt_tid
hibyplay toggle|play|pause|next|prev
hibyplay cover [RUTA] OUT.raw
```

Probado en el PC con un `hiby_player` falso y la librería compilada para x86: sondeo + decodificador + `fopen`/`fclose`, cambio de pista, parada y carátulas FLAC/MP3. En el aparato, con la v4 instalada: `status` y `cover` [V].

## 4. Protección del audio (petición extra): hilo PCM a SCHED_FIFO

- **Qué hace [I hasta reiniciar con la v5]**: la primera vez que un hilo escribe al PCM (el ioctl WRITEI de tinyalsa o `snd_pcm_writei`), la librería lo pasa a `SCHED_FIFO` con prioridad 10 (`pthread_setschedparam`).
  - En este firmware ese hilo es `HiBy_Main_Thread` (tid 1022 en esta sesión) [V].
  - Escribe una línea en `/usr/data/hibymod/disp.log` y deja `rt_tid`/`rt_prio`/`rt_err` en `hm_state`.
  - Se desactiva creando `/usr/data/hibymod/no_rt`.
- **Por qué es seguro**: el hilo pasa casi todo el tiempo bloqueado esperando al DMA. FIFO solo le da prioridad al despertar; no le da más CPU.
  - El kernel tiene el límite de tiempo real activo: `sched_rt_runtime_us = 950000` / `period = 1000000` [V]. Aunque el hilo se desbocara, la UI conservaría al menos el 5 %.
  - Hoy todos los hilos del reproductor son `SCHED_OTHER`, nice 0 [V].
- **El decodificador (`lg_music_player`) no se toca**: sin medir no hay pruebas de que haga falta, y darle tiempo real a un hilo que consume CPU sí podría robársela a la UI. Si tras medir hiciera falta, la opción prudente es `nice -5` (`setpriority`) y no FIFO.
- **Cómo comprobarlo tras reiniciar**:
  1. `cat /usr/data/hibymod/disp.log`, que debe decir `rt: hilo N (HiBy_Main_Threa) SCHED_FIFO 10: ok`.
  2. `hibyplay raw`.
  3. `chrt -p N`, que debe dar `SCHED_FIFO`, prioridad 10.
  4. O el campo 41 de `/proc/<pid>/task/N/stat`, que debe valer 1. Ojo: los nombres con espacios desplazan los campos, mejor `chrt`.
  5. Probar sin cortes: grabación de la UI a 44 Hz con música 24/192 sonando.

## 5. Reproducir un fichero, un álbum o una lista: opciones

No hay ninguna orden externa para eso (§1). Opciones, de más a menos recomendable:

1. **Llamada interna desde la librería, en el hilo de la UI** [I, recomendada para la app Música].
   - La librería ya corre dentro del reproductor y tiene un punto de entrada en su hilo de presentación (`ioctl FBIOPAN`), donde se puede ejecutar una orden encolada en `hm_state`.
   - Falta localizar por RE la función que usa el explorador de ficheros o la lista de canciones al tocar una pista, es decir, "cargar esta lista y empezar en el índice i". Pistas para buscarla:
     - el despachador de mensajes `0x4368e0(id, buf, 0)`, que usa el manejador `play` de DLNA con id 0x15;
     - las cadenas `lg_music_player`, `hl_media_list_get_playlist`, `SELECT rowid FROM list_tb_%d WHERE path = ?`, `<path><cue_id>` y `open Y:\music_list.lst fail`.
   - La cola vive en `list_tb_1` de `usrlocal_media.db`. Escribirla directamente no basta: el reproductor la tiene cacheada.
   - Coste estimado: 2–4 días de RE + pruebas, con reinicio por iteración.
2. **Automatizar la UI de fábrica bajo la captura** [I]: el shell pone `C` (el panel muestra su imagen mientras el reproductor sigue dibujando debajo) y navega Explorador → carpeta → pista con toques inyectados. Funciona sin RE, pero es frágil (posiciones, idioma, tiempos) y tarda 1–2 s.
3. **DLNA `set_uri`** sobre `/data/dmr_streamer`: exige activar DLNA (un ajuste) y deja el reproductor en modo DLNA, sin cola. Descartado.
4. **Motor propio (MPD)**: ver `investigacion_reproductor_mpd.md`. Descartado por ahora.

## 6. Arquitectura recomendada

```
hiby_player ── hibymod_disp.so v5 ──► /dev/shm/hibymod_fbstate (np_open[], pcm_*, rt_*)
                                              │ mmap (solo lectura)
hibyshell ── nowplaying.inc.c: np_get() cada 0,5–1 s ──► CC "Now Playing", widgets, mini-reproductor
          ├─ np_cover() al cambiar track_id (hilo) ──► caché 128×128 en /usr/data/hibymod/cache/np
          └─ playctl.inc.c: teclas evdev ──► reproductor (play/pausa/sig./ant.)
App Música (cuadrícula de álbumes): índice propio leyendo una COPIA de usrlocal_media.db;
          "reproducir álbum" = opción 1 de §5 cuando esté (mientras tanto, opción 2)
```

**Integración en el shell (sin tocar otros ficheros en este encargo)**: `#include "nowplaying.inc.c"` y `#include "playctl.inc.c"` después de `tageditor.inc.c`. Los nombres van con prefijo `np_`/`pc_`/`NP_`.

**Despliegue de la librería**: copiar `bin/hibymod_disp.so` a `/usr/data/hibymod/preload/` y reiniciar. Hasta entonces `np_get` usa la fuente `/proc`, que ya funciona.
