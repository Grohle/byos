# Funciones ocultas del firmware y mejoras de audio (HiBy R3 II 2025)

Fecha: 2026-11-09.

**Marcas usadas en el documento:**
- **[V]** verificado: en el rootfs, por desensamblado o con lecturas de solo lectura en el dispositivo.
- **[I]** inferido: no se ha probado en el hardware.

**Herramientas nuevas en `~/hiby/tools/`:**

| Herramienta | Qué hace |
|---|---|
| `xref.py` | Referencias a cadenas en `hiby_player` |
| `hpdis.py`, `d3.sh` | Desensamblan un rango con anotaciones |
| `callers.py`, `callargs.py` | Llamadas a una función y su `$a0` |
| `memref.py` | Accesos a globales |
| `jt.py` | Tablas de salto |
| `kodis.py`, `korel.py` | Desensamblado y relocaciones de los `.ko` |
| `ctl.py` | Controles ALSA del ES9219 |

Requieren capstone y pyelftools, que ya están instalados en `~/hiby/venv`. La caché de desensamblado está en `~/hiby/hp_dis.pkl`.

---

## 0. Resumen

1. **No hay potencia oculta** [V]. La "ganancia" Alta/Baja no es analógica: es atenuación digital del registro de volumen del ES9219.
   - **Alta** usa la tabla `HDB`: con volumen 100 escribe 0, es decir, 0 dB, el máximo.
   - **Baja** usa la tabla `MDB`, no `LDB`: con volumen 100 escribe 12, unos −6 dB.
   - La tabla `LDB` (−12 dB) no se usa.
   - Con ganancia Alta y volumen 100 la salida ya está a fondo de escala. No existe ningún modo "turbo" ni "super gain" en el binario ni en el kernel.
   - Line Out sigue **el mismo camino analógico** que la salida de auriculares de 3,5 mm, con el volumen fijo al máximo.
2. **El filtro digital está roto o capado: solo funcionan 2 de los 8 filtros del ES9219** [V].
   - La interfaz ofrece 3 opciones.
   - El reproductor escribe `0` para la primera y `6` para las otras dos.
   - Además, el driver del kernel convierte en 0 cualquier valor distinto de 6.
   - Por eso "Slow" y "Minimum phase" suenan igual, y los filtros 1, 2, 3, 4 y 7 son inalcanzables.
   - **Se pueden habilitar parcheando 1 instrucción del módulo** (sección 2.2). Es la mejora de audio real más valiosa.
3. **Límite de volumen UE** [V]. `config.json` (perfil 2025) activa `vol_warn_enable=1`, con aviso en 34 (3,5 mm) y 28 (4,4 mm).
   - Hay un tope duro de 50/40 (`lock_vol`), pero solo si está activa la opción de desarrollador **"Volume lock"**.
   - En el `user.ini` del usuario esa opción está **desactivada** (offset 0x77c = 0). No hay tope activo; solo el diálogo "¿subir por encima del nivel recomendado?" y el recordatorio de exposición prolongada.
4. **El modo desarrollador ya está activado** en el dispositivo (param 0x7b, `user.ini`+0x780 = 1) [V]. Solo da "Volume lock" y la captura de pantalla con el botón Play.
5. Hay **interruptores ocultos activados por ficheros en la SD** [V]:
   - modo fábrica;
   - *monkey test* (¡peligroso!);
   - log de batería;
   - volcado de carátulas.
6. Hay **funciones desactivadas por configuración** [V], que se pueden activar por bind-mount:
   - modo coche;
   - doble toque para despertar;
   - standby;
   - BD de canciones en la SD;
   - caché de imágenes en la SD;
   - manual de instrucciones;
   - "USB current limited" en modo DAC.
7. **Ajustes de sonido "de laboratorio"** expuestos en ALSA y no usados por el reproductor [V]:
   - compensación de THD C2/C3 del ES9219, con valor 0 de fábrica;
   - una interfaz sysfs de escritura de registros en bruto: `/sys/bus/i2c/devices/3-0048/write_reg_val`.

---

## 1. Ganancia, volumen y potencia

### 1.1 Las tablas de ganancia y qué usa cada modo [V]

**Origen de las tablas.** `usr/resource/ot_devices_2025.json` contiene `VOLUMES[HW].Gains` con tres tablas:
- `HDB`: 255, 176 … 1, 0;
- `MDB`: … 13, 12;
- `LDB`: … 25, 24.

Las tres tienen 101 pasos y aplican sobre el registro de volumen del ES9219, en pasos de 0,5 dB.

**Selección en el código.** La función `0x47d060` de `hiby_player` elige la tabla:

```
gain != 0  ->  ot_set_gain("HDB")    (Alta)
gain == 0  ->  ot_set_gain("MDB")    (Baja)
```

`LDB` solo se importa ("Imported %d LDB gains") y nunca se selecciona. La interfaz solo tiene dos iconos (`menu/gain_l.png` y `gain_h.png`) y `/data/menu_cfg` incluye la casilla `gain` en el menú desplegable.

**Volumen hardware y software.**
- Con `SET_HW_SW_VOL_BOTH: 1`, el volumen del usuario se aplica en **software** (tabla `SW.HDB`, en 0,1 dB: −120 dB … 0).
- El registro HW solo marca el techo de la ganancia.
- En la lectura en vivo, `amixer` muestra `Left/Right Playback Volume = 0` [V], es decir, 0 dB: coherente con ganancia Alta.

**Driver.**
- `es9219_vol_left_put` escribe el registro 0x0F y `..._right_put` el 0x10, en ambos chips.
- El *master trim* (registros 0x11–0x14) vale `0x7FFFFFFF` (`es9219_reg_def_*`), que es el máximo.
- **No queda margen digital.**

**Puertos y amplificador.**
- El ES9219 es DAC + amplificador de auriculares integrado. No hay ningún chip amplificador externo con control de ganancia.
- `codec_board_ot_put` solo toca: GPIO `ch_sel` PA15, `mute` PC27, el regulador `aldo3`, la alimentación de los DAC (PA10/PA11) y las salidas GPO 0x1b/0x1c del HBC3000 (sección 1.2).
- No se ha encontrado ningún bit de "ganancia analógica" en el driver.

Lo que no sabemos [I]: el ES9219C podría tener registros de modo o ganancia del amplificador interno no documentados públicamente.
- En las tablas por defecto aparecen registros sin nombre conocido: 0x1A=0x62, 0x1B=0xC4, 0x20=0x1B, 0x27=0xCC, 0x86=0xCC (solo en el chip "negativo"), 0x92=0xFC y 0x93=0x01.
- Sin la hoja de datos del ES9219C, tocarlos es **experimentar a ciegas, con riesgo de ruido a fondo de escala en los auriculares**. No recomendado.

**Conclusión: para más potencia, la única vía es la salida balanceada de 4,4 mm**, que usa los dos ES9219 en puente, o un amplificador externo.

### 1.2 `Output Port Switch` (0–7): significado real [V]

Esto resuelve la pregunta abierta en `investigacion_reproductor_mpd.md`. El desensamblado de `codec_board_ot_put` en `x1600_hiby_r3ii_sound_card.ko` da:

| Valor | Salida | Qué hace |
|---|---|---|
| 0, 5, 6, 7 | ninguna | `hbc3000_disable`, DAC1 y DAC2 apagados, GPO 0x1b y 0x1c a 0, regulador apagado |
| 1 | auriculares 3,5 mm | mute, `spdif_mode(0)`, `hbc3000_enable`, **DAC1 encendido, DAC2 apagado**, GPO 0x1c 0→1, `ch_sel=0`, `aldo3` encendido, unmute |
| 2 | Line Out | **idéntico a 1**. El paso 1↔2 no hace nada en el driver. Solo cambia el volumen fijo (`LO_VOLUME_INDEX: 100`) |
| 3 | balanceado 4,4 mm | **DAC1 y DAC2 encendidos**, GPO 0x1c y 0x1b encendidos, `ch_sel=1`, regulador encendido, unmute |
| 4 | S/PDIF | `spdif_mode(1)`, HBC3000 encendido, los dos DAC apagados |

Los valores 5–7 no hacen nada nuevo: no hay modos ocultos.

En reposo, el reproductor deja el puerto a 0. `amixer` mostraba `Output Port Switch=0` y `headset=1` [V].

### 1.3 Límites de volumen UE y avisos [V]

**Fichero de configuración.** `/usr/resource/config.json` (`z:\config.json`) corresponde al perfil "R3II_2025". Su sección `volume`:

```json
"vol_warn_enable":1,
"lock_vol":[{"headset":50},{"balance":40}],
"default_vol":[{"headset":20},{"balance":15}],
"warn_vol":[{"headset":34},{"balance":28}]
```

El `config_r3ii.json` antiguo (no se usa) tenía `vol_warn_enable:0`, `lock_vol:100` y `warn_vol:102`.

**Parseo.** Lo hace `0x42dfe0`, que lo guarda en la global `0x969594`. El getter es `0x42ddc0(tipo)`:

| Tipo | Campo |
|---|---|
| 0 | `vol_warn_enable` |
| 1 | `lock_vol[puerto]` |
| 2 | `default_vol` |
| 3 | `warn_vol` |

**`warn_vol`.**
- `0x42a0a0` devuelve `warn_vol[puerto]`, o 101 si no hay configuración.
- `0x420b58` lo compara con el volumen y lanza el diálogo `vol_ask`: "Want to turn up the volume above the recommended level?".

**`lock_vol`.**
- `0x42aa20` devuelve `lock_vol[puerto]` **solo si param 0x7a == 1**; si no, devuelve 100.
- `0x42ca60` recorta entonces el "volumen máximo" (param 0x10) y el volumen actual a ese valor.
- Param 0x7a es la opción **Developer options → "Volume lock"** (`0x4e7c40`: lista `volume_locked` / `screen_short`).
- En el `user.ini` del usuario, el offset 0x77c (param 0x7a) vale **0**: **no hay tope**.
- En el mismo fichero, el offset 0x238 (param 0x10, "Max volume") vale 100.

**`volume_long_time_high`.** Es el aviso de exposición prolongada (EN 50332-3), diálogo `volume_long_time_high.dlg`. Lo controla el param 0x77 (`0x535580`).

**Región.** No influye en los límites. `/usr/data/region` vale `lang=en` y el país en `user.ini` es "ES". Los límites vienen solo de `config.json`.

### 1.4 Otras claves de `ot_devices_2025.json` que entiende el binario [V/I]

El parser `0x73d230` lee estas claves:
- `VERSION`, `SET_HW_SW_VOL_BOTH`, `CHANGE_PRIORITY`;
- `OLD_LIBSMART_AUDIO`;
- `DOP_MAX_VOLUME`, `USB_OUT_MAX_VOLUME`, `SPDIF_MAX_VOLUME`, `SPDIF_MAX_192K`;
- `USE_VOLUME_CHIP`, `LO_VOLUME_INDEX`, `A2DP_FIXED_GAIN`;
- `ANALOG_PCM_HW_SAMPLE_MAX`, `SW_VOLUME_FILTER`, `SW_MIX_GAIN_ENABLE`;
- `SampleBit`;
- `AnalogDsdD2p/Dop/Native`.

El fichero del R3 II 2025 **solo fija 4 de ellas**. La semántica exacta del resto no está desensamblada [I]. Las interesantes:

- `LO_VOLUME_INDEX` (hoy 100): nivel fijo de Line Out. Bajarlo (p. ej. a 80) da un line-out atenuado para amplificadores sensibles. Riesgo bajo, porque baja el nivel.
- `AnalogDsdD2p: "dop"`. Aunque el usuario elija "DSD → PCM" en la interfaz, sale por DoP.
  - Cambiarlo a `"d2p"` [I] usaría `libdsd2pcm.so`, que existe en `usr/lib`. Así el DSD pasaría por PEQ/MSEB y por el volumen software.
  - Coste de CPU alto en DSD256 en un solo núcleo. Hay que probarlo.
- `SET_HW_SW_VOL_BOTH` / `USE_VOLUME_CHIP` [I]: podrían pasar el volumen al registro del ES9219, que trabaja en 32 bits dentro del chip, en lugar del software.
  - **Riesgo:** si se configura mal, el HW queda a 0 dB con el volumen software anulado, lo que da volumen máximo.
  - Probar solo con los auriculares desconectados.

### 1.5 Profundidad de bits de la salida [V/I]

- `pcm_tiny_alsa_init` (`0x7385c0`) abre el PCM en 32 bits si la cadena entrega 32 (`PCM_FORMAT_PCM 32bit`) y en 16 bits en caso contrario [V].
- Si un FLAC de 16 bits sale a 16 bits **con volumen software**, a volumen 30 (≈ −20 dB) se pierden unos 3,3 bits de resolución efectiva [I].
- No se puede comprobar en vivo: el kernel no tiene `/proc/asound` [V].
- **Pendiente:** registrar `SNDRV_PCM_IOCTL_HW_PARAMS` con el shim LD_PRELOAD que ya existe. Si sale a 16 bits, forzar 32 bits (o el volumen HW) sería una mejora audible a volúmenes bajos.

---

## 2. Filtros digitales y compensación de THD

### 2.1 El fallo [V]

**Reproductor.** `system_if_set_digital_filter(tipo)` está en `0x480a80`. Las cadenas internas son "fast rolloff(default)", "slow rolloff" y "minimum phase".

```
tipo 0 -> escribe 0 en ALSA 'Digital Filter'
tipo 1 o 2 -> escribe 6
```

**Kernel.** En `codec_es9219.ko`, la función `es9219_digital_filter_put` (`.text+0x1674`) hace:

```
xori  v0, a0, 6
movn  a0, zero, v0        ; si valor != 6 -> 0
sll   a0, a0, 5           ; -> es9219_set_digital_filter(): reg 0x07, máscara 0xE0
```

**Valores efectivos de `filter_shape` (bits 7:5 del registro 7):**
- `0`: rápido, fase lineal;
- `6`: según la nomenclatura de la familia Sabre (ES9038Q2M y similares), "hybrid fast roll-off, minimum phase" [I].

La interfaz (`settings_digital_filter.dlg`: sharp / slow / mp_sharp) promete 3 filtros, pero "slow" y "minimum phase" son **el mismo filtro**.

### 2.2 Cómo habilitar los 8 filtros (recomendado)

Los valores de `filter_shape` del registro 7 en la familia Sabre son [I, pendiente de confirmar en el ES9219]:

| Valor | Filtro |
|---|---|
| 0 | rápido, fase lineal (por defecto) |
| 1 | lento, fase lineal |
| 2 | rápido, fase mínima |
| 3 | lento, fase mínima |
| 4 | apodizante, rápido, fase lineal |
| 5 | reservado |
| 6 | híbrido, rápido, fase mínima |
| 7 | brickwall |

**Parche del módulo** (`module_driver/codec_es9219.ko`, md5 original `0d698e6f04fe301a063aec53b726f7cd`):

| Offset en el fichero | Original | Nuevo | Efecto |
|---|---|---|---|
| `0x16e4` | `0b 20 02 00` (`movn a0,zero,v0`) | `00 00 00 00` (`nop`) | El valor ALSA *v* va directo: filtro *v* (`v<<5`) |
| `0x260c` y `0x2610` | `06 00 00 00` ×2 (max y platform_max) | `07 00 00 00` ×2 | Permite el valor 7 (brickwall) |

**Comportamiento con el parche:**
- `user_filter_setting` persiste en el kernel, y `es9219_dai_prepare` lo vuelve a aplicar en cada apertura del PCM. Basta con escribirlo una vez después de que el reproductor arranque o cambie el filtro: `amixer -c0 cset name='Digital Filter' N`.
- El reproductor seguirá escribiendo 0 o 6 cuando el usuario toque el ajuste original. Opciones: ocultar ese ajuste en el tema, o que `hibyshell` reaplique el valor elegido (lo guarda en `/usr/data/hibymod/audio.conf`).

**Riesgo bajo.**
- Ningún valor del registro 7 puede dañar nada: solo cambia la forma del filtro.
- Si el `.ko` estuviera mal, la tarjeta de sonido no cargaría (sin audio), pero el sistema arranca. Se recupera con `hibymod reinstall` o con el recovery.

**Alternativa sin tocar el kernel:** `echo "7 20" > /sys/bus/i2c/devices/3-0048/write_reg_val` (registro 7 = 0x20 → filtro 1). Problema: `es9219_dai_prepare` lo pisa en cada pista, porque llama a `es9219_set_digital_filter(user_filter_setting)`, que reescribe los bits 7:5. Habría que repetirlo después de cada `SNDRV_PCM_IOCTL_PREPARE`, con un hook en el shim LD_PRELOAD. Es peor solución.

### 2.3 Compensación de THD C2/C3 [V]

- Los controles ALSA `ES9219 THD C2_CH1/CH2` y `C3_CH1/CH2` van de 0 a 65535 y hoy valen 0 [V].
- Escriben los registros 0x16/0x17 (C2) y 0x18/0x19 (C3), más el 0x30=0, en los dos chips.
- **El reproductor nunca los usa** (no hay ninguna referencia en `hiby_player`).
- Sirven para calibrar la cancelación de armónicos 2.º y 3.º de cada placa. Se pueden usar también como "colorante" (inyectar H2, efecto "a válvulas", parecido al "Harmonic Controller" de otros modelos de HiBy).
- Sin analizador de audio, el ajuste es a oído.
- **Riesgo:** no daña nada. Valores extremos producen distorsión audible.
- **Recomendación:** experimental, con preajustes suaves (p. ej. C2 = ±200…800) y siempre con un botón "restablecer a 0".

### 2.4 Registro sysfs en bruto [V]

En `/sys/bus/i2c/devices/3-0048/` hay tres atributos, todos de **solo escritura** (modo 0200):
- `write_reg_val`: `"%x %hhx"`. Escribe el registro en los DAC encendidos **y lo guarda en la tabla de valores por defecto**, que se reaplica en cada `prepare`.
- `reg_val`: `"w reg val"` o `"r reg n"`. La lectura no imprime nada: inútil.
- `write_lr_flag`.

También existen `/dev/i2c-3` y `CONFIG_I2C_CHARDEV` [V]. Con `I2C_SLAVE_FORCE` se podrían **leer** los registros reales de los chips 0x48 y 0x49 desde una herramienta propia. Útil para confirmar el estado del filtro y la THD.

⚠️ No escribir otros registros sin la hoja de datos. Los registros 0x01 (entrada), 0x0C (DPLL) y 0x0F/0x10 (volumen; 0 = 0 dB) pueden producir **ruido o señal a fondo de escala**. Probar siempre con los auriculares desconectados.

---

## 3. Funciones ocultas o desactivadas

### 3.1 `set_functions.json` (Ajustes del sistema) [V]

Formato: `{"clave":0/1}`. Valores de serie con 0:

| Clave | Qué es | Valoración |
|---|---|---|
| `usb_mode` | Selección del modo USB (ya activada en la fase 1 del mod) | — |
| `car_mode` / `car_mode_auto_play` | Modo coche: reproduce al conectar la alimentación USB y se apaga al quitarla (`car_mode_shutdown`) | Útil en el coche; riesgo bajo |
| `standby` | Standby profundo (existe `test_standby.dlg`) | Probar el consumo; riesgo bajo |
| `double_touch_wakeup` | Doble toque para despertar | Necesita soporte del gt9xx [I]; puede gastar batería |
| `dac_charge_disable` | "USB current limited": no carga del móvil en modo DAC | Útil con móviles; riesgo nulo |
| `operating_instruction` | Manual con código QR | Irrelevante |

El binario conoce más claves que no aparecen en el JSON:
- `update_type`, `brightness`, `bkl_time`;
- `stream_media`: **sirve para ocultar Tidal/Qobuz**, cosa que pidió el usuario;
- `step_record`: podómetro. No hay driver de g-sensor en `module_driver`, así que no aplica;
- `wireless_set`, `bluetooth_set`.

### 3.2 `config.json` → `function.fcn0` [V]

| Clave | Valor de serie | Qué hace |
|---|---|---|
| `tf_music_db_enable` | 0 | Guarda la BD de canciones en la SD (`/data/mnt/sd_0/.temp/usrlocal_media.db`). La propia interfaz lo recomienda "cuando hay muchas canciones". Reduce el desgaste de UBIFS. **Interesante para bibliotecas grandes**, pero hay que probar si afecta al rendimiento y a la RAM |
| `tf_image_cache_enable` | 0 | Caché de carátulas en la SD (`.temp/image_cache.db`) |
| `lyric_color_enable` | 0 | Color de letras (ya activado en el mod) |
| `playmenu_eq_disable` | 0 | — |
| `dark_theme_enable`, `screen_short_enable`, `dac_to_store`, `book_set_percent`, `most_played` | 1 | — |

### 3.3 Modo desarrollador [V]

- Se activa en Acerca de con varios toques ("Perform 2 more steps…", `0x52b820`) y queda en param 0x7b.
- **En este dispositivo ya está activo** (`user.ini`+0x780 = 1).
- Opciones: **Volume lock** (tope UE 50/40, hoy desactivado) y **captura con un toque corto de Play** (param 0x80).
- La cadena `gain_enable` ("Enable gain adjustment") existe en `developer_options.ini`, pero no está en la lista de opciones de esta versión.

### 3.4 Interruptores por fichero en la SD (`/data/mnt/sd_0/…`) [V]

| Fichero | Efecto | Riesgo |
|---|---|---|
| `hiby_linux_factory_mode` | Arranca la actividad de fábrica `hiby_test` (`test/hiby_test.view`), con pruebas de LCD, teclas, LED, audio, BT, carga, OTG, FM, RF, táctil, WiFi, TF, versión, QR y standby | Bajo-medio. Puede tocar ajustes. Quitar el fichero para salir |
| `hiby_linux_auto_test` | **Monkey test**: inyecta toques aleatorios (`MONKEY`, `autotest_capture`) | ⚠️ **Alto**: puede borrar canciones o cambiar ajustes. No usar |
| `battery_log.txt` (si existe) | Activa el param 0x16: añade una línea "fecha porcentaje" periódicamente | Nulo. Útil para medir autonomía |
| `get_cover_switch` | Crea `list_cover/` y `plane_cover/` en la SD y activa un volcado de carátulas [I: probablemente las miniaturas ya decodificadas] | Bajo. Podría servir para que `hibyshell` reutilice carátulas |

Otras pistas sin investigar a fondo: `a:\log.txt`, `debug.txt` (volcado del heap de la GUI) y `/etc/bluetooth/pts_enable` (pruebas BT PTS).

### 3.5 Funciones de otros modelos que no aplican aquí [V]

El mismo binario sirve también para TempoTec V3 (`V3_ANALOG_2025`), Hidizs AP80 PRO MAX y R3PROII. Estas funciones no aplican al R3 II 2025:
- filtros FPGA "Darwin" y `osr_bypass` (`/sys/devices/platform/hm100`, que no existe);
- "Harmonic Controller" (`/data/harmonic.json`, `getEarphoneSet`);
- NOS, DRE (`NOS_EN`/`DRE_EN`: no hay esos controles ALSA);
- campo sonoro.

Forzarlas no tiene hardware detrás.

### 3.6 Carga de la batería (mejora general) [V/I]

- `module_driver/axp2101.sh` carga el driver con `charge_voltage_limit=4400` y `charge_term_current=100`.
- Sin embargo, la batería informa de `POWER_SUPPLY_VOLTAGE_MAX_DESIGN=4350000`, y al terminar la carga ("Full") medía 4,313 V [V].
- Puede que el 4400 sea solo un valor nominal del driver [I].
- Bajar a 4350 (o a 4200 en un modo "cuidado de batería") alargaría la vida de la batería a costa de unos 5–10 % de capacidad. Riesgo bajo, porque bajar es seguro.
- Antes de cambiarlo, conviene leer `/sys/module/axp2101/parameters/charge_voltage_limit`. No se pudo: adb se desconectó porque el dispositivo entró en reposo.

---

## 4. Recomendaciones ordenadas

| # | Acción | Beneficio | Riesgo auditivo/daño | Riesgo de brick | Implementación |
|---|---|---|---|---|---|
| 1 | **Parche del filtro digital** (0x16e4 + max=7) y selector de 8 filtros en `hibyshell` | Real y audible, la única mejora de "calidad" disponible | Nulo | Bajo (sin audio en el peor caso; recovery) | Parchear el `.ko` al construir el rootfs + `amixer cset` desde el shell |
| 2 | Quitar avisos UE: override de `config.json` con `vol_warn_enable:0` (y `warn_vol` 102 / `lock_vol` 100 para que "Volume lock" no haga nada) | Comodidad: sin el diálogo `vol_ask` ni avisos repetidos | **Hay que dejar una nota al usuario**: quita una protección auditiva, no añade potencia | Nulo (JSON; si falla el parseo, se restaura el original) | Bind-mount desde `/usr/data/hibymod/override/usr/resource/config.json` |
| 3 | Verificar los bits de salida (shim LD_PRELOAD que registre `HW_PARAMS`); si son 16 bits con volumen software, forzar S32 | Mejor resolución a volumen bajo | Bajo (probar sin auriculares) | Nulo | Shim existente (`hibymod_disp`) + hook de `ioctl` |
| 4 | `tf_music_db_enable:1` (y quizá `tf_image_cache_enable:1`) | Menos escritura y uso de la NAND con bibliotecas grandes | Nulo | Bajo (requiere reescanear) | Override de `config.json` o activar el ajuste en la interfaz si aparece |
| 5 | `car_mode`, `dac_charge_disable`, `standby` en `set_functions.json`; `stream_media:0` para ocultar el streaming | Funciones de serie ocultas | Nulo | Nulo | Override de `set_functions.json` |
| 6 | THD C2/C3 como "color" experimental con reset | Personalización del sonido | Bajo (distorsión audible) | Nulo | `amixer cset` desde `hibyshell` |
| 7 | `LO_VOLUME_INDEX` ajustable (line-out atenuado) | Mejor acople a amplificadores externos | Baja el nivel: seguro | Nulo | Override de `ot_devices_2025.json` |
| 8 | `AnalogDsdD2p:"d2p"` | PEQ y MSEB con DSD | Bajo | Nulo | Override JSON; medir CPU con DSD128/256 |
| 9 | Límite de carga a 4,35 V | Vida de la batería | Nulo | Bajo | Cambiar `axp2101.sh` en el rootfs |
| ✗ | "Más potencia" o ganancia extra por software | **No existe**: Alta = 0 dB, fondo de escala | — | — | Usar la salida de 4,4 mm o un amplificador externo |
| ✗ | Registros desconocidos del ES9219 (0x1A, 0x1B, 0x86…) | Desconocido | **Alto** (posible ruido a fondo de escala) | Bajo | No, sin hoja de datos |
| ✗ | `hiby_linux_auto_test` | — | — | Daño de datos | No usar |

---

## 5. Cómo habilitarlo: lo más importante

### 5.1 Filtros (parche del kernel)

Al construir el rootfs (`tools/` del mod, antes de `mksquashfs`):

```python
p='rootfs/module_driver/codec_es9219.ko'; b=bytearray(open(p,'rb').read())
import hashlib; assert hashlib.md5(b).hexdigest()=='0d698e6f04fe301a063aec53b726f7cd'
assert b[0x16e4:0x16e8]==bytes.fromhex('0b200200'); b[0x16e4:0x16e8]=b'\0\0\0\0'
for o in (0x260c,0x2610):
    assert b[o:o+4]==b'\x06\0\0\0'; b[o:o+4]=b'\x07\0\0\0'
open(p,'wb').write(b)
```

En ejecución, en el arranque del shell y al elegir un filtro: `amixer -c 0 cset name='Digital Filter' <0..7>`. Guardar la elección en `/usr/data/hibymod/audio.conf`.

Hay que reaplicarla después de que el reproductor arranque o cuando el usuario toque el ajuste original. Una vía: vigilar con inotify `/usr/data/user.ini`, o hacer polling barato de `amixer cget`.

Para comprobar que funciona, leer el registro 7 del chip 0x48 por `/dev/i2c-3` con una herramienta propia de solo lectura, o hacer una prueba de escucha con un barrido.

### 5.2 Quitar los avisos de volumen UE

1. Copiar `usr/resource/config.json` a `/usr/data/hibymod/override/usr/resource/config.json`.
2. Cambiar `"vol_warn_enable":1` a `0`. Opcionalmente, poner `lock_vol` a 100 y `warn_vol` a 102 en los dos puertos, igual que en el R3 II original.
3. Reiniciar: la configuración solo se lee al arrancar.
4. Comprobar que, al subir de 34, ya no aparece `vol_ask`.

**Aviso para el usuario:** esto no aumenta la potencia. Quita los avisos de seguridad auditiva.

### 5.3 Funciones de `set_functions.json`

1. Override de `usr/resource/set_functions.json` con, por ejemplo, `"car_mode":1`, `"car_mode_auto_play":1` y `"dac_charge_disable":1`.
2. Para ocultar Tidal/Qobuz, añadir `{"stream_media":0}` [I: la clave existe en el binario; hay que comprobar el efecto en la interfaz].

### 5.4 Pruebas pendientes en el dispositivo (con el usuario, sin auriculares puestos)

1. `amixer cget name='Output Port Switch'` reproduciendo por 3,5 mm (debe dar 1), por 4,4 mm (3) y con Line Out (2).
2. Registrar `HW_PARAMS` con el shim para saber si la salida va en S16 o S32 con FLAC de 16 bits.
3. Leer `/sys/module/axp2101/parameters/charge_voltage_limit`.
4. Leer los registros 0x00–0x40 y 0x80–0x95 de los ES9219 0x48/0x49 por `/dev/i2c-3` (solo lectura) para documentar el estado real.
