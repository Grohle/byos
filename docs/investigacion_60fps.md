# ¿60 fps en la UI del HiBy R3 II? Panel, controlador LCD y opciones

Fecha: 2026-11-10.

Marcas: **[V]** verificado (lectura de solo lectura en el aparato, desensamblado de los `.ko` o hoja de datos); **[I]** inferido, sin probar en el hardware.

En el aparato solo se hicieron lecturas: `devmem <addr> 32` **sin valor**, `cat` de sysfs/proc, `/proc/interrupts` y `hibyctl pantime 40` (40 presentaciones del mismo búfer, sin cambio visible). No se escribió nada, ni se reinició, ni se mató ningún proceso.

---

## 0. Resumen

1. **El panel no tiene memoria propia.** El controlador es un **Sitronix ST77903** (módulo `lcd_st77903_8bit.ko`). Es un driver **sin RAM** ("without Display Ram") conectado por **RGB serie de 8 bits**: cada píxel ocupa 3 ciclos de DOTCLK. El DPU del X1600 lo **refresca continuamente** desde la DDR, con una cadena de descriptores que apunta a sí misma [V].
   - La hipótesis anterior era: "SLCD/MCU-8080 con GRAM; cada `FBIOPAN` transfiere la imagen". **Es incorrecta.**
   - `FBIOPAN_DISPLAY` **no transfiere nada**: cambia la dirección del búfer y **espera al final del fotograma en curso**. `pantime` da tiempos repartidos entre 0,5 y 22,1 ms, no constantes [V].
2. **Los 44 fps son el refresco del panel.** Valor exacto: **44,30 Hz (22,57 ms)**. El panel se configura a "45 Hz" (`/sys/class/graphics/fb0/modes` = `U:320x480p-45`). El reloj de píxel queda en MPLL/33 = 42,42 MHz y el fotograma ocupa 957.681 ciclos. El IRQ de `jzfb` cuenta 222 interrupciones en 5,02 s, es decir, 44,2 Hz [V].
3. **60 fps quedan fuera de la especificación del panel.** La hoja del ST77903 exige un **tiempo de línea > 40 µs**.
   - Con 480 líneas visibles más el borrado vertical, eso limita el panel a **~50 Hz como máximo dentro de especificación**.
   - A 60 Hz la línea dura ~33–34 µs, un **15–18 % por debajo del mínimo**.
   - Se puede *probar*, pero no hay garantía de imagen correcta: el riesgo es de carga insuficiente de los píxeles (contraste, color, bandas, parpadeo). El daño físico es improbable [I].
4. **No sirven para subir los fps:**
   - **Actualizaciones parciales:** el panel no tiene GRAM y el DPU barre siempre el fotograma entero; no hay ventana CASET/RASET en modo RGB.
   - **Bus de 16 bits:** el ST77903 en RGB solo admite 8 bits serie.
   - **Ajustes de DMA:** el ancho de banda es de 13,5 MB/s, trivial.
   - Los dirty rows solo ahorran CPU en la copia.
5. **Recomendación:**
   - **(a)** Hacerlo perfecto a 44,3 Hz: cero fotogramas perdidos y animaciones muestreadas al instante de presentación (§5).
   - **(b)** Si se quiere más: parchear `lcd_st77903_8bit.ko` en el firmware para **50 Hz dentro de especificación** (+13 %; valores exactos en §4.2).
   - **(c)** 60 Hz solo como experimento reversible (§4.3).

---

## 1. Cadena de pantalla (evidencia)

### 1.1 Módulos y parámetros [V]

| Elemento | Valor |
|---|---|
| Panel | `module_driver/lcd_st77903_8bit.ko` ("st77903_8bit lcd panel driver"). Init por SPI de 3 hilos por GPIO (CS=PB04, SCL=PA00, SDA=PA01, RST=PB05) |
| Controlador | `module_driver/soc_fb.ko` (Ingenic, `x1600_clean/ingenic.linux/module_driver/soc/x1600/fb/jzfb.c` + `lcdc.c`), `dpu@0x13050000` |
| Parámetros | `soc_fb.ko lcd_is_inited=0 frame_num=2 pan_display_sync=1` (`/sys/module/soc_fb/parameters`: 2 / 0 / 1) |
| fb0 | `modes = U:320x480p-45`, `virtual_size = 320,960`, 16 bpp, `stride = 640` |
| Sysfs útil | Solo `blank`, `pan`, `rotate`, `mode(s)`, `state`. **No hay ningún ajuste de reloj ni de temporización.** No existe `/sys/devices/platform/*lcd*` |

### 1.2 `lcdc_data` del panel (`.data` del `.ko`, desplazamiento de fichero 0x14a0) [V]

La disposición se deduce de `auto_calculate_pixel_clock` y `jzfb_blank` en `soc_fb.ko`:

| Off. | Campo | Valor |
|---|---|---|
| +0x04 | refresh | **45** |
| +0x08/+0x0c | xres/yres | 320 / 480 |
| +0x10 | pixclock | 0 (se calcula a partir de `refresh`) |
| +0x14 | left_margin (HBP) | 393 |
| +0x18 | right_margin (HFP) | 498 |
| +0x1c | upper_margin (VBP) | 7 |
| +0x20 | lower_margin (VFP) | 19 |
| +0x24 | hsync_len | 72 |
| +0x28 | vsync_len | 1 |
| +0x30 | lcd_mode | **4** (TFT RGB serie: 3 ciclos por píxel visible) |

**Fórmula del driver para `lcd_mode == 4`** (desensamblado de `auto_calculate_pixel_clock`):

```
pixclock = ((vtotal − yres) × htotal + (htotal + 2·xres) × yres) × refresh
```

- htotal = 72 + 393 + 320 + 498 = **1283**; vtotal = 1 + 7 + 480 + 19 = **507**.
- Ciclos por fotograma = 27×1283 + 1923×480 = **957.681**.
- Con refresh 45 se piden 43,10 MHz. `ingenic_cgu_set_rate` usa `div = ceil(padre/rate)`, así que sale **MPLL 1400 / 33 = 42,424 MHz**, lo que da **44,30 Hz** y una línea visible de 1923 ciclos = **45,3 µs**.

### 1.3 Registros leídos (solo lectura) [V]

| Dirección | Valor | Significado |
|---|---|---|
| CPM LPCDR `0x10000064` | `0x44000020` | Fuente MPLL (bits 31:30 = 01), bit 26 (lo pone el driver), CDR = 0x20 ⇒ /33 |
| CPM MPLL `0x10000014` | `0x0AF0C9CD` | 1400 MHz |
| `0x13059000` (HSYNC) | `0x00480503` | hsync 72, htotal 1283 |
| `0x13059004` (VSYNC) | `0x000101FB` | vsync 1, vtotal 507 |
| `0x13059008` (HDE) | `0x01D10311` | píxeles visibles 465..785 |
| `0x1305900c` (VDE) | `0x000801E8` | líneas visibles 8..488 |
| `0x13059010` | `0x00000004` | modo 4 (RGB serie) |
| `0x13051000` (dirección del descriptor) | `0x02525000` | Descriptor en DDR: `next = 0x02525000` (**él mismo ⇒ barrido continuo**), `buf = 0x02600000`, stride 0x140 |
| `0x13052004` (estado) | `0x0000000F` | |

### 1.4 Qué hace `FBIOPAN_DISPLAY` (`jzfb_pan_display` → `lcdc_pan_display`) [V]

Para `lcd_mode < 6` (TFT):
1. Escribe la dirección del nuevo descriptor en `0x13051000` y `1` en `0x13051004`.
2. Pone el estado a 1 y **duerme hasta la interrupción de fin de fotograma**: `DC_INT_FLAG` `0x13052010` bit 17 ⇒ estado = 2. El tiempo de espera máximo es de 30 jiffies.

Con `pan_display_sync = 1` se espera también esa misma condición. La rama SLCD (`lcd_mode ≥ 6`) sí sería "un disparo por fotograma", pero **no es la que se usa**.

**Medidas:**
- `hibyctl pantime 40` (esperas aleatorias de 5–27 ms entre presentaciones): `5.6 18.1 10.5 15.5 2.5 3.5 17.5 0.5 … 22.1 … 21.0`, todo entre 0,5 y 22,1 ms. Es una espera de vsync con periodo ≈ 22,6 ms, no una transferencia [V].
- `/proc/interrupts`, IRQ 39 `jzfb`: 32060 → 32282 en 5,02 s, es decir, **44,2 Hz**, con IRQ en cada fotograma [V].

**Por qué escribir después de presentar produce parpadeo:** al volver el pan, el DPU ya está barriendo *ese* búfer, así que escribir en él es tearing. La regla actual ("escribir y luego presentar") es correcta.

### 1.5 Init del ST77903 (decodificada de `st77903_8bit_power_on`) [V]

```
F0 C3 / F0 96 / F0 A5            (desbloqueo de comandos)
C1 66 07 4B 0B · C2 66 07 4B 0B · C3 44 04 44 04 · C4 44 04 44 04 · C5 45 00 · D6 00 · D7 00
E0 D2 09 0C 07 06 23 2E 43 46 17 13 13 2D 33   (gamma +)
E1 D2 09 0C 07 05 23 2E 33 46 17 13 13 2D 33   (gamma −)
E5 5A F5 64 33 22 25 10 77 77 77 77 77 77 77 · E6 (igual)
EC 00 55 00 00 00 88 · 36 (MADCTL, parámetro según rotación) · 3A 06 (18 bits)
B2 09 · B3 01 · B4 01 · B5 00 08 00 08 (porches del modo idle) · B6 EF 27 (480 gates, 320 columnas)
A4 C0 63 · A5/A6 00 00 00 00 00 0B 2A BA 02 · BA 5A 09 43 00 23 01 00
BB/BC 00 21 00 25 83 87 18 00 · BD 33 44 FF FF 98 A7 7A 89 52 FF 06
ED C3 · E4 40 0F 2F · A0 00 06 06 (RGBIF: RCM=0 DE+SYNC, VBP 6, HBP 6) · 35 00 (TE) · 21 · 38 · 11 · 29
```

El panel va en modo DE (`A0` RCM = 0). Por eso los porches enormes del host (HBP 393) funcionan aunque el IC diga HBP 6: los delimita la señal DE.

### 1.6 Hoja de datos del ST77903 (Sitronix, P0.5) [V]

Fuente: [ST77903_SPEC_P0.5.pdf](https://dl.espressif.com/AE/esp-iot-solution/ST77903_SPEC_P0.5.pdf).

- *"400RGB x 480 … without Display Ram"*. Interfaces: QSPI (50 MHz máx.) y *"Serial RGB (888, 666, 565)"*; *"only 6bit data format"*, D[7:0]; *"one pixel data needs to take three DOTCLKs"*.
- **p. 30: *"Minimum line time must be > 40us; Vsync width and Vsync porch > 40us"*.** Ejemplo: a 60 Hz con 400 líneas la línea dura 40,36 µs. **Con 480 líneas, 60 Hz no cabe.**
- p. 37: *"In without ram mode, hpw+hbp ≥ 60"*. Los mínimos y máximos de porches y de DOTCLK están en "TBD" (hoja preliminar).
- Espressif, para este mismo chip a 320×480 en 8 bits, usa el preset `ST77903_RGB_320_480_PANEL_48HZ_RGB_TIMING` (24 MHz, porches H 3/3/6, V 1/6/6): unos **48,8 Hz** con una línea de 41,5 µs. Es coherente con el límite de ~50 Hz ([cabecera esp_lcd_st77903_rgb](https://github.com/espressif/esp-iot-solution/blob/master/components/display/lcd/esp_lcd_st77903_rgb/include/esp_lcd_st77903_rgb.h)).

---

## 2. Máximo teórico

```
fps = f_DOTCLK / ciclos_por_fotograma
ciclos_por_fotograma = Vblank × htotal + 480 × (hblank + 960)

Límite del panel: t_línea = (hblank + 960) / f_DOTCLK ≥ 40 µs
  ⇒ fps_máx ≈ 1 / (40 µs × (480 + Vblank_efectivo))
```

- Con un borrado vertical mínimo realista (~14 líneas cortas): **≈ 50–51 Hz**.
- Hoy se pierde la mitad de cada línea en porches: 963 de 1923 ciclos. Pero eso no limita, porque el tope lo pone el tiempo de línea del panel, no el reloj.

---

## 3. Opciones evaluadas

| Opción | fps alcanzable | Riesgo | Veredicto |
|---|---|---|---|
| Actualizaciones parciales (CASET/RASET, solo filas sucias) | 44,3 (sin cambio) | — | **No aplica**: el panel no tiene RAM y el DPU barre siempre el fotograma completo. Los dirty rows solo ahorran memcpy |
| Bus de 16 bits | — | — | **Imposible**: el ST77903 en RGB solo tiene 8 bits serie (3 ciclos por píxel). El hardware no tiene más líneas cableadas a ese modo |
| RGB565 en vez de 666 | 44,3 | — | No cambia nada: siguen siendo 3 DOTCLK por píxel |
| DMA, ancho de banda | 44,3 | — | El DPU lee 13,5 MB/s de la DDR, que es trivial; no es el cuello de botella |
| Quitar las presentaciones del reproductor durante las animaciones | 44,3 **sin pérdidas** | Nulo | **Sí**. Cada pan bloquea hasta el vsync: si el reproductor y el shell presentan en el mismo periodo, el shell pierde un fotograma |
| Subir solo LPCDR (porches de serie) a /30 | **48,7** (línea 41,2 µs) | Bajo-medio | Dentro de especificación. Se revierte solo al apagar y encender la pantalla [I] |
| Porches recortados + refresh 50 en el `.ko` | **50,0** (línea 40,9 µs) | Bajo-medio | **Mejor opción dentro de especificación** (§4.2) |
| LPCDR /24 (porches de serie) | **60,9** (línea 33,0 µs, DOTCLK 58,3 MHz) | **Alto** (imagen) | Fuera de especificación un −18 % |
| Porches recortados + refresh 62 en el `.ko` | **61,1** (línea 33,5 µs, DOTCLK 51,85 MHz) | **Alto** (imagen) | Fuera de especificación un −16 %, con menos DOTCLK que la anterior |
| Overclock de CPU o DDR | 44,3 | Alto | No toca el refresco (ver `investigacion_overclock_energia.md`) |

**Tabla de divisores con los porches de serie** (957.681 ciclos por fotograma, MPLL = 1400 MHz):

| Divisor (CDR) | DOTCLK | Refresco | Línea | ¿Dentro de especificación? |
|---|---|---|---|---|
| /33 (0x20), hoy | 42,42 MHz | 44,30 Hz | 45,3 µs | Sí |
| /30 (0x1D) | 46,67 MHz | 48,73 Hz | 41,2 µs | Sí |
| /29 (0x1C) | 48,28 MHz | 50,41 Hz | 39,8 µs | Límite (−0,5 %) |
| /28 (0x1B) | 50,00 MHz | 52,21 Hz | 38,5 µs | No |
| /25 (0x18) | 56,00 MHz | 58,47 Hz | 34,3 µs | No |
| /24 (0x17) | 58,33 MHz | 60,91 Hz | 33,0 µs | No |

El DOTCLK máximo del X1600 en modo TFT no está verificado: la hoja de Mouser no se pudo descargar (bloqueo del servidor). Por encima de ~50 MHz, por un bus de 8 bits y un FPC, también hay riesgo de integridad de señal [I].

---

## 4. Cómo se haría (NO aplicado)

### 4.1 Ojo: el reloj se reprograma al despertar [V]

`jzfb_blank` (al desbloquear la pantalla) llama a `clk_set_rate(pclk, pixclock)` y reescribe todas las temporizaciones. Consecuencias:
- Un cambio por `/dev/mem` **se pierde en cada apagado y encendido de la pantalla**, si el reproductor usa `FBIOBLANK`, cosa probable [I]. Es una **red de seguridad para experimentar**, pero obliga a reaplicarlo.
- La vía limpia y permanente es **parchear el módulo del panel en el firmware**, igual que ya se hizo con `codec_es9219.ko` (`investigacion_firmware_oculto.md`).

### 4.2 Recomendado si se quiere más de 44: 50 Hz dentro de especificación (parche de `lcd_st77903_8bit.ko`)

Cambios en `.data`, palabras de 32 bits little-endian, desplazamiento de **fichero** = 0x14a0 + campo:

| Fichero | Campo | Valor de serie | Nuevo |
|---|---|---|---|
| 0x14a4 | refresh | 45 (`2D 00 00 00`) | **50** (`32 00 00 00`) |
| 0x14b4 | left_margin (HBP) | 393 | **300** (`2C 01 00 00`) |
| 0x14b8 | right_margin (HFP) | 498 | **404** (`94 01 00 00`) |
| 0x14c0 | lower_margin (VFP) | 19 | **6** (`06 00 00 00`) |

Resultado [I, calculado con la fórmula del driver]:
- htotal = 1096, vtotal = 494; 848.624 ciclos por fotograma.
- Se piden 42,43 MHz ⇒ `ceil(1400/42,43)` = 33 ⇒ 42,424 MHz (**el mismo reloj de hoy**) ⇒ **49,99 Hz**.
- Línea visible: 1736 ciclos = **40,9 µs**, que cumple > 40 µs. hsync + HBP = 372, que cumple ≥ 60. Borrado vertical: 14 líneas ≈ 360 µs, que cumple > 40 µs.
- Lo que se toca es la forma del barrido, no la frecuencia del bus: el riesgo eléctrico es mínimo.

Registros que debería dejar el driver, para comprobarlos con `devmem` tras arrancar:

| Registro | Valor esperado |
|---|---|
| `0x13059000` | `0x00480448` |
| `0x13059004` | `0x000101EE` |
| `0x13059008` | `0x017402B4` |
| `0x1305900c` | `0x000801E8` |
| LPCDR | `0x44000020` (sin cambios) |

Prueba de aceptación:
- `cat /sys/class/graphics/fb0/modes` debe dar `U:320x480p-50`.
- IRQ `jzfb` ≈ 250 por cada 5 s.
- `pantime` con un máximo de ~20 ms.
- Inspección visual: gris uniforme, degradados, texto fino, sin parpadeo ni bandas.

**Riesgo.** Si el panel no lo acepta, la pantalla sale mal y hay que reflashear con el `.ko` de serie desde recovery (la recovery oficial está en mtd3/mtd4). Antes de flashear, conviene validarlo en caliente con §4.3 a /30 (48,7 Hz).

### 4.3 Experimento reversible en caliente (solo LPCDR; requiere el permiso del usuario)

Secuencia estándar de Ingenic que sigue `ingenic_cgu_set_rate` [V la lógica]: escribir el divisor con CE, esperar a que se limpie BUSY y quitar CE. **Las posiciones de los bits** (CE = 29, BUSY = 28) siguen la convención de Ingenic y no están confirmadas con el manual del X1600 [I].

```sh
# 48,7 Hz (dentro de especificación): /30
devmem 0x10000064 32 0x6400001D
while [ $(( $(devmem 0x10000064 32) & 0x10000000 )) -ne 0 ]; do :; done
devmem 0x10000064 32 0x4400001D
# 60,9 Hz (FUERA de especificación): /24  → 0x64000017 … 0x44000017
# Restaurar: 0x64000020, esperar BUSY, 0x44000020  (o apagar y encender la pantalla [I])
```

Medir con IRQ `jzfb` por segundo y `hibyctl fps`. Dejar el panel 1–2 minutos con una imagen fija y otra en movimiento, y comprobar si hay imagen lavada, bandas, parpadeo o líneas perdidas.

**Riesgos:**
- Fuera de especificación, la imagen puede degradarse o romperse; se recupera restaurando LPCDR o reiniciando.
- Cambiar el DOTCLK en mitad de un fotograma puede dar uno corrupto: sin consecuencias.
- El daño al panel es improbable: es solo frecuencia de barrido, no voltaje [I].

**No tocar MPLL:** cuelgan de él la SD, la DDR y el AHB.

---

## 5. Recomendación concreta

1. **Ya: hacer perfectos los 44,3 Hz.** Es lo que más se nota.
   - **Un solo dueño del vsync durante las animaciones.** En modo 'P' o 'C', retener las presentaciones del reproductor ('H') o fusionarlas, para que cada periodo de 22,57 ms lo use el shell. Si no, las dos presentaciones en el mismo periodo se serializan y se pierde un fotograma.
   - **Ritmo:** pintar el siguiente fotograma justo al volver el pan (hay ~22 ms de presupuesto) y muestrear la curva de animación en el **instante en que se verá**: t_retorno_pan + 22,57 ms, no `now()`. Así no hay judder aunque haya jitter de CPU.
   - Duraciones de iOS a 44,3 Hz: 300 ms ≈ 13,3 fotogramas, suficiente para un ease-out cúbico fluido.
   - Corregir la guía: `GUIA_UI_AGENTES.md` dice "cada `FBIOPAN_DISPLAY` transfiere la imagen". En realidad **espera al vsync** de un panel sin RAM; la regla práctica (escribir y luego presentar) sigue valiendo.
2. **Opcional, con el permiso del usuario:**
   - validar en caliente /30 (48,7 Hz), §4.3;
   - si se ve perfecto, incluir el parche de **50 Hz** (§4.2) en el próximo firmware;
   - ajustar el ritmo del shell a 20,0 ms.
   - Ganancia: +13 % de fluidez, dentro de la especificación del panel.
3. **60 Hz:**
   - **No como valor por defecto.** Supera el tiempo de línea mínimo del ST77903 en un 16–18 %.
   - Como mucho, un experimento visual con /24 o `refresh = 62` y porches recortados.
   - Solo se adoptaría si, tras minutos de prueba, no hay ningún artefacto. Aun así sería "funciona en esta unidad", sin garantía en otras ni con temperatura.

---

## 6. Reproducir

```sh
# WSL
cd ~/hiby/rootfs/module_driver
~/hiby/venv/bin/python ~/hiby/tools/kodis.py soc_fb.ko auto_calculate_pixel_clock jzfb_pan_display lcdc_pan_display jzfb_irq_handler jzfb_blank
~/hiby/venv/bin/python ~/hiby/tools/kodis.py lcd_st77903_8bit.ko st77903_8bit_power_on   # init SPI (bit 8 = D/C)
~/hiby/venv/bin/python ~/hiby/tools/korel.py lcd_st77903_8bit.ko .data                   # lcdc_data (fichero 0x14a0)

# Aparato (solo lectura)
cat /sys/class/graphics/fb0/modes /sys/module/soc_fb/parameters/*
devmem 0x10000064 32; for a in 0x13059000 0x13059004 0x13059008 0x1305900c 0x13059010 0x13051000; do devmem $a 32; done
grep jzfb /proc/interrupts; sleep 5; grep jzfb /proc/interrupts
/usr/data/hibymod/t/hibyctl pantime 40
```

`ingenic_cgu_set_rate` (0x803bf0b4) se leyó con `devmem` de la memoria física 0x3bf0b4 y se desensambló con capstone. Calcula `div = ceil(padre/rate) − 1` y escribe con CE, esperando a BUSY.
