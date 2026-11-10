# Relojes, overclock, ahorro de energía y control de carga (HiBy R3 II 2025)

Fecha: 2026-11-10.

Marcas: **[V]** verificado (lectura de solo lectura en el dispositivo, desensamblado del `.ko`/`hiby_player` o rootfs); **[I]** inferido, sin probar en el hardware.

En el dispositivo solo se ejecutaron lecturas: `cat` de sysfs/proc, `devmem <addr> 32` **sin valor** (lectura) de registros CPM y de la tabla de PLL del kernel, `ps`/`top`/`wpa_cli status`/`rfkill list`. No se montó debugfs, no se escribió nada.

---

## 0. Resumen

1. **La CPU ya va a ~1104 MHz**, un 10 % por encima de los 1,0 GHz de la hoja de datos del X1600 [V]. No hay cpufreq, ni governor, ni cpuidle, ni `jz_clk`; el reloj de CPU está registrado en el kernel como **solo lectura** (`ingenic_cpccr_rate_ro_ops`) [V].
2. **Overclock: no recomendado.** Solo es posible escribiendo los registros CPM por `/dev/mem` (existe, sin `STRICT_DEVMEM`, y está el applet `devmem`). Ganancia nula para nuestra UI, que está limitada por la transferencia SLCD a 44 fps y por la memoria (DDR/AHB cuelgan del MPLL, que no se puede tocar sin romper SD, LCD y, quizá, audio). Riesgo: inestabilidad alta, brick bajo.
3. **Voltaje del núcleo:** `dcdc2` = 1,10 V fijo (min = max en las *constraints*); no hay interfaz de usuario para cambiarlo. Hacerlo por I²C crudo al AXP2101 es el **único riesgo real de brick blando** (el PMIC conserva el valor mientras haya batería). **No hacerlo.**
4. **Ahorro real:** brillo, no redibujar cuando no cambia nada (y nunca con la pantalla apagada), 30 fps en Ahorro, WiFi y BT apagados cuando no se usan. El *underclock* (dividir la CPU a 552 MHz) es posible y relativamente seguro, pero ahorra poco porque el idle ya usa `wait` + NO_HZ; queda como experimental.
5. **No hay sensor de corriente.** `current_now` de `ac`, `usb` y `axp_battery` son valores configurados o constantes, no medidos [V]. El consumo solo se puede medir por la pendiente de `battery/voltage_now` y `battery/capacity` (CW2015).
6. **"Solo alimentación USB, no cargar la batería": `echo 0 > /sys/class/power_supply/axp_battery/charger`** (pone la corriente de carga ICC del AXP2101 a 0 mA). `echo 1` la restaura [V por desensamblado]. Hay que **reafirmarlo tras cada enchufado**: el hilo del driver vuelve a aplicar la corriente de carga 5 s después de cada cambio de VBUS [V].
7. La opción de HiBy `dac_charge_disable` ("USB current limited") **no es eso**: en modo DAC limita la entrada USB a 100 mA y desactiva la carga, así que el aparato **tira de su batería** para no gastar la del móvil [V].

---

## 1. Árbol de relojes

### 1.1 Lo que hay (y lo que no) [V]

| Elemento | Estado |
|---|---|
| `/sys/devices/system/cpu/cpu0/cpufreq`, `cpuidle` | No existen |
| `/proc/jz/` | Solo `dmmu/` y `efuse/` (sin relojes) |
| debugfs | Soportado (`nodev debugfs`) y con `clk_summary` en el kernel (`clk_summary_show` en kallsyms), pero **no montado**. Montarlo (`mount -t debugfs none /sys/kernel/debug`) daría el árbol de relojes exacto; no se hizo por las reglas de esta tarea |
| Reloj de CPU en el framework clk | `ingenic_cpccr_rate_ro_ops`, `ingenic_cpccr_mux_ro_ops`: **solo lectura**, el kernel nunca cambia la frecuencia de la CPU |
| Tabla de PLL del kernel (`x1600_pll_rate_table`, 0x80798dc4) | 1000, 900, 800, 700, 600, 500, 400 MHz. Fórmula `24 MHz × M / N / OD1 / OD0` (ej.: 1000 = 125/3/1/1; 900 = 75/1/2/1) |
| `/dev/mem` | `crw-r----- 1,1`; no hay `devmem_is_allowed` en kallsyms ⇒ **sin STRICT_DEVMEM**: acceso total a la memoria física |
| `devmem` | Applet de busybox en `/sbin/devmem` |
| Idle | `wait instruction: yes`, `nohz_mode: 2` (tickless), `CPM.LCR = 0x1F00` (modo de bajo consumo = IDLE) |
| `/sys/power/state` | `freeze standby mem` (el reproductor usa `echo mem` para su standby) |

### 1.2 Registros CPM leídos (base 0x10000000) [V lectura; decodificación I salvo nota]

| Reg. | Valor | Decodificación |
|---|---|---|
| CPAPCR (0x10) | `0x02E049CD` | M=46, N=1, OD1=1, OD0=1 ⇒ **APLL = 24×46 = 1104 MHz** |
| CPMPCR (0x14) | `0x0AF0C9CD` | M=175, N=3, OD=1/1 ⇒ **MPLL = 1400 MHz** |
| CPCCR (0x00) | `0x9A094410` | CPU ← APLL, AHB0/AHB2 ← MPLL; CDIV=0 (/1), L2DIV=1 (/2), H0DIV=4 (/5), H2DIV=4 (/5), PDIV=9 (/10) |
| DDRCDR (0x2C) | `0xA0000003` | DDR ← MPLL /4 |
| MSC0CDR (0x68) | `0x4000000D` | MPLL/(2×14) = **50,0 MHz exactos** (SD "high speed", que confirma MPLL = 1400 y la fórmula) |
| LPCDR (0x64) | `0x44000020` | Píxel LCD ← MPLL/33 ≈ 42 MHz |
| I2S CDR (0x60, 0x70, 0x7C, 0x80) | `0x68000C35`, `0x61A`… | Divisor fraccionario M/N; **fuente no confirmada** (APLL, MPLL o EXCLK) |
| CPCSR (0xD4) | `0xF8000000` | PLLs estables, sin cambios de divisor en curso |

**Frecuencias actuales:**

| Reloj | Frecuencia | Nota |
|---|---|---|
| CPU | **1104 MHz** | BogoMIPS 1099,36 lo corrobora [V] |
| L2 | 552 MHz | |
| AHB0 / AHB2 | 280 MHz | |
| APB | 140 MHz | |
| DDR | 350 MHz | DDR2-700, 64 MB (`mem=64M`) |

**Frecuencia nominal.** La hoja de datos del X1600/X1600E (Lumissil/Ingenic) da **1,0 GHz** para el núcleo XBurst1 ([datasheet en Mouser](https://www.mouser.lt/datasheet/2/198/X1600_X1600E_DS-3196268.pdf); DigiKey dice lo mismo, "1 Core 1GHz", [ficha](https://www.digikey.com/en/products/detail/lumissil-microsystems/X1600/17753858)). El cargador de arranque de HiBy [I] ya pone el APLL a 1104 MHz, por encima de la tabla del propio kernel (máx. 1000).

### 1.3 Reguladores del AXP2101 (`/sys/module/axp2101/parameters/*`, `/sys/class/regulator/*`) [V]

| Raíl | Voltaje | Uso probable [I] |
|---|---|---|
| dcdc1 | 3,3 V | E/S |
| **dcdc2** | **1,10 V** (`min_microvolts = max_microvolts = 1100000`) | **VDD del núcleo** |
| dcdc3 | 1,8 V | DDR2 / E/S |
| dcdc4 | 1,2 V | |
| aldo2, aldo4, bldo1, bldo2 | 3,3 V, encendidos | |
| aldo3 | 3,3 V | Audio (el reproductor lo apaga y enciende) |

- `/sys/class/regulator/*` es de solo lectura.
- Los `reg-virt-consumer.*` no tienen driver enlazado: no hay `microvolts` escribible.
- La única vía para cambiar dcdc2 sería I²C crudo (bus 0, dirección 0x34, `I2C_SLAVE_FORCE`, registro DCDC2 del AXP2101) por debajo del driver y de su regmap.

---

## 2. ¿Overclock? Análisis y recomendación

### 2.1 Qué haría falta [I]

Todo por `/dev/mem`, sin que el kernel se entere:

1. Pasar la CPU a otra fuente o a un divisor alto (CPCCR con `CE_CPU`, bit 22, y esperar a que se limpie CPCSR.bit0).
2. Reprogramar CPAPCR con un M mayor (p. ej. M=50 ⇒ 1200 MHz) y esperar al bit de PLL estable.
3. Volver a seleccionar el APLL.

**El MPLL (DDR, AHB, APB, SD, LCD) no se puede tocar:** cambiarlo desajusta el reloj de la SD (50 MHz), el píxel del LCD, el UART y probablemente el I²S. Por tanto, el overclock solo afecta a la CPU y a la L2, no a la memoria.

### 2.2 Riesgos

| Riesgo | Nivel | Motivo |
|---|---|---|
| Brick | Bajo | El APLL se reprograma en cada arranque, así que un reinicio lo restaura. Pero si el sistema se cuelga *durante* una escritura en la NAND (UBIFS de `/usr/data`), se puede corromper `user.ini` o la BD |
| Inestabilidad | **Alto** | Ya va a +10 % sobre el nominal con 1,10 V fijos. No hay margen conocido, y sin subir el voltaje lo esperable son cuelgues aleatorios |
| Calor | Bajo-medio | Encapsulado BGA 9×9 sin disipador, dentro de una carcasa pequeña |
| Cortes de audio | Medio | La fuente del reloj I²S no está confirmada. Si cuelga del APLL, cambia el tono o hay chasquidos al conmutar |
| Retardos del kernel | Medio | `loops_per_jiffy` está calibrado a 1104 MHz: con más frecuencia `udelay()` se queda corto (I²C por GPIO, SDIO, panel) |

### 2.3 Ganancia

- El panel SLCD tope a 44 fps (22,7 ms por fotograma).
- El shell pinta en 5–10 ms y el resto lo dominan las copias en memoria y la transferencia `FBIOPAN_DISPLAY`.
- Todo eso cuelga del MPLL, la DDR y el AHB, que no cambian.
- Un +9 % de CPU (1200 MHz) ahorraría menos de 1 ms por fotograma y **no sube los fps**.
- Tampoco lo necesita la decodificación de audio, salvo quizá DSD256 con DSP, que está por medir.

### 2.4 Recomendación

**No hacer overclock.** Riesgo **medio** (sobre todo inestabilidad) para una ganancia **nula** en la UI.

- **Voltaje de dcdc2: NUNCA.** El AXP2101 conserva el registro mientras la batería esté conectada, no solo tras un reinicio en caliente. Si el arranque no lo reprograma, un voltaje inestable provocaría un bucle de cuelgues hasta agotar la batería (brick blando). Riesgo **alto**.

---

## 3. Ahorro de energía

### 3.1 Medir el consumo desde el espacio de usuario [V]

| Fuente | Qué es realmente |
|---|---|
| `/sys/class/power_supply/battery/` (CW2015, i2c-0 0x62) | **La batería de verdad**: `voltage_now` en µV (4362000 llena), `capacity` en % (resolución 1 %), `status`. `time_to_empty_now = 8191` mientras carga |
| `/sys/class/power_supply/axp_battery/` | Medidor del AXP casi desactivado: `voltage_now = 0`, `capacity = 0`, `current_now` = constante 200000 (en `axp210x_bat_get_property`). `status` sí parece real ("Full") [I] |
| `/sys/class/power_supply/usb/current_now` | **No es una medida**: devuelve la corriente de carga configurada (campo +0x28 del driver; hoy 500) |
| `usb/voltage_now` (4400) y `ac/*` | Valores configurados, no medidos |
| `usb/online` | VBUS presente (registro del PMIC) [V] |

**Método.** Con el aparato desenchufado:
1. Registrar cada 60 s `date`, `battery/capacity` y `battery/voltage_now` en `/usr/data/hibymod/bat.log`, nunca en `/tmp`. Basta con un `sleep 60` en un bucle del shell, que es barato.
2. Comparar los modos con la misma pista, el mismo volumen y la misma salida, durante 30–60 min cada uno.
3. Resultado: %/h y mV/h. A partir de ~3,7 V la curva es casi lineal.

El firmware también tiene su propio registro: crear `/data/mnt/sd_0/battery_log.txt` (ver `investigacion_firmware_oculto.md` §3.4).

### 3.2 Palancas, por impacto estimado [I salvo nota]

| Palanca | Ruta y valores | Ahorro | Riesgo |
|---|---|---|---|
| **Brillo** | `/sys/class/backlight/backlight_pwm0/brightness`: 0–101 (`max_brightness = 101`, hoy 100) [V]. `bl_power`: 0 encendido, 4 apagado. El reproductor también lo escribe (`backlight_set`), así que hay que reaplicarlo tras despertar la pantalla | **El mayor** con la pantalla encendida | Nulo |
| **No pintar con la pantalla apagada** | Si `bl_power != 0` o `brightness == 0`, el shell no hace `FBIOPAN` ni compone. Cada `FBIOPAN` es una transferencia SLCD completa | Grande | Nulo |
| **Fotogramas** | Pintar solo si hay algo que cambiar (sucio → presentar). 44 fps solo durante animaciones; 30 fps en Ahorro; `present_rows` para el reloj y el volumen; barra de estado cada 60 s | Medio | Nulo |
| **WiFi apagada** | Los scripts de HiBy: `/usr/bin/wifi_off.sh` (mata `wpa_supplicant`/`udhcpc`, `ifconfig wlan0 down`), `/usr/bin/wifi_down.sh` (además `rfkill block wifi`) y `/usr/bin/wifi_on.sh` / `wifi_up.sh` [V]. Mejor usar el ajuste del reproductor para que su interfaz no se desincronice | Medio-alto | Nulo |
| **Ahorro de WiFi** | El driver `cywdhd` implementa `wl_cfg80211_set_power_mgmt` [V], pero **no hay `iw`**, solo `wpa_cli`. Haría falta un `iw` estático o una herramienta nl80211 mínima (`NL80211_CMD_SET_POWER_SAVE`). DHD suele activar PM2 al asociarse [I], así que quizá ya está activo; se puede consultar con `NL80211_CMD_GET_POWER_SAVE` | Bajo-medio | Bajo (latencia en el streaming) |
| **Bluetooth apagado** | `/usr/bin/bt_disable` (`bt-adapter --set Powered Off`) [V], o `rfkill block bluetooth`. Hoy corren `bluetoothd`, `bluealsa`, `bt-agent` y `brcm_patchram_plus`. Mejor usar el ajuste del reproductor | Medio | Nulo |
| **Write-back de la NAND** | `/proc/sys/vm/dirty_expire_centisecs` = 3000 y `dirty_writeback_centisecs` = 300 [V]. El reproductor ajusta `dirty_*` | Bajo | Bajo (más pérdida si se apaga en seco) |
| **Underclock** | §3.3 | Bajo | Medio (experimental) |
| **Servicios** | `adbd` (inactivo, casi no gasta), `sys_server`. No hay daemons inútiles claros | Muy bajo | Medio (no matar procesos de HiBy) |
| **Reguladores** (`dcdc3_always_pwmmode = 0`) | Ya en PFM/PWM automático [V] | — | — |

### 3.3 Underclock experimental (solo modo Ahorro, apagado por defecto) [I, sin probar]

- **Qué cambia.** Solo los divisores de CPU y L2 en CPCCR; el PLL no se toca.
  - CPU = APLL/2 = 552 MHz.
  - L2 = APLL/4 = 276 MHz.
- **Por qué es más seguro que el overclock.**
  - `udelay()` sale *más largo*, lo cual es seguro.
  - El MPLL (DDR, SD, LCD) no cambia.
  - Los temporizadores (OST) van con EXCLK de 24 MHz.
- **Procedimiento.** Semántica de CPCCR como en X1000/X1830; hay que comprobarla:

  ```sh
  v=$(devmem 0x10000000 32)                    # 0x9A094410
  # bajar: CDIV=1, L2DIV=3, CE_CPU (bit22)=1; resto de bits intacto
  devmem 0x10000000 32 $(( (v & ~0xFF) | 0x31 | 0x400000 ))
  while [ $(( $(devmem 0x100000D4 32) & 1 )) -ne 0 ]; do :; done  # CDIV_BUSY
  devmem 0x10000000 32 $(( ($(devmem 0x10000000 32)) & ~0x400000 ))
  # restaurar: (v & ~0xFF) | 0x10, con la misma secuencia CE/BUSY
  ```

- **Riesgos.**
  - Carrera con el kernel: es mínima, porque el kernel nunca escribe CPCCR en marcha.
  - Si los bits CE/BUSY del X1600 no son como en el X1000, la CPU puede colgarse. Un reinicio lo arregla.
  - DSD o el ecualizador podrían quedarse sin CPU y cortar el audio.
- **Ahorro esperado.** Pequeño: sin bajar el voltaje, la potencia dinámica solo baja durante el tiempo activo, y la CPU está en `wait` más del 80 % del tiempo en reproducción.
- **Primero hay que medirlo con §3.1.** Si no se ve ≥3–5 % de autonomía, descartarlo.

---

## 4. Carga: "usar solo USB, no cargar la batería"

### 4.1 Driver `axp2101.ko` (desensamblado con `kodis.py`) [V]

**`/sys/class/power_supply/axp_battery/charger` (rw):**
- `charger_store`:
  - Escribir `0` ⇒ flag +0x280 = 1 y `axp2101_icchg_set(0)`, que pone el **registro 0x62 (ICC, corriente de carga) a 0 mA**.
  - Escribir distinto de 0 ⇒ flag = 0 e ICC = valor guardado en +0x28 (la última `constant_charge_current_max`).
- `charger_show`: imprime `!flag`, es decir, **1 = carga permitida, 0 = deshabilitada**. Hoy vale `1`.

**`/sys/class/power_supply/usb/constant_charge_current_max` (rw), prop 0x1e:**
- Guarda el valor en +0x28.
- Si es ≤ 500: aplica ICC ya y pone el contador +0x289 = 5.
- Si es > 500: aplica la ICC con retraso, desde el hilo del driver.

**`/sys/class/power_supply/usb/charge_control_limit_max` (rw), prop 0x22:**
- Pone el límite de corriente de entrada VBUS (0 ⇒ 2000 mA).
- Pone el registro 0x18 bit 1 (habilitación de carga de la batería): **se borra solo si el valor es exactamente 100**.

**Hilo `thread_dosomthing`:** se ejecuta cada ~1 s.
- Cada cambio de VBUS pone el contador a 0.
- Con VBUS presente, cuando el contador llega a 5 (unos 5 s), **vuelve a aplicar `axp210x_set_charge_current(+0x28)` ignorando el flag de `charger`**.
- Consecuencia: `charger = 0` **se pierde en cada reenchufado**, y `cat charger` seguirá diciendo 0 aunque esté cargando otra vez.

### 4.2 Lo que hace `hiby_player` [V]

**Función `0x481de0(MaxPower, limit_max, current_max)`.** Escribe, solo si el valor cambia:
- `/sys/kernel/config/usb_gadget/android0/configs/c.1/MaxPower`;
- `usb/charge_control_limit_max`;
- `usb/constant_charge_current_max`.

Se llama en los eventos USB:

| Situación | Valores (MaxPower, límite, ICC) |
|---|---|
| Enumerado por un host (`/sys/class/udc/13500000.otg_new/state` = `configured`) y modo USB ≠ DAC | (500, 500, 500) |
| Host, modo **DAC** (param 8 = 0) **y** `dac_charge_disable` activo (param 0x5f = 1) | **(10, 100, 0)**: entrada ≤ 100 mA, carga desactivada, ICC 0. El R3 vive de su batería |
| Host, modo DAC y `dac_charge_disable` = 0 | (500, 500, 500) |
| Cargador de pared (VBUS sin host) | (500, 0, 1000) o (1500, 0, 1500): entrada 2000 mA, carga 1–1,5 A |
| Desenchufado (`CHARGE_REMOVE_MSG`) | (10, 100, 0 si param 0x5f = 1, si no 100) |

**Dónde se guardan los parámetros.** `get_param(n)` (`0x42a500`) lee de la imagen de `user.ini`:

| Parámetro | Offset en `user.ini` | Significado | Valor hoy |
|---|---|---|---|
| param 8 | **+0x274** | Modo USB: 0 = DAC (UAC), 1 = almacenamiento (`system_if_usb_select_device`) | 1 |
| param 0x5f | **+0x6f8** | "USB current limited" (`dac_charge_disable`) [I la etiqueta; V el uso] | 0 |

**Conclusión.** La opción de HiBy sirve para no vaciar el móvil, no para "alimentar por USB sin cargar". Para lo que se pide, hay que usar `axp_battery/charger`.

### 4.3 Diseño para el shell

```sh
# Activar "solo USB" (no carga la batería; el sistema se alimenta de VBUS por el power-path del AXP2101)
echo 0 > /sys/class/power_supply/axp_battery/charger
# Desactivar (vuelve la ICC configurada por el reproductor)
echo 1 > /sys/class/power_supply/axp_battery/charger
```

**Reglas:**
1. **El estado deseado lo guarda el shell** en `/usr/data/hibymod/power.conf` (`nocharge=1`). No se puede fiar de `cat charger`, por §4.1.
2. **Vigilante** (en el bucle del shell, barato):
   - leer `usb/online` cada 1–2 s;
   - en el paso 0→1, esperar **7 s** (más que los 5 s del hilo y que los eventos del reproductor) y escribir `0` otra vez;
   - además, reafirmarlo cada 30 s mientras `online = 1`;
   - al pasar a `online = 0` no hace falta nada.
3. **Protección de batería:** si `battery/capacity` < 20 %, rehabilitar la carga automáticamente y avisar.
   - Variante "cuidado de batería": carga hasta 80 % (`charger = 0`) y reanuda por debajo del 75 % (`charger = 1`).
4. **Leer el estado real:**
   - `axp_battery/status` (Charging / Not charging / Full) [I: falta comprobar que cambia tras `charger = 0`];
   - la tendencia de `battery/voltage_now`: con ICC 0, el voltaje en reposo baja de ~4,36 V y deja de subir.
5. **Prueba pendiente** (con permiso del usuario):
   - con la batería por debajo del 90 % y enchufado al PC, `echo 0 > …/charger`;
   - durante 10 min, la `capacity` y el `voltage_now` no deben subir y `status` debería cambiar;
   - luego `echo 1`.
6. **Límite de entrada.** En un PC el reproductor deja la entrada a 500 mA, que basta para el sistema. Si un pico lo supera, el AXP completa con la batería; no es dañino.

**Detectar la conexión y el "modo DAC":**

| Qué | Cómo |
|---|---|
| VBUS | `/sys/class/power_supply/usb/online` (1/0) [V] |
| Host o cargador | `/sys/class/udc/13500000.otg_new/state`: `configured` con PC o móvil [V, hoy]; con cargador, otro estado (p. ej. `not attached`) [I] |
| Modo DAC elegido | `user.ini` +0x274 == 0 [V] |
| Modo DAC activo | Función UAC enlazada en `/sys/kernel/config/usb_gadget/android0/configs/c.1/` (hoy hay `ffs.adb` y `mass_storage.0`) y existencia de `/dev/uac_sa` [I] |
| "USB current limited" de HiBy | `user.ini` +0x6f8 [V lectura] |

**Dato aparte, que completa §3.6 del informe anterior** [V]:
- `charge_voltage_limit = 4400` y `charge_term_current = 100`. La batería es de 4350 mV de diseño y al llenarse marca 4,362 V.
- Bajar a 4350 en `module_driver/axp2101.sh` sigue siendo lo recomendado.
- El parámetro es escribible (`rw-r--r--`), pero solo se aplica al cargar el módulo [I].

---

## 5. Diseño de los tres modos

Se implementa en `hibyshell` con un ajuste `perf_mode` en `/usr/data/hibymod/power.conf`. Al cambiar de modo o al arrancar se aplican las palancas, y se reaplican al despertar la pantalla y en los eventos USB.

| Palanca | **Ahorro** | **Normal** (por defecto) | **Rendimiento** |
|---|---|---|---|
| Fotogramas del shell | Tope 30 fps; solo en animación | 44 fps en animación, 0 en reposo | 44 fps (el panel no da más), 0 en reposo |
| Animaciones | Más cortas (≤200 ms), sin desenfoque o cristal recalculado (fondos precompuestos) | Las de la guía (250–350 ms) | Las de la guía, con precarga de carátulas o miniaturas |
| Barra de estado / reloj | `present_rows` cada 60 s | Cada 30 s | Cada 30 s |
| Pantalla apagada (`bl_power != 0`) | Shell en pausa total (sin `FBIOPAN`, polling ≥1 s) | Igual | Igual |
| Brillo (`/sys/class/backlight/backlight_pwm0/brightness`, 0–101) | `min(usuario, 40)` | Usuario | Usuario |
| Tiempo hasta apagar la pantalla | 15 s (por el ajuste del reproductor, `bkl_time`) | Usuario | Usuario |
| WiFi | Apagada si no hay streaming o relay (`wifi_off.sh`, o mejor el ajuste del reproductor); si se usa, power save activo (nl80211) | Como la deje el usuario; PS por defecto del driver | Encendida; **PS desactivado** (nl80211 `set_power_save off`) para menos latencia en el relay de YouTube |
| Bluetooth | Apagado si no hay sink A2DP conectado (`bt_disable`, o el ajuste del reproductor) | Usuario | Usuario |
| CPU | 1104 MHz; **opcional, experimental**, CDIV/2 = 552 MHz (§3.3), desactivado por defecto y nunca con DSD | 1104 MHz (de serie) | 1104 MHz (**sin overclock**) |
| Prioridad | Tareas de fondo del shell (miniaturas, escaneo) con `nice 10` y en lotes | `nice 5` | `nice 0`; **nunca por encima de `hiby_player`** (cortes de audio) |
| NAND (`/proc/sys/vm/dirty_expire_centisecs`) | 6000 | 3000 (de serie) | 3000 |
| Carga (`axp_battery/charger`) | Independiente del modo: interruptor propio "Solo USB (no cargar)" y "Cuidado de batería 80 %" (§4.3) | ← | ← |

**Notas de implementación:**
- Guardar los valores originales (brillo, `dirty_*`, CPCCR) para restaurarlos al salir del modo o del shell.
- Si el reproductor reescribe el brillo al despertar, reaplicar el tope de Ahorro tras el evento de pantalla encendida.
- Validar cada modo con el registro de §3.1 (%/h) y con el bucle *gauntlet* de `GUIA_UI_AGENTES.md` (30 fps en Ahorro no debe mostrar fotogramas a medio pintar).

---

## 6. Evidencias principales (comandos de solo lectura)

```
/proc/cpuinfo: "ingenic,x1600_halley6_module_base", BogoMIPS 1099.36, wait instruction: yes
devmem 0x10000000 → 0x9A094410 (CPCCR); 0x10000010 → 0x02E049CD (APLL); 0x10000014 → 0x0AF0C9CD (MPLL)
devmem 0x1000002C → 0xA0000003 (DDR); 0x10000068 → 0x4000000D (MSC0); 0x10000004 → 0x00001F00 (LCR)
devmem 0x00798dc4.. → tabla PLL {1000,125,3,1,1},{900,75,1,2,1},{800,100,1,3,1},{700,175,2,3,1},{600,25,1,1,1},…
/proc/kallsyms: ingenic_cpccr_rate_ro_ops, clk_summary_show; sin devmem_is_allowed ni cpufreq
/sys/class/regulator/regulator.2 (dcdc2): 1100000, min=max=1100000
/sys/class/power_supply/axp_battery/charger = 1 ; usb/constant_charge_current_max = 500 ; usb/charge_control_limit_max = 500
/sys/class/udc/13500000.otg_new/state = configured ; user.ini+0x274 = 1 ; user.ini+0x6f8 = 0
/sys/module/axp2101/parameters/charge_voltage_limit = 4400 ; battery/voltage_now = 4362000 (Full)
/proc/sys/vm: dirty_background_ratio 10, dirty_ratio 20, dirty_expire 3000, dirty_writeback 300 ; nohz_mode 2
```

Desensamblado: `~/hiby/venv/bin/python ~/hiby/tools/kodis.py rootfs/module_driver/axp2101.ko charger_store charger_show axp210x_usb_set_property axp210x_usb_get_property thread_dosomthing axp2101_icchg_set.isra.1`. En `hiby_player`: `hpdis.py 0x481de0 0x481f10`, `0x482040 0x4820e4`, `0x42a500` (tabla de saltos 0x7f09c0) y `0x47dba0`.
