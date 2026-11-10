# ¿Sustituir hiby_player por MPD + UI propia? Investigación (HiBy R3 II 2025)

Fecha: 2026-11-09. Marcas: **[V]** = verificado en el dispositivo o en el rootfs extraído; **[I]** = inferido o estimado (sin probar en el hardware).

## 1. Ruta de audio: viable con ALSA estándar, sin ioctls propietarios

- **Controles ALSA de la tarjeta 0 [V]** (`amixer contents`): `Left/Right Playback Volume` (0–255), `Mute Output`, `Soft Mute`, `Digital Filter` (0–6), `ES9219 THD C2/C3_CH1/CH2` (compensación de THD, 0–65535), `MQA Enable`, `MQA Lock`, `Output Port Switch` (0–7) y `DOP_EN` (bool). Los PCM son `pcmC0D0p` (I2S→ES9219) y `pcmC0D1p` (S/PDIF, dai `x1600-spdif`) [V].
- **Cambio de frecuencia y modo DSD dentro del kernel [V]**: el driver de la tarjeta (`x1600_hiby_r3ii_sound_card.ko`) llama a `hbc3000_set_samplerate`, `hbc3000_set_spdif_mode` y `es9219_set_power_dac1/2` desde `hw_params` y desde `dop_en_put`/`codec_board_ot_put`. El chip auxiliar HBC3000 (que el kernel programa por JTAG/SPI) gestiona los relojes. Un `snd_pcm_hw_params` normal basta.
- **No hacen falta ioctls propietarios para el audio [V]**: el reproductor original solo abre `/dev/sa-config`, que únicamente devuelve el modelo `r3ii_2025`, y `/dev/sa_hgl_dma`, un DMA de usuario que se usa para gráficos (6 MB de memoria reservada) y no para audio. Para la salida analógica escribe PCM con ioctls ALSA en crudo (`ot_tiny_alsa`, `SNDRV_PCM_IOCTL_*`) y para Bluetooth usa libasound (`bluealsa:DEV=%s`).
- **Configuración original [V]** (`/usr/resource/ot_devices_2025.json`):
  - Frecuencias analógicas de 32 k a **768 kHz**; la salida S/PDIF llega hasta 192 k.
  - DSD **siempre por DoP** en la salida analógica (`AnalogDsdNative: "dop"`), así que el máximo es DSD256 (DoP a 705,6 k).
  - Bluetooth hasta 192 k y DAC USB externo (tarjeta 3) hasta 384 k.
- **Volumen y ganancia [V]**: la ganancia alta/media/baja **no es un relé analógico**. Son tres tablas de atenuación (HDB/MDB/LDB) sobre el registro de volumen del ES9219, con 101 pasos, donde 0 equivale a 0 dB y 255 a silencio. A eso se suma una tabla de volumen software (`SET_HW_SW_VOL_BOTH`) y `LO_VOLUME_INDEX: 100` para line out. Todo esto se puede replicar fácilmente. **Riesgo:** con el reproductor en reposo los dos volúmenes valen 0 (= 0 dB, el máximo). Nuestro daemon debe fijar un volumen seguro **antes** de abrir el PCM.
- **Puertos [V/I]**:
  - [V] `sa_sound_switch` publica la detección en `/sys/class/switch/{headset,balance}/state`.
  - [I] El reproductor elige el puerto escribiendo en `Output Port Switch`. La correspondencia de los valores 0–7 con headset, balanced, lineout y S/PDIF **falta confirmarla**: basta leer `amixer cget name='Output Port Switch'` mientras el usuario cambia de salida.
  - Ojo: `/sys/devices/platform/hm100` (los filtros "Darwin" de FPGA) **no existe** en este modelo [V]. Aquí solo hay 7 filtros internos del ES9219.
- **Bit-perfect [I]**: MPD con `device "hw:0,0"`, `mixer_type "none"` y sin resampler debería ser bit-perfect. El volumen se ajusta escribiendo los controles L/R desde nuestro daemon, porque MPD solo maneja un control de mezclador. El reproductor original usa PCM de 16 y 32 bits [V, strings], así que MPD tendrá que negociar S32 para 24 bits.
- **DSD con MPD [I]**: usar `dop "yes"`. El problema es que `DOP_EN` debe estar activado solo mientras suena DSD, y MPD no gestiona controles por formato. Hay dos soluciones:
  - un parche pequeño (~30 líneas) en la salida ALSA de MPD (la opción robusta);
  - que la UI lo active al ver `audio: dsd…` en el estado (puede llegar tarde y producir un pop o ruido inicial).
  - Hay que comprobar si `DOP_EN=on` afecta al PCM normal.
  - El DSD nativo y el D2P a la manera de HiBy no aplican. MPD sí tiene conversión DSD→PCM propia, pero con mucho coste de CPU [I].
- **Lo que se pierde**:
  - **Se pierde del todo**: MSEB, PEQ/EQ de HiBy, el renderizado MQA (el ES9219 lo soporta, pero el desempaquetado lo hace el reproductor) y el balance L/R.
  - **Se pierde si no se reimplementa**:
    - AirPlay: `shairport -o ot` envía el audio **a través de hiby_player** por `/var/run/airplay_socket_server` [V]. Habría que pasar a shairport-sync con salida ALSA.
    - DLNA (`dmrd`) [V].
    - Modo DAC USB (gadget UAC + `/dev/uac_sa`, gestionado por el reproductor) [V].
  - **Se mantiene**: gapless y ReplayGain (MPD los soporta de serie) y los filtros digitales (`Digital Filter`).

## 2. MPD en este hardware

- **CPU/ABI [V]**: XBurst a 1,1 BogoMIPS·10³, un solo núcleo, sin ASEs DSP declaradas. El userland original es **hard-float** (el FP ABI de hiby_player es "Hard float"), así que para MPD conviene compilar `mipsel-linux-musleabihf` y no soft-float, porque faad, opus, vorbis y la conversión DSD usan coma flotante.
- **Compilación [V/I]**: MPD actual exige **C++23 (GCC 14 / clang 19)**, Meson ≥1.2 y fmt (mpd.readthedocs.io). zig 0.17.1 incluye clang/libc++ recientes, así que es viable [I]. Habría que compilar en estático, con dependencias mínimas:
  - libFLAC, libmad o mpg123, faad2, opus y libogg/vorbis (o tremor, en enteros), wavpack;
  - dsf/dff ya vienen integrados en MPD;
  - para ALAC/M4A haría falta un FFmpeg recortado (+1–1,5 MB).
  - Estimación: 1–2 semanas hasta tener una toolchain estable [I].
- **RAM [I]**: el núcleo ocupa unos 3–5 MB de RSS si `audio_buffer_size` baja a 1–2 MB (por defecto son 4 MB). **El problema es la base de datos**: MPD la mantiene entera en memoria, a unos 0,4–1 KB por pista (menos con un `metadata_to_use` mínimo). Así queda en la práctica:

  | Pistas | Base de datos en RAM | ¿Cabe en ~12 MB? |
  |---|---|---|
  | 2 500 | ~1–2,5 MB | Sí |
  | 10 000 | ~4–10 MB | Justo |
  | 50 000 | ~20–50 MB | No |

  Para colecciones grandes: MPD sin base de datos (reproducción por ruta) más un índice SQLite propio en la UI.
- **Arranque [I]**: 1–3 s con 10 k pistas desde la SD. El primer escaneo de 50 k pistas tardaría decenas de minutos.
- **Coste de decodificación [I]** (por analogía con ARM11 a 700 MHz / Cortex-A5): FLAC 16/44 ~5–10 %, FLAC 24/96 ~10–20 %, MP3 (libmad) ~10–20 %, Opus/AAC ~15–25 % en hard-float. DoP DSD256 cuesta poco (empaquetado). DSD→PCM en float sube a un 30–60 %.

## 3. UI propia (fb0 + evdev)

- **Base recomendada**: LVGL v9 con sus drivers `lv_linux_fbdev` y `lv_evdev`. El táctil goodix es multitouch tipo A en píxeles [V], y LVGL usa el primer punto. Para las teclas se lee event0/2/3 [V].
- **RAM [I]**:
  - Búferes de dibujo: 2 × 1/10 de pantalla = 60 KB, o 1 buffer completo de 300 KB (320×480×2).
  - El fb mmapeado (600 KB) es memoria del kernel.
  - Heap de LVGL: 0,5–1 MB.
  - Carátula 300×300 RGB565: 180 KB.
  - Fuentes latinas pre-renderizadas (lv_font_conv, 4 bpp): 50–150 KB por tamaño.
  - Para CJK, `tiny_ttf` (stb_truetype) con caché de glifos, porque pre-renderizadas ocuparían MB.
  - **Total UI: ~3–6 MB de RSS. Con MPD, ~7–12 MB**: cabe en el presupuesto solo si el reproductor original no está corriendo [I].
- **Transiciones tipo iOS [I]**: no hay que re-renderizar LVGL en cada frame.
  1. Se hace un `lv_snapshot` de la pantalla de origen y otro de la de destino.
  2. Se anima un desplazamiento con easing copiando filas (300 KB por frame, ~1–2 ms).
  3. Se presenta con el pan del doble buffer (yoffset).
  - Hay que evitar opacidad, sombras y blur a pantalla completa.
  - El objetivo es ir sincronizado con los ~44 Hz del panel [V], o con 22 Hz en las escenas pesadas.

## 4. Integración y riesgos

- **[V] `hiby_player.sh` ejecuta `reboot` cuando hiby_player termina.** Antes de nada hay que cambiar el lanzador. El reproductor activo ya es `/usr/data/hibymod/fast/hiby_player`, así que hay un punto de inyección.
- **Exclusividad**: hiby_player tiene abiertos fb0, `sa_hgl_dma` y los 5 evdev [V], y ocupa unos 14 MB de RSS [V]. Con MemAvailable en ~12–13 MB [V], **no pueden convivir** los dos reproductores con UI. El híbrido tiene que ser secuencial: uno se detiene y se lanza el otro.
- **Fallback seguro**: un lanzador que lea un flag (p. ej. `/usr/data/hibymod/player=stock|mpd`) y vuelva a stock si el alternativo falla N veces o si se pulsa una tecla en el arranque. Hay que ejecutar el alternativo **sin reboot en bucle**.
- **sys_server** [V] seguiría siendo necesario, porque hace montaje de SD y USB-MSC (`MOUNT:*` por `/var/run/sys_server`), D-Bus de bluez/bluealsa, shairport on/off y MPRIS. La UI tendría que hablar ese protocolo de socket, que no está documentado.
- **Bluetooth [V/I]**: bluealsa ya corre y el plugin `libasound_module_pcm_bluealsa.so` está presente [V]. MPD puede usar `device "bluealsa:DEV=…"` como salida [I]. El emparejamiento por D-Bus quedaría a cargo de nuestra UI.
- **DAC USB (modo gadget)**: lo implementa el reproductor original. Rehacerlo es caro.

## 5. Esfuerzo y recomendación

**Recomendación:** **(a)** como uso diario, y **(b)** solo como prototipo con condiciones de paso. **No ir a (c) por ahora.** El audio PCM y DoP es técnicamente viable sin blobs, pero alcanzar la paridad real (MSEB/PEQ, USB DAC, AirPlay/DLNA, MQA, gestión de energía y biblioteca grande en 56 MB de RAM) costaría unos 3–6 meses-persona [I]. Además, lo más valorado del original se perdería o habría que reescribirlo.

| Fase | Contenido | Esfuerzo [I] |
|---|---|---|
| 0. Verificación (puerta) | MPD estático hard-float. Con el usuario presente y el reproductor original parado: `aplay --dump-hw-params` en hw:0,0, mapeo de `Output Port Switch`, prueba de DoP con `DOP_EN`, medición de RSS y CPU con 2,5 k y 10 k pistas | 1–2 sem |
| 1. Motor sin UI | MPD + daemon de control (tablas HDB/MDB/LDB, puertos, filtro, DoP, volumen seguro) + lanzador con fallback | 2–3 sem |
| 2. UI LVGL básica | Reproduciendo, cola, navegación, ajustes, teclas, transiciones con snapshot | 4–6 sem |
| 3. Biblioteca | Índice SQLite propio, caché de carátulas, fuentes CJK | 2–3 sem |
| 4. Sistema | BT/bluealsa, suspensión y batería, protocolo de sys_server | 3–5 sem |
| 5. Extras | PEQ (filtro propio de MPD con biquads), AirPlay con shairport-sync, (USB DAC) | 3–6 sem |

**Siguientes pasos si se hace (b):**
1. Cambiar `hiby_player.sh` por un lanzador con selector y fallback, sin `reboot`.
2. Ejecutar la Fase 0 y decidir con estos criterios:
   - salida bit-perfect a 44,1–768 k;
   - DoP funcionando;
   - MPD + UI por debajo de 10 MB de RSS con la biblioteca real.
3. Si la base de datos de MPD no cabe, valorar un motor propio más ligero (dr_flac/minimp3 + ALSA dentro de la misma app), que rondaría 2–4 MB.
